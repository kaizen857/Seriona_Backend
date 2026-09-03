# Seriona 后端设计文档

> 本文档完全基于当前源码（根 `CMakeLists.txt`、`app/`、`src/`、`inc/`、`tests/`）重建，只描述源码可证明的现状。
> 事实优先级：CMake 配置与源码 > 测试注册 > 本文件；本文件与源码冲突时以源码为准。

## 1. 项目简介

Seriona 是一个独立的 C++23 音乐库后端：接收一个音乐根目录（或单个文件），扫描目录中的音频文件与 CUE 索引，构建播放列表树，并提供"命令→状态→事件"式的播放控制接口。当前仓库内唯一的前端是终端控制器（`app/`），它以单行状态栏的方式驱动后端；后端本身与 UI 解耦，通过订阅机制对外发布播放状态与曲库快照。

关键选型：

- 构建：CMake 3.20+，C++23，仅 CXX；无 CI、无格式化配置、无 CMakePresets。
- 音频解码：FFmpeg（`libavformat/libavcodec/libavutil/libavfilter/libswresample`）；输出：miniaudio（vendored 单头文件）。
- 标签读取：外部仓库 TagReader（`TagReaderCore`），经适配层接入。
- 元数据缓存：SQLite（固定 v3 schema）；哈希：libxxhash（XXH3/XXH64）。
- 平台媒体集成：Linux 上经 sdbus-c++ 发布 MPRIS 2.x 对象；Windows 仅有占位实现。
- 日志：spdlog（默认 logger `seriona`，滚动文件 5MB×3）；测试：doctest（vendored）。
- 并发：`bshoshany/thread-pool` v4.1.0（FetchContent 固定版本）。

## 2. 整体架构

分层结构（依赖方向自上而下，`inc/seriona/` 是稳定契约边界）：

```
seriona（可执行文件，app/）
  └─ 直接编译 main / terminal_controller / terminal_io / runtime_paths / logging
      └─ seriona_control（编排枢纽：命令、状态归约、事件分发）
          ├─ PRIVATE 链接 seriona_audio   （播放管线 + 波形生成）
          ├─ PRIVATE 链接 seriona_scanner （扫描管线 + SQLite 缓存 + watcher）
          └─ PRIVATE 链接 seriona_metadata（平台媒体集成：MPRIS）
seriona_app（静态库，仅 application_logging/runtime_paths/logging，当前无内部消费者）
```

五个静态库：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`；别名目标 `SerionaBackend::{audio,scanner,metadata,control,app}`。

线程模型概览（源码可证明）：

| 线程 | 归属 | 职责 |
|---|---|---|
| 音频工作线程（单） | `seriona_audio` | 解码填充、状态机、seek、无缝交接、进度发布（全部串行化） |
| miniaudio 回调线程 | `seriona_audio` | 无锁读 PCM 队列 + 增益（实时约束，见 §8） |
| 扫描线程（单） | `seriona_scanner` | 串行执行扫描请求（容量 16 的队列） |
| 扫描 worker 池 | `seriona_scanner` | BS::thread_pool 并发读取元数据（TagReader 有信号量限流） |
| watcher 线程 | `seriona_scanner` | 文件系统事件（含目录被移出监视根的 IN_MOVE_SELF；孤立 IN_MOVED_FROM 经 ~100ms 超时 flush 以 destroy 事件转发，单文件移出监视根即时感知）→ 完整事件入队 → 50ms 去抖归并 → 分类器精准增量更新；60s 周期对账兜底 |
| 控制事件循环（单） | `seriona_control` | 所有命令归约与后端事件处理（串行化点） |
| 订阅投递线程（每订阅类型一个） | `seriona_control` | 快照拷贝后异步回调订阅者 |
| 封面解析线程 | `seriona_control` | TagReader 封面提取（有界 latest-wins 队列） |
| metadata worker（单） | `seriona_metadata` | 播放状态异步转发到平台后端 |

## 3. 项目目录

```
app/                 可执行文件 seriona：main、terminal_controller、terminal_io
inc/seriona/         稳定公共契约（按模块分组；实现导向头不属稳定边界）
src/audio/           播放核心（service 与状态机、device/buffer/clock/events 子目录）、
                     ffmpeg 解码与滤镜、waveform 波形生成（scalar/simd/avx2/strategy_a/b）
