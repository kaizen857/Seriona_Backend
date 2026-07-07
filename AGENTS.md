# AGENTS.md

## 项目边界
- 当前目录就是仓库根目录；不要扫描上级目录找 Seriona 源码或配置。
- 这是纯 C++23 音乐播放器后端；不要把 Qt/QML/UI 加进本仓库，系统媒体集成只能留在 metadata 私有实现层。
- `README.md` 只有标题；`DESIGN.md` 是架构背景，当前事实以 CMake 和源码为准。
- 未发现 repo-local CI、formatter、lint、pre-commit、lockfile 或 `CMakePresets.json`；不要按不存在的工具链猜命令。
- `.clangd` 指向 `build/` 编译数据库；`.vscode/settings.json` 配置了 clangd 参数（32 线程、clang-tidy、C++23）。
- `build/`、`build-default/`、`build-missing-tagreader/` 和 `.omo/` 都不是项目源码；检索时优先排除。
- 面向用户的回答和新增项目文档使用中文。

## 入口与权威来源
- 目标清单只看 `CMakeLists.txt`、`app/CMakeLists.txt`、`tests/CMakeLists.txt`、`tools/CMakeLists.txt`。
- 应用目标是 `seriona`（输出到 `${PROJECT_BINARY_DIR}/seriona`），编译 `app/main.cpp`、`app/terminal_controller.cpp`、`app/terminal_io.cpp`、`src/app/runtime_paths.cpp`、`src/logging/logging.cpp`，并链接 `seriona_control`。
- 核心静态库目标：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`；公开 API 在 `inc/seriona/...`，实现主要在 `src/...`。
- `app/main.cpp` 只校验一个路径参数；真实终端流程在 `app/terminal_controller.cpp`，调用 `makeProductionMediaController()` 装配 audio、scanner、metadata。
- 音频契约在 `inc/seriona/audio/audio_contracts.h`：对外门面 `AudioPlayer`，服务接口 `AudioPlaybackService`。
- 扫描契约在 `inc/seriona/scanner/scanner_contracts.h` 和 `inc/seriona/scanner/file_scanner_service.h`；`makeFileScannerService()` 走内部依赖装配。
- 控制契约在 `inc/seriona/control/control_contracts.h`，连接 audio、scanner 和 metadata；metadata 对外入口在 `inc/seriona/metadata/metadata_contracts.h` 的 `MetadataSharingService`。

## 构建与测试
- 配置：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`；测试默认已开启，但显式传参更稳。
- 构建：`cmake --build build`；可用 `-j<N>` 并行。
- 全量测试：`ctest --test-dir build --output-on-failure`。
- 单个或分组测试：`ctest --test-dir build -R <regex> --output-on-failure`；测试名都是 `seriona.*` 格式。
- 常用分组：
  - `-R 'seriona\.audio'`（音频所有测试）
  - `-R 'seriona\.scanner'`（扫描所有测试）
  - `-R 'seriona\.metadata'`（元数据所有测试）
  - `-R 'seriona\.control'`（控制所有测试）
  - `-R 'seriona\.audio\.waveform'`（波形生成测试）
  - `-R 'seriona\.scanner\.cache'`（缓存测试）
- 测试框架是 doctest；新增测试二进制必须正好提供一个 doctest main（多数目标在 CMake 里定义 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`，少数测试源码自行定义）。
- 性能测试在 `tests/scanner/scanner_perf_test.cpp` 和 `scanner_detailed_perf_test.cpp`；这两个目标不会被 `add_test` 自动注册到 CTest，需要直接运行二进制。
- 可选工具默认不构建；需要 `seriona_miniaudio_platform_probe` 时重新配置 `-DSERIONA_BUILD_TOOLS=ON`。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg (`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`)、`libxxhash`；`find_package` 需要 `spdlog` 和 `SQLite3`；Linux 还需要 `sdbus-c++`。
- CMake 从相邻目录查找 `TagReaderCore`：先试 `../TagReader`；若不存在则从 `https://github.com/kaizen857/TagReader.git` 自动拉取。也可显式传 `-DSERIONA_TAGREADER_SOURCE_DIR=<path>`。
- FetchContent 自动拉取 `bshoshany/thread-pool` v4.1.0，别名 `BS::thread_pool`。
- 第三方头文件在 `third_party/`：doctest、miniaudio、watcher（目录监听，来源 `third_party/watcher/include`）。

