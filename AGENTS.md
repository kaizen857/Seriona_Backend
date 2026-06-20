# AGENTS.md

## 项目定位
- 当前目录就是仓库根目录；调查本项目不要扫描上级目录。
- `seriona` 是音乐播放器纯 C++ 后端；后端不得引入 Qt/QML 依赖，前端和系统媒体集成只应通过未来的 `mediaController` 边界交互。
- 面向用户的回答和新增项目文档使用中文。

## 代码与模块边界
- 公开头文件放在 `inc/seriona/...`，实现放在 `src/...`；当前已落地的业务代码集中在音频模块。
- 音频模块门面类型是 `AudioPlayer`，服务语义是 `AudioPlaybackService`，共享契约集中在 `inc/seriona/audio/audio_contracts.h`。
- 音频模块只负责 FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和上行播放事件；不要把 UI、QML、MPRIS/SMTC、SQLite 或媒体库扫描逻辑耦合进去。
- `docs/audio-player.md` 是当前音频模块约束的精简来源；`DESIGN.md` 仍含早期“空项目”文字，涉及现状时以 CMake 和源码为准。
- `.omo/` 是 OpenCode 运行时/计划记录，不作为项目源码；除非用户明确要求，不要修改或引用它作为可交付内容。

## 构建与测试
- 项目使用 CMake 3.20+、C++20、`pkg-config` 查找 FFmpeg 组件：`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`。
- 默认构建测试：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`，再运行 `cmake --build build`。
- 全量测试命令是 `ctest --test-dir build --output-on-failure`。
- 运行单个 CTest 用 `ctest --test-dir build -R <test-name> --output-on-failure`；测试名在 `tests/CMakeLists.txt` 的 `add_test(NAME seriona.* ...)` 中维护。
- 可选工具默认不构建；需要时配置 `-DSERIONA_BUILD_TOOLS=ON`，当前工具目标是 `seriona_miniaudio_platform_probe`。
- 目前没有仓库级 formatter、lint、clang-tidy 或预提交配置；不要声称存在未验证命令。

## 实时音频硬约束
- miniaudio 实时回调只允许读取预备好的 PCM、补零和更新原子计数。
- 实时回调中禁止调用 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁，以及设备 init/start/stop/uninit 等生命周期函数。
- 测试默认应使用 fake backend 或生成 fixture，不依赖真实音频硬件。
