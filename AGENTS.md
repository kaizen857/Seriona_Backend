# AGENTS.md

## 仓库边界与权威来源
- 当前目录就是仓库根目录；不要向上扫描 Seriona 源码或配置。`build/`、`build-*/` 和 `.omo/` 是生成或工作目录，不是源码。
- 这是独立的 C++23 音乐播放器后端；不要引入 Qt/QML/UI。系统媒体集成只能留在 metadata 私有实现层。
- `README.md` 只有标题，`DESIGN.md` 同时包含目标和未来设计；当前事实以根 `CMakeLists.txt`、各子目录 `CMakeLists.txt` 和源码为准。
- 仓库没有 CI、格式化配置或 `CMakePresets.json`；`.clangd` 与 VS Code clangd 配置固定读取 `build/` 的编译数据库。
- 面向用户的回答和新增项目文档使用中文。

## 构建、运行与依赖
- 配置并构建：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N>`。
- 应用目标是 `seriona`，输出为 `build/seriona`；运行形式是 `build/seriona /path/to/music-root-or-file`，且只接受一个已存在的路径参数。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg（`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`）和 `libxxhash`，还需 CMake 可找到 `spdlog`、`SQLite3`；Linux 另需 `sdbus-c++`。
- `TagReaderCore` 优先取 `-DSERIONA_TAGREADER_SOURCE_DIR=<path>` 或相邻 `../TagReader`，否则 CMake 从 `https://github.com/kaizen857/TagReader.git` 的 `main` 分支拉取。线程池由 FetchContent 固定为 `bshoshany/thread-pool` v4.1.0。
- 可用 `SERIONA_BUILD_APP`、`SERIONA_BUILD_TESTS` 和 `SERIONA_BUILD_TOOLS` 控制目标；前三者默认分别为 ON、ON、OFF。

## 入口与模块边界
- `app/main.cpp` 负责参数/路径校验和顶层异常边界；终端生命周期、运行时路径及生产控制器创建在 `app/terminal_controller.cpp`。`src/control/media_controller_module.cpp` 的带数据库路径重载连接 miniaudio、scanner、metadata 和 SQLite 文件夹排序设置；无路径重载使用 no-op 排序存储。
- 静态库目标是 `seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`；`seriona` 当前只链接 `seriona_control`，未链接 `seriona_app`。公开 API 在 `inc/seriona/...`，实现主要在 `src/...`。
- 控制层契约在 `inc/seriona/control/`，负责跨模块编排；底层模块不要直接互相持有业务接口。
- 音频稳定入口是 `inc/seriona/audio/audio_contracts.h` 中的 `AudioPlayer` 和 `AudioPlaybackService`。
- scanner 稳定契约仅看 `inc/seriona/scanner/scanner_contracts.h` 与 `file_scanner_service.h`。TagReader、SQLite、watcher 和 FFmpeg 细节不得泄漏进这两个公共头；TagReader 适配实现位于 `src/scanner/tag_reader_metadata_adapter.cpp`。
- metadata 公共入口是 `inc/seriona/metadata/metadata_contracts.h` 的 `MetadataSharingService`。MPRIS/sdbus-c++ 与 Windows 平台类型留在 `src/metadata/` 私有实现；Linux 和 Windows 平台源文件由根 CMake 条件加入。

## 不能破坏的实现约束
- miniaudio 数据回调最终进入 `AudioOutputDevice::renderCallback()`。该实时路径只读取 PCM 队列、填充静音、应用音量/静音并更新原子计数；不要加入 FFmpeg、事件回调、日志、动态分配、阻塞锁或设备生命周期操作。
- 音频测试使用 fake `AudioOutputDeviceBackend` 或测试代码现场生成的短音频 fixture，不应依赖真实音频硬件或版权媒体。
- scanner 缓存实现类型是 `SQLiteCache`，当前 schema 为 v3：任何 `PRAGMA user_version=0` 的库都会直接按 v3 初始化，任何非 0 且非 v3 的版本都报 unsupported；迁移 v2、备份回滚及 phase1 集成目标仍在 `tests/CMakeLists.txt` 中禁用，不要假设迁移桥存在。
- `seriona_audio` 必须私有链接 `BS::thread_pool`；根 CMake 对此有配置期 FATAL_ERROR 检查。AVX2/FMA 编译参数只施加于 `src/audio/waveform_simd_avx2.cpp`。

## 测试与工具
- 全量测试：`ctest --test-dir build --output-on-failure`；先用 `ctest --test-dir build -N` 查看实际注册名。
- 聚焦测试：`ctest --test-dir build -R '<regex>' --output-on-failure`。稳定分组包括 `seriona\.audio`、`seriona\.scanner`、`seriona\.metadata`、`seriona\.control`、`seriona\.audio\.waveform` 和 `seriona\.scanner\.cache`。
- 测试多数使用 doctest；每个测试二进制必须恰好有一个 `main`。多数目标由 CMake 注入 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`，少数目标自带普通入口，添加测试时不要重复定义。
- `seriona.audio.waveform.perf` 的 CTest 超时为 240 秒。播放状态机的 cancellation case 被拆成 `seriona.playback_state_machine_cancellation`，普通目标显式排除它。
- `seriona_scanner_perf_test` 与 `seriona_scanner_detailed_perf_test` 只构建、不注册到 CTest；需直接运行 `build/tests/<target>`。`seriona.audio_fixture` 与 `seriona.scanner.cache.perf` 则是已注册的 CTest。
- 启用 `-DSERIONA_BUILD_TOOLS=ON` 后会构建 `seriona_scanner_cold_perf`；`seriona_miniaudio_platform_probe` 标记为 `EXCLUDE_FROM_ALL`，仍需 `cmake --build build --target seriona_miniaudio_platform_probe` 显式构建。
- 依赖错误路径测试可分别配置 `SERIONA_SCANNER_SIMULATE_MISSING_SQLITE`、`SERIONA_SCANNER_SIMULATE_MISSING_XXHASH`、`SERIONA_METADATA_SIMULATE_MISSING_SDBUS`（仅 Linux）；这些开关会故意让配置失败，正常构建不要启用。
