# Scanner 并发扫描策略调研与 Seriona 建议

## 结论摘要

Seriona 当前最适合采用“串行目录枚举 + 有界并行文件处理”的两阶段模型：`reconcileRoot()` 继续负责确定根目录快照、目录 hash、缓存根和最终提交顺序；音频文件进入一个容量受限的工作队列，由小型固定线程池并行执行 `reconcileAudio()` 中最慢的文件 hash、SQLite 缓存命中判断和 `metadataReader_->read()`。不建议首轮改成完全并行目录遍历，也不建议引入 C++23 coroutine/async I/O。

推荐默认值：保留单个 `scanWorkerLoop()` 作为一次扫描的协调线程；每个 root 内启动 `min(4, max(2, hardware_concurrency / 2))` 个文件处理 worker，队列容量为 `worker_count * 4` 到 `worker_count * 8`；所有 SQLite 写入、根结果合并、`publishEvent()` 和 playlist snapshot 发布仍回到协调线程按确定顺序执行。对 1000-10000 个音频文件、消费级 SSD、缓存命中率较高的典型库，预计冷扫描可提升约 1.5x-3x，热扫描可提升约 1.2x-2x；上限主要受文件 hash 顺序读带宽、TagReader 线程安全/内部并发、SQLite 提交策略限制。

## 当前 Seriona 扫描链路

`scanWorkerLoop()` 在单个后台线程中消费 `ScanRequest`，然后调用 `runScan()`。`runScan()` 串行遍历 roots，每个 root 调 `reconcileRoot()`。`reconcileRoot()` 先通过 `discoverScannerPaths()` 枚举文件系统，然后对每个音频文件调用 `reconcileAudio()`。`reconcileAudio()` 当前是串行热点：它会计算文件内容 hash，查 SQLite root cache，必要时调用外部 TagReader 适配器的 `metadataReader_->read()`，并组装 `CachedSong`、歌词、错误和发布用 metadata。

这条链路有几个重要约束：

1. SQLite cache 的公共契约以 root 为单位加载和保存，`SQLiteScannerCache::saveRoot()` 单事务替换 root 的目录、歌曲、错误；不要让多个 worker 直接并发写同一个 root。
2. 当前事件版本号和 `FileScanned` 发布顺序由 `runScan()` 串行控制；并发处理后也应由协调线程发布事件，避免 event version 与 UI 快照乱序。
3. `TagReader` 是外部依赖，Seriona 只知道调用 `metadataReader_->read()`；其线程安全和内部线程模型未知，因此并发度要保守，并需要可配置降级到 1。
4. 新增的性能统计已经区分 hash time 与 TagReader time，可作为并发改造前后的基线和自动调参依据。

## 真实项目证据

### ripgrep / ignore crate

ripgrep 的 `ignore` crate 提供 `WalkParallel`，官方文档描述为多线程递归目录迭代器。其实现使用固定 worker 与 work-stealing stack；源码注释说明使用 stack 是为了偏深度优先，降低路径和 ignore matcher 的峰值内存。`WalkBuilder::threads(n).build_parallel()` 是主要入口，`threads(0)` 会按启发式自动选择线程数。

这证明“完全并行目录遍历 + work stealing”在大量文件、复杂过滤规则、搜索工作直接附着在遍历回调上的场景很有效。ripgrep 作者在讨论中也指出，目录遍历并行度不是越高越好：4 线程相对 1 线程可接近 2x，但 8/16 线程要看树形结构、ignore 文件成本和调度开销。

### fd

fd 直接基于 `ignore::WalkBuilder` 构建 parallel walker。其 README 明确把“parallelized directory traversal”作为速度来源，并提供 `-j/--threads` 控制线程数；默认线程数取 `available_parallelism()`，并设置上限 64。fd 的 `--exec` 也支持对匹配结果并行执行外部命令，并允许 `--threads=1` 回退串行。

这说明文件搜索工具常把“遍历”和“后续轻量处理”合并到同一并行 walker 中，但它们通常不需要维护 Seriona 这种 root 级 cache transaction、稳定 playlist tree、TagReader 外部库调用与事件发布顺序。

### restic

