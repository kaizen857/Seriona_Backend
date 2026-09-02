# AGENTS.md

## 边界与事实来源
- 当前目录即仓库根；不要向上扫描 Seriona。`build/`、`build-*/`、`.omo/` 是生成或工作目录，不是源码。
- 本仓库后续所有更改必须且只能提交到 `develop` 分支；禁止直接在 `main` 分支提交。修改、暂存或提交前运行 `git status --short --branch` 确认当前分支为 `develop`。
- `main` 只作为稳定基线或同步来源；推送 `develop`、创建合并请求或合并回 `main` 必须由用户明确要求。
- 当前事实按根/子目录 `CMakeLists.txt`、源码、测试排序核对；它们高于完整项目文档 `README.md`（2026-08 public 准备时重写）与描述长期稳定设计的 `DESIGN.md`（后者的定位与维护规范见「DESIGN.md 维护规范」）。
- 仓库无 CI 和格式化配置；`CMakePresets.json` 提供 `release` 预设（输出 `build/release`，`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON` 与 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）。`.clangd` 与 VS Code clangd 固定读取 `build/` 编译数据库。
- 面向用户的回复、新增项目文档和提交信息使用中文。
- 这是独立 C++23 后端；生产代码不得引入 Qt/QML/UI，平台媒体集成留在 metadata 私有实现。
- 根目录跟踪文件非源码：`FILE_SCANNER_ANALYSIS.md`（旧项目历史分析，同工作区根 `docs/` 性质），只作背景材料；根目录 `docs/` 是已跟踪的历史分析/方案文档（watcher 修复方案、性能研究等），同样只作背景材料，不是事实来源。

## DESIGN.md 维护规范
- `DESIGN.md` 是描述项目长期稳定设计的架构文档，不是开发日志、变更记录或实现细节文档；应保持稳定，避免随开发逐渐演变为实现文档或变更日志。
- 任何开发任务在收尾前，必须将“是否需要更新 `DESIGN.md`”作为检查项之一（非可选步骤）：判定需要则必须在同一次任务内同步更新，判定不需要则不改动文档，也不要为了完成检查而做无意义修改。
- 判断是否更新只看一条标准：新开发者仅读当前 `DESIGN.md` 是否会错误理解项目现状。不依据“是否改了代码”或“是否动了架构名称”。
- 通常不应更新：Bug 修复、代码重构、性能优化、实现细节调整、内部接口调整、参数修改、不影响整体设计的小功能开发、代码风格调整、测试补充等。
- 通常应当更新：整体架构调整、模块新增或删除、模块职责变化、模块协作关系变化、启动流程变化、核心运行流程变化、配置体系变化、扩展机制变化、长期维护方式变化，以及其他会影响开发者理解项目整体设计的重要修改。

## 构建、运行与依赖
- 配置并构建：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N>`。
- **Windows 构建工具链固定为 MSVC（Visual Studio 2022 x64 + Visual Studio 生成器，依赖经 vcpkg 提供），是 Windows 下唯一受支持的构建工具；禁止使用 MinGW/msys2 等其它工具链构建或验证本仓库**（ABI 与 Windows SDK 链接语义不同，会产出 MSVC 下不存在的构建路径与测试结果，例如 WASAPI 枚举缺 `ksuser` 链接、symlink/文件时间等权限行为差异）。命令行工具 `build/seriona` 的交互终端模式仅支持 Unix 类系统（Windows 上恒报 `interactive terminal input is required`，属预期，验证走前端应用或测试二进制）。
- 运行：`build/seriona /path/to/music-root-or-file`；只接受一个已存在的文件或目录路径。
- 配置要求 CMake 3.20+、C++23、`pkg-config` 可解析 FFmpeg 的 `libavformat libavcodec libavutil libavfilter libswresample` 与 `libxxhash`，CMake 可找到 `spdlog`、`SQLite3`。
- 仅当 CMake 条件 `UNIX AND NOT APPLE` 成立时还要求 `pkg-config` 可解析 `sdbus-c++`；不要把该条件改写成“Linux”。
- TagReader 依赖：优先 `find_package(TagReaderCore CONFIG QUIET)`（CI 顺序链复用已安装产物）；未命中时依次使用 `-DSERIONA_TAGREADER_SOURCE_DIR=<path>`、相邻 `../TagReader`，否则 FetchContent 拉取 `https://github.com/kaizen857/TagReader.git` 的 `main`；vendored 的 TagReader 以 `EXCLUDE_FROM_ALL` 加入且其测试目标被剥离，ctest 里不会有 TagReader 测试。
- `SERIONA_INSTALL_EXPORT=ON`（默认 OFF，前端嵌入时不受影响）：独立构建时安装导出 5 库 + 3 头库 + BS_thread_pool，生成 `SerionaBackendConfig`（模板 `cmake/SerionaBackendConfig.cmake.in`，`find_dependency` 重建 FFMPEG/XXHASH/PIPEWIRE/SDBUS/SQLite3/spdlog/TagReaderCore），供 CI 顺序链 `find_package(SerionaBackend)` 复用。
- Release 优化三仓库统一：LTO 由 `release` preset 提供，GNU/Clang 下 `-march=x86-64` 基线（替代 `-march=native`，产物可跨机器分发）；警告级别经 `seriona_enable_warnings()`（GNU/Clang `-Wall -Wextra -Wpedantic`，MSVC `/W4 /permissive-`），全仓无 `-Werror`。
- `SERIONA_BUILD_APP`、`SERIONA_BUILD_TESTS`、`SERIONA_BUILD_TOOLS` 默认分别为 ON、ON、OFF；线程池 FetchContent 固定 `bshoshany/thread-pool` v4.1.0。

