
2026-06-17：T1 的第三方头文件采用系统安装的正式源文件快照：`/usr/include/doctest/doctest.h` 与 `/usr/include/miniaudio/miniaudio.h`，并复制到 `third_party/doctest/doctest.h` 与 `third_party/miniaudio/miniaudio.h`，以保持显式 include 路径契约。

2026-06-17：T2 的公共音频边界仅暴露值语义数据和服务接口，不引入 FFmpeg/miniaudio 原始句柄、Qt/QML、MPRIS、SMTC、SQLite 或 UI 类型；事件通道统一通过 `BackendEventSink = std::function<void(BackendEvent)>`。