src/scanner/         扫描服务/编排器/worker 池、cache/（SQLite v3）、哈希与身份、
                     路径分类、播放列表树、文件夹缩略图解析、TagReader 适配
src/metadata/        平台元数据分享（backend 抽象、MPRIS Linux 实现、Windows 占位）
src/control/         媒体控制器、事件循环、状态归约器、播放上下文构建、封面解析、订阅存储、
                     设置存储（SQLite 文件夹排序/应用设置）
src/app/             应用日志与运行时路径（亦编入 seriona_app）
src/logging/         内部日志模块（无公共头，编入式复用）
src/thumbnail/       缩略图服务（Qt/QImage，未接入任何构建目标，生产禁用）
third_party/         vendored：doctest、miniaudio、watcher（wtr/watcher 头文件库）
tests/               doctest 测试（70+ 目标，见 §9）
tools/               SERIONA_BUILD_TOOLS=ON 才构建：scanner_cold_perf、miniaudio_platform_probe、
                     watch_root_move_audit
docs/、*.md          项目演进记录文档，非事实来源
```

## 4. 模块说明

### 4.1 seriona_audio（播放与波形）

- `AudioPlaybackService`（接口，`audio_contracts.h`）+ 唯一实现 `SingleTrackAudioPlaybackService`：13 个异步控制方法 + 1 个同步 `queryPlaybackClock`（合计 14，勿与测试专用 `AudioPlayer` 的 13 个方法混淆）；所有操作入命令队列由单音频工作线程执行。
- 播放状态机 `PlaybackStateMachine`：Idle → Loading → Ready → Playing ⇄ Paused，另有瞬时 Draining、Stopped、Error；每次迁移发 `PlaybackStateChanged`。seek 为 begin/cancel/complete 三阶段，带 generation 防过期完成。
- `AudioOutputDevice` + 后端接口 `AudioOutputDeviceBackend`：生产后端为 `MiniaudioOutputDeviceBackend`（`MINIAUDIO_IMPLEMENTATION` 仅在该 TU 实例化）；回调经 `renderCallback` 只做无锁读队、补静音、增益、原子计数。输出格式协商（`AudioSampleFormat`，含 `Int24`）与设备枚举/选择：`enumeratePlaybackDevices` 上报设备能力（nativeDataFormats 提取），`AudioOutputConfig.preferredDeviceId`（枚举索引字符串）经 `resolvePreferredDevice` 解析并绑定对应设备，选错格式自动回退并通知。
- `PcmBufferQueue`：无锁 SPSC 字节环 + generation 失效机制（seek 防竞态）；`PlaybackClock`：帧计数驱动（非墙钟）。
- `AudioEventDispatcher`：锁内取 sink 副本、锁外回调；`BackendEvent` 信封带 monotonicVersion/timestamp。
- FFmpeg：`FfmpegAudioSource`（解复用+解码，含 MP3 尾部 ID3v1 净化与损坏尾部截断）、`FfmpegFilterPipeline`（libavfilter 图：abuffer→aformat→abuffersink，输入签名变化时惰性重建）；两者均 pimpl，公共头不暴露任何 AV 类型。
- 波形生成：公共入口 `buildAudioWaveform`；按容器选择策略——MP4 族走 PacketBatches（单输入、250 包一批、克隆解码器）、其余走 SeekChunks（每 chunk 独立解码器 + 1 秒 preroll）；能量核运行时按 CPUID 选择 AVX2（仅 `waveform_simd_avx2.cpp` 编译期加 `-mavx2;-mfma`）/ 标量。波形生成当前仓库内无生产调用方（面向未来可视化消费）。
- 测试专用：`AudioPlayer` 类（`src/audio/audio_player.cpp`）不在库内，仅测试目标直接编译。

### 4.2 seriona_scanner（扫描与缓存）

- `FileScannerService`（接口）+ `FileScanner` 门面 + 工厂 `makeFileScannerService([deps])`；依赖注入经 `FileScannerServiceDependencies{metadataReader, watcherFactory, databasePath, coverExportDir, watcherDebounce}`。
- 扫描主流程（`file_scanner_orchestrator.cpp`）：入队（容量 16）→ 单扫描线程 `runScan` → 逐 root `decideScanMode`（目录树哈希 vs 缓存比对，决定 Full/Incremental）→ `reconcileRoot` 四阶段（发现 → 增量计划/任务准备 → worker 并发元数据读取 → 歌词协调；末段计时为空）→ 返回后由 `recordScanRootDecision` 做缓存写回（单事务，失败整体回滚）→ 聚合构建 `PlaylistTreeSnapshot` → `resolveFolderThumbnails`（扫描收尾：为非根 Directory 节点解析 node-level 缩略图）→ 发布事件。
- 事件顺序：ScanStarted →（每 root：ScanError/FileScanned）→ ProgressUpdated（worker 阶段按 `progressInterval`（默认 250ms）节流中途发布，结束后必发一次最终汇总；`filesScanned` 为"已完成节点数"（含内联 CUE 曲目/容器与失败任务），结束恒有 `filesScanned + filesSkipped == filesDiscovered`）→ PlaylistSnapshotUpdated → ScanCompleted；取消路径先发一条 code=Cancelled 的 ScanError 再发 ScanStopped。
- 缓存：`SQLiteCache`，固定 v3 schema（`user_version=0` 初始化、非 0 非 3 抛 unsupported，无迁移桥）；5 张表 content/locations/lyrics/scan_roots/scan_errors + 8 个索引；WAL + `synchronous=NORMAL` + 64MB 页缓存；写事务 `BEGIN IMMEDIATE`、读写各持独立互斥锁（并发依赖 SQLite busy timeout 500ms）。**实际读写的是 `<databasePath>.scan-roots.sqlite` 独立文件**；传入的主库文件仅被打开初始化，扫描流程不读写。缓存另提供路径级精准写接口（按路径前缀删除、子树改名改写既有行），供事件驱动的精准增量更新使用，不读取元数据。
- 身份哈希：`computeContentId`（duration/title/artist 链式 XXH64）与 `computeLocationId`（路径/大小/mtime，CUE 轨道追加 offset/index）——实现经 `hash_utils.cpp` 文本包含 `song_identity.cpp` 进入生产库。目录树哈希：XXH3_128bits 流式 Merkle（只含文件名/类型/子哈希，跳过 `.lrc`）。
- 并发配置：worker 数默认 `hardware_concurrency`，TagReader 并发默认同 worker 数；环境变量 `SERIONA_SCANNER_WORKERS`、`SERIONA_SCANNER_TAGREADER_CONCURRENCY` 覆盖，`SERIONA_SCANNER_DISABLE_CONCURRENCY=1` 强制串行。
- 自动更新（事件驱动精准增量 + 对账兜底）：wtr watcher（vendored 头文件库，补丁转发目录被移出监视根的 IN_MOVE_SELF；孤立 IN_MOVED_FROM 经 ~100ms 超时 flush 以 destroy 事件转发，单文件移出监视根即时感知）→ 完整 `WatchEvent` 入队 → 50ms 去抖归并 → 事件分类器按 rename 对 / IN_MOVE_SELF / create / modify / destroy × 文件 / 目录 归并去重 → 可精准定位时走路径级精准更新（树补丁 + SQLite 精准 API，不触发全根扫描）；无法分类 / watcher 消息 / 队列溢出 / 移入含未扫描文件的新目录 → 回落全根增量重扫；另有 60s 周期对账兜底（目录树哈希探测，仅变化才重扫）。
- TagReader 适配：`TagReader::Read`/`ReadCueSheet`（全局命名空间外部库）；适配头直接包含 `<TagReader.hpp>` 并暴露其类型，属实现导向头。
- 文件夹缩略图解析（scanner-internal `folder_thumbnail_resolver.{h,cpp}`，不进 `inc/seriona/` 稳定边界）：扫描收尾阶段为非根 Directory 节点解析 node-level 缩略图，回填 `PlaylistNode::thumbnailPath` 随快照下发（Track 节点该字段留空，歌曲缩略图走 `SongMetadata::thumbnailPath`）。case 1 经导出 seam（生产侧由 `exportFolderCoverThumbnail` 装配 `TagReader::ExportFolderCover`，ThumbnailOnly+Ignore，只查目录自身）导出封面；case 2 回退取后代歌曲中已解析缩略图的第一首（(filename, relativeDirectory) 字节序升序）；根目录恒空；确定性全序比较，seam 异常被吞掉，单文件夹失败不阻断扫描。
- 测试专用：`scan_scheduler.{h,cpp}`（通用任务调度器）不编译进任何库，仅测试目标直接编译。

### 4.3 seriona_metadata（平台媒体集成）

- `MetadataSharingService`（接口，`metadata_contracts.h`）+ `MetadataSharingServiceImpl`：`update()` 投递到内部 worker 线程异步转发给后端。
- 后端抽象 `MetadataServiceBackend`，按 `MetadataBackendKind`（Noop/Linux/Windows）选择；Linux 下经 sdbus-c++ 在 session bus 发布 `org.mpris.MediaPlayer2.seriona`（Root + Player 两接口 vtable、PropertiesChanged 信号；仅位置变化的更新不发信号）。
- MPRIS 内部以 `IMprisBus`/`IMprisObject` 抽象隔离 sdbus（测试可注入假总线）；命令（Play/Pause/Seek/SetPosition 等）经 `registerCommandCallback` 回传控制层，带能力门禁。
- Windows 后端为占位（接受但不发布）；`platformExtension` 为不透明 `shared_ptr<void>`。
- 未接线：`MetadataSynchronizer`（同步计划器）编译进库但生产管线未使用；`metadataServiceSynchronize`/`metadataServiceDefaultResult`/`metadataMprisSmokeResult` 无调用点。
- 条件编译：`UNIX AND NOT APPLE` 追加 mpris 实现并链接 sdbus-c++；`WIN32` 追加 windows 实现。公共契约不暴露任何平台类型。

### 4.4 seriona_control（编排核心）

- `MediaController`（pimpl 门面）：`submitCommand`（21 种命令，同步阻塞直到执行完成）、`enumeratePlaybackDevices`（设备枚举）、`scanLibrary`、三路订阅（playerState/libraryState/domainNotifications）、快照查询、`start/shutdown`。命令面含播放/扫描/排序、输出配置（`ConfigureOutput`，携带 `AudioOutputConfig`）、删除（`DeleteTrack`/`DeleteFolder`，直接删原文件，目标经 `targetPath` 传入）、临时队列（`PlayNextTrack`/`ClearPlayQueue`/`RemoveFromQueue`）；播放快照含 `queueEntries`（`[{trackId, nodeId}]`）临时队列字段。另提供应用设置键值读写（`getAppSetting`/`setAppSetting`/`removeAppSetting`，经 `AppSettingsStore` 落库，供前端设置/导航/播放统计三控制器持久化）。
- 命令与后端事件共用单事件循环线程：命令 → `ControlStateReducer`（纯函数归约，含 shuffle 历史、seek 状态抑制、版本去重、PlaybackEnded 自动下一曲/Repeat One）→ `ControlReduction{result, intents, notifications}` → 提交快照 → 发布订阅者 → `executeIntents` 翻译为 audio 调用。
- 播放上下文：`buildPlaybackContextOrder` 从播放列表树快照 DFS 收集轨道 + 多规则排序（缺失值 First/Last）+ 锚点定位；Root/Folder 两种作用域。
- 依赖注入：`MediaControllerDependencies`（audio/scanner/metadata/folderSortSettingsStore/appSettingsStore/artworkResolver），缺失自动回退 noop；生产工厂接线 miniaudio 后端、带 databasePath/coverExportDir 的 scanner、Linux metadata、SQLite 文件夹排序存储（databasePath 非空时）、SQLite 应用设置存储（与排序存储共享 databasePath）。
- 封面解析：`ArtworkResolver`（有界 latest-wins 队列 + 结果 epoch 失效）+ 归约器 generation 校验，结果回填 `player_.artwork.localPath`。
- 文件夹排序：`FolderSortSettingsStore` 抽象（Noop/SQLite 实现，手写 JSON 解析）；`ApplyFolderSortRules` 命令持久化、扫描启动时重放、播放上下文构建时回填。
- 应用设置：`AppSettingsStore` 抽象（Noop/SQLite 实现，接口含 `set`/`get`/`remove`/`listByGroup`（按 key 排序）），`app_settings` 表 `(group_name, key, value, updated_at_ms)` 主键 `(group_name, key)`；`getAppSetting` 失败返回 nullopt（未启动/未存储），值以不透明字符串存储（后端不解释，前端负责编解码）。
- 订阅分发：每订阅类型一个独立投递线程，快照拷贝后异步回调，避免阻塞归约线程。

### 4.5 seriona_app 与入口层

- `seriona` 可执行文件直接编译 `main.cpp`、`terminal_controller.cpp`、`terminal_io.cpp`、`runtime_paths.cpp`、`logging.cpp`，链接 `seriona_control`（不链接 `seriona_app`），仅 Release 追加 FFmpeg（用于压制 FFmpeg 库日志）。
- `seriona_app` 静态库（application_logging/runtime_paths/logging）当前无内部消费者，作为对外分发单元存在。
- `src/logging/`：内部日志模块（无公共头），编入式复用（scanner、seriona_app、可执行文件各编译一份）；`createDedicatedLogger` 供 scanner 创建 TagReader 独立日志器。
- 公共入口：`initializeApplicationLogging(RuntimePaths)` 与 `resolveRuntimePaths(executablePath)`（`inc/seriona/app/`）。

## 5. 模块关系

- 库链接（根 CMakeLists）：`seriona_control` PRIVATE → audio/scanner/metadata/SQLite3/spdlog；`seriona_audio` PUBLIC → FFmpeg + third_party 头、PRIVATE → BS::thread_pool/spdlog；`seriona_scanner` PUBLIC → SQLite3/xxhash/TagReaderCore/thread_pool/watcher 头/spdlog；`seriona_metadata` PRIVATE → spdlog（Linux 追加 sdbus-c++）。
- 数据流：`terminal_io`（按键）→ `MediaControlCommand` → 事件循环 → 归约 → audio 调用 → `BackendEvent` → 归约 → 订阅者；快照同时驱动 metadata 分享。
- 扫描数据流：`FileScannerService` 事件（含 `PlaylistTreeSnapshot`）→ control 归约更新曲库 → 播放上下文重建（当前曲消失自动续播）。
- 头级循环依赖：`metadata_contracts.h` 包含 `control_contracts.h`，control 侧前置声明 `MetadataSharingService` 打破环。
- 跨模块内部头耦合：`media_controller_module.cpp` 包含 scanner 私有头 `file_scanner_service_internal.h`（使用 `FileScannerServiceDependencies`）。
- 公共契约边界：audio 看 `audio_contracts.h`，scanner 看 `scanner_contracts.h`/`file_scanner_service.h`，metadata 看 `metadata_contracts.h`，control 看 `control_contracts.h`/`media_controller.h`；新增稳定契约不得暴露 TagReader、SQLite、watcher、FFmpeg、MPRIS/sdbus、Windows 类型。

## 6. 启动流程

```
main(argc=2, 路径存在)                       main.cpp
  └─ runTerminalController(musicPath)        terminal_controller.cpp
      ├─ TerminalMode 检查（非 tty 退出）
      ├─ resolveRuntimePaths → ensureDirectoriesExist     SerionaData/（便携）或 XDG 目录（安装模式）
      ├─ (Release) av_log_set_level(AV_LOG_QUIET)
      ├─ prepareLogFile(logs/) → 生成时间戳日志名 → logging::initialize(console=off, file, level)
      ├─ makeProductionMediaController({}, library.sqlite, artwork)
      ├─ runTerminalControllerSession
      │   ├─ 订阅 playerState/libraryState/notifications 三路
      │   ├─ controller.start()
      │   ├─ scanLibrary({musicPath, recursive}, Full)
      │   └─ 命令循环：readAction(100ms) → 按键映射命令 → submitCommand；q 退出
      └─ Stop → controller.shutdown() → 退订 → spdlog::shutdown()
