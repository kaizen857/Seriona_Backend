# AGENTS.md

## 项目边界
- 当前目录就是仓库根目录；不要扫描上级目录找 Seriona 源码或配置。
- `seriona` 是纯 C++23 音乐播放器后端；不要在后端引入 Qt/QML/UI/MPRIS/SMTC。
- 没有 README、CI、formatter、lint、pre-commit 或 OpenCode 配置可引用；以 CMake 文件和源码为准。
- 面向用户的回答和新增项目文档使用中文。
- 元数据共享模块的实现文件例外：仅 `src/metadata/` 下的平台适配实现文件可使用 MPRIS、SMTC、`sdbus-c++`、WinRT；这些标识不得泄漏到公共头文件、音频路径、扫描路径、实时音频路径或其他通用模块。
- `.omo/evidence/metadata-sharing-module-implementation/` 允许作为本计划的已提交执行证据目录；除此之外，`.omo/` 仍然不是项目源码。

## 入口与模块
- 目标清单在 `CMakeLists.txt`、`app/CMakeLists.txt`、`tests/CMakeLists.txt`；应用入口是 `app/main.cpp`，生成目标 `seriona`。
- 公开头文件在 `inc/seriona/...`，实现主要在 `src/...`。
- 音频公共契约集中在 `inc/seriona/audio/audio_contracts.h`：对外门面是 `AudioPlayer`，服务接口是 `AudioPlaybackService`。
- 音频实现只做 FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和 `BackendEventSink` 上行事件；细节见 `docs/audio-player.md`。
- 扫描公共契约在 `inc/seriona/scanner/scanner_contracts.h` 和 `inc/seriona/scanner/file_scanner_service.h`；`seriona_scanner` 已有路径、哈希、树、调度、SQLite 缓存等组件，但 `makeFileScannerService()` 仍返回 null service。
- `.omo/` 是 OpenCode 运行时/计划记录，不是项目源码；除非用户明确要求，不要修改或引用为交付内容。

## 构建与测试
- 配置：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`。
- 构建：`cmake --build build`。
- 全量测试：`ctest --test-dir build --output-on-failure`。
- 单个测试：`ctest --test-dir build -R <test-name> --output-on-failure`；测试名见 `tests/CMakeLists.txt` 的 `add_test(NAME seriona.* ...)`。
- 可选工具默认不构建；需要 `seriona_miniaudio_platform_probe` 时重新配置 `-DSERIONA_BUILD_TOOLS=ON`。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg (`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`)、SQLite3、`libxxhash`。
- CMake 通过绝对路径加入 `/home/kaizen857/cppProject(app_and_lib)/TagReader` 的 `TagReaderCore`；该目录或依赖缺失会在配置期失败。

## 实时音频硬约束
- miniaudio 实时回调只允许读取预备好的 PCM、补零和更新原子计数。
- 实时回调中禁止调用 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁，以及设备 init/start/stop/uninit 等生命周期函数。
- 测试默认使用 fake backend 或生成 fixture，不依赖真实音频硬件。
