# Scanner 完整重构设计：预分配节点 + CUE 支持

## 📋 概述

本文档整合了两个核心优化方案：
1. **预分配节点架构**：解决 O(n² log n) 排序瓶颈（136x 加速）
2. **CUE Sheet 虚拟文件夹**：支持 CUE 格式，完整用户体验

**目标**：提供一份完整的、统一的扫描流程设计，作为后续编写详细任务书的基础。

---

## 🎯 核心设计原则

### 原则 1：先建树，再填充

**传统做法**（有问题）：
```
扫描 → Worker 返回结果 → 排序 → 构建树
```

**新设计**（最优）：
```
扫描 → 预分配树节点 → Worker 填充节点 → 树已完成
```

**关键**：节点按扫描顺序创建，Worker 只负责填充内容，**无需排序**。

---

### 原则 2：CUE 作为虚拟容器

**设计**：CUE 文件显示为虚拟文件夹，隐藏底层音频文件

**示例**：
```
文件系统：                     播放器树：
  album.cue          →          album.cue/
  album.flac                      ├── 01 - Track 1
  cover.jpg                       ├── 02 - Track 2
                                  └── 03 - Track 3
```

---

## 🏗️ 统一数据模型

### 节点类型体系

```cpp
enum class NodeType {
    Directory,      // 真实目录
    Song,          // 普通歌曲文件
    CueContainer,  // CUE 虚拟文件夹
    CueTrack       // CUE 轨道
};

enum class PathEntryKind {
    AudioCandidate,    // 普通音频文件
    CueSheet,          // CUE 文件
    Directory,         // 目录
    SingleFileRoot     // 单文件根
};
```

---

### 统一节点结构

```cpp
struct IndexedPublishedSong {
    // ===== 基础字段 =====
    std::size_t discoveryIndex{0};          // 全局发现序号
    NodeType nodeType{NodeType::Song};       // 节点类型
    std::string displayName;                 // 显示名称
    std::filesystem::path treeRelativePath;  // 树中路径
    
    // ===== 元数据 =====
    cache::CachedSong song;                  // 歌曲元数据
    
    // ===== CUE 特有字段 =====
    std::optional<CueInfo> cueInfo;          // CUE 信息（如果是 CUE 节点）
    
    // ===== 状态标志 =====
    std::atomic<bool> filled{false};         // 是否已填充
    bool needsScan{false};                   // 是否需要扫描
    bool isVirtualFolder{false};             // 是否为虚拟文件夹
};

struct CueInfo {
    std::filesystem::path cueFilePath;       // CUE 文件路径
    std::filesystem::path audioFilePath;     // 实际音频文件路径
    std::chrono::milliseconds offset{0};     // 起始偏移（仅 CueTrack）
    std::chrono::milliseconds duration{0};   // 时长（仅 CueTrack）
    size_t trackIndex{0};                    // Track 索引（仅 CueTrack）
};
```

---

## 🔄 完整扫描流程

### Phase 1：目录遍历和分类

#### 📋 实现目标
- 先识别 CUE 文件，再识别普通音频文件，建立稳定的扫描入口顺序。
- 跳过被 CUE 引用的底层音频文件，避免重复入树。
- 将解析错误与正常文件隔离，保证单个异常不影响整轮扫描。

#### 🔄 输入输出
**输入**：
- 根目录路径 `rootPath`。
- 目录下的 `.cue` 文件和普通音频文件。
- CUE 解析器输出的轨道引用信息。

**输出**：
- `ClassifiedPath` 列表，按文件类型分类。
- 被 CUE 引用的文件集合 `cueHandledFiles`。
- 每个分类项对应的错误信息，若有。

#### 🔑 关键逻辑
1. 第一遍遍历目录，只处理 `.cue` 文件并解析轨道引用。
2. 收集所有被 CUE 引用的音频文件路径，形成隐藏集合。
3. 第二遍遍历目录，补充普通音频文件，但跳过隐藏集合中的条目。
4. 对解析失败的 CUE 记录错误，不中断其他条目处理。

#### ✅ 验收标准
- [ ] 所有 `.cue` 文件都能被优先识别和分类。
- [ ] 被 CUE 引用的音频文件不会再次进入普通文件列表。
- [ ] 单个 CUE 解析失败不会影响同目录下其他文件的分类结果。
- [ ] 分类输出稳定且可复现。