```

运行时路径规则（`runtime_paths.cpp`）分两种构建模式（编译期宏 `SERIONA_INSTALLED_MODE` 选择，默认便携）：
- 便携模式：可执行文件目录（Linux 经 `/proc/self/exe`）下的 `SerionaData/` 为 data root。
- 安装模式（Linux 安装版，`cmake -DSERIONA_INSTALLED_MODE=ON`）：遵循 XDG Base Directory，应用 ID 为 `org.kaizen857.Seriona`——数据 `$XDG_DATA_HOME/org.kaizen857.Seriona`、日志 `$XDG_STATE_HOME/org.kaizen857.Seriona/logs`、封面缓存 `$XDG_CACHE_HOME/org.kaizen857.Seriona/artwork`；`XDG_*` 未设置时回退 `$HOME` 默认值，相对路径忽略（规范要求）。
- 两种模式下布局一致：日志实际文件为 `logs/seriona-<时间戳>.log`（`RuntimePaths.logFile` 中的 `seriona.log` 仅为逻辑位），数据库 `library.sqlite`，封面目录 `artwork`；`resolvePortableRuntimePaths`/`resolveInstalledRuntimePaths` 均无条件编译，`resolveRuntimePaths` 依宏选择，测试直接以环境变量注入覆盖 installed 分支。

## 7. 核心运行流程

### 7.1 播放控制链路

`submitCommand(Play/Seek/...)` → 控制事件循环 → 归约器产出意图 → `executeIntents` 调 `AudioPlaybackService` → 音频工作线程执行（状态机迁移、解码填充、时钟推进）→ `BackendEvent`（100ms 节流的进度事件、状态变更、seek 不连续事件、错误）→ 控制事件循环 → 归约器按 monotonicVersion 去重 → 快照发布 → 订阅投递线程回调；同一快照同时传给 metadata 服务发布到平台。

### 7.2 扫描流程

扫描存在两条路径：**全量/增量重扫**（手动 `scanLibrary`、事件无法精准定位时的回落）与**事件驱动的精准增量**（watcher 事件主路径），两者共用扫描队列串行化。

**全量/增量重扫**：入队 → 单扫描线程逐 root 计算目录树哈希，与 `scan-roots.sqlite` 中 `CachedScanRoot` 比对决定全量/增量；增量时逐文件用 `computeLocationId`（路径/大小/mtime）判定 added/changed/unchanged/deleted，unchanged 走缓存直灌（含 CUE 轨道与歌词），changed/added 构造 worker 任务并发读元数据；结束后歌词协调（外置 LRC 哈希比对/重解析/清除）→ 缓存写回（单事务，失败整体回滚）→ `PlaylistTreeBuilder` 聚合 → `resolveFolderThumbnails` 为非根 Directory 节点解析 node-level 缩略图（`folder_thumbnail_resolver`：生产 seam 经 `exportFolderCoverThumbnail` 接 `TagReader::ExportFolderCover` 导出，回退取后代歌曲已解析缩略图，根目录恒空）→ `PlaylistSnapshotUpdated`。

**事件驱动精准增量**（watcher 事件主路径）：文件系统事件（含目录被移出监视根的 `IN_MOVE_SELF`，经 vendored wtr 补丁转发；孤立 `IN_MOVED_FROM` 经 ~100ms 超时 flush 以 destroy 事件转发，单文件移出监视根即时感知）→ 完整 `WatchEvent` 入队 → 50ms 去抖归并 → 事件分类器按 rename 对 / IN_MOVE_SELF / create / modify / destroy × 文件 / 目录 归并同批去重 → 可精准定位的批次执行路径级操作：长生命周期 `PlaylistTreeBuilder` 成员的子树删除 / 子树改名 / 单歌 upsert（树补丁）+ SQLite 精准 API（路径前缀删除、子树改名改写既有行），不读取元数据、不触发扫描；每批处理后发布完整快照（控制层整树替换假设不变）。无法分类的事件 / watcher 消息 / 队列溢出标记 / 移入含未扫描文件的新目录 / 根自身被移出 → 回落全根增量重扫。

**周期对账兜底**：事件驱动之外，以可注入周期（默认 60s）对全部根做目录树哈希探测，仅 hash 变化才回落重扫、无变化零发布；覆盖事件丢失、队列溢出等静默失效，保证极端情况下最终收敛。

### 7.3 元数据分享（Linux MPRIS）

快照发布 → `metadata->update` → metadata worker → Linux 后端 `publishCurrentSnapshot`：映射为 MPRIS 属性（trackid/length/artUrl/xesam:url 等）→ 若相对上次仅 Playing 状态下的位置变化则不发 `PropertiesChanged` → 否则发信号；外部控制命令（如 MPRIS 客户端 Play/Seek）经 sdbus 方法 → 能力门禁 → `MediaControlCommandSink` → 控制层归约。

### 7.4 封面解析

`ResolveArtwork` 意图 → `ArtworkResolver`（1 in-flight + 1 pending，最新请求覆盖）→ TagReader 读取封面并导出到 `artwork/` 目录 → 结果回控制事件循环 → 归约器按 generation + trackId 校验 → 回填 `artwork.localPath`。

## 8. 配置方式

- 构建期（CMake 选项）：`SERIONA_BUILD_APP`（默认 ON）、`SERIONA_BUILD_TESTS`（默认 ON）、`SERIONA_BUILD_TOOLS`（默认 OFF）、`SERIONA_TAGREADER_SOURCE_DIR`（TagReader 源码路径）；三个 `SERIONA_*_SIMULATE_MISSING_*` 选项会故意令配置失败（依赖门禁演示）。
- 运行时：**无配置文件**。路径全部由可执行文件位置推导（§6）；scanner 并发由环境变量 `SERIONA_SCANNER_WORKERS`、`SERIONA_SCANNER_TAGREADER_CONCURRENCY`、`SERIONA_SCANNER_DISABLE_CONCURRENCY` 调节（非法值警告并忽略）。
- 命令行：单参数（音乐根目录或文件），必须存在。
- 日志级别：Release 构建 logger 级别 info、Debug 构建 trace；控制台 sink 在终端 UI 下恒关闭。运行时可经 `setLogLevel`（`inc/seriona/app/application_logging.h`）调整（前端设置窗口接线）。

## 9. 测试

- 构建：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N>`；运行 `build/seriona <音乐根目录或文件>`。
- 发现/运行：`ctest --test-dir build -N`；`ctest --test-dir build --output-on-failure`；聚焦 `ctest --test-dir build -R '<regex>' --output-on-failure`（常用：`seriona\.audio`、`seriona\.scanner`、`seriona\.metadata`、`seriona\.control`、`seriona\.logging`、`seriona\.runtime_paths`、`seriona\.application_logging`）。
- doctest 二进制必须恰有一个 main：多数目标由 CMake 注入 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`（含 `seriona_tests`，其 tests/main.cpp 不定义 main）；真正自带 main、未注入宏的是 `seriona_audio_fixture_tests`、`seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test`。注意 `seriona_scanner_cache_schema_tests` 链接裸 `sqlite3`。
- 特殊注册：cancellation 状态机测试单独注册为 `seriona.playback_state_machine_cancellation`（普通状态机测试排除它）；`seriona.audio.waveform.perf` 超时 240s；`seriona.control_artwork_resolver` 超时 60s。
- 禁用目标：`seriona_scanner_cache_tests`、`seriona_scanner_cache_content_tests`、v2→v3 migration、backup rollback、phase1 integration 在 `tests/CMakeLists.txt` 中整段注释，不要假设可运行。
- 性能目标：`seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test` 只构建不注册 CTest，直接运行 `build/tests/<目标>`；`tools/scanner_cold_perf`（-DSERIONA_BUILD_TOOLS=ON）为独立冷扫描基准。
- 测试隔离：音频测试使用 fake `AudioOutputDeviceBackend` 或测试现场生成的短音频 fixture；扫描测试使用 `scanner_test_harness` 与测试接缝（`file_scanner_orchestrator_test_access.h` 的全局观察者，仅测试包含）。

## 10. 扩展方式

- 音频后端：实现 `AudioOutputDeviceBackend` 并经 `makeAudioPlaybackService(backend)` 注入。
- 元数据后端：实现 `MetadataServiceBackend` 或复用 MPRIS 抽象（`IMprisBus`/`IMprisObject`）；通过 `MetadataSharingOptions.backendKind` + `platformExtension` 选择。
- 扫描依赖：`FileScannerServiceDependencies` 注入自定义元数据读取器（`TagMetadataReader`）与 watcher 工厂。
- 排序存储：实现 `FolderSortSettingsStore` 抽象。
- 应用设置：实现 `AppSettingsStore` 抽象（Noop 或 SQLite 落库）。
- 控制器依赖：`MediaControllerDependencies` 全量注入，缺失项自动回退 noop——可用于无 UI/无真实硬件的场景。
- 对外消费：订阅 `PlayerStateSnapshot`/`LibraryStateSnapshot`/领域通知即可构建新前端；`AudioPlaybackService` 与 `FileScannerService` 也可独立使用。
- 前端集成：终端控制器是 `TerminalActionReader` 抽象之上的唯一实现，新 UI 可替换入口层而保持 control 不变。

## 11. 开发建议

- 实时路径红线：`AudioOutputDevice::renderCallback()` 内禁止 FFmpeg、事件回调、日志、动态分配、阻塞锁、设备生命周期操作。
- schema 红线：`SQLiteCache` schema 固定 v3，`user_version=0` 直接初始化、非 0 非 3 报错；不存在迁移桥，改 schema 必须同步 `sqlite_cache_connection.cpp` 内嵌 SQL 与 `cache/schema.sql`（一致性仅靠测试校验）。
- 编译归属陷阱：`audio_player.cpp`、`scan_scheduler.cpp` 只被测试目标编译；`song_identity.cpp` 经 `hash_utils.cpp` 文本包含进库——生产代码不要依赖这些文件的独立编译单元身份。AVX2/FMA 参数只允许施加于 `waveform_simd_avx2.cpp`（根 CMake 仅对该文件施加 `-mavx2;-mfma`，无专门守卫；FATAL_ERROR 守卫只针对 `BS::thread_pool` 链接）。
- 未接线代码（勿假设生效）：`MetadataSynchronizer`、`buildScalarWaveformBars`、scanner `ProgressThrottle` 类（orchestrator 自持轻量时间节流，未用此类）、`platformExtension`（Windows）。设备选择已接线（§4.1：`preferredDeviceId` 解析绑定，非纯标签）。
- 新稳定契约不得暴露第三方类型（TagReader/SQLite/watcher/FFmpeg/MPRIS/sdbus/Windows）。
- 文档优先级：本文件低于 CMake 配置、源码与测试注册；README 仅有标题，`docs/` 为演进记录。

## 12. 维护建议

- 启动日志：`SerionaData/logs/seriona-<时间戳>.log`（5MB×3 滚动，总量超 50MB 自动清理最旧）。
- 缓存重置：删除 `SerionaData/` 下 `*.scan-roots.sqlite`（与 `library.sqlite`）即可强制下次全量扫描并重建状态。
- 常见故障：配置失败时按 FATAL_ERROR 提示安装缺失系统库（sqlite、xxhash、sdbus-c++）；`ctest` 中波形/封面相关目标超时较长，聚焦时用 `-R` 正则。
- 提交与文档语言：中文（与仓库既有约定一致）。