## 入口与模块边界
- 五个静态库是 `seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`，并导出别名 `SerionaBackend::{audio,scanner,metadata,control,app}`（前端 `Seriona/CMakeLists.txt` 通过别名链接）。可执行文件 `seriona` 直接编译 terminal、runtime_paths、logging 源，链接 `seriona_control` 而不链接 `seriona_app`，仅 Release 直接追加 `PkgConfig::SERIONA_FFMPEG`。
- `app/main.cpp` 做单参数/路径校验和顶层异常边界；`app/terminal_controller.cpp` 与 `terminal_io.{h,cpp}` 管终端生命周期、运行时路径、日志及生产控制器创建。
- `makeProductionMediaControllerDependencies(databasePath, coverExportDir)` 连接 miniaudio、scanner、生产 metadata；databasePath 非空时使用 SQLite 文件夹排序存储。
- `makeDefaultMediaControllerDependencies()` 使用 no-op 音频和文件夹排序存储，但仍调用真实 scanner/metadata 工厂。
- 前端应用设置的公共契约在 `inc/seriona/control/app_settings_store.h`：`AppSettingsStore`（`set`/`get`/`remove`/`listByGroup`，value 为不透明字符串，前端负责 QVariant ↔ 字符串编解码）+ `makeSQLiteAppSettingsStore` 工厂，编入 `seriona_control`，与 FolderSortSettingsStore 同库不同表、实现须线程安全。
- 对外契约以 `inc/seriona/` 为边界：audio 看 `audio_contracts.h`，scanner 稳定入口只看 `scanner_contracts.h`/`file_scanner_service.h`，metadata 看 `metadata_contracts.h`，跨模块编排走 control；`inc/` 中的 TagReader adapter、SQLite cache 等实现导向头不属稳定边界，新增稳定契约不得暴露 TagReader、SQLite、watcher、FFmpeg、MPRIS/sdbus 或 Windows 类型。
- 公共契约错误风格：typed enum（`MediaControllerErrorCode`、`ScannerErrorCode` 等）+ result struct（`MediaControllerCommandResult`、`ScannerTaskResult` 等），异常（`std::runtime_error`/`std::invalid_argument`）只从实现抛出；公共头不声明 `throw`、不用 `std::expected`。
- `src/logging/` 是无公共头的 `seriona::logging` 内部模块，编入 scanner/app/可执行文件及测试；公共入口是 `inc/seriona/app/application_logging.h` 的 `initializeApplicationLogging` 与 `setLogLevel`（运行时日志等级），入口公共 API 还包括 `runtime_paths.h`：提供 `RuntimePaths`（dataRoot/logFile/databasePath/artworkDir，`ensureDirectoriesExist()`）与 `resolveRuntimePaths` 双模式——编译定义 `SERIONA_INSTALLED_MODE` 时走 XDG（`resolveInstalledRuntimePaths`），否则便携模式（exeDir/SerionaData，`resolvePortableRuntimePaths` 在 Linux 经 `/proc/self/exe` 解析，否则回退传入的 executablePath（绝对路径时），再回退 `current_path()`）。
- 部分 `inc/seriona/` 头（`scan_scheduler.h`、`song_identity.h`）与 `src/audio/audio_player.cpp` 的实现只被测试目标直接编译，未进任何静态库：生产代码调用这些符号会在最终链接报 undefined reference；`AudioPlayer` 类本质是测试专用封装，生产走 `makeAudioPlaybackService`。

