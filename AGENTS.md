# AGENTS.md

## 项目边界
- 当前目录就是仓库根目录；不要扫描上级目录找 Seriona 源码或配置。
- 这是纯 C++23 音乐播放器后端；不要加入 Qt/QML/UI，系统媒体集成只能留在 metadata 私有实现层。
- `README.md` 只有标题；未发现 CI、formatter、lint、pre-commit、lockfile、`CMakePresets.json` 或 repo-local OpenCode 配置，优先相信 CMake 和源码。
- 面向用户的回答和新增项目文档使用中文；`.omo/` 是 OpenCode 运行记录，不是项目源码。

## 入口与权威来源
- 目标清单只看 `CMakeLists.txt`、`app/CMakeLists.txt`、`tests/CMakeLists.txt`、`tools/CMakeLists.txt`。
- 应用目标是 `seriona`，编译 `app/main.cpp`、`app/terminal_controller.cpp`、`app/terminal_io.cpp`、`src/app/runtime_paths.cpp`、`src/logging/logging.cpp`，并链接 `seriona_control`。
- 核心静态库目标：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`；公开 API 在 `inc/seriona/...`，实现主要在 `src/...`。
- `app/main.cpp` 只校验一个路径参数；真实终端控制流程在 `app/terminal_controller.cpp`，通过 `makeProductionMediaController()` 装配 audio、scanner、metadata。
- 音频契约在 `inc/seriona/audio/audio_contracts.h`：对外门面 `AudioPlayer`，服务接口 `AudioPlaybackService`。
- 扫描契约在 `inc/seriona/scanner/scanner_contracts.h` 和 `inc/seriona/scanner/file_scanner_service.h`；`makeFileScannerService()` 走内部依赖装配。
- 控制契约在 `inc/seriona/control/control_contracts.h`，连接 audio、scanner 和 metadata；metadata 对外入口在 `inc/seriona/metadata/metadata_contracts.h` 的 `MetadataSharingService`。

## 构建与测试
- 配置：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`；测试默认已开启，但显式传参更稳。
- 构建：`cmake --build build`。
- 全量测试：`ctest --test-dir build --output-on-failure`。
- 单个或分组测试：`ctest --test-dir build -R <regex> --output-on-failure`；测试名都是 `seriona.*` 格式。
- 常用分组：`-R 'seriona.audio'`、`-R 'seriona.scanner'`、`-R 'seriona.metadata'`、`-R 'seriona.control'`。
- 测试框架是 doctest；所有测试二进制都定义 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`，不需要手动写 main。
- 可选工具默认不构建；需要 `seriona_miniaudio_platform_probe` 时重新配置 `-DSERIONA_BUILD_TOOLS=ON`。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg (`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`)、`libxxhash`；`find_package` 需要 `spdlog` 和 `SQLite3`；Linux 还需要 `sdbus-c++`。
- CMake 从相邻目录查找 `TagReaderCore`：先试 `../TagReader`，再试 `../../cppProject(app_and_lib)/TagReader`；两者都不存在会在配置期失败。

## 硬约束
- 音频模块边界：FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和 `BackendEventSink` 上行事件。
- miniaudio 数据回调走 `AudioOutputDevice::renderCallback()`；实时路径只读 PCM、应用音量/静音、更新原子计数，禁止 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁和设备生命周期调用。
- 音频测试默认用 fake `AudioOutputDeviceBackend` 或生成的短 fixture，不依赖真实音频硬件或版权媒体。
- scanner 的稳定契约头是 `scanner_contracts.h` 和 `file_scanner_service.h`；不要把 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型泄漏进这两个契约头。
- scanner 内部 TagReader 适配在 `inc/seriona/scanner/tag_reader_metadata_adapter.h` / `src/scanner/tag_reader_metadata_adapter.cpp`，watcher/SQLite 细节留在 scanner 内部实现。
- Scanner cache Phase 1 已有 `SQLiteCacheV3` 的 V2→V3 迁移桥：迁移成功后会保留 `<database>.bak` 备份，后续工作不要假设 scanner cache 只有 V2 状态，也不要提前把生产 `SQLiteScannerCache` 当成已切换到 V3。
- metadata 平台细节留在 `src/metadata/`；不要把 MPRIS、sdbus-c++ 或 Windows 私有类型泄漏到公共契约、audio、scanner 或实时路径。
