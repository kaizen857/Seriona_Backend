# Seriona_Backend

Seriona 的本地音乐库后端：一个纯 C++23 实现的后端，负责扫描本地音乐目录、读取元数据、播放音频、管理播放状态，并提供终端入口。后端不依赖 Qt 或任何 UI 框架；桌面前端 Seriona（Qt Quick 工程）通过 CMake 子树或 FetchContent 方式接入本仓库的库目标。

## 项目简介

Seriona_Backend 接收一个音乐根目录（或单个音乐文件），扫描目录中的音频文件与 CUE 索引，构建播放列表树，并提供「命令 → 状态 → 事件」式的播放控制接口。本仓库内自带一个终端控制器（`app/`），以单行状态栏的方式驱动后端；后端本身与 UI 解耦，通过订阅机制对外发布播放状态与曲库快照，任何前端都可以基于 `inc/seriona/` 下的公共契约集成。

关键选型：

| 领域 | 选型 |
|------|------|
| 构建 | CMake 3.20+，C++23，仅 CXX |
| 音频解码 | FFmpeg（`libavformat` / `libavcodec` / `libavutil` / `libavfilter` / `libswresample`） |
| 音频输出 | miniaudio（vendored 单头文件） |
| 标签读取 | 外部仓库 TagReader（`TagReaderCore`），经适配层接入 |
| 元数据缓存 | SQLite（固定 v3 schema）；哈希 libxxhash（XXH3 / XXH64） |
| 平台媒体集成 | Linux 上经 sdbus-c++ 发布 MPRIS 2.x 对象；Windows 仅有占位实现 |
| 日志 | spdlog；测试框架 doctest（vendored） |
| 并发 | `bshoshany/thread-pool` v4.1.0（FetchContent 固定版本） |

## 功能特性

### 扫描与曲库（seriona_scanner）

- 文件系统扫描：接收一个或多个扫描根，递归发现音频文件；通过目录树哈希与缓存比对自动决定走全量（Full）还是增量（Incremental）扫描。
- 内容缓存：`SQLiteCache` 使用固定 v3 schema（`user_version=3`），表结构覆盖 content / locations / lyrics / scan_roots / scan_errors，采用 WAL 模式与独立读写锁。
- 精准增量写入：缓存提供路径级写入接口（`deleteLocationsByPathPrefix` / `replaceLocationsBySubtree`），事件驱动的增量更新只触碰变化路径，不触发全根重扫。
- 目录监视器：集成 vendored 的 wtr/watcher 头文件库，监视文件系统事件；事件经 50ms 去抖归并后由分类器做精准增量更新；另有 60 秒周期对账（`reconcileInterval = 60000ms`）作为兜底。
- 歌曲身份：基于 libxxhash 计算内容哈希（XXH64 链式 `computeContentId`）与位置哈希，同一音频字节跨路径可去重；目录树哈希使用 XXH3_128bits 流式 Merkle。
- CUE 与 LRC：解析 CUE 索引生成子曲目（含 offset / duration / sourceFilePath 元数据），解析内嵌与外置 LRC 歌词并做歌词协调。
- 播放列表树：`PlaylistTreeBuilder` 构建 Root / Directory / Album / Disc / Track 五级节点树，提供 `upsertSong` / `removeSubtree` / `renameSubtree` 等精准更新能力。

### 元数据（seriona_metadata）

- 标签读取：经 `TagReaderCore` 适配层读取音频标签（标题、艺人、专辑、音轨号、年份、采样率、位深等）。
- MPRIS 集成：在 Linux（`UNIX AND NOT APPLE`）上经 sdbus-c++ 于 session bus 发布 `org.mpris.MediaPlayer2.seriona`（Root 与 Player 两接口），支持属性同步与外部命令回传（Play / Pause / Seek / SetPosition 等，带能力门禁）；Windows 仅有占位后端。
- 封面解析与导出：控制层 `ArtworkResolver` 负责封面解析（有界 latest-wins 队列 + generation 失效），扫描器经导出 seam 将文件夹封面导出到封面导出目录，随快照下发 `artworkPath` / `thumbnailPath`。