## 不可破坏的约束
- miniaudio 回调最终进入 `AudioOutputDevice::renderCallback()`；实时路径只能读 PCM 队列、补静音、应用音量/静音、更新原子计数，禁止 FFmpeg、事件回调、日志、动态分配、阻塞锁和设备生命周期操作。
- scanner 缓存实现为 `SQLiteCache`，schema 固定 v3：`user_version=0` 直接初始化 v3，任何非 0 且非 3 版本报 unsupported；不存在 v2 迁移桥。缓存另有事件驱动的路径级精确写 API `deleteLocationsByPathPrefix`/`replaceLocationsBySubtree`（`inc/seriona/scanner/cache/sqlite_cache.h`），`PlaylistTreeBuilder` 提供 `upsertSong`/`removeSubtree`/`renameSubtree`，服务依赖含 `reconcileInterval{60000}` 的 60s 周期对账兜底。
- 音频测试使用 fake `AudioOutputDeviceBackend` 或测试现场生成的短音频 fixture；不要依赖真实硬件、版权媒体或仓库媒体样本。
- `seriona_audio` 必须 PRIVATE 链接 `BS::thread_pool`，根 CMake 有 FATAL_ERROR 守卫；AVX2/FMA 参数仅允许施加于 `src/audio/waveform_simd_avx2.cpp`。
- 路径文本必须经 `src/scanner/path_utf8.h` 的 `pathToUtf8`/`pathFromUtf8` 往返（`src/audio/path_text.h` 同规则），禁止直接 `std::filesystem::path::string()/generic_string()` 进路径通道（Windows 按 ANSI 代码页转换，非 ASCII 路径抛异常或乱码；约束注释另见 sqlite_cache.cpp、logging.h、sqlite_folder_sort_settings_store.cpp）。

## 测试与工具
- 发现：`ctest --test-dir build -N`；全量：`ctest --test-dir build --output-on-failure`；聚焦：`ctest --test-dir build -R '<regex>' --output-on-failure`。
- 常用正则有 `seriona\.audio`、`seriona\.scanner`、`seriona\.metadata`、`seriona\.control`、`seriona\.logging`、`seriona\.runtime_paths`、`seriona\.application_logging`。
- doctest 测试二进制必须恰有一个 `main`；多数目标由 CMake 注入 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`，少数自带普通入口，禁止重复定义。
- cancellation 单独注册为 `seriona.playback_state_machine_cancellation`，普通状态机测试显式排除它；`seriona.audio.waveform.perf` 超时 240s，`seriona.control_artwork_resolver` 超时 60s，`seriona.scanner.wtr_integration` 超时 180s。
- `seriona_scanner_cache_tests`、`seriona_scanner_cache_content_tests`、v2→v3 migration、backup rollback、phase1 integration 目标在 `tests/CMakeLists.txt` 中禁用，不要假设可运行或已有迁移。
- `scanner_song_identity_tests.cpp` 无独立目标，song identity 用例经 `seriona.scanner.song_identity`（`seriona_scanner_hash_tests --test-case="scanner song identity*"`）运行；`seriona.scanner.cue_parsing` 与 `seriona.scanner.folder_thumbnail` 是各自独立目标。
- `seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test` 只构建不注册 CTest，直接运行 `build/tests/<target>`；`seriona.audio_fixture`、`seriona.scanner.cache.perf` 已注册。
- `-DSERIONA_BUILD_TOOLS=ON` 才加入 `seriona_scanner_cold_perf`、`seriona_miniaudio_platform_probe` 和 `seriona_watch_root_move_audit`；只有 `seriona_miniaudio_platform_probe` 是 `EXCLUDE_FROM_ALL`（用 `cmake --build build --target seriona_miniaudio_platform_probe` 显式构建），`seriona_watch_root_move_audit` 随默认 `all` 目标构建、链接 `seriona_scanner` 且 include 路径伸入 `src/` 私有头 `file_scanner_service_internal.h`（审计“目录移出监视根”场景）。
- `SERIONA_SCANNER_SIMULATE_MISSING_SQLITE`、`SERIONA_SCANNER_SIMULATE_MISSING_XXHASH`、`SERIONA_METADATA_SIMULATE_MISSING_SDBUS` 会故意令配置失败；第三项仅在 `UNIX AND NOT APPLE` 分支生效，正常构建不要开启。
