# Scanner 架构对比：旧 MusicPlayer vs 新 Seriona

## 执行摘要

旧 MusicPlayer 项目对 5209 个文件的扫描仅需 **3.4 秒**，而当前 Seriona 架构是**单线程串行**。对比分析显示：旧项目采用**四阶段流水线 + 全局线程池并发**，实现了目录遍历、音频解析、封面处理的完全重叠；新项目需要引入类似架构才能达到相似性能。

---

## 旧 MusicPlayer 架构（Qt 项目）

### 核心设计

**四阶段流水线**：
1. **Phase 1 - 目录扫描**（主线程，98ms）：递归遍历文件系统，识别音频文件、CUE、封面图片、子目录
2. **Phase 2 - 音频解析**（worker 池，2877ms wall time）：并发执行 FFmpeg probe + TagLib parse
3. **Phase 3 - 聚合**（主线程，35ms）：统计、树结构汇总、触发延迟封面任务
4. **Phase 4 - 封面处理**（worker 池，89ms wall time）：内嵌封面提取、缩放、缓存

**线程模型**：
- 全局单例 `SimpleThreadPool`，所有扫描共享
- 批量提交（`K_BATCH_SIZE=64`）：减少任务调度开销
- 主线程负责目录遍历、结果聚合、事件发布
- Worker 只执行纯计算任务（FFmpeg、TagLib、封面解码），不修改树结构

**性能统计**：
- 区分 wall time（Phase 1-4）和 cumulative worker CPU time（FFmpeg、TagLib、Cover）
- 用 `std::atomic<int64_t>` 累计 worker 耗时，避免锁竞争
- `ScopedTimer` RAII 自动计时

### 性能报告格式

```
========== Performance Analysis Report ==========
Total Wall Time  : 3409.34 ms
Processed Files  : 5209
-----------------------------------------------
[Phase 1] Dir Scan (Main Thread): 98.028 ms
[Phase 2] Wait Audio (Wall Time): 2877.12 ms
[Phase 3] Aggregation           : 35.395 ms
[Phase 4] Wait Covers (Wall Time):89.076 ms
-----------------------------------------------
>> Cumulative Worker CPU Time (Sum of all threads):
   - FFmpeg Probe   : 20433.7 ms
   - TagLib Parse   : 12610.6 ms
   - Cover Process  : 44087.1 ms
===============================================
```

**关键洞察**：
- Phase 1 目录扫描仅占 2.9%（98ms / 3409ms），证明目录遍历不是瓶颈
- Phase 2 占 84.4%（2877ms），但 cumulative worker CPU time 高达 33秒（20433+12610），说明并发度约 11-12x
- Phase 4 封面处理的 cumulative time（44秒）远超 wall time（89ms），并发度约 500x，说明封面是 I/O 轻量级任务

### 代码证据

**目录遍历 + 批量分发**（`scanAndDispatch`）：
```cpp
// 主线程递归遍历目录
static void scanAndDispatch(const fs::path &dirPath, 
                            const std::shared_ptr<PlaylistNode> &currentNode, 
                            std::stop_token stoken) {
    std::vector<std::shared_ptr<PlaylistNode>> audioNodesToSubmit;
    auto &pool = SimpleThreadPool::instance().get_native_pool();
    
    // 遍历当前目录，收集音频文件
    for (const auto &entry : fs::directory_iterator(dirPath)) {
        if (isAudio(entry)) {
            auto fileNode = std::make_shared<PlaylistNode>(entry.path(), false);
            currentNode->addChild(fileNode);
            audioNodesToSubmit.push_back(fileNode);
        }
    }
    
    // 批量提交到线程池（64个一批）
    for (size_t i = 0; i < total; i += K_BATCH_SIZE) {
        std::vector<std::shared_ptr<PlaylistNode>> batch(...);
        pool.submit_task([batch = std::move(batch)]() {
            for(const auto& node : batch) processNodeTask(node);
        });
    }
    
    // 递归子目录（仍在主线程）
    for (const auto &subDir : subDirs) {
        auto childNode = std::make_shared<PlaylistNode>(subDir, true);
        scanAndDispatch(subDir, childNode, stoken);
        currentNode->addChild(childNode);
    }
}
```

**Worker 任务**（`processNodeTask`）：
```cpp
static void processNodeTask(std::shared_ptr<PlaylistNode> node) {
    {
        ScopedTimer timer(Profiler::t_taglib);  // 累计到原子变量
        // TagLib 解析标签
        TagLib::FileRef f(path);
        // ... 提取 title/artist/album/year
    }
    {
        ScopedTimer timer(Profiler::t_ffmpeg);
        // FFmpeg 读取技术参数
        AVFormatContext *ctx = ...;
        // ... 获取 duration/sampleRate/bitDepth
    }
    Profiler::count_files++;
}
```

