# 音频播放模块实现说明

## 目的

本文档面向开发者，汇总 `AudioPlayer` 音频播放模块的实现边界、实时回调约束、依赖分工和已验证命令。内容只记录已经在设计文档与实现证据里确认过的事实，不把未验证的设想写成约定。

## 命名边界

1. 对外公开的门面类型是 `AudioPlayer`。
2. 内部实现和服务语义使用 `AudioPlaybackService`。
3. 上行事件统一通过 `BackendEventSink` 发送给控制层。

## 架构边界

1. 音频模块只负责解封装、解码、filter graph、输出设备和播放时钟。
2. `mediaController` 是唯一的跨模块编排点，音频模块不直接观察 UI、QML、MPRIS、SMTC 或 SQLite。
3. 后端是纯 C++ 设计，不引入 Qt 依赖，也不把 QML 作为音频模块的一部分。
4. 输出模式分为直出和混音，混音模式才要求无缝切歌。

## FFmpeg 约束

1. FFmpeg 负责解封装与解码，公共头通过 Pimpl 隐藏原始类型。
2. filter graph 的目标格式转换应放在 `aformat` 节点，不要依赖 `abuffersink` 后置设置目标采样格式。
3. 读取、解码、flush、seek、drain 都在普通服务路径处理，不进入实时回调。

## miniaudio 约束

1. `miniaudio` 只负责设备交互，不负责解码、重采样、声道转换或混音。
2. 实时回调路径只做 PCM 读取、补零和原子计数更新。
3. 测试默认使用 fake backend，不依赖真实音频硬件。

## SPSC 与 PCM 队列

1. 项目内使用固定容量 ring buffer 作为 SPSC PCM 队列，不引入额外并发队列依赖。
2. 队列写满时返回失败并累计 overflow 或 dropped 计数。
3. 队列读空时补零并累计 underrun 计数。
4. consumer 路径不分配内存，不持有阻塞锁。

## 回调禁止事项

实时回调里禁止做以下事情：

1. 调用 FFmpeg。
2. 调用 `BackendEventSink`。
3. 写日志。
4. 动态分配内存。
5. 使用阻塞锁。
6. 调用设备生命周期函数，例如 init、start、stop、uninit。

## 事件与状态

1. 播放事件使用值语义传递，避免把内部可变对象指针直接暴露到控制层。
2. `BufferUnderrun` 等事件在普通服务路径上报，不从实时回调直接发出。
3. 控制层以权威快照对外发布状态，UI 和系统媒体集成只订阅快照，不直接观察底层模块。

## 测试与素材约束

1. 默认测试使用生成的短音频 fixture。
2. fixture 不包含版权音乐，也不要求真实设备。
3. 平台专用硬件 harness 如有需要，属于 T15 的非默认内容，且依赖真实硬件。

## 已验证命令

1. `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`
2. `cmake --build build`
3. `ctest --test-dir build --output-on-failure`

## 设计依据

1. `DESIGN.md` 已确认音频模块应保持纯 C++，并通过 `mediaController` 做跨模块通信。
2. `.omo/plans/audio-player-module.md` 已确认 `AudioPlayer` 与 `AudioPlaybackService` 的分层命名。
3. `.omo/notepads/audio-player-module/` 中的 T5 到 T13 记录已确认 FFmpeg、miniaudio、SPSC 队列、回调守卫和事件边界。
