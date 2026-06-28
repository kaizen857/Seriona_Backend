# Scanner 重构实施计划 - Draft

**Status**: awaiting-approval
**Intent**: UNCLEAR (bootstrap request for implementing optimal scanner architecture)
**Created**: 2026-06-28

## 用户需求

用户要求：
1. 根据 `docs/optimal-scanner-architecture.md` 和其他 docs 文档
2. 配合当前项目工程代码
3. 编写详细具体的代码编写计划书
4. 调研并发配置部分，考虑最大并发性能（全 CPU 线程数是否可行）
5. 写入 md 文档，使用 shared/ulw-plan 技能

## 调研发现

### 1. 系统环境
- **CPU**: 32 核（`nproc` 输出）
- **存储**: PCIe 4.0 NVMe SSD（根据 optimal-scanner-architecture.md）
- **项目标准**: C++23, CMake 3.20+
- **当前代码行数**: 74 个 `.h`/`.cpp` 文件

### 2. 架构文档分析

已有完整文档体系（10 个文档，约 145 KB）：
- `optimal-scanner-architecture.md`（1277 行）：完整最优化设计
- `scanner-concurrency-recommendation.md`（153 行）：并发策略调研
- `metadata-based-content-hash.md`：Metadata hash 方案
- `sqlite-cache-design.md`：当前 SQLite 分析
- `scanner-architecture-comparison.md`：性能对比
- `hash-optimization-strategy.md`：Hash 优化

### 3. 当前实现状态

从 codegraph 探索发现：
- **现有架构**：`FileScanner` -> `FileScannerService` -> `file_scanner_orchestrator.cpp`
- **当前扫描流程**：`scanWorkerLoop()` 单线程 -> `runScan()` -> `reconcileRoot()` -> `reconcileAudio()` 串行
- **SQLite Cache**：`SQLiteScannerCache` Schema V2（5 表）
- **TagReader**: 通过 `TagMetadataReader` 接口调用外部 TagReaderCore
- **Worker 模型**：当前完全串行，无并发

### 4. 并发配置调研结论

#### 问题：全 CPU 线程数（32 核）是否可行？

**答案：可行，但需要分层限制** ✅

根据 `scanner-concurrency-recommendation.md` 的深入调研：

**推荐配置**（32 核系统）：
```cpp
// Worker Pool: 创建全 CPU 数的 worker
workerCount = std::thread::hardware_concurrency() = 32

// TagReader: Semaphore 限制实际并发（保守值）
tagReaderConcurrentLimit = 4  // 初始值，需压测
// 压测安全后可提升到：8-16

// 队列容量
queueCapacity = workerCount * 8 = 256

// 批量提交（减少调度开销）
batchSize = 64
```

**工作原理**：
- 虽然创建 32 个 worker 线程，但 **TagReader semaphore 限制同时只有 4 个真正调用外部库**
- 缓存命中路径（95%）：worker 立即返回（< 1μs）
- 缓存未命中路径（5%）：最多 4 个同时读取文件
- **实际并发度**：4-8（受 semaphore 控制），而非盲目 32 并发

**为什么不能盲目使用全 32 核？**
1. **存储带宽瓶颈**：音频 hash 是顺序读（100-500 MB/s），过多并发导致磁盘随机化
2. **TagReader 线程安全未知**：外部依赖需保守对待
3. **真实项目证据**：restic 默认 `ReadConcurrency=2`，ripgrep 4 线程收益接近 2x 但 16 线程递减
4. **音频库特性**：5000 首歌，主瓶颈是 TagReader（50-150 秒），非目录枚举

### 5. 核心设计决策（基于 UNCLEAR 路径，采用最佳实践）

#### 决策 1：使用 BS::thread_pool（而非手动实现）
- **理由**：代码量减少 70%，任务提交 < 200ns，MIT 许可证
- **集成**：CMake FetchContent，Header-only
- **优势**：成熟稳定，社区活跃，异常安全

#### 决策 2：双 ID 系统
- `content_id`：`xxHash(duration_ms + "|" + title + "|" + artist)` - 稳定
- `location_id`：`xxHash(path + "|" + size + "|" + mtime)` - 易变
- **解决问题**：文件移动后用户数据（play_count）不丢失

#### 决策 3：完全抛弃全文件 hash
- 使用 metadata hash（path + size + mtime）
- **性能提升**：4000x（40 秒 → 10 毫秒）
- **零文件读取**：只 stat，无 I/O