**四阶段主循环**（`scanDir`）：
```cpp
void FileScanner::scanDir(std::stop_token stoken) {
    Profiler::reset();
    auto start_total = std::chrono::high_resolution_clock::now();
    
    // Phase 1: 目录扫描 + 任务分发
    {
        ScopedTimer t1(Profiler::t_structure_scan);
        ScannerLogic::scanAndDispatch(rootPath, rootNode, stoken);
    }
    
    // Phase 2: 等待音频解析完成
    {
        ScopedTimer t2(Profiler::t_wait_audio);
        SimpleThreadPool::instance().get_native_pool().wait();
    }
    
    // Phase 3: 后处理聚合（统计、封面分发）
    {
        ScopedTimer t3(Profiler::t_aggregation);
        ScannerLogic::postProcessAggregation(rootNode);
    }
    
    // Phase 4: 等待封面任务完成
    {
        ScopedTimer t4(Profiler::t_wait_cover);
        SimpleThreadPool::instance().get_native_pool().wait();
    }
    
    auto end_total = std::chrono::high_resolution_clock::now();
    Profiler::printReport(total_us);
}
```

---

## 当前 Seriona 架构（C++23 后端）

### 核心设计

**单阶段串行**：
- 单个 `scanWorkerLoop()` 后台线程，从队列取 `ScanRequest`
- `runScan()` 串行遍历 roots，调用 `reconcileRoot()`
- `reconcileRoot()` 串行枚举目录（`discoverScannerPaths()`），然后逐个文件调用 `reconcileAudio()`
- `reconcileAudio()` 串行执行：文件 hash → SQLite cache 查询 → TagReader → lyrics → 错误收集

**无并发**：
- 所有操作在单线程顺序执行
- TagReader 外部库可能内部并发，但 Seriona 不知情
- 性能完全受限于单线程吞吐

**性能统计**（刚添加）：
```cpp
void runScan(const std::vector<ScannerRoot>& roots, ScanMode) {
    const auto scanStartTime = std::chrono::steady_clock::now();
    std::uint64_t totalHashTimeMs = 0;
    std::uint64_t totalTagReaderTimeMs = 0;
    
    // ... 串行处理所有文件 ...
    
    const auto scanEndTime = std::chrono::steady_clock::now();
    spdlog::info("scan performance: total={}ms, hash={}ms, tagreader={}ms, other={}ms",
                 totalScanTimeMs, totalHashTimeMs, totalTagReaderTimeMs,
                 totalScanTimeMs - totalHashTimeMs - totalTagReaderTimeMs);
}
```

**当前输出示例**：
```
[info] scan complete: 2 discovered, 2 scanned, 0 skipped, 3 errors
[info] scan performance: total=2ms, hash=0ms, tagreader=0ms, other=2ms
[info] scan performance per file: avg_hash=0.0ms, avg_tagreader=0.0ms
```

**问题**：
- 没有 phase 划分，无法识别瓶颈在目录遍历还是文件处理
- 没有 cumulative worker CPU time，无法评估并发收益
- 无法区分 wall time vs CPU time，并发效果不可测量

---

## 对比分析

| 维度 | 旧 MusicPlayer | 新 Seriona | 差距 |
|------|---------------|-----------|------|
| **并发模型** | 全局线程池 + 批量提交 | 单线程串行 | ❌ 无并发 |
| **目录遍历** | 主线程递归，即遍历即分发 | 主线程递归，遍历后串行处理 | ⚠️ 遍历后串行 |
| **音频解析** | Worker 池并发（FFmpeg+TagLib） | 串行调用 TagReader | ❌ 无并发 |
| **封面处理** | 延迟到 Phase 4 并发处理 | 内联在 `reconcileAudio()` | ❌ 串行 |
| **缓存策略** | 内存 memo + 封面 CAS | SQLite root cache + content hash | ✅ SQLite 更持久 |
| **性能报告** | 4 阶段 + cumulative CPU time | 单阶段 + 总计时 | ❌ 缺少细分 |
| **批量调度** | 64 个文件一批 | 无批量概念 | ❌ 每次单文件 |
| **取消支持** | `std::stop_token` 协作式 | `atomic<bool>` 取消标志 | ✅ 都支持 |

**性能预估**（5000 首歌）：
- 旧项目实测：3.4 秒
- 新项目估算（串行）：
  - 目录枚举：~100ms（与旧项目相近）
  - 文件 hash（假设 40GB @ 250MB/s）：~160 秒
  - TagReader（假设 15ms/首）：~75 秒
  - 串行总计：~235 秒（约 **70x 慢**）