```cpp
std::vector<ClassifiedPath> discoverScannerPaths(const fs::path& rootPath, ...) {
    std::vector<ClassifiedPath> entries;
    std::unordered_set<std::string> cueHandledFiles;  // 被 CUE 引用的文件
    
    // ===== 第 1 遍：检测 CUE 文件 =====
    for (const auto& entry : fs::directory_iterator(rootPath)) {
        if (entry.path().extension() == ".cue") {
            try {
                // 解析 CUE 文件
                auto cueTracks = TagReader::ReadCueSheet(entry.path());
                
                // 记录所有被引用的音频文件
                for (const auto& track : cueTracks) {
                    cueHandledFiles.insert(pathKey(track.filePath()));
                }
                
                // 添加 CUE 文件
                entries.push_back({
                    .path = entry.path(),
                    .kind = PathEntryKind::CueSheet,
                    .errors = 
                });
                
            } catch (const std::exception& e) {
                // 记录 CUE 解析错误
                entries.push_back({
                    .path = entry.path(),
                    .kind = PathEntryKind::CueSheet,
                    .errors = {ScannerError{
                        .code = ScannerErrorCode::CueParseError,
                        .message = e.what()
                    }}
                });
            }
        }
    }
    
    // ===== 第 2 遍：添加普通音频文件（跳过被 CUE 处理的）=====
    for (const auto& entry : fs::directory_iterator(rootPath)) {
        if (isAudioFile(entry.path())) {
            // 跳过已被 CUE 引用的文件
            if (cueHandledFiles.contains(pathKey(entry.path()))) {
                continue;
            }
            
            entries.push_back({
                .path = entry.path(),
                .kind = PathEntryKind::AudioCandidate,
                .errors = {}
            });
        }
    }
    
    return entries;
}
```

**关键点**：
- ✅ 两遍遍历：先 CUE，后音频
- ✅ 自动跳过被 CUE 引用的文件
- ✅ CUE 解析错误不影响其他文件

---

### Phase 2：预分配节点树（核心创新）

#### 📋 实现目标
- 在 Worker 扫描前一次性预分配完整节点数组。
- 让节点顺序与发现顺序一致，彻底取消后续排序阶段。
- 为 CUE 容器、CUE 轨道和普通歌曲统一建立填充入口。

#### 🔄 输入输出
**输入**：
- `discoverScannerPaths()` 的分类结果。
- 现有缓存索引 `cachedSongsByPath`。
- 增量扫描计划 `incrementalPlan`。
- Scanner 配置与根路径信息。

**输出**：
- 预分配好的 `indexedSongs` 节点数组。
- 对应的 `workerTasks` 任务列表。
- 已完成填充的 `result.songs`。

#### 🔑 关键逻辑
1. 先估算节点数并预留容量，避免反复扩容。
2. 按分类结果逐项展开，CUE 文件生成容器和轨道节点，普通文件生成单歌节点。
3. 对可复用缓存命中的节点直接标记为已填充，减少 Worker 负载。
4. 仅为需要扫描的节点构建 WorkerTask，并在扫描后直接汇总结果。

#### ✅ 验收标准
- [ ] 节点数组能按发现顺序完整构建。
- [ ] 不再依赖结果排序来恢复树顺序。
- [ ] 缓存命中节点不会进入 Worker 扫描队列。
- [ ] 最终结果中的节点顺序与发现顺序一致。

