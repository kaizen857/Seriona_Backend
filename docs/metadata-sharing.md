# 元数据共享模块说明

## 目标

这个模块负责把控制层快照发布到系统媒体协议层，并把系统媒体控制命令收回到统一控制命令通道。它只做元数据共享，不直接碰音频解码、扫描、队列或 UI。

当前状态是 Linux 优先可用且已验证，Windows 侧只保留编译隔离的官方 SMTC 代码形状，当前阶段不把 Windows 运行时行为写成已验证结论。

## 模块边界

- 对外统一入口是 `MetadataSharingService`。
- 平台选择通过 `MetadataBackendKind` 和 `MetadataSharingOptions` 完成，公共 API 不按操作系统改签名。
- `PlatformMediaState` 只承载控制层快照和 1 秒时间线更新间隔，不承载平台专有对象。
- `MetadataSyncResult` 只描述是否接受、是否变化、返回的状态和错误信息。
- 元数据共享层只消费 `PlayerStateSnapshot`，不直接依赖音频播放实现或扫描实现。

## 字段矩阵

下表是当前平台表面会收到的字段。未列出的内部字段不会发布到系统媒体载荷。

| 语义 | Linux MPRIS | Windows SMTC |
| --- | --- | --- |
| 曲目身份 | `mpris:trackid`，自定义对象路径 | 轨道元数据对象，不暴露 Linux 风格对象路径 |
| 文件地址 | `xesam:url`，本地路径会转成 `file://` URI | 只走 Windows 支持的本地文件或 URI 流程 |
| 标题 | `xesam:title` | `MusicProperties.Title` |
| 艺术家 | `xesam:artist` | `MusicProperties.Artist` |
| 专辑 | `xesam:album` | `MusicProperties.AlbumTitle` |
| 专辑艺术家 | `xesam:albumArtist` | 作为可支持的扩展字段处理 |
| 流派 | `xesam:genre` | 作为可支持的扩展字段处理 |
| 封面 | `mpris:artUrl` | `Thumbnail`，通过 `RandomAccessStreamReference` |
| 播放状态 | `PlaybackStatus` | 播放状态表面 |
| 进度 | `Position` | 时间线表面 |
| 时长 | `mpris:length` | 时长表面 |
| 循环 | `LoopStatus` | 作为扩展路径处理 |
| 随机 | `Shuffle` | 作为扩展路径处理 |
| 音量 | `Volume` | 当前阶段不承诺 Windows 音量支持 |
| 控制能力 | `CanPlay` 等能力标志 | 对应能力只在支持时暴露 |

## Linux MPRIS 映射

Linux 侧是当前唯一已验证运行时路径。实现使用 `sdbus-c++`，对外暴露 `org.mpris.MediaPlayer2` 和 `org.mpris.MediaPlayer2.Player`。

- `mpris:trackid` 由仓库自定义对象路径生成，空曲目才回退到 no-track sentinel。
- `xesam:title`、`xesam:artist`、`xesam:album`、`xesam:albumArtist`、`xesam:genre` 直接来自展示元数据。
- `mpris:length`、`Position`、`Duration`、`Buffered`、`SeekableFrom`、`SeekableTo` 使用微秒值。
- `mpris:artUrl` 来自本地封面地址，Linux 本地封面走 `file://` URI。
- `PlaybackStatus`、`LoopStatus`、`Shuffle`、`Volume`、`CanPlay`、`CanPause`、`CanStop`、`CanSeek`、`CanGoNext`、`CanGoPrevious`、`CanControl` 会映射到对应 MPRIS 属性。
- `Play`、`Pause`、`PlayPause`、`Stop`、`Next`、`Previous`、`Seek`、`SetPosition`、`Volume`、`LoopStatus`、`Shuffle` 都会桥接到统一控制命令。

## Windows SMTC 官方流程

Windows 侧当前只保留官方流程骨架，方便后续在真实桌面宿主里接上。

官方路径应当是 `ISystemMediaTransportControlsInterop::GetForWindow(...)` 获取控制实例，然后设置 `DisplayUpdater.Type = Music`，填充 `MusicProperties`，再通过 `Thumbnail = RandomAccessStreamReference` 和 `Update()` 发布。

- 本地封面应走 `StorageFile::GetFileFromPathAsync(absPath)`，再用 `CreateFromFile(file)`。
- URI 封面走 `CreateFromUri` 或等价的受支持 URI 流程，只接受应用和 `http(s)` 等受支持 scheme。
- 需要真实窗口句柄 `HWND`，没有宿主窗口时不应把 Windows 运行时行为写成已完成。
- 当前阶段没有 Windows 运行测试，只有编译隔离和结构说明。

## 1 秒时间线策略

- 时间线更新采用 1 秒节流。
- 播放中每过 1 秒更新一次时间线，不重复发送静态元数据。
- 暂停、停止、切歌、seek、恢复播放时，时间线可以立即刷新。
- 这个策略只影响 timeline dirty path，不影响静态元数据 dirty path。

## 静态元数据脏标记策略

静态元数据只在真正变化时发布，典型变化包括：

- 曲目身份变化
- 标题、艺术家、专辑、专辑艺术家、流派变化
- 封面变化
- 能力集合变化

纯进度变化不应触发静态元数据重发。这样可以避免每 1 秒把封面、标题和其他静态字段一起刷一遍。

## 内部字段与不发布字段

下面这些内容只在模块内部使用，不进入 Linux MPRIS 或 Windows SMTC 载荷：

- `version`
- `sampledAt`
- 播放错误详情，例如错误码和错误消息的内部摘要
- 输出格式信息
- 其他仅用于新鲜度判断、录制或调试的内部状态

## Linux 测试门禁

Linux 是当前验证主线，相关测试门禁如下：

1. `cmake --build build --target seriona_metadata_mpris_tests`
2. `ctest --test-dir build -R seriona.metadata --output-on-failure`

MPRIS 侧已有 fake bus / fake object 的测试边界，用来验证对象模型、命令桥接、封面 URI 和能力门控，而不是依赖真实桌面会话。

## 当前阶段结论

- Linux 侧已经是可验证的当前路径。
- Windows 侧当前只保证 API 形状和编译隔离。
- Windows 运行时行为、宿主窗口接入和实际 SMTC 发布，当前阶段都不写成已验证结果。