### 音频播放（seriona_audio）

- 播放管线：FFmpeg 解复用与解码（`FfmpegAudioSource`，含 MP3 尾部 ID3v1 净化与损坏尾部截断）+ libavfilter 滤镜图（abuffer → aformat → abuffersink，输入签名变化时惰性重建）+ miniaudio 输出后端。
- 输出格式：支持 Int16 / Int24 / Int32 / Float32 PCM 目标格式协商；输出协商失败或降级时发布 `OutputModeFallback` 事件，解码失败以类型化 `PlaybackErrorCode::DecodeFailed` 事件上报（含 drain 守卫超时）。
- 波形生成：`buildAudioWaveform` 按容器选择策略（MP4 族走 PacketBatches、其余走 SeekChunks）；能量核按 CPUID 在运行时选择 AVX2（仅 `waveform_simd_avx2.cpp` 编译期加 `-mavx2 -mfma`）或标量实现。
- 播放状态机：Idle → Loading → Ready → Playing ⇄ Paused，另有瞬态 Draining / Stopped / Error；seek 为 begin / cancel / complete 三阶段，带 generation 防过期完成。

### 控制层（seriona_control）

- 命令面：`MediaController::submitCommand` 支持 21 种命令，覆盖播放控制、上下文播放、排序、输出配置、删除与临时播放队列：

| 类别 | 命令 |
|------|------|
| 播放控制 | Play、Pause、Stop、TogglePlayPause、SeekTo、SeekBy、SetVolume、SetMuted、SetRepeatMode、SetShuffle、SkipNext、SkipPrevious、SelectTrack |
| 上下文播放 | StartPlaybackFromContext（Root / Folder 两种作用域，多规则排序 + 锚点定位） |
| 排序 | ApplyFolderSortRules（每文件夹排序设置，SQLite 持久化） |
| 输出配置 | ConfigureOutput（携带 `AudioOutputConfig`） |
| 删除 | DeleteTrack、DeleteFolder（直接删除原文件，无回收站） |
| 临时播放队列 | PlayNextTrack、ClearPlayQueue、RemoveFromQueue |

- 播放队列：临时队列（`queueEntries: [{trackId, nodeId}]`）不持久化，重启清空；快照含 `PlaybackEnded` 自动下一曲 / Repeat One 逻辑。
- 状态归约：命令与后端事件共用单事件循环线程，经纯函数 `ControlStateReducer` 归约后提交快照并分发订阅者。
- 文件夹排序存储：`FolderSortSettingsStore` 抽象（Noop / SQLite 实现），按根路径 + 文件夹节点 ID 持久化多规则排序设置，扫描启动时重放。

### 终端入口（app / seriona_app）

- `seriona` 可执行文件：接收一个已存在的文件或目录路径，驱动完整生产管线（miniaudio 播放、扫描、生产 metadata），单行状态栏展示播放与扫描状态。
- 运行时日志：`initializeApplicationLogging` 初始化 spdlog（滚动文件），`setLogLevel` 支持运行时切换全局日志等级。

## 架构概览

分层结构（依赖方向自上而下，`inc/seriona/` 是稳定契约边界）：

```
seriona（可执行文件，app/）
  ├─ 直接编译 main / terminal_controller / terminal_io / runtime_paths / logging
  └─ seriona_control（编排枢纽：命令、状态归约、事件分发）
      ├─ PRIVATE 链接 seriona_audio    （播放管线 + 波形生成）
      ├─ PRIVATE 链接 seriona_scanner  （扫描管线 + SQLite 缓存 + watcher）
      └─ PRIVATE 链接 seriona_metadata （平台媒体集成：MPRIS）
seriona_app（静态库，application_logging / runtime_paths / logging）
```