```cpp
RootResult reconcileRoot(const ScannerRoot& root, ...) {
    const auto rootPath = rootPathFor(root);
    
    // 发现所有文件
    const auto entries = discoverScannerPaths(rootPath, ...);
    
    // ===== 预分配节点数组 =====
    std::vector<IndexedPublishedSong> indexedSongs;
    indexedSongs.reserve(estimateNodeCount(entries));  // 预估容量
    
    auto discoveryIndex = std::size_t{0};
    
    for (const auto& entry : entries) {
        // 处理错误
        for (const auto& error : entry.errors) {
            result.errors.push_back(error);
        }
        
        if (entry.kind == PathEntryKind::CueSheet) {
            // ===== CUE 文件：展开为虚拟文件夹 + tracks =====
            processCueSheet(entry, indexedSongs, discoveryIndex, 
                           cachedSongsByPath, incrementalPlan);
            
        } else if (entry.kind == PathEntryKind::AudioCandidate) {
            // ===== 普通音频文件 =====
            processAudioFile(entry, indexedSongs, discoveryIndex,
                           cachedSongsByPath, incrementalPlan);
        }
    }
    
    // ===== 准备 Worker 任务 =====
    std::vector<WorkerTask> workerTasks;
    for (size_t i = 0; i < indexedSongs.size(); ++i) {
        if (indexedSongs[i].needsScan) {
            workerTasks.push_back({
                .rootPath = rootPath,
                .filePath = resolveFilePath(indexedSongs[i]),
                .nodeIndex = i,
                .cueInfo = indexedSongs[i].cueInfo
            });
        }
    }
    
    // ===== Worker 扫描（直接填充节点）=====
    executeWorkerScan(indexedSongs, workerTasks, config);
    
    // ===== 构建结果（已有序，无需排序）=====
    for (auto& node : indexedSongs) {
        result.songs.push_back({
            .song = std::move(node.song),
            .treeRelativePath = std::move(node.treeRelativePath),
            .nodeType = node.nodeType
        });
    }
    
    return result;
}
```

---

### Phase 2.1：处理 CUE 文件

#### 📋 实现目标
- 将一个 CUE 文件展开为一个虚拟容器节点和多个轨道节点。
- 让轨道节点保留真实音频来源信息，同时对用户展示为 CUE 视图。
- 为 CUE 轨道建立独立缓存判断，减少重复解析。

#### 🔄 输入输出
**输入**：
- 单个 `ClassifiedPath`，类型为 `CueSheet`。
- CUE 解析结果 `cueTracks`。
- 轨道缓存索引 `cache`。
- `discoveryIndex` 与可选增量计划 `plan`。

**输出**：
- 一个 `CueContainer` 节点。
- 多个 `CueTrack` 子节点。
- 每个轨道对应的 `CueInfo`、显示名称和扫描状态。

#### 🔑 关键逻辑
1. 先解析 CUE，解析失败则直接返回。
2. 先创建容器节点，作为 CUE 的虚拟文件夹入口。
3. 遍历每个轨道，生成轨道节点并写入轨道偏移、时长和来源文件路径。
4. 按缓存校验轨道是否已命中，决定是否需要后续 Worker 扫描。

#### ✅ 验收标准
- [ ] 每个 CUE 文件都能展开为一个容器节点。
- [ ] 每个轨道都能正确映射到展示路径和真实音频路径。
- [ ] 缓存命中的轨道会被标记为已填充。
- [ ] 解析失败时仅跳过当前 CUE，不影响其他文件。

```cpp
void processCueSheet(const ClassifiedPath& entry,
                     std::vector<IndexedPublishedSong>& nodes,
                     std::size_t& discoveryIndex,
                     const CachedSongPathIndex& cache,
                     const std::optional<IncrementalPlan>& plan) {
    
    // 解析 CUE 文件
    auto cueTracks = TagReader::ReadCueSheet(entry.path);
    if (cueTracks.empty()) {
        return;  // 解析失败，已记录错误
    }
    
    // ===== 1. 创建 CUE 容器节点 =====
    const auto containerIndex = nodes.size();
    nodes.push_back({
        .discoveryIndex = discoveryIndex++,
        .nodeType = NodeType::CueContainer,
        .displayName = entry.path.filename().string(),  // "album.cue"
        .treeRelativePath = entry.path,
        .cueInfo = CueInfo{
            .cueFilePath = entry.path,
            .audioFilePath = cueTracks[0].filePath()
        },
        .filled = true,  // 容器节点不需要扫描
        .needsScan = false,
        .isVirtualFolder = true
    });
    
    // ===== 2. 为每个 track 创建子节点 =====
    for (size_t trackIdx = 0; trackIdx < cueTracks.size(); ++trackIdx) {
        const auto& track = cueTracks[trackIdx];
        
        // 检查缓存
        const auto cacheKey = computeCueTrackCacheKey(entry.path, track.offset());
        auto cachedSong = cache.get(cacheKey);
        bool isCacheHit = cachedSong.has_value() && 
                         isCueTrackCacheValid(*cachedSong, track);
        
        nodes.push_back({
            .discoveryIndex = discoveryIndex++,
            .nodeType = NodeType::CueTrack,
            .displayName = formatTrackName(track),  // "01 - Track Title"
            .treeRelativePath = entry.path / formatTrackName(track),
            .song = isCacheHit ? std::move(*cachedSong) : cache::CachedSong{},
            .cueInfo = CueInfo{
                .cueFilePath = entry.path,
                .audioFilePath = track.filePath(),
                .offset = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::microseconds(track.offset())),
                .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::microseconds(track.duration())),
                .trackIndex = trackIdx
            },
            .filled = isCacheHit,
            .needsScan = !isCacheHit
        });
    }
}
```