restic 的备份路径采用更接近 Seriona 的模型：主 goroutine 遍历目录树，并把文件读取、tree 保存、blob 上传委托给 worker pools。其 `Options` 中 `ReadConcurrency` 默认值为 2，注释说明这是实验后对多数场景的 sweet spot；文档也提示 fast NVMe 可以提高 read concurrency，但过高连接/并发会增加资源消耗并降低性能。restic 的扫描统计本身仍可串行遍历，且提供 `--no-scan` 避免额外文件系统 I/O 拖慢网络/FUSE。

这证明对会读取文件内容、上传/保存、维护目录结构的系统，生产实践更偏向“枚举协调线程 + 有界 worker pool”，而不是把目录递归本身无限并行化。

### beets

beets 是音频库管理器。近期改动把多个 metadata source plugin 的候选查找改成 `ThreadPoolExecutor` 并发执行，PR 说明目标是加速 I/O-bound lookup，启用多个插件时可减少数秒等待。beets CLI 文档也显示更新扫描依赖文件 mtime 来跳过未变化文件。

这说明音频 metadata 领域常把外部 metadata/provider 查询视为 I/O-bound 并发任务，但并发边界放在“独立文件/插件任务”层，而不是破坏库数据库的一致性边界。

### Jellyfin

Jellyfin 音乐解析器在部分目录判断中使用 `Parallel.ForEach` 遍历子目录，例如 artist/album resolver 并行检查子目录是否含音乐。Provider manager 则维护 local/remote metadata provider 的顺序配置。Jellyfin 也存在 TODO 提示某些 media segment extraction 仍未并行化。

这说明媒体服务器会局部使用目录级并行，但通常只在无强顺序副作用的解析阶段使用；metadata provider 顺序和任务调度仍需要集中控制。

## 四种策略对比

### 1. 串行目录 walk + 并行 metadata/file 处理（skeleton-first）

模式：协调线程先枚举目录结构和候选音频路径，生成稳定 skeleton；随后把音频文件任务放入有界队列，worker 并行执行 hash、cache 判断、TagReader，最后协调线程按路径或发现顺序合并结果。

优点：实现复杂度中等，最符合 Seriona 当前 `reconcileRoot()`/`reconcileAudio()` 分层；目录 hash、missing file reconciliation、SQLite root transaction 和事件发布顺序容易保持；可用现有 hash/tagreader 统计评估效果；worker 数可保守限制，保护消费级 SSD 和未知 TagReader。

缺点：目录枚举本身仍串行；如果库是数十万小目录、每目录延迟很高，遍历阶段不能充分并行。但 Seriona 目标是 1000-10000 个音频文件，典型音乐库目录数量通常远低于文件数，SSD 上约 1ms/目录的枚举成本通常不是主瓶颈。

适配 Seriona：强烈推荐。

### 2. 完全并行目录 traversal + work stealing

模式：目录和文件都是 work item，worker 发现子目录后继续把工作推回 work-stealing 队列。ripgrep/ignore 和 fd 证明该模型适合搜索工具。

优点：目录树很宽或过滤规则昂贵时能充分利用多核；动态负载均衡好；对只读搜索、无强提交顺序的工具性能很好。

缺点：实现复杂，终止条件、队列内存、取消和错误传播都更难；ripgrep 相关 issue 也提到约束目录队列大小容易死锁，因为 producer 和 consumer 是同一批 worker。对 Seriona 来说，还会增加目录 hash 顺序、root cache reconcile、event ordering、playlist tree deterministic ordering 的复杂度。

适配 Seriona：不建议作为第一阶段。只有当性能统计证明目录枚举占总扫描时间超过 30%-40%，或目标扩展到超大目录树/网络盘时，再考虑引入成熟 walker 或内部 work-stealing traversal。

### 3. Producer-consumer + 有界队列

模式：单 producer 或少量 producer 枚举路径，固定 worker 从 bounded queue 取文件处理任务；队列满时 producer 阻塞形成 backpressure。restic 的 archiver/file saver/tree saver 体现了这种 bounded-worker 思路。

优点：背压明确、内存受控；能把 I/O-heavy hash 与 CPU/I/O mixed TagReader 重叠；比 fully parallel traversal 更容易保持 Seriona 的根级 cache 和事件契约；C++23 标准库用 `std::mutex`、`std::condition_variable`、`std::jthread` 可直接实现。

缺点：如果 producer 和 consumer 分工太粗，队列满时目录枚举会暂停；需要谨慎处理取消、异常收集、结果排序和 worker 生命周期。

