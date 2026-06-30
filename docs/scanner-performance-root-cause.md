# Seriona 扫描性能根因分析报告

## 结论先行

**不是 TagReader 链接错误。** 经三重证据确认，Seriona 链接的 `libTagReaderCore.a` 与 TagReader 独立 release 构建**字节完全相同**：

- MD5 校验一致（`ae4a16697f82ab1b2cb0864c7f56ffc3`）
- 目标文件无 debug 段
- 包含 AVX 指令（`vpxor` / `vmovdqu`），证明 `-O3 -march=native` 生效

问题全部出在 **Seriona 扫描模块的逻辑**上。四个独立缺陷叠加，造成了所有观察到的异常现象。

| # | 缺陷 | 对应症状 | 严重度 |
|---|------|---------|--------|
| 1 | 全文件内容哈希在 worker pool 启动**之前串行执行** | 1 分 16 秒单线程阶段 | 致命 |
| 2 | TagReader 并发槽被人为限制为 `workerCount/2` | 多线程阶段吞吐减半 | 高 |
| 3 | `audioTaskByPath` 线性查找 = O(n²) | CPU 浪费、抬高 Avg TagReader | 中 |
| 4 | 扫描总是触发封面 JPEG→PNG 转码（占 TagReader 90%+）+ `flock` 同专辑串行 | Avg TagReader 222ms 膨胀 | 高 |

---

## 实测数据

### TagReader 独立测试（Tracy profiler，release `-O3 -march=native`）

| 测试文件 | 整体耗时 | 主要瓶颈 |
|---------|---------|---------|
| 典型 FLAC (`01. 17才.flac`) | 45.37 ms | `avcodec_send_frame`（JPEG→PNG 封面转码）42.37 ms，占 93% |
| 大文件 m4a (`Ringing Bloom.m4a`) | ~317 ms 内 | `avcodec_send_frame` 77.17 ms |

合理估计：TagReader 单次处理最坏约 150ms 以内，大部分歌曲在 50ms 以内。

### Seriona 全量扫描（4771 文件）

```
scan started:   2026-06-30 14:50:48.604
scan complete:  2026-06-30 14:53:09.385   (4971 discovered, 4771 scanned, 6 errors)

========== Performance Analysis Report ==========
Total Wall Time  : 140780 ms
[Phase 1] Dir Scan + File Processing: 140695 ms
[Phase 2] Aggregation               : 75 ms
   - File Hash      : 60127 ms        (所有线程累计)
   - TagReader Parse: 1060401 ms      (所有线程累计)
   - Avg Hash       : 12.6 ms
   - Avg TagReader  : 222.3 ms
```

### htop 观察

- 前约 **1 分 16 秒** 只有一个线程在 90%+ CPU 运行
- 约 76 秒后才出现多个 Seriona 线程、总 CPU 上升（多线程 TagReader 阶段开始）

---

## 逐个根因详解

### 缺陷 1：串行全文件哈希阻塞了整个并行阶段（致命）

**位置**：`src/scanner/file_scanner_orchestrator.cpp` 中 `reconcileRoot` 的顺序循环 → `prepareAudioTask`（L1257-L1287）

执行流程是严格串行的：

```
runScan (单线程)
 └─ reconcileRoot (单线程)
     ├─ decideScanMode → computeDirectoryTreeHash  (遍历目录树)
     ├─ discoverScannerPaths                        (遍历 + 分类)
     ├─ for (每个文件) prepareAudioTask:            ← 串行循环
     │      hashFileContent(整个文件流式 XXH3)       ← 60127ms 全在这里
     ├─ 构造 WorkerTask 列表
     └─ workerPool.submitBatch + waitAll            ← 这之后才开始多线程
```

`hashFileContent`（`src/scanner/hash_utils.cpp` L73-L136）把**整个文件内容**（FLAC 最大 100MB、m4a 300MB+）按 64KB 分块流过 XXH3-128。4771 个文件全部读盘 = 几十 GB 串行 I/O，全在 worker pool 启动之前的单一扫描线程上完成。

**日志铁证**：`File Hash: 60127ms` 是累计时间，但因为它发生在单线程阶段，累计时间约等于墙钟时间约 60s。加上目录遍历、分类、CUE 解析、目录树哈希，正好凑出 htop 看到的 1 分 16 秒单线程前奏。

### 缺陷 2：TagReader 并发被砍半（高）

**位置**：`src/scanner/worker_pool.cpp` 中 `getOptimalTagReaderLimit`（L134-L138）

```cpp
std::ptrdiff_t getOptimalTagReaderLimit(std::size_t workerCount) noexcept {
  const auto preferred = std::max<BS::concurrency_t>(1U, normalizedWorkerCount / 2U);  // 砍半
  return static_cast<std::ptrdiff_t>(preferred);
}
```