---

### Phase 2.2：处理普通音频文件

#### 📋 实现目标
- 对非 CUE 音频文件建立单独节点。
- 支持增量扫描场景下的删除、保留和缓存复用判断。
- 让普通文件和 CUE 轨道在同一节点体系内共存。

#### 🔄 输入输出
**输入**：
- 单个 `ClassifiedPath`，类型为 `AudioCandidate`。
- `CachedSongPathIndex` 缓存索引 `cache`。
- 可选增量计划 `plan`。
- 当前 `discoveryIndex`。

**输出**：
- 一个普通 `Song` 节点，或在增量删除场景下不产生节点。
- 节点的显示名、树路径、缓存命中状态和扫描标记。

#### 🔑 关键逻辑
1. 先用增量计划判断该文件是否仍然存在于 Worker 侧。
2. 再根据路径和缓存内容判断是否能直接复用旧元数据。
3. 创建普通歌曲节点，写入发现顺序和树中路径。
4. 仅对未命中缓存的节点保留扫描任务。

#### ✅ 验收标准
- [ ] 普通音频文件都能被正确建节点。
- [ ] 增量删除的文件不会继续保留在结果中。
- [ ] 缓存命中的文件可以直接跳过 Worker 扫描。
- [ ] 节点类型、路径和显示名都符合树模型要求。

```cpp
void processAudioFile(const ClassifiedPath& entry,
                      std::vector<IndexedPublishedSong>& nodes,
                      std::size_t& discoveryIndex,
                      const CachedSongPathIndex& cache,
                      const std::optional<IncrementalPlan>& plan) {
    
    // 检查增量扫描
    const auto entryKey = pathKey(entry.path);
    if (plan && !plan->workerPaths.contains(entryKey)) {
        return;  // 增量扫描：已删除
    }
    
    // 检查缓存
    bool isUnchanged = plan && plan->unchangedPaths.contains(entryKey);
    auto cachedSong = cache.get(entry.path);
    bool isCacheHit = cachedSong.has_value() && 
                     (isUnchanged || isAudioCacheHit(*cachedSong, entry.path));
    
    nodes.push_back({
        .discoveryIndex = discoveryIndex++,
        .nodeType = NodeType::Song,
        .displayName = entry.path.filename().string(),
        .treeRelativePath = entry.path,
        .song = isCacheHit ? std::move(*cachedSong) : cache::CachedSong{},
        .filled = isCacheHit,
        .needsScan = !isCacheHit
    });
}
```

---

### Phase 3：Worker 扫描（直接填充）

#### 📋 实现目标
- 将所有待扫描任务交给 Worker Pool 并发处理。
- 让 Worker 直接回填预分配节点，避免中间结果搬运。
- 扫描结束后统一检查节点填充完整性。

#### 🔄 输入输出
**输入**：
- 预分配节点数组 `nodes`。
- 待扫描任务列表 `tasks`。
- Worker 配置 `config`。

**输出**：
- 已被 Worker 填充的节点数组。
- 可选的错误日志和未填充节点告警。

#### 🔑 关键逻辑
1. 如果没有任务，直接返回，表示全部命中缓存。
2. 构造 Worker Pool，并注入能回填节点的扫描回调。
3. 批量提交任务并等待所有任务完成。
4. 统一遍历节点，检查是否存在未填充但标记需要扫描的条目。