适配 Seriona：推荐的具体实现形式。可先在 `reconcileRoot()` 内局部使用，不改变全局 `scanWorkerLoop()` 单扫描串行语义。

### 4. Async I/O + coroutines/futures

模式：用 async runtime 或 futures 把目录读、文件 hash、metadata extraction 表达为异步任务。

优点：理论上可减少线程阻塞，适合高延迟网络 I/O 或已有 async runtime 的应用。

缺点：C++23 标准库没有跨平台文件 async I/O runtime；Linux io_uring、Windows Overlapped I/O、第三方 runtime 都会引入平台分支和依赖。TagReader 的 `metadataReader_->read()` 是外部同步调用，SQLite cache 当前也是同步接口；把同步 I/O 包成 future 最终仍需要线程池，反而增加复杂度。

适配 Seriona：不推荐。除非未来 Seriona 引入跨平台 async runtime 且 TagReader/SQLite 都提供 async API，否则不要用 coroutine 改造 scanner。

## 音频库扫描的 I/O 特性

目录枚举在消费级 SSD 上通常是低毫秒级；即使按 1ms/目录估算，1000 个 album 目录约 1 秒，通常低于对 1000-10000 个音频文件计算内容 hash 与读取 tags 的总耗时。并行目录遍历带来的收益存在，但不是 Seriona 的第一瓶颈。

文件 hash 是顺序读，典型吞吐可能在 100-500 MB/s，受 SSD、文件大小、缓存热度、系统页缓存影响。对音乐库，单文件几十 MB，盲目开 8-16 个 hash worker 可能让磁盘随机化、页缓存抖动并拖慢 TagReader。并发读默认应低，建议从 2-4 worker 开始，并用新增 hash time 统计验证。

TagReader metadata extraction 是 CPU+I/O 混合任务，还可能读取封面、lyrics 或容器索引。由于内部线程安全未知，Seriona 应把 TagReader 并发度作为独立上限：即使文件 worker 有 4 个，也可以用 semaphore 限制同时 `metadataReader_->read()` 为 1-2 个。上线前应做 ThreadSanitizer 或压力测试；如果发现 TagReader 非线程安全，仍可并行 hash/cache，但把 TagReader 调用串行化。

SQLite cache 已经按 root 加载 `CachedRoot`，并以 content hash 和 path/mtime 等信息判断是否需要重读 metadata。热扫描时，如果 cache 命中率高，瓶颈会转移到目录枚举、mtime/hash 判断和 cache 查找。此时并行 `reconcileAudio()` 的收益下降，甚至可能因为 worker 调度变慢。因此并发策略应保留快速路径：缓存命中文件只做轻量处理，未命中/变更文件才进入重 TagReader 路径。

## Seriona 具体建议

### 线程模型

1. 保留全局单 `scanWorkerLoop()`，避免多个 scan request 同时修改 snapshot、cache 和 watcher 状态。
2. 在每个 `reconcileRoot()` 内创建 root-local worker pool，默认 worker 数 `min(4, max(2, hardware_concurrency / 2))`；如果 `hardware_concurrency` 不可用，默认 2。
3. 新增 TagReader semaphore，默认 `min(2, worker_count)`；若压力测试不安全或外部库文档不保证线程安全，则默认 1。
4. 使用 bounded MPMC 或单 producer/multi consumer 队列，容量 `worker_count * 4` 或 `worker_count * 8`。队列只放音频文件任务，不放目录任务，避免 work-stealing traversal 的终止/背压复杂度。
5. 每个 worker 只返回纯数据结果：`PublishedSong`、`CachedSong`、错误、hash/tagreader耗时、skipped/scanned 标记。worker 不调用 `publishEvent()`，不直接更新 `snapshot_`，不直接保存 root。

### 集成点