**并发潜力**（基于旧项目证据）：
- 音频解析并发度：11-12x（2.9秒 wall time / 33秒 CPU time）
- 封面处理并发度：500x（89ms wall time / 44秒 CPU time，但封面是轻量级）
- 如果新项目引入 4-8 worker + 批量提交，预计可降到 20-40 秒（仍比旧项目慢 6-12x，因为额外的文件 hash）

---

## Seriona 改进建议

### 1. 引入四阶段架构

**Phase 1 - 目录枚举**（主线程）：
```cpp
void runScan(const std::vector<ScannerRoot>& roots, ScanMode) {
    const auto scanStartTime = std::chrono::steady_clock::now();
    std::uint64_t phaseEnumTimeMs = 0;
    std::uint64_t phaseAudioWaitTimeMs = 0;
    std::uint64_t phaseAggregationTimeMs = 0;
    std::uint64_t phaseCoverWaitTimeMs = 0;
    
    // Phase 1: 目录枚举 + 任务分发
    {
        const auto t1 = std::chrono::steady_clock::now();
        for (const auto& root : roots) {
            const auto entries = discoverScannerPaths(root, pathConfig);
            for (const auto& entry : entries) {
                if (isAudioFile(entry)) {
                    audioTaskQueue.push({entry.path, cachedRootPtr, ...});
                }
            }
        }
        phaseEnumTimeMs = elapsedMs(t1);
    }
    
    // Phase 2: 等待音频任务完成
    {
        const auto t2 = std::chrono::steady_clock::now();
        threadPool.wait();
        phaseAudioWaitTimeMs = elapsedMs(t2);
    }
    
    // Phase 3: 聚合 + 封面分发
    {
        const auto t3 = std::chrono::steady_clock::now();
        aggregateResults();
        dispatchCoverTasks();
        phaseAggregationTimeMs = elapsedMs(t3);
    }
    
    // Phase 4: 等待封面任务
    {
        const auto t4 = std::chrono::steady_clock::now();
        threadPool.wait();
        phaseCoverWaitTimeMs = elapsedMs(t4);
    }
}
```

### 2. 添加 Cumulative Worker CPU Time

**原子累计计时器**：
```cpp
struct ScannerPerfStats {
    std::atomic<int64_t> totalHashTimeUs{0};
    std::atomic<int64_t> totalTagReaderTimeUs{0};
    std::atomic<int64_t> totalCoverTimeUs{0};
    std::atomic<int64_t> totalCacheQueryTimeUs{0};
    std::atomic<int> filesProcessed{0};
};

// Worker 中使用 RAII 计时
struct ScopedPerfTimer {
    std::atomic<int64_t>& target;
    std::chrono::steady_clock::time_point start;
    
    ScopedPerfTimer(std::atomic<int64_t>& counter) 
        : target(counter), start(std::chrono::steady_clock::now()) {}
    
    ~ScopedPerfTimer() {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        target.fetch_add(elapsed, std::memory_order_relaxed);
    }
};

// 在 reconcileAudio() 中
void reconcileAudioWorker(AudioTask task, ScannerPerfStats& stats) {
    {
        ScopedPerfTimer timer(stats.totalHashTimeUs);
        hash = hashFileContent(task.path);
    }
    {
        ScopedPerfTimer timer(stats.totalCacheQueryTimeUs);
        cachedSong = cache.findByHash(hash);
    }
    if (!cachedSong) {
        ScopedPerfTimer timer(stats.totalTagReaderTimeUs);
        metadata = metadataReader_->read(task.path, coverExportDir_);
    }
    stats.filesProcessed.fetch_add(1, std::memory_order_relaxed);
}
```

### 3. 性能报告格式升级

```cpp
void printScannerPerfReport(int64_t totalWallTimeMs, const ScannerPerfStats& stats) {
    spdlog::info("\n========== Performance Analysis Report ==========");
    spdlog::info("Total Wall Time  : {:.2f} ms", totalWallTimeMs);
    spdlog::info("Processed Files  : {}", stats.filesProcessed.load());
    spdlog::info("-----------------------------------------------");
    spdlog::info("[Phase 1] Dir Enum (Main Thread) : {:.2f} ms", phaseEnumTimeMs);
    spdlog::info("[Phase 2] Wait Audio (Wall Time)  : {:.2f} ms", phaseAudioWaitTimeMs);
    spdlog::info("[Phase 3] Aggregation             : {:.2f} ms", phaseAggregationTimeMs);
    spdlog::info("[Phase 4] Wait Covers (Wall Time) : {:.2f} ms", phaseCoverWaitTimeMs);
    spdlog::info("-----------------------------------------------");
    spdlog::info(">> Cumulative Worker CPU Time (Sum of all threads):");
    spdlog::info("   - File Hash      : {:.2f} ms", stats.totalHashTimeUs.load() / 1000.0);
    spdlog::info("   - Cache Query    : {:.2f} ms", stats.totalCacheQueryTimeUs.load() / 1000.0);
    spdlog::info("   - TagReader Parse: {:.2f} ms", stats.totalTagReaderTimeUs.load() / 1000.0);
    spdlog::info("   - Cover Process  : {:.2f} ms", stats.totalCoverTimeUs.load() / 1000.0);
    spdlog::info("===============================================");
    
    // 计算并发度
    int64_t totalCpuTimeMs = (stats.totalHashTimeUs.load() + 
                              stats.totalCacheQueryTimeUs.load() +
                              stats.totalTagReaderTimeUs.load() + 
                              stats.totalCoverTimeUs.load()) / 1000;
    if (phaseAudioWaitTimeMs > 0) {
        double concurrency = static_cast<double>(totalCpuTimeMs) / phaseAudioWaitTimeMs;
        spdlog::info("Effective Concurrency: {:.1f}x", concurrency);
    }
}
```