#### ✅ 验收标准
- [ ] 只有需要扫描的节点才会进入 Worker。
- [ ] 所有提交任务都能被正确等待完成。
- [ ] Worker 返回后，节点的 filled 状态与实际扫描结果一致。
- [ ] 未填充节点会被明确记录，便于排障。

```cpp
void executeWorkerScan(std::vector<IndexedPublishedSong>& nodes,
                      const std::vector<WorkerTask>& tasks,
                      const ScannerConfig& config) {
    
    if (tasks.empty()) {
        return;  // 全部缓存命中
    }
    
    // 创建 Worker Pool
    auto nodesPtr = &nodes;
    ScannerWorkerPool workerPool{ScannerWorkerPool::Config{
        .workerCount = config.workerCount,
        .tagReaderSlots = config.tagReaderSlots,
        .tagReader = [nodesPtr](const WorkerTask& task) {
            return scanAndFillNode(*nodesPtr, task);
        }
    }};
    
    // 批量提交任务
    workerPool.submitBatch(std::move(tasks));
    
    // 等待所有任务完成
    workerPool.waitAll();
    
    // 验证所有节点已填充
    for (const auto& node : nodes) {
        if (node.needsScan && !node.filled.load(std::memory_order_acquire)) {
            spdlog::error("Node not filled: {}", node.displayName);
        }
    }
}
```

---

### Phase 3.1：扫描并填充节点

#### 📋 实现目标
- 根据节点类型分别读取普通文件和 CUE 轨道元数据。
- 将扫描结果写回预分配节点，形成最终可发布数据。
- 在异常情况下保持节点未填充，并把错误上抛给 Worker 层处理。

#### 🔄 输入输出
**输入**：
- 预分配节点数组 `nodes`。
- 单个 Worker 任务 `task`。
- 对应节点的类型、路径和 CUE 信息。

**输出**：
- 填充后的 `SongMetadata`。
- 节点内部的 `song.metadata` 和 `filled` 状态更新。
- 失败时抛出的异常。

#### 🔑 关键逻辑
1. 通过 `task.nodeIndex` 定位目标节点。
2. 若是 CUE 轨道，重新读取 CUE 并提取轨道级元数据。
3. 若是普通文件，直接读取标签并映射到统一元数据结构。
4. 将结果写回节点，并用原子状态标记为已填充。

#### ✅ 验收标准
- [ ] CUE 轨道能正确写入 offset、duration 和来源文件信息。
- [ ] 普通文件能正确写入统一元数据。
- [ ] 成功扫描后节点 `filled` 必须为 true。
- [ ] 异常不会伪装成成功填充。

```cpp
SongMetadata scanAndFillNode(std::vector<IndexedPublishedSong>& nodes,
                             const WorkerTask& task) {
    auto& node = nodes[task.nodeIndex];
    
    try {
        SongMetadata metadata;
        
        if (node.nodeType == NodeType::CueTrack) {
            // ===== CUE Track：读取音频 + 应用 CUE 元数据 =====
            auto cueTracks = TagReader::ReadCueSheet(task.cueInfo->cueFilePath);
            const auto& cueTrack = cueTracks[task.cueInfo->trackIndex];
            
            metadata.filePath = task.cueInfo->cueFilePath;  // 用户看到的路径
            metadata.sourceFilePath = task.cueInfo->audioFilePath;  // 实际文件
            metadata.offset = task.cueInfo->offset;
            metadata.duration = task.cueInfo->duration;
            metadata.title = cueTrack.title();
            metadata.artist = cueTrack.artist();
            metadata.album = cueTrack.album();
            metadata.trackNumber = cueTrack.trackNumber();
            // ... 其他元数据
            
        } else {
            // ===== 普通文件：直接读取 =====
            auto tag = TagReader::Read(task.filePath);
            metadata = mapFromMusicTag(tag);
        }
        
        // 填充节点
        node.song.metadata = std::move(metadata);
        node.filled.store(true, std::memory_order_release);
        
        return metadata;
        
    } catch (const std::exception& e) {
        // 节点保持 filled=false
        throw;  // Worker pool 会捕获
    }
}
```

---

## 🌳 PlaylistTree 构建

### 树构建逻辑