1. `runScan()`：保持 roots 串行。它继续累计 `discovered`、`skipped`、`scanned`、`totalHashTimeMs`、`totalTagReaderTimeMs`，继续集中发布 `ScanStarted`、`ScanError`、`FileScanned`、`ProgressUpdated`、`PlaylistSnapshotUpdated`、`ScanCompleted`。
2. `reconcileRoot()`：保留 `discoverScannerPaths()` 的串行 skeleton 枚举，先得到目录集合、音频候选和旧 `CachedRoot`。随后调用新的 root-local `processAudioFilesConcurrently()`。
3. `reconcileAudio()`：改造成可被 worker 调用的纯函数式核心，输入包含 root path、audio path、cached root view、config、metadata adapter、取消 token；输出结构包含 song/error/stats。不要在这里发布事件或写 SQLite。
4. `cachedSongByPath()`：当前对 `root->songs` 线性查找。并发前应在协调线程构建 `unordered_map<string, CachedSong const*>` 或复制轻量 view，避免每个 worker 对 10000 首歌做 O(n) 查找。这个优化对热扫描可能比并发更重要。
5. SQLite：worker 不共享 `SQLiteScannerCache` 写事务。`reconcileRoot()` 收齐结果后构建新的 `CachedRoot`，再由协调线程调用 `cache.saveRoot()`，保持现有 cache contract。
6. 事件：为了稳定 UI 和测试，按 `discoverScannerPaths()` 的发现顺序或规范化路径排序发布 `FileScanned`。worker 完成顺序只影响内部吞吐，不影响外部事件顺序。

### 性能预估

假设 5000 首歌、平均 8MB、总 40GB：冷扫描如果 hash 吞吐单线程 250MB/s，单纯 hash 约 160 秒；2-4 个 worker 在 SSD/page cache 允许时可能把 hash wall time 降到 60-100 秒，但不会线性，因为存储带宽共享。TagReader 若平均 10-30ms/首，串行为 50-150 秒；并发 2 个可降到 30-90 秒。两者重叠后，总体冷扫描预计 1.5x-3x。

热扫描下，若 80%-95% 文件 cache 命中并跳过 TagReader，剩余成本主要是目录枚举、mtime/hash 判断、cache lookup 和结果合并。此时如果仍必须读取完整文件 hash，收益接近 hash 并发上限；如果 Seriona 后续能用 mtime/size 快速判定不变文件并跳过 hash，则热扫描收益主要来自优化 `cachedSongByPath()` map 和目录枚举，预计 1.2x-2x。

对 1000 首以内的小库，线程创建和队列调度可能抵消收益。建议阈值：候选音频数少于 `worker_count * 8` 时走串行路径，或复用长期 worker pool 避免每个 root 创建线程。

### 实现复杂度

低复杂度阶段：先把 `cachedSongByPath()` 的线性查找改为 root-local map，并继续串行扫描。风险低，热扫描收益稳定。

中复杂度阶段：在 `reconcileRoot()` 增加 bounded queue + worker pool，只并发 `reconcileAudio()` 的纯处理部分；协调线程合并结果、保存 SQLite、发布事件。需要新增取消传播、异常收集、结果排序和 TagReader semaphore，但不改变公共 API。

高复杂度阶段：完全并行目录遍历或跨 root 并行。需要重做目录 hash、missing file reconciliation、event ordering、cache writer 和 watcher interaction，不建议现在做。

## 建议落地顺序

1. 先用现有性能日志在三种库上建立基线：1000 首、5000 首、10000 首；分别记录 cold scan、warm scan、少量文件变更 scan。
2. 实现 root-local cached song map，把 cache hit 查找从 O(n) 降到 O(1)，确保不改变事件和 cache 输出。
3. 抽出 `reconcileAudio()` 的无副作用 worker core，添加串行路径测试，确认错误和歌词选择逻辑一致。
4. 增加 bounded file worker pool，默认 worker=2，TagReader 并发=1；验证通过后把默认提升为 worker=2-4、TagReader=1-2。
5. 用性能统计比较 hash wall time、TagReader wall time、total scan time，并保留配置开关使用户可降级到串行。

## 不建议事项

1. 不要引入 C++23 coroutine async scanner。当前依赖全是同步 API，最终仍会落到线程池。
2. 不要让 worker 直接调用 `publishEvent()` 或直接写 SQLite root，避免事件乱序和 cache contract 变化。
3. 不要默认用 `hardware_concurrency()` 全量线程数扫描音频文件；音频 hash 是存储带宽瓶颈，过高并发会拖慢消费级 SSD 和 TagReader。
4. 不要在未确认 TagReader 线程安全前放开 `metadataReader_->read()` 的并发。
5. 不要把 ripgrep/fd 的 parallel walker 原样搬进 Seriona。它们的目标是搜索吞吐，Seriona 的关键约束是音频 metadata、SQLite root cache、事件顺序和稳定播放列表快照。