**目标输出**：
```
========== Performance Analysis Report ==========
Total Wall Time  : 28450.23 ms
Processed Files  : 5209
-----------------------------------------------
[Phase 1] Dir Enum (Main Thread) : 102.15 ms
[Phase 2] Wait Audio (Wall Time)  : 25320.45 ms
[Phase 3] Aggregation             : 48.32 ms
[Phase 4] Wait Covers (Wall Time) : 2979.31 ms
-----------------------------------------------
>> Cumulative Worker CPU Time (Sum of all threads):
   - File Hash      : 85432.12 ms
   - Cache Query    : 1205.67 ms
   - TagReader Parse: 12890.34 ms
   - Cover Process  : 3421.89 ms
===============================================
Effective Concurrency: 4.1x
```

### 4. 批量任务提交

```cpp
void reconcileRootConcurrent(const ScannerRoot& root, ...) {
    const auto entries = discoverScannerPaths(root, pathConfig);
    std::vector<AudioTask> tasks;
    
    for (const auto& entry : entries) {
        if (isAudioFile(entry)) {
            tasks.push_back({entry.path, cachedRootPtr, ...});
        }
    }
    
    // 批量提交（64个一批，减少调度开销）
    constexpr size_t BATCH_SIZE = 64;
    for (size_t i = 0; i < tasks.size(); i += BATCH_SIZE) {
        size_t end = std::min(i + BATCH_SIZE, tasks.size());
        std::vector<AudioTask> batch(tasks.begin() + i, tasks.begin() + end);
        
        threadPool.submit([batch = std::move(batch), &stats, &results]() {
            for (const auto& task : batch) {
                auto result = reconcileAudioWorker(task, stats);
                results.push_back(result);
            }
        });
    }
}
```

---

## 实施优先级

### 高优先级（立即）
1. ✅ **添加 cumulative worker CPU time 统计**：用原子变量 + `ScopedPerfTimer`，零侵入性
2. ✅ **改进性能报告格式**：匹配旧项目输出，便于对比
3. ⚠️ **Phase 划分**：标记当前四个关键阶段（枚举、等待、聚合、完成），即使现在都是串行

### 中优先级（并发改造前）
4. **建立基线**：在 `/home/kaizen857/Music/` 运行当前串行版本，记录完整 phase breakdown
5. **cachedSongByPath() 优化**：从 O(n) 线性查找改为 `unordered_map<path, CachedSong*>`
6. **提取纯 worker 函数**：`reconcileAudioWorker()` 无副作用版本，便于并行化

### 低优先级（并发改造）
7. **引入 root-local 线程池**：worker 数 2-4，队列容量 `worker_count * 8`
8. **批量任务提交**：64 个文件一批
9. **TagReader semaphore**：默认并发度 1-2，确保线程安全

---

## 关键差异总结

| 特性 | 旧 MusicPlayer（3.4秒） | 新 Seriona（预估 235秒） |
|------|----------------------|---------------------|
| 并发架构 | 全局线程池 + 四阶段流水线 | 单线程串行 |
| 性能报告 | Phase breakdown + cumulative CPU time | 单条总计日志 |
| 文件 hash | 无（依赖 mtime） | 强制全文件 hash（慢） |
| TagReader | 直接调用 TagLib + FFmpeg | 外部库（线程安全未知） |
| 封面处理 | 延迟并发（Phase 4） | 内联串行 |
| 批量调度 | 64 个一批 | 无批量 |
| 缓存粒度 | 内存 memo（轻量） | SQLite root（持久但慢） |

**核心瓶颈**：Seriona 的全文件 hash 是不可避免的（用于 cache 一致性），但可以通过并发、批量提交、Phase 流水线将影响降到最低。目标是达到旧项目的 phase 划分清晰度和并发度可测量性。