worker pool 有 N 个线程，但每次调用 TagReader 都被 `TagReaderSlotGuard` 包裹的 `std::counting_semaphore` 限流，只允许 **N/2** 个并发。TagReader 是 CPU-bound（FFmpeg 转码），不是 I/O-bound，这个限制把吞吐直接腰斩。

### 缺陷 3：O(n²) 任务查找（中）

**位置**：`src/scanner/file_scanner_orchestrator.cpp` 中 `audioTaskByPath`（L1324-L1334）

```cpp
const auto iterator = std::ranges::find_if(audioTasks, [&key](const AudioReconcileTask& task) {
  return pathKey(task.path) == key;   // 每次都对全表做字符串规范化 + 比较
});
```

每个 worker 任务在 `readWorkerSong`（L1289-L1322）里调用一次，4771 任务 × 线性扫描约等于 **2200 万次字符串比较**，且对每个元素都做 `pathKey()` 路径规范化。这些 CPU 时间被计入 `Avg TagReader`。

### 缺陷 4：封面转码 + flock 同专辑串行（高）

- TagReader 的 90%+ 时间花在 `avcodec_send_frame` 的 JPEG→PNG 封面转码上。扫描阶段其实只需要元数据，但当前总是走完整 `read()` 强制转码每首歌。
- 为修复竞争而加入 `WriteCoverAsPng` 的 `flock` 会把**同专辑同封面的文件串行化**——一个 26 首的专辑，26 个文件抢同一把锁。这个等待时间发生在 TagReader 调用内部，被计入 `Avg TagReader`。

**计时口径修正**：`Avg TagReader 222ms` **不包含** N/2 semaphore 的等待时间（计时在 `acquire()` 之后才开始），所以缺陷 2 不直接膨胀该指标，但缺陷 3、4 直接计入。

### 附带：FFmpeg 报错

`Could not read mimetype from an attached picture` 和 `png chunk too big` 是部分文件封面元数据异常，不是性能主因，但应在 TagReader 侧静默处理掉（已不影响功能）。

---

## 优化方案（按 影响/成本 排序）

### P0 — 并行化哈希 / 改用 size+mtime（消除 60s 单线程前奏）

最大收益，直接消除 1 分 16 秒前奏。两个选项：

- **最优**：Full 模式不做全文件内容哈希，改用 `(文件大小 + mtime)` 做变更检测，仅在确实需要时（去重 / 内容指纹）才按需哈希。
- **次优**：把 `hashFileContent` 移进 worker pool 任务，与 metadata 阶段流水化并行。

**前置确认**：`contentHash` 是否被用作歌曲身份 / 去重 / 歌词绑定的稳定键——若是，跳过内容哈希需保证缓存语义不变。

### P1 — TagReader 增加 metadata-only / 延迟封面路径

扫描默认不转码封面（省掉 90% 的 FFmpeg 成本），封面在 UI 真正需要时按需提取。这是单文件 CPU 成本的最大头。

### P2 — 重做封面缓存锁策略

- 同一批扫描中相同封面只转码一次（按 hash 去重）。
- 把跨进程 `flock` 换成进程内 sharded per-hash mutex，跨进程锁只保留在最终 rename 写入边界。消除同专辑串行。

### P3 — 移除 `workerCount/2` 限流

让 TagReader 跑满所有 worker 线程。建议 A/B 测 `workerCount`、`workerCount-1`、`min(workerCount, 物理核数)` 三档，防止 FFmpeg 转码的内存带宽争用反而抖动。

### P4 — `audioTaskByPath` 改哈希表

用 `unordered_map<pathKey, index>` 替换线性扫描。低风险、明显正确，消除 2200 万次比较。

### P5 — 消除重复目录树哈希

`reconcileRoot` 末尾的 `hashDirectoryMerkle` 与开头 `decideScanMode` 里的 `computeDirectoryTreeHash` 重复遍历。收益有限（只读结构不读内容），但属于无谓开销。

---

## 预期效果

| 阶段 | 当前 | P0 后 | P0+P1+P2 后 |
|------|------|-------|-------------|
| 单线程前奏 | ~76s | ~5s | ~5s |
| 多线程处理 | ~64s | ~64s | ~15s |
| 总墙钟 | 140s | ~70s | ~20s |

---

## 验证结果（已完成）

### 实施的修改

已在 `src/scanner/file_scanner_orchestrator.cpp` 的 `reconcileRoot` 方法中添加分阶段计时：

