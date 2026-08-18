# AGENTS.md

## 边界与事实来源
- 当前目录即仓库根；不要向上扫描 Seriona。`build/`、`build-*/`、`.omo/` 是生成或工作目录，不是源码。
- 当前事实按根/子目录 `CMakeLists.txt`、源码、测试排序核对；它们高于仅有标题的 `README.md` 与描述长期稳定设计的 `DESIGN.md`（后者的定位与维护规范见「DESIGN.md 维护规范」）。
- 仓库无 CI、格式化配置和 `CMakePresets.json`；`.clangd` 与 VS Code clangd 固定读取 `build/` 编译数据库。
- 面向用户的回复、新增项目文档和提交信息使用中文。
- 这是独立 C++23 后端；生产代码不得引入 Qt/QML/UI，平台媒体集成留在 metadata 私有实现。已跟踪的 `src/thumbnail/`、`inc/seriona/thumbnail/` 因使用 `QImage`/`QImageReader` 未被任何 CMake 目标包含，禁止接入生产。
- 根目录两个跟踪文件非源码：`detailed-scanner-perf-report.txt`（生成的性能报告产物）与 `FILE_SCANNER_ANALYSIS.md`（旧项目历史分析，同工作区根 `docs/` 性质），均只作背景材料；根目录 `.ruff_cache/` 是未跟踪的 ruff 工作缓存，不是源码。

## DESIGN.md 维护规范
- `DESIGN.md` 是描述项目长期稳定设计的架构文档，不是开发日志、变更记录或实现细节文档；应保持稳定，避免随开发逐渐演变为实现文档或变更日志。
- 任何开发任务在收尾前，必须将“是否需要更新 `DESIGN.md`”作为检查项之一（非可选步骤）：判定需要则必须在同一次任务内同步更新，判定不需要则不改动文档，也不要为了完成检查而做无意义修改。
- 判断是否更新只看一条标准：新开发者仅读当前 `DESIGN.md` 是否会错误理解项目现状。不依据“是否改了代码”或“是否动了架构名称”。
- 通常不应更新：Bug 修复、代码重构、性能优化、实现细节调整、内部接口调整、参数修改、不影响整体设计的小功能开发、代码风格调整、测试补充等。
- 通常应当更新：整体架构调整、模块新增或删除、模块职责变化、模块协作关系变化、启动流程变化、核心运行流程变化、配置体系变化、扩展机制变化、长期维护方式变化，以及其他会影响开发者理解项目整体设计的重要修改。

## 构建、运行与依赖
- 配置并构建：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N>`。
- 运行：`build/seriona /path/to/music-root-or-file`；只接受一个已存在的文件或目录路径。
- 配置要求 CMake 3.20+、C++23、`pkg-config` 可解析 FFmpeg 的 `libavformat libavcodec libavutil libavfilter libswresample` 与 `libxxhash`，CMake 可找到 `spdlog`、`SQLite3`。
- 仅当 CMake 条件 `UNIX AND NOT APPLE` 成立时还要求 `pkg-config` 可解析 `sdbus-c++`；不要把该条件改写成“Linux”。
- 若无现成 `TagReaderCore`，依次使用 `-DSERIONA_TAGREADER_SOURCE_DIR=<path>`、相邻 `../TagReader`，否则 FetchContent 拉取 `https://github.com/kaizen857/TagReader.git` 的 `main`；vendored 的 TagReader 以 `EXCLUDE_FROM_ALL` 加入且其测试目标被剥离，ctest 里不会有 TagReader 测试。
- `SERIONA_BUILD_APP`、`SERIONA_BUILD_TESTS`、`SERIONA_BUILD_TOOLS` 默认分别为 ON、ON、OFF；线程池 FetchContent 固定 `bshoshany/thread-pool` v4.1.0。