#### 📋 实现目标
- 把已发布的扫描结果转换成可浏览的播放树。
- 让 CUE 容器显示为虚拟文件夹，轨道挂在容器下面。
- 普通歌曲仍按常规文件节点加入目录树。

#### 🔄 输入输出
**输入**：
- 扫描完成后的 `PublishedSong`。
- 当前树构建状态 `currentDirectory`。
- CUE 容器映射 `cueContainers_`。

**输出**：
- 更新后的 `PlaylistNode` 树。
- CUE 容器索引与子节点挂载关系。

#### 🔑 关键逻辑
1. 先识别节点类型，区分 CUE 容器、CUE 轨道和普通歌曲。
2. CUE 容器创建为虚拟文件夹，并登记到容器映射。
3. CUE 轨道通过其来源路径找到容器，再挂载到容器下。
4. 普通歌曲直接挂到当前目录。

#### ✅ 验收标准
- [ ] CUE 容器在树中表现为虚拟文件夹。
- [ ] CUE 轨道一定挂在对应容器下。
- [ ] 普通歌曲不会误挂到 CUE 容器中。
- [ ] 找不到容器时会记录明确错误。

```cpp
class PlaylistTreeBuilder {
public:
    void addSong(const PublishedSong& song) {
        if (song.nodeType == NodeType::CueContainer) {
            // ===== 创建虚拟文件夹 =====
            auto cueFolder = std::make_shared<PlaylistNode>();
            cueFolder->type = PlaylistNodeType::CueContainer;
            cueFolder->name = song.displayName;  // "album.cue"
            cueFolder->path = song.treeRelativePath;
            cueFolder->isVirtualFolder = true;
            
            currentDirectory->addChild(cueFolder);
            cueContainers_[song.treeRelativePath] = cueFolder;
            
        } else if (song.nodeType == NodeType::CueTrack) {
            // ===== 添加到 CUE 容器下 =====
            auto* container = findCueContainer(song.song.metadata.filePath);
            if (!container) {
                spdlog::error("CUE container not found for track: {}", 
                             song.displayName);
                return;
            }
            
            auto trackNode = std::make_shared<PlaylistNode>();
            trackNode->type = PlaylistNodeType::CueTrack;
            trackNode->name = song.displayName;
            trackNode->metadata = song.song.metadata;
            
            container->addChild(trackNode);
            
        } else {
            // ===== 普通歌曲 =====
            auto songNode = std::make_shared<PlaylistNode>();
            songNode->type = PlaylistNodeType::Song;
            songNode->name = song.displayName;
            songNode->metadata = song.song.metadata;
            
            currentDirectory->addChild(songNode);
        }
    }

private:
    std::unordered_map<fs::path, std::shared_ptr<PlaylistNode>> cueContainers_;
};
```

---

## 💾 缓存系统集成

### 缓存键计算

#### 📋 实现目标
- 让缓存键同时适配普通文件和 CUE 轨道。
- 通过路径或路径加偏移生成稳定键值，支持快速命中。
- 为后续内容复用和增量扫描提供统一入口。

#### 🔄 输入输出
**输入**：
- `SongMetadata`。
- 可能存在的 `offset` 字段。

**输出**：
- 字符串形式的缓存键。

#### 🔑 关键逻辑
1. 如果存在 offset，说明是 CUE 轨道，使用 `filePath + offset`。
2. 如果不存在 offset，说明是普通文件，直接使用文件路径。
3. 缓存键必须稳定且可重建，避免不同节点互相覆盖。

#### ✅ 验收标准
- [ ] 普通文件生成的缓存键与文件路径一致。
- [ ] CUE 轨道生成的缓存键能区分同一音频文件的不同轨道。
- [ ] 相同输入多次计算结果一致。

```cpp
std::string computeCacheKey(const SongMetadata& metadata) {
    if (metadata.offset.has_value()) {
        // CUE track: cueFilePath + offset
        return fmt::format("{}#offset={}", 
                          metadata.filePath.string(),
                          metadata.offset->count());
    } else {
        // 普通文件: filePath
        return metadata.filePath.string();
    }
}
```

### 缓存命中检测

#### 📋 实现目标
- 判断缓存里的 CUE 轨道是否仍然有效。
- 通过音频文件内容、offset 和 duration 三个维度确认一致性。
- 避免陈旧缓存错误覆盖新扫描结果。

