# AGENTS.md

## 项目边界
- 当前目录就是仓库根目录；不要扫描上级目录找 Seriona 源码或配置。
- `seriona` 是纯 C++23 音乐播放器后端；不要引入 Qt/QML/UI，系统媒体集成也不得进入通用后端路径。
- `README.md` 只有标题；没有 CI、formatter、lint、pre-commit 或 repo-local OpenCode 配置，优先相信 CMake 与源码。
- 面向用户的回答和新增项目文档使用中文。
- `.omo/` 是 OpenCode 运行时/计划记录，不是项目源码；除非用户明确要求，不要修改或引用为交付内容。

## 入口与权威来源
- 目标清单在 `CMakeLists.txt`、`app/CMakeLists.txt`、`tests/CMakeLists.txt`；应用入口是 `app/main.cpp`，目标名 `seriona`。
- 公开头文件在 `inc/seriona/...`，实现主要在 `src/...`。
- 音频公共契约集中在 `inc/seriona/audio/audio_contracts.h`：对外门面是 `AudioPlayer`，服务接口是 `AudioPlaybackService`。
- 扫描公共契约在 `inc/seriona/scanner/scanner_contracts.h` 和 `inc/seriona/scanner/file_scanner_service.h`；`makeFileScannerService()` 当前创建 orchestrated scanner service。
- 元数据共享对外入口是 `MetadataSharingService`；平台细节只允许留在 `src/metadata/` 私有实现，不要泄漏到公共头、音频、扫描或实时路径。

## 构建与测试
- 配置：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`。
- 构建：`cmake --build build`。
- 全量测试：`ctest --test-dir build --output-on-failure`。
- 单个测试：`ctest --test-dir build -R <test-name> --output-on-failure`；测试名见 `tests/CMakeLists.txt` 的 `add_test(NAME seriona.* ...)`。
- 分组示例：scanner 用 `ctest --test-dir build -R 'seriona.scanner' --output-on-failure`，metadata 用 `ctest --test-dir build -R seriona.metadata --output-on-failure`。
- 可选工具默认不构建；需要 `seriona_miniaudio_platform_probe` 时重新配置 `-DSERIONA_BUILD_TOOLS=ON`。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg (`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`)、SQLite3、`libxxhash`；Linux 还需要 `sdbus-c++`。
- CMake 从相邻目录查找 `TagReaderCore`：先试 `../TagReader`，再试 `../../cppProject(app_and_lib)/TagReader`；两者都不存在会在配置期失败。

## 硬约束
- 音频模块只负责 FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和 `BackendEventSink` 上行事件。
- miniaudio 实时回调只允许读取预备好的 PCM、补零和更新原子计数；禁止 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁和设备生命周期调用。
- 音频测试默认使用 fake backend 或生成的短 fixture，不依赖真实音频硬件或版权媒体。
- scanner 公共头不得暴露 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型；默认测试使用 fake watcher。