- 五个静态库：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`，并导出别名目标 `SerionaBackend::{audio,scanner,metadata,control,app}`，供前端按别名链接。
- 可执行文件 `seriona` 直接编译 terminal / runtime_paths / logging 源，链接 `seriona_control` 而不链接 `seriona_app`，仅 Release 追加 FFmpeg（用于压制 FFmpeg 库日志）。
- 公共契约边界 `inc/seriona/` 按模块组织：

| 模块 | 稳定契约头 |
|------|-----------|
| audio | `inc/seriona/audio/audio_contracts.h`（`AudioPlaybackService` 接口、`PlaybackEvent` 事件族、`AudioOutputConfig`） |
| scanner | `inc/seriona/scanner/scanner_contracts.h`（`FileScannerService` 接口、`SongMetadata`、`PlaylistTreeSnapshot`）、`file_scanner_service.h`（`FileScanner` 门面与工厂） |
| metadata | `inc/seriona/metadata/metadata_contracts.h`（`MetadataSharingService` 接口） |
| control | `inc/seriona/control/control_contracts.h`（21 种命令、快照结构、订阅句柄）、`media_controller.h`（`MediaController` 门面与生产工厂） |
| app | `inc/seriona/app/application_logging.h`、`runtime_paths.h` |

- 错误风格：公共契约使用 typed enum（`MediaControllerErrorCode`、`ScannerErrorCode`、`PlaybackErrorCode` 等）+ result struct（`MediaControllerCommandResult`、`ScannerTaskResult` 等）；异常只从实现抛出，公共头不声明 `throw`、不使用 `std::expected`。
- 实现导向头（TagReader 适配、SQLite cache、watcher、FFmpeg 类型等）不属稳定边界；新增稳定契约不得暴露 TagReader、SQLite、watcher、FFmpeg、MPRIS/sdbus 或 Windows 类型。

## 构建要求

- CMake 3.20 或更高版本，支持 C++23 的编译器（GCC / Clang / MSVC）。
- `pkg-config` 可解析的 FFmpeg 开发包：`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`。
- `pkg-config` 可解析的 `libxxhash`（scanner 哈希依赖）。
- CMake 可找到的 `spdlog`（`find_package(spdlog CONFIG REQUIRED)`）。
- CMake 可找到的 `SQLite3`（`find_package(SQLite3 REQUIRED)`）。
- 仅在 `UNIX AND NOT APPLE` 平台上额外要求 `pkg-config` 可解析的 `sdbus-c++`（MPRIS 元数据分享依赖）。
- vendored 依赖（无需安装）：miniaudio（单头文件）、doctest（测试框架）、wtr/watcher（头文件库）。
- FetchContent 依赖：`bshoshany/thread-pool` v4.1.0（构建期自动拉取）。

以 Arch Linux 为例的安装命令：

```bash
sudo pacman -S cmake ffmpeg xxhash spdlog sqlite sdbus-c++
```

## 构建与运行

```bash
cmake -S . -B build -DSERIONA_BUILD_TESTS=ON
cmake --build build -j<N>
```

构建产物为 `build/seriona`。运行：

```bash
build/seriona /path/to/music-root-or-file
```

运行约束：

- 只接受一个参数，且必须是已存在的文件或目录路径；参数个数不对时打印用法并返回 2，路径不存在时返回 1。
- 传入目录时扫描整个目录树；传入文件时以该文件为曲库。
- 应用会初始化运行时路径（数据根、数据库、封面导出目录、日志文件）并启动生产播放管线（miniaudio 输出、真实扫描器与生产 metadata）。

CMake 选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `SERIONA_BUILD_APP` | ON | 构建 `seriona` 可执行文件 |
| `SERIONA_BUILD_TESTS` | ON | 构建测试并注册 CTest |
| `SERIONA_BUILD_TOOLS` | OFF | 构建可选工具（性能测试、平台探针、监视根移动审计） |
| `SERIONA_TAGREADER_SOURCE_DIR` | 空 | 指定 TagReader 源码目录（见下节） |

另有三个仅用于验证依赖门禁的开关（`SERIONA_SCANNER_SIMULATE_MISSING_SQLITE`、`SERIONA_SCANNER_SIMULATE_MISSING_XXHASH`、`SERIONA_METADATA_SIMULATE_MISSING_SDBUS`），开启后会故意令配置失败，正常构建不要开启。

## TagReader 集成

后端依赖外部仓库 [kaizen857/TagReader](https://github.com/kaizen857/TagReader) 提供的 `TagReaderCore`（C++23 标签读取库）。解析顺序：

1. 若已有现成的 `TagReaderCore` 目标（例如上层工程先加入），直接复用。
2. 配置期显式传入 `-DSERIONA_TAGREADER_SOURCE_DIR=<path>`，使用该源码目录。
3. 未显式指定时，检查相邻目录 `../TagReader`。
4. 以上都不可用时，通过 FetchContent 拉取 `https://github.com/kaizen857/TagReader.git` 的 `main` 分支。