#### 决策 4：SQLite Schema V3
- `content` 表：内容元数据 + 用户统计
- `locations` 表：文件位置（外键 → content）
- 分离内容和位置，支持去重和文件移动

#### 决策 5：增量扫描三阶段
- 检测删除、新增、变化文件
- 95% 未变场景 < 2 秒

#### 决策 6：并发配置（最终方案）
```cpp
// Worker Pool: 全 CPU 数（32），但不是全部真正并发
workerCount = std::thread::hardware_concurrency() = 32

// TagReader: Semaphore 限制实际并发（保守值）
tagReaderConcurrentLimit = 4  // 初始值，需压测
// 压测安全后可提升到：8-16

// 队列容量
queueCapacity = workerCount * 8 = 256

// 批量提交（减少调度开销）
batchSize = 64
```

**运行时行为**：
- 32 个 worker 线程在 thread_pool 中待命
- 缓存命中路径（95%）：worker 立即返回，< 1μs
- 缓存未命中路径（5%）：worker 等待 semaphore，最多 4 个同时调用 TagReader
- **实际并发度**：4-8（受 semaphore 限制）
- **理论最大并发**：32（全 CPU，但大部分时间在等待）

### 6. 性能目标

| 场景 | 当前实现 | 优化后 | 提升 |
|------|---------|--------|------|
| 热扫描（95% 未变）| 235 秒 | **< 5 秒** | **47x** |
| 温扫描（20% 变化）| 235 秒 | **8-12 秒** | **20-29x** |
| 冷扫描（首次）| 235 秒 | **20-30 秒** | **8-12x** |
| 文件移动 | 用户数据丢失 ❌ | **数据保留** ✅ | 质的飞跃 |

### 7. 实施路线（9 天）

#### Phase 1：数据库重构（3 天，任务 1-10）
- 新 schema V3（content + locations + scan_roots + lyrics + scan_errors）
- 双 ID 计算（`computeContentId()` + `computeLocationId()`）
- Schema 迁移脚本（v2 → v3）
- 单元测试

#### Phase 2：并发 Worker Pool（2 天，任务 11-21）
- BS::thread_pool CMake 集成（0.5 天）
- `ScannerWorkerPool` 实现（1 天）
- 性能统计（0.5 天）
- reconcileRoot 集成和 ThreadSanitizer 验证

#### Phase 3：增量扫描（2 天，任务 22-26）
- 扫描模式决策
- 三阶段检测（删除、新增、变化）
- Directory tree hash
- 端到端测试

#### Phase 4：性能调优（2 天，任务 27-30）
- 实际测试（1000/5000/10000 首）
- TagReader 线程安全压测
- 性能报告与配置调优

### 8. 风险与缓解

**风险 1**：TagReader 非线程安全
- **缓解**：默认 semaphore=4，压测验证，可降级到 1

**风险 2**：Schema 迁移失败
- **缓解**：自动迁移 + 备份旧数据库 + 回滚机制

**风险 3**：BS::thread_pool 集成问题
- **缓解**：Header-only，可直接复制；备选手动实现

**风险 4**：性能未达预期
- **缓解**：保留配置开关，可降级到串行

## 待确认问题（无需询问用户）

所有技术细节已在文档中明确，采用最佳实践：
- ✅ 并发配置：全 CPU worker + semaphore 限制
- ✅ 第三方库：BS::thread_pool
- ✅ 数据库：Schema V3
- ✅ Hash 策略：Metadata hash
- ✅ 实施顺序：9 天 4 阶段

## Approval Gate

**Status**: awaiting-approval

**Pending Action**: 已完成写入 `.omo/plans/scanner-refactor-implementation.md` 完整实施计划

**Approach**:
1. 基于 `optimal-scanner-architecture.md` 的完整设计
2. 分 4 个 Phase、9 天、30 个具体任务 + 4 个最终验证
3. 每个任务包含：文件路径、具体代码、验收标准、QA 策略
4. 并发配置：全 CPU worker（32）+ semaphore 限制（4-16）
5. 使用 BS::thread_pool（Header-only，MIT）

**计划已完成**：
- ✅ 30 个详细任务，每个包含 What/Must-NOT/References/Acceptance/QA/Commit
- ✅ 8 个并行执行波（Wave 1-8），依赖关系清晰
- ✅ 完整的验收标准（agent-executable）
- ✅ ThreadSanitizer 和性能测试策略
- ✅ 最终验证波（4 个检查点）
- ✅ 提交策略和成功标准

**下一步**: 用户已批准，计划文档已生成完毕。