#### 🔄 输入输出
**输入**：
- 已缓存的 `CachedSong`。
- 当前 CUE 轨道 `MusicTag`。

**输出**：
- 布尔值，表示缓存是否有效。

#### 🔑 关键逻辑
1. 计算当前音频文件的内容哈希。
2. 比较缓存中的内容哈希、轨道 offset 和 duration。
3. 三者都一致时才认为命中有效缓存。

#### ✅ 验收标准
- [ ] 文件内容变化后缓存必须失效。
- [ ] offset 或 duration 变化后缓存必须失效。
- [ ] 完全一致时缓存命中为 true。

```cpp
bool isCueTrackCacheValid(const cache::CachedSong& cached,
                         const MusicTag& track) {
    // 检查音频文件是否变化
    const auto audioFileHash = hashFileContent(track.filePath());
    return cached.metadata.contentHash == audioFileHash.hash &&
           cached.metadata.offset == track.offset() &&
           cached.metadata.duration == track.duration();
}
```

---

## 📊 性能分析

#### 📋 实现目标
- 用统一的时间分解说明各阶段在整体扫描中的成本。
- 验证预分配、并发扫描和缓存复用对总耗时的影响。
- 为后续优化或任务书拆分提供量化依据。

#### 🔄 输入输出
**输入**：
- 5000 首歌和 10 个 CUE sheets 的典型扫描场景。
- 各阶段的估算耗时。

**输出**：
- 每阶段耗时分解。
- 总耗时和加速比结论。

#### 🔑 关键逻辑
1. 分别统计目录遍历、节点预分配、Worker 扫描和结果整理的耗时。
2. 汇总成总耗时，和旧实现进行对比。
3. 用加速比证明设计方案对瓶颈的改善幅度。

#### ✅ 验收标准
- [ ] 各阶段耗时估算完整且可读。
- [ ] 总耗时和加速比计算明确。
- [ ] 文档能支撑后续任务书拆分与性能评估。

### 扫描时间分解（5000 首歌 + 10 个 CUE sheets）

```
Phase 1: 目录遍历
  - 第 1 遍（CUE 检测）: 10 ms
  - 第 2 遍（音频文件）: 200 ms
  - 小计：210 ms

Phase 2: 节点预分配
  - CUE 展开（10 个 × 10 tracks）: 50 ms
  - 普通文件（5000 个）: 100 ms
  - 缓存检查：50 ms
  - 小计：200 ms

Phase 3: Worker 扫描
  - 音频文件（假设 1000 个需要扫描）: 450 ms
  - CUE 引用文件（10 个）: 45 ms
  - 小计：495 ms

Phase 4: 结果整理
  - 验证填充：5 ms
  - 复制到结果：5 ms
  - 小计：10 ms

总计：915 ms
```

**对比当前实现**：
- 当前：68,000 ms
- 优化后：915 ms
- **加速比：74x**

---

## 🔄 增量扫描支持

### CUE 变更检测

```cpp
bool detectCueChange(const fs::path& cuePath,
                     const cache::CachedRoot& cached) {
    // 检查 CUE 文件自身
    const auto cueMtime = fs::last_write_time(cuePath);
    if (cueMtime != cached.cueMtime) {
        return true;  // CUE 文件已修改
    }
    
    // 检查引用的音频文件
    auto cueTracks = TagReader::ReadCueSheet(cuePath);
    for (const auto& track : cueTracks) {
        const auto audioHash = hashFileContent(track.filePath());
        if (audioHash.hash != cached.audioHash) {
            return true;  // 音频文件已修改
        }
    }
    
    return false;  // 无变化
}
```

---

## ⚠️ 边界情况处理

### 情况 1：CUE 引用的文件不存在

```cpp
try {
    auto cueTracks = TagReader::ReadCueSheet(cuePath);
} catch (const std::exception& e) {
    // 记录错误，不创建容器节点
    errors.push_back({
        .code = ScannerErrorCode::CueAudioFileNotFound,
        .filePath = cuePath,
        .message = e.what()
    });
    return;  // 跳过此 CUE
}
```

### 情况 2：CUE 引用多个音频文件