## 硬约束
- 音频模块边界：FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和 `BackendEventSink` 上行事件。
- miniaudio 数据回调走 `AudioOutputDevice::renderCallback()`；实时路径只读 PCM、应用音量/静音、更新原子计数，禁止 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁和设备生命周期调用。
- 音频测试默认用 fake `AudioOutputDeviceBackend` 或生成的短 fixture（fixture 生成目录在 CMake 里配置为 `${CMAKE_CURRENT_BINARY_DIR}/generated_waveform_fixtures`），不依赖真实音频硬件或版权媒体。
- `seriona_audio` 链接 `BS::thread_pool`（私有依赖，用于波形生成器内部线程池），若未正确链接会触发配置期 FATAL_ERROR。
- scanner 的稳定契约头是 `scanner_contracts.h` 和 `file_scanner_service.h`；不要把 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型泄漏进这两个契约头。
- scanner 内部 TagReader 适配在 `inc/seriona/scanner/tag_reader_metadata_adapter.h` / `src/scanner/tag_reader_metadata_adapter.cpp`，watcher/SQLite 细节留在 scanner 内部实现。
- scanner cache 当前实现是 `SQLiteCacheV3`：空库初始化为 schema v3，非 v3 schema 会报 unsupported；CMake 里 V2 迁移/备份回滚测试（`seriona_scanner_migration_v2_to_v3_tests`、`seriona_scanner_backup_rollback_tests`、`seriona_scanner_phase1_integration_tests`）和一个内容测试（`seriona_scanner_cache_v3_content_tests`）仍是注释状态，不要假设迁移桥已接入。
- metadata 平台细节留在 `src/metadata/`；不要把 MPRIS、sdbus-c++ 或 Windows 私有类型泄漏到公共契约、audio、scanner 或实时路径。
- metadata 在 Linux 编译时额外链接 `src/metadata/metadata_mpris_backend.cpp` 和 `metadata_mpris_linux.cpp`；Windows 编译时链接 `metadata_windows_private.cpp`。

## 依赖模拟开关（测试专用）
CMake 提供三个模拟依赖缺失的开关，用于验证配置期错误信息：
- `-DSERIONA_SCANNER_SIMULATE_MISSING_SQLITE=ON`：模拟 SQLite3 缺失
- `-DSERIONA_SCANNER_SIMULATE_MISSING_XXHASH=ON`：模拟 libxxhash 缺失
- `-DSERIONA_METADATA_SIMULATE_MISSING_SDBUS=ON`：模拟 sdbus-c++ 缺失（仅 Linux）

这些开关会让配置失败并输出安装提示（例如 `sudo pacman -S sqlite`）；正常构建不要传这些参数。

## 特殊测试场景
- `seriona_audio.waveform.perf` 设置了 240 秒超时（`TIMEOUT 240`），其余测试无显式超时。
- 部分测试按 test-case 名称进一步分组，例如 `seriona.playback_state_machine` 排除 `*cancellation*` 案例，单独用 `seriona.playback_state_machine_cancellation` 运行。
- 波形生成测试按策略拆分：contract、basic、ffmpeg、scalar、simd、id3v1、strategy_a、strategy_b、thread_pool、public、perf。
- scanner 测试覆盖：路径工具、哈希、树构建、调度器、worker pool（basic/process/batch/stats/config）、phase2 TSAN 压力、scan mode 决策、增量计划、增量 e2e、cache v3（basic/locations/auxiliary/schema）、TagReader 适配和并发、service、watcher、错误日志。
- metadata 测试覆盖：contract、mapper、service、service recording、MPRIS（含 smoke 测试）。
- control 测试覆盖：contract、media controller。

## AVX2 编译
`src/audio/waveform_simd_avx2.cpp` 使用特殊编译标志：
- MSVC：`/arch:AVX2`
- GCC/Clang：`-mavx2 -mfma`

其余源文件不强制 AVX2。