以 `EXCLUDE_FROM_ALL` 方式加入的 vendored TagReader 会关闭其测试构建并剥离其测试目标，因此 ctest 中不会出现 TagReader 的测试。

## 测试

测试框架为 vendored 的 doctest。全量运行：

```bash
ctest --test-dir build --output-on-failure
```

定向运行：

```bash
ctest --test-dir build -R '<regex>' --output-on-failure
```

稳定测试分组（可按前缀聚焦）：

| 前缀 | 覆盖范围 |
|------|----------|
| `seriona.audio` | 播放管线：解码、滤镜、PCM 队列、时钟、状态机、事件分发、波形、输出协商、无缝切换 |
| `seriona.scanner` | 扫描：缓存 schema 与位置、CUE/LRC、树构建、哈希与身份、watcher、增量扫描、两遍发现、worker 池、端到端 |
| `seriona.metadata` | 元数据：mapper、service、MPRIS |
| `seriona.control` | 控制层：命令归约、播放上下文、删除命令、封面解析、排序存储、临时队列 |
| `seriona.logging` | 内部日志模块 |
| `seriona.runtime_paths` | 运行时路径解析 |
| `seriona.application_logging` | 应用日志初始化与运行时等级切换 |

## 目录结构

```
app/                  seriona 可执行文件：main、terminal_controller、terminal_io
inc/seriona/          稳定公共契约（按模块分组：audio / scanner / metadata / control / app）
src/audio/            播放核心（service / state_machine / device / buffer / clock / events）、
                      ffmpeg 解码与滤镜、waveform 波形生成（scalar / simd / avx2 / strategy_a / strategy_b）
src/scanner/          扫描服务与编排器、worker 池、cache/（SQLite v3）、哈希与身份、
                      路径分类、LRC 解析、播放列表树、文件夹缩略图解析、TagReader 适配
src/metadata/         平台元数据分享（backend 抽象、MPRIS Linux 实现、Windows 占位）
src/control/          媒体控制器、事件循环、状态归约器、播放上下文构建、封面解析、订阅存储
src/app/              应用日志与运行时路径（编入 seriona_app）
src/logging/          内部日志模块（无公共头，编入式复用）
third_party/          vendored：doctest、miniaudio、watcher（wtr/watcher 头文件库）
tests/                doctest 测试（按模块组织，注册进 CTest）
tools/                SERIONA_BUILD_TOOLS=ON 才构建：scanner_cold_perf、miniaudio_platform_probe、
                      watch_root_move_audit
docs/、*.md           历史分析文档，非事实来源
```

说明：仓库中另有 `src/thumbnail/` 与 `inc/seriona/thumbnail/` 目录被 Git 跟踪，但未接入任何 CMake 目标，不属于生产功能。

## 许可证

本项目以 GPL-3.0 许可证发布，详见仓库根目录的 [LICENSE](LICENSE) 文件。