## 入口与模块边界
- 五个静态库是 `seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`。可执行文件 `seriona` 直接编译 terminal、runtime_paths、logging 源，链接 `seriona_control` 而不链接 `seriona_app`，仅 Release 直接追加 `PkgConfig::SERIONA_FFMPEG`。
- `app/main.cpp` 做单参数/路径校验和顶层异常边界；`app/terminal_controller.cpp` 与 `terminal_io.{h,cpp}` 管终端生命周期、运行时路径、日志及生产控制器创建。
- `makeProductionMediaControllerDependencies(databasePath, coverExportDir)` 连接 miniaudio、scanner、生产 metadata；databasePath 非空时使用 SQLite 文件夹排序存储。
- `makeDefaultMediaControllerDependencies()` 使用 no-op 音频和文件夹排序存储，但仍调用真实 scanner/metadata 工厂。
- 对外契约以 `inc/seriona/` 为边界：audio 看 `audio_contracts.h`，scanner 稳定入口只看 `scanner_contracts.h`/`file_scanner_service.h`，metadata 看 `metadata_contracts.h`，跨模块编排走 control；`inc/` 中的 TagReader adapter、SQLite cache 等实现导向头不属稳定边界，新增稳定契约不得暴露 TagReader、SQLite、watcher、FFmpeg、MPRIS/sdbus 或 Windows 类型。
- 公共契约错误风格：typed enum（`MediaControllerErrorCode`、`ScannerErrorCode` 等）+ result struct（`MediaControllerCommandResult`、`ScannerTaskResult` 等），异常（`std::runtime_error`/`std::invalid_argument`）只从实现抛出；公共头不声明 `throw`、不用 `std::expected`。
- `src/logging/` 是无公共头的 `seriona::logging` 内部模块，编入 scanner/app/可执行文件及测试；公共入口是 `inc/seriona/app/application_logging.h` 的 `initializeApplicationLogging`，入口公共 API 还包括 `runtime_paths.h`。
- 部分 `inc/seriona/` 头（`scan_scheduler.h`、`song_identity.h`）与 `src/audio/audio_player.cpp` 的实现只被测试目标直接编译，未进任何静态库：生产代码调用这些符号会在最终链接报 undefined reference；`AudioPlayer` 类本质是测试专用封装，生产走 `makeAudioPlaybackService`。

## 不可破坏的约束
- miniaudio 回调最终进入 `AudioOutputDevice::renderCallback()`；实时路径只能读 PCM 队列、补静音、应用音量/静音、更新原子计数，禁止 FFmpeg、事件回调、日志、动态分配、阻塞锁和设备生命周期操作。
- scanner 缓存实现为 `SQLiteCache`，schema 固定 v3：`user_version=0` 直接初始化 v3，任何非 0 且非 3 版本报 unsupported；不存在 v2 迁移桥。缓存另有事件驱动的路径级精确写 API `deleteLocationsByPathPrefix`/`replaceLocationsBySubtree`（`inc/seriona/scanner/cache/sqlite_cache.h`），`PlaylistTreeBuilder` 提供 `upsertSong`/`removeSubtree`/`renameSubtree`，服务依赖含 `reconcileInterval{60000}` 的 60s 周期对账兜底。
- 音频测试使用 fake `AudioOutputDeviceBackend` 或测试现场生成的短音频 fixture；不要依赖真实硬件、版权媒体或仓库媒体样本。
- `seriona_audio` 必须 PRIVATE 链接 `BS::thread_pool`，根 CMake 有 FATAL_ERROR 守卫；AVX2/FMA 参数仅允许施加于 `src/audio/waveform_simd_avx2.cpp`。

## 测试与工具
- 发现：`ctest --test-dir build -N`；全量：`ctest --test-dir build --output-on-failure`；聚焦：`ctest --test-dir build -R '<regex>' --output-on-failure`。
- 常用正则有 `seriona\.audio`、`seriona\.scanner`、`seriona\.metadata`、`seriona\.control`、`seriona\.logging`、`seriona\.runtime_paths`、`seriona\.application_logging`。
- doctest 测试二进制必须恰有一个 `main`；多数目标由 CMake 注入 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`，少数自带普通入口，禁止重复定义。
- cancellation 单独注册为 `seriona.playback_state_machine_cancellation`，普通状态机测试显式排除它；`seriona.audio.waveform.perf` 超时 240s，`seriona.control_artwork_resolver` 超时 60s，`seriona.scanner.wtr_integration` 超时 180s。
- `seriona_scanner_cache_tests`、`seriona_scanner_cache_content_tests`、v2→v3 migration、backup rollback、phase1 integration 目标在 `tests/CMakeLists.txt` 中禁用，不要假设可运行或已有迁移。
- `seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test` 只构建不注册 CTest，直接运行 `build/tests/<target>`；`seriona.audio_fixture`、`seriona.scanner.cache.perf` 已注册。
- `-DSERIONA_BUILD_TOOLS=ON` 才加入 `seriona_scanner_cold_perf`、`seriona_miniaudio_platform_probe` 和 `seriona_watch_root_move_audit`；只有 `seriona_miniaudio_platform_probe` 是 `EXCLUDE_FROM_ALL`（用 `cmake --build build --target seriona_miniaudio_platform_probe` 显式构建），`seriona_watch_root_move_audit` 随默认 `all` 目标构建、链接 `seriona_scanner` 且 include 路径伸入 `src/` 私有头 `file_scanner_service_internal.h`（审计“目录移出监视根”场景）。
- `SERIONA_SCANNER_SIMULATE_MISSING_SQLITE`、`SERIONA_SCANNER_SIMULATE_MISSING_XXHASH`、`SERIONA_METADATA_SIMULATE_MISSING_SDBUS` 会故意令配置失败；第三项仅在 `UNIX AND NOT APPLE` 分支生效，正常构建不要开启。