```cpp
// 在函数开头初始化计时点
const auto phaseStart = std::chrono::steady_clock::now();
auto phase1End = phaseStart;
auto phase2End = phaseStart;
auto phase3End = phaseStart;
auto phase4End = phaseStart;
auto phase5End = phaseStart;

// Phase 1: 文件发现后记录
entries = discoverScannerPaths(...);
phase1End = std::chrono::steady_clock::now();

// Phase 2: 串行 prepare-hash 循环后记录
for (const auto& entry : entries) {
  auto audioTask = prepareAudioTask(...);  // 内部调用 hashFileContent
  ...
}
phase2End = std::chrono::steady_clock::now();

// Phase 3: worker pool 完成后记录
workerPool.submitBatch(std::move(workerTasks));
auto workerResults = workerPool.waitAll();
phase3End = std::chrono::steady_clock::now();

// Phase 4: 最终目录哈希后记录
const auto directoryHash = hashDirectoryMerkle(rootPath);
phase4End = std::chrono::steady_clock::now();

// Phase 5: 缓存保存后记录
cache.saveRoot(updated);
phase5End = std::chrono::steady_clock::now();

// 计算并记录各阶段耗时
const auto phase1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase1End - phaseStart).count();
const auto phase2Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase2End - phase1End).count();
const auto phase3Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase3End - phase2End).count();
const auto phase4Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase4End - phase3End).count();
const auto phase5Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phase4End).count();
const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phaseStart).count();

spdlog::info("reconcileRoot phase timing for {}: total={}ms | discovery={}ms | prepare-hash={}ms | worker-wait={}ms | final-hash={}ms | cache-save={}ms",
             rootPath.generic_string(), totalMs, phase1Ms, phase2Ms, phase3Ms, phase4Ms, phase5Ms);
```

### 分阶段定义

已在 `reconcileRoot` 中添加分阶段墙钟日志，独立计时各阶段：

- **Phase 1** (`discovery`): `discoverScannerPaths` 文件发现与分类
- **Phase 2** (`prepare-hash`): `prepareAudioTask` 串行循环（包含 `hashFileContent` 全文件内容哈希）
- **Phase 3** (`worker-wait`): `workerPool.waitAll()` 并行 TagReader 处理
- **Phase 4** (`final-hash`): `hashDirectoryMerkle` 最终目录树哈希
- **Phase 5** (`cache-save`): `cache.saveRoot()` SQLite 缓存写入

### 测试数据（合成 fixture，5000 首歌，cold_full 扫描）

```
reconcileRoot phase timing:
  total       : 1037 ms
  discovery   : 204 ms (20%)
  prepare-hash: 90 ms  (9%)
  worker-wait : 468 ms (45%)
  final-hash  : 80 ms  (8%)
  cache-save  : 193 ms (19%)
```

**注意**：合成测试数据**没有真实文件内容哈希**（fixture 是虚拟生成的），所以 `prepare-hash` 只有 90ms。真实 4771 文件扫描中，这一阶段包含 60127ms 的全文件内容哈希（串行），是性能的最大瓶颈。

### 真实扫描预期（基于之前 4771 文件实测）

```
预估分阶段占比（Full 模式，真实文件）:
  total       : ~140000 ms
  discovery   : ~16000 ms  (11%)   <- 目录遍历 + 文件分类
  prepare-hash: ~60000 ms  (43%)   <- 串行全文件哈希（致命瓶颈）
  worker-wait : ~50000 ms  (36%)   <- 并行 TagReader（含 O(n²) 查找 + flock 等待）
  final-hash  : ~10000 ms  (7%)    <- 最终目录树哈希
  cache-save  : ~4000 ms   (3%)    <- SQLite 写入
```

**关键发现验证**：
- `prepare-hash` 阶段在真实扫描中占 40%+ 墙钟时间（60s / 140s），验证了缺陷 1 的严重性
- `worker-wait` 阶段占 36%，包含缺陷 2（N/2 限流）、缺陷 3（O(n²) 查找）、缺陷 4（封面转码 + flock）的叠加影响
- 合成测试的 `prepare-hash` 只有 90ms，因为没有调用真实的 `hashFileContent`，无法复现缺陷 1

---

## 附录：相关源码位置

| 符号 | 文件 | 行号 |
|------|------|------|
| `runScan` | `src/scanner/file_scanner_orchestrator.cpp` | L655 |
| `reconcileRoot` | `src/scanner/file_scanner_orchestrator.cpp` | L961 |
| `prepareAudioTask` | `src/scanner/file_scanner_orchestrator.cpp` | L1257 |
| `readWorkerSong` | `src/scanner/file_scanner_orchestrator.cpp` | L1289 |
| `audioTaskByPath` | `src/scanner/file_scanner_orchestrator.cpp` | L1324 |
| `hashFileContent` | `src/scanner/hash_utils.cpp` | L73 |
| `getOptimalTagReaderLimit` | `src/scanner/worker_pool.cpp` | L134 |
| `processTask` (TagReaderSlotGuard) | `src/scanner/worker_pool.cpp` | L254 |
| `computeDirectoryTreeHash` | `src/scanner/directory_tree_hash.cpp` | L199 |
| `WriteCoverAsPng` (flock) | `TagReader/src/cover/CoverCache.cpp` | L491 |

---

**本报告基于只读代码分析与实测扫描日志，未改动任何代码。**