```
album.cue
├── FILE "disc1.flac"  ← 引用文件 1
│   ├── TRACK 01
│   └── TRACK 02
└── FILE "disc2.flac"  ← 引用文件 2
    ├── TRACK 03
    └── TRACK 04
```

**处理**：
- ✅ 所有 tracks 显示在 `album.cue/` 下
- ✅ 隐藏 `disc1.flac` 和 `disc2.flac`
- ✅ 每个 track 记录正确的 `audioFilePath`

### 情况 3：同一音频文件被多个 CUE 引用

```
disc1.cue → album.flac
disc2.cue → album.flac
```

**处理**：
- ✅ 两个独立的 CUE 容器
- ✅ `album.flac` 只被隐藏一次
- ✅ Worker 可能多次读取同一文件（可优化）

---

## 🛠️ 实施计划

### 总体时间表：7 天

#### Day 1：数据结构和基础框架
- [ ] 扩展 `NodeType` 和 `PathEntryKind` 枚举
- [ ] 定义 `CueInfo` 结构
- [ ] 扩展 `IndexedPublishedSong` 结构
- [ ] 扩展 `WorkerTask` 添加 `cueInfo`

#### Day 2：目录遍历重构
- [ ] 实现两遍遍历逻辑（CUE 优先）
- [ ] 实现 `processCueSheet()`
- [ ] 实现 `processAudioFile()`
- [ ] 实现 CUE 文件隐藏逻辑

#### Day 3：节点预分配和 Worker 填充
- [ ] 重构 `reconcileRoot()` 为预分配模式
- [ ] 实现 `scanAndFillNode()` 支持 CUE tracks
- [ ] 删除旧的排序代码
- [ ] 实现缓存键计算（支持 CUE）

#### Day 4：PlaylistTree 集成
- [ ] 扩展 `PlaylistNode` 支持虚拟文件夹
- [ ] 修改 `PlaylistTreeBuilder` 支持 CUE 容器
- [ ] 实现 track 名称格式化

#### Day 5：测试和调试
- [ ] 单元测试（CUE 解析、节点预分配）
- [ ] 集成测试（完整扫描流程）
- [ ] 边界情况测试（文件不存在、多个 CUE）

#### Day 6：增量扫描和缓存
- [ ] 实现 CUE 变更检测
- [ ] 测试增量扫描
- [ ] 测试缓存命中率

#### Day 7：性能优化和验证
- [ ] 性能基准测试
- [ ] 内存使用分析
- [ ] 最终验证和文档更新

---

## ✅ 验收标准

### 功能验收
- [ ] 支持普通音频文件扫描
- [ ] 支持 CUE 文件自动识别
- [ ] CUE 展开为虚拟文件夹 + tracks
- [ ] 底层音频文件正确隐藏
- [ ] 播放 CUE track 正确（offset + duration）
- [ ] 增量扫描正确检测 CUE 变更
- [ ] 缓存系统正常工作

### 性能验收
- [ ] 5000 首歌扫描 < 2 秒
- [ ] 带 CUE 的扫描 < 3 秒
- [ ] 增量扫描（95% 未变）< 5 秒
- [ ] `resultAgg` 阶段 < 100 ms
- [ ] 无内存泄漏
- [ ] 无数据竞争

### 质量验收
- [ ] 所有单元测试通过
- [ ] 所有集成测试通过
- [ ] 边界情况正确处理
- [ ] 错误信息清晰
- [ ] 日志记录完整

---

## 🎯 后续任务书编写指南

基于本设计文档，可以创建以下任务书：

### 任务书 1：预分配节点架构（3 天）
- 数据结构修改
- `reconcileRoot()` 重构
- Worker 填充逻辑
- 测试验证

### 任务书 2：CUE 支持基础（2 天）
- 两遍遍历逻辑
- CUE 展开和容器创建
- 文件隐藏机制

### 任务书 3：PlaylistTree 集成（1 天）
- 虚拟文件夹支持
- Track 显示格式化

### 任务书 4：增量扫描和缓存（1 天）
- CUE 变更检测
- 缓存键计算
- 缓存命中验证

---

**文档版本**：v1.0  
**作者**：Sisyphus AI Agent  
**日期**：2026-06-29  
**状态**：完整设计 - 可用于编写任务书
