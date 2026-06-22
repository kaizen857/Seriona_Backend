---
slug: metadata-sharing-module-implementation
status: plan-written
intent: clear
pending-action: await user start-work or plan changes
approach: 先更新项目指令以允许 metadata 平台适配层使用 MPRIS/SMTC/sdbus-c++/WinRT；再落地纯 C++23、平台无关的元数据共享公共契约与服务门面；补齐 mediaController 所需的最小快照/命令桥接契约和测试替身；Linux MPRIS 必须作为真实 sdbus-c++ 适配落地并可测试，Windows SMTC 先编码并编译隔离，不让平台 API 泄漏到通用接口。
---

# Draft: metadata-sharing-module-implementation

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
- C1 | 平台无关元数据共享契约：PlayerStateSnapshot、MediaControlCommand、MetadataSharingService、订阅/命令 sink、错误/能力类型 | active | DESIGN.md:97, DESIGN.md:153, DESIGN.md:620
- C2 | 快照到平台模型映射核心：从控制层完整快照筛选/转换为 MPRIS/SMTC 实际支持字段；不把内部版本号、采样时间、输出格式、错误摘要等非协议字段直接传给 OS | active | DESIGN.md:121, DESIGN.md:622, DESIGN.md:624, web:MPRIS Player, web:Microsoft SMTC
- C3 | 服务门面与生命周期：支持 start/stop、snapshot update、命令回传、失败降级、Noop/Fake 后端测试缝 | active | DESIGN.md:101, DESIGN.md:113, DESIGN.md:115
- C4 | Linux MPRIS 适配边界：强制使用系统 `sdbus-c++` 依赖，通过私有实现文件包含 `<sdbus-c++/sdbus-c++.h>` 暴露 org.mpris.MediaPlayer2/Player；不能只停留在模型层 | active | DESIGN.md:119, DESIGN.md:624, web:MPRIS v2
- C5 | Windows SMTC 适配边界：保留接口和编译隔离；真实桌面适配需要宿主提供顶层 `HWND` 并通过 `ISystemMediaTransportControlsInterop::GetForWindow(...)` 获取 SMTC；首版非 Windows/无 WinRT/无 HWND 时不影响构建并退化为 Noop | active | DESIGN.md:120, DESIGN.md:626, web:Microsoft SMTC, web:ISystemMediaTransportControlsInterop
- C6 | 构建、测试与文档：新增 seriona_metadata 目标、metadata tests、app link-only 集成、中文开发文档与证据文件 | active | CMakeLists.txt:63, tests/CMakeLists.txt:1

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
- 首版实现范围 | 默认先做“可测试的元数据共享核心 + Noop/Fake 后端 + 平台适配接口 + 字段筛选矩阵”，但 Linux MPRIS 不是可选占位，必须以系统 `sdbus-c++` 真实适配落地；Windows SMTC 先做接口隔离和官方路径编码，不强制运行测试 | 当前仓库尚无 mediaController，直接接 MPRIS/SMTC 会把平台 API 绑到未稳定的控制层；DESIGN.md 要求平台 API 只在适配层、失败不影响核心播放链路；用户要求 Linux 侧必须具备实际 MPRIS 功能 | reversible
- mediaController 缺失处理 | 默认同步规划一个最小 control 契约头（不实现完整控制器），让 OS 代理只订阅快照并回传命令 | DESIGN.md 明确 OS 代理只能通过 mediaController 观察/控制，但仓库当前没有 mediaController 源码 | reversible
- 进度同步策略 | 播放期间系统媒体时间线按 1 秒节流更新；暂停、seek、恢复、切歌、停止等非连续状态变化时立即同步；歌曲名、歌手、专辑、封面、能力等非连续字段只在实际曲目或能力状态变化时更新 | 用户明确希望系统媒体控制器中的播放时间接近实时，不能接受 5 秒才更新；1 秒更新比 Microsoft Learn 的约 5 秒建议更激进但仍可控 | reversible
- MPRIS 时间单位 | 默认核心内部使用 chrono milliseconds，MPRIS 适配层转换为 microseconds | MPRIS Position/SetPosition 使用 microseconds | reversible
- 封面路径 | 默认核心模型保存本地 filesystem path 与可选 URI，平台适配层负责转换为目标平台支持的 URI 或流引用；Windows 本地文件不首选 `file://` | DESIGN.md 要求封面导出目录由后端统一管理，元数据共享只使用后端提供路径或 URI | reversible
- Windows 封面传输 | Windows 侧按 Microsoft 官方 SMTC 示例实现：使用 `SystemMediaTransportControlsDisplayUpdater`，设置 `Type = MediaPlaybackType::Music`、写入 `MusicProperties`，封面写入 `Thumbnail = RandomAccessStreamReference`，最后调用 `DisplayUpdater.Update()`；本地导出封面用 `StorageFile::GetFileFromPathAsync(absPath)` + `RandomAccessStreamReference::CreateFromFile(file)`，app/package/appdata/http(s) URI 才用官方示例的 `CreateFromUri(uri)` | 官方示例展示的是 `DisplayUpdater`/`MusicProperties`/`Thumbnail`/`Update()` 这条路径；`CreateFromUri` 有效 scheme 只列出 http/https/ms-appx/ms-appdata，普通本地文件应走 `CreateFromFile` 而不是规划 `file://` | reversible
- Windows 桌面入口 | Windows 真实 SMTC 适配默认要求宿主层传入顶层 `HWND`，通过 `ISystemMediaTransportControlsInterop::GetForWindow(HWND, riid, ...)` 获得控件；纯后端 CLI 无窗口时不规划强行创建 UI/窗口，默认退化 Noop 或等待前端宿主注入窗口句柄 | Microsoft 桌面 WinRT 支持文档明确 `SystemMediaTransportControls.GetForCurrentView()` 依赖 UWP view，桌面 app 应使用 `ISystemMediaTransportControlsInterop`；该接口要求传入调用进程所属顶层窗口 | reversible
- Windows timeline/repeat/shuffle 可用性 | Windows UWP/WinRT `SystemMediaTransportControls` class 支持 `UpdateTimelineProperties`、`AutoRepeatMode`、`ShuffleEnabled`、`PlaybackRate`；桌面 COM `ISystemMediaTransportControls` 基础文档只列基础按钮/状态/DisplayUpdater，计划必须要求实现前验证并使用 `ISystemMediaTransportControls2` 或 C++/WinRT 投影访问这些扩展成员，否则 Windows 首版只承诺基础按钮/状态/metadata/thumbnail | 官方桌面基础接口文档成员较少；公开 WinRT class 文档和 Windows SDK IDL/第三方 metadata 均显示扩展接口存在，但需要实现期原型验证 | reversible
- 平台优先级 | 当前阶段优先保证 Linux MPRIS 的可用性和正确性；Windows 侧可以开始编码保持接口与编译隔离，但暂不要求任何 Windows 侧测试，因为当前没有 Windows 开发/验证环境进入 | 用户明确指定当前阶段平台优先级；计划应避免被 Windows 测试阻塞，同时不允许破坏统一 API | reversible
- 顶层 API 一致性 | 顶层对外 API 必须完全一致，不能因为 Linux/Windows/Noop 后端不同而改变；平台差异只能隐藏在 factory、后端实现、能力查询或运行时降级里 | 用户明确要求；也符合 DESIGN.md 平台 API 不泄漏到通用接口的原则 | reversible
- Linux 任务验收 | 每个 Linux 端功能完成后必须由 agent 执行测试；测试通过才能进入下一个功能，测试不通过必须打回修复 bug 后重测；Windows 端不适用该测试约束 | 用户明确指定仅 Linux 端强制测试/打回；计划需把每个功能的 QA 命令和 evidence 路径写清楚 | reversible
- 功能级提交 | 每完成一个“功能”而不是每个计划任务都需要 commit；git 读取操作无限制，写入操作只允许 `git add` 和 `git commit`，禁止 reset/revert/checkout 等回退或其它写入式 git 操作 | 用户明确约束；计划和执行提示必须避免任何历史重写/回退工作流 | reversible
- 并行调度 | 可并行的功能可以并行委派编写；存在上下游依赖的功能必须按顺序执行；若并行功能产生 commit 文件冲突，需要在前向修复中解决冲突后再提交 | 用户明确约束；计划要标明 dependency matrix 和 commit grouping，避免并行任务抢同一文件 | reversible
- AGENTS 过时约束处理 | 执行计划的第 1 个任务必须更新 `AGENTS.md`：允许 metadata 平台适配实现文件使用 MPRIS/SMTC/sdbus-c++/WinRT，仍禁止泄漏到公共头、audio/scanner、Qt/QML/UI 和实时音频路径，并显式允许提交 `.omo/evidence/metadata-sharing-module-implementation/` 作为本计划证据 | 用户明确指出元数据共享模块必须具备这些依赖；当前 `AGENTS.md` 还限制 `.omo/` 交付引用，需要在执行开始时一并修正 | reversible
- Linux MPRIS 依赖策略 | Linux 侧强制要求真实 MPRIS 功能，配置阶段必须检测系统 `sdbus-c++`，私有实现使用旧项目兼容的 `<sdbus-c++/sdbus-c++.h>` include；缺依赖应明确配置失败，不能静默降级为仅模型/fake | 用户明确要求不能只编写模型且无法完成实际功能，并说明系统已有 sdbus-c++ | reversible
- Evidence 路径 | 每个任务证据文件改放 `.omo/evidence/metadata-sharing-module-implementation/task-<N>.md`，不再放 `build/`；执行 Todo 1 时同步更新 `AGENTS.md` 允许该 evidence 子目录作为本计划交付证据 | `build/` 被 `.gitignore` 忽略，而用户要求功能级提交包含证据；根 `AGENTS.md` 默认说 `.omo/` 不是项目源码，需要为本计划加例外 | reversible
- 内部快照字段外传 | 默认内部 `PlayerStateSnapshot` 可以包含 `当前曲目、文件路径、展示元数据、封面、播放状态、真实位置、总时长、版本号、采样时间、音量、静音、循环、随机、能力、输出格式与错误摘要`，但平台层只能传输目标系统支持的子集；其余字段只用于去重、节流、调试、错误降级或未来 UI/日志，不写入 MPRIS/SMTC | MPRIS/SMTC 官方文档均列出固定属性/元数据字段，没有通用任意状态包；直接外传完整内部快照会违反平台协议语义 | reversible
- 测试策略 | 默认 TDD：先写契约/映射/生命周期/命令桥测试，再实现；Linux 必须配置并编译真实 `sdbus-c++` MPRIS 适配，行为测试通过 fake D-Bus adapter seam 和固定 smoke CTest 执行，不要求 live 用户 D-Bus session；Windows 不要求 shell 媒体控件 | 可在当前 Linux/CI 环境稳定验证，不引入硬件/UI 依赖，同时保证 Linux MPRIS 不退化为仅模型 | reversible

## Findings (cited - path:lines)
- `DESIGN.md:97` 定义元数据共享模块职责：同步当前播放状态和媒体元数据给 OS。
- `DESIGN.md:101` 定义模块位置：它与 UI 同级，订阅 `mediaController` 状态，并把 OS 控制命令下传给 `mediaController`。
- `DESIGN.md:113` 到 `DESIGN.md:115` 限定平台 API 只出现在平台适配层，通用接口必须平台无关，系统交互失败不影响核心播放链路。
- `DESIGN.md:119` 到 `DESIGN.md:122` 指定 Linux MPRIS/D-Bus、Windows SMTC，以及首期同步歌名、歌手、专辑、封面、时长、当前播放时间。
- `DESIGN.md:153` 到 `DESIGN.md:162` 固定通信原则：所有跨模块通信经过 `mediaController`，OS 代理不能直接观察音频/扫描模块，日志/SQLite/系统媒体集成都不能绕过控制层改写业务状态。
- `DESIGN.md:189` 到 `DESIGN.md:195` 给出 `PlayerStateSnapshot` 至少字段：当前曲目、文件路径、展示元数据、封面、播放状态、真实位置、总时长、版本号、采样时间、音量、静音、循环、随机、能力、输出格式与错误摘要。
- `DESIGN.md:201` 到 `DESIGN.md:208` 规定真实进度来自音频模块，`mediaController` 只缓存/发布真实快照；系统媒体时间线可低频节流，Windows 建议播放期间约每 5 秒同步。
- `DESIGN.md:620` 到 `DESIGN.md:628` 给出元数据共享模块详细边界：OS 命令转内部控制命令；MPRIS 字段优先 `xesam:title`、`xesam:artist`、`xesam:album`、`mpris:artUrl`、`mpris:length`、`mpris:trackid`、`xesam:url`；Windows 目标 SMTC。
- `DESIGN.md:646` 到 `DESIGN.md:658` 规定事件/快照值语义、版本/时间戳去旧、上层订阅返回可注销句柄且初次订阅应立即获得当前快照。
- `inc/seriona/audio/audio_contracts.h:64` 到 `inc/seriona/audio/audio_contracts.h:75` 已有 `TrackPlaybackRequest`，可作为元数据共享当前曲目模型输入之一，但缺少 album/cover/能力/循环/随机等上层快照字段。
- `inc/seriona/audio/audio_contracts.h:100` 到 `inc/seriona/audio/audio_contracts.h:106` 已有 `PlaybackClockSnapshot`，可复用 position/version/sampledAt/continuous 语义。
- `inc/seriona/audio/audio_contracts.h:160` 到 `inc/seriona/audio/audio_contracts.h:168` 已有 audio-side `BackendEvent`/`BackendEventSink`，但它属于 audio namespace，不能直接等同未来 control/backend 通用事件契约。
- `inc/seriona/scanner/scanner_contracts.h:87` 到 `inc/seriona/scanner/scanner_contracts.h:113` 已有 `SongMetadata`，可为控制层快照提供 title/artist/album/albumArtist/genre/duration/filePath/sourceFilePath/lyrics 等元数据来源。
- `CMakeLists.txt:63` 到 `CMakeLists.txt:90` 显示现有业务模块按静态库目标组织；元数据共享模块应新增独立 `seriona_metadata` 静态库，而不是塞进 audio/scanner。
- `tests/CMakeLists.txt:421` 到 `tests/CMakeLists.txt:455` 显示测试以 doctest + CTest 单目标注册；元数据共享应新增 `seriona.metadata_*` 测试目标。
- MPRIS v2 官方规范要求 `/org/mpris/MediaPlayer2` 对象实现 `org.mpris.MediaPlayer2` 和 `org.mpris.MediaPlayer2.Player`，通过 `PropertiesChanged` 发布状态/元数据变化；Player 支持 `Next`、`Previous`、`Pause`、`PlayPause`、`Stop`、`Play`、`Seek`、`SetPosition`，`Position` 和 `SetPosition` 单位为 microseconds。
- MPRIS `org.mpris.MediaPlayer2.Player` 实际支持的状态字段是固定属性集：`PlaybackStatus`、`LoopStatus`、`Rate`、`Shuffle`、`Metadata`、`Volume`、`Position`、`MinimumRate`、`MaximumRate`、`CanGoNext`、`CanGoPrevious`、`CanPlay`、`CanPause`、`CanSeek`、`CanControl`；不包含内部 `version`、`sampledAt`、输出格式、错误摘要。
- MPRIS `Metadata` 是 `a{sv}` map，但规范只要求/建议媒体元数据语义：必有 `mpris:trackid`，可有 `mpris:length`、`mpris:artUrl`，其它应使用 Xesam 字段（例如 `xesam:title`、`xesam:artist`、`xesam:album`、`xesam:url`）；这不是传输任意播放器内部状态的通道。
- MPRIS `org.mpris.MediaPlayer2` 根接口还支持 `CanQuit`、`Fullscreen`、`CanSetFullscreen`、`CanRaise`、`HasTrackList`、`Identity`、`DesktopEntry`、`SupportedUriSchemes`、`SupportedMimeTypes`；这些是播放器/桌面集成属性，不承载曲目内部诊断字段。
- Microsoft Learn SMTC 文档要求更新播放状态、显示元数据和 timeline；timeline 需要 StartTime/EndTime/Position，若要 PositionChangeRequest 需设置 MinSeekTime/MaxSeekTime，并建议播放期间约每 5 秒同步一次、暂停/seek 等状态变化时再次同步。
- 用户偏好覆盖默认 SMTC 建议：系统媒体控制器播放时间必须按 1 秒节流更新；静态媒体元数据不做定时刷新，只在曲目/元数据实际变化时更新。
- Windows `SystemMediaTransportControls` 实际支持固定属性/事件：`PlaybackStatus`、`AutoRepeatMode`、`PlaybackRate`、`ShuffleEnabled`、`IsPlay/Pause/Next/Previous/Stop/...Enabled`、`ButtonPressed`、`PlaybackPositionChangeRequested` 等；不支持传输内部版本号、采样时间、输出格式或错误摘要。
- Windows 桌面 app 不能按 UWP 路径直接依赖 `SystemMediaTransportControls.GetForCurrentView()`；Microsoft 桌面 WinRT 支持文档把 `SystemMediaTransportControls` 列为需要替代机制的 `XxxForCurrentView` 类，并指定使用 `ISystemMediaTransportControlsInterop`。
- `ISystemMediaTransportControlsInterop::GetForWindow(...)` 要求 `appWindow` 是调用进程所属顶层窗口；这意味着本项目纯 C++ 后端/CLI 进程没有宿主窗口时，Windows 真实 SMTC 适配不能被无条件启用。
- Windows 桌面 COM `ISystemMediaTransportControls` 文档列出基础按钮能力、`PlaybackStatus`、`DisplayUpdater`、`ButtonPressed` 与 `PropertyChanged`；`UpdateTimelineProperties`、`AutoRepeatMode`、`ShuffleEnabled`、`PlaybackRate` 位于 WinRT class/扩展接口语义中，实现前必须用 Windows SDK 头/IDL 或 C++/WinRT 投影做原型验证。
- Windows `SystemMediaTransportControlsDisplayUpdater` 支持 `Type`、`Thumbnail`、`AppMediaId`、`MusicProperties`、`ImageProperties`、`VideoProperties`；音乐字段通过 `MusicDisplayProperties` 限定为 `Title`、`Artist`、`AlbumArtist`、`AlbumTitle`、`AlbumTrackCount`、`Genres`、`TrackNumber`。
- Windows `SystemMediaTransportControlsDisplayUpdater.Thumbnail` 的类型是 `Windows.Storage.Streams.RandomAccessStreamReference`；本地文件路径需要先取得 `IStorageFile`/`StorageFile` 再用 `RandomAccessStreamReference::CreateFromFile(...)`，而不是把路径字符串直接写给 SMTC。
- Windows `RandomAccessStreamReference::CreateFromUri(...)` 官方列出的有效 URI scheme 是 `http`、`https`、`ms-appx`、`ms-appdata`；官方文档未把普通本地 `file://` 列为有效 scheme。
- Windows `StorageFile::GetFileFromPathAsync(...)` 可从绝对本地 path 取得 `StorageFile`，但文档说明 path 不能是相对路径或 URI，且使用 `/` 也不接受；这说明 Windows 本地封面的受支持路径是“path -> StorageFile -> RandomAccessStreamReference::CreateFromFile”，不是“file:// URI -> CreateFromUri”。
- 用户要求 Windows 侧封面传输使用官方例子；因此最终计划应写成 Microsoft SMTC manual-control 示例等价流程：`DisplayUpdater.Type = Music`、设置 `MusicProperties`、设置 `Thumbnail`、调用 `Update()`，并把本地封面文件获取作为 `Thumbnail` 的输入准备步骤。
- Windows timeline 字段限定为 `StartTime`、`EndTime`、`Position`、`MinSeekTime`、`MaxSeekTime`；它用于系统时间线展示/seek 请求，不承载输出格式、错误摘要或内部采样时间。
- sdbus-c++ 文档显示 D-Bus 对象通过 connection、object path、vtable 方法/信号/属性注册，并可用 RAII Slot 管理动态注册生命周期；这适合把 MPRIS 注册放在 Linux 适配层 RAII 对象中。

## System field support matrix
- 内部 `当前曲目/trackId`: MPRIS 支持为 `mpris:trackid`（D-Bus object path）并用于 `SetPosition` 防 stale；该 path 必须是合法 D-Bus object path，且普通曲目 ID 不应使用 `/org/mpris...` 保留前缀；SMTC 可放入 `DisplayUpdater.AppMediaId` 或由适配层内部保存用于 stale 过滤。
- 内部 `文件路径`: MPRIS 可映射为 `xesam:url` 或 file URI；SMTC 可用于 `CopyFromFileAsync` 或缩略图/显示元数据来源，但不应暴露裸本地路径作为任意字段。
- 内部 `展示元数据`: MPRIS 可映射到 `xesam:title`、`xesam:artist`、`xesam:album`、以及其它 Xesam 音乐字段；SMTC MusicProperties 支持 Title/Artist/AlbumArtist/AlbumTitle/Genres/TrackNumber/AlbumTrackCount。
- 内部 `封面`: MPRIS 可映射为 `mpris:artUrl`，Linux 可使用旧项目同类 `file://` URL；SMTC 映射为 `DisplayUpdater.Thumbnail = RandomAccessStreamReference`，Windows 本地导出封面走 `GetFileFromPathAsync(absPath)` + `CreateFromFile(StorageFile)`，仅 app/package/appdata/http(s) URI 走 `CreateFromUri(Uri)`。
- 内部 `播放状态`: MPRIS 可映射为 `PlaybackStatus` 的 Playing/Paused/Stopped；SMTC 可映射为 `PlaybackStatus`。
- 内部 `真实位置`: MPRIS 可映射为 `Position` microseconds，seek 跳变可发 `Seeked`；SMTC 可映射为 timeline `Position`。
- 内部 `总时长`: MPRIS 可映射为 `mpris:length` microseconds；SMTC 可映射为 timeline `EndTime` 和 `MaxSeekTime`。
- 内部 `音量`: MPRIS 有 `Volume`；SMTC 文档中的 SystemMediaTransportControls 不提供通用 app 音量设置字段，只有系统 SoundLevel 只读，因此 Windows 首版不应承诺同步音量。
- 内部 `静音`: MPRIS 没有独立 mute 字段，可按项目策略映射为 Volume 0 或不传；SMTC 无独立 mute 字段，首版不外传。
- 内部 `循环`: MPRIS 支持 `LoopStatus`；SMTC 支持 `AutoRepeatMode`。
- 内部 `随机`: MPRIS 支持 `Shuffle`；SMTC 支持 `ShuffleEnabled`。
- 内部 `能力`: MPRIS 支持 `CanGoNext`、`CanGoPrevious`、`CanPlay`、`CanPause`、`CanSeek`、`CanControl`；SMTC 支持 `IsPlayEnabled`、`IsPauseEnabled`、`IsNextEnabled`、`IsPreviousEnabled`、`IsStopEnabled` 等按钮能力。
- 内部 `版本号`: MPRIS/SMTC 均不外传；只在适配层内部用于去旧、命令 stale 检查、去重。
- 内部 `采样时间`: MPRIS/SMTC 均不外传；只用于内部节流、真实位置 freshness 评估和日志/测试断言。
- 内部 `输出格式`: MPRIS/SMTC 均不外传；如需显示只能未来走 UI/日志/领域通知，不进入 OS metadata transport。
- 内部 `错误摘要`: MPRIS/SMTC 均无标准错误摘要字段；系统媒体集成失败只记录日志/领域通知，不写入 Metadata/MusicProperties。

## Decisions (with rationale)
- D1: 计划先新增 `inc/seriona/metadata/metadata_contracts.h`，定义平台无关数据/命令/服务接口；不把 MPRIS/SMTC 类型暴露到公共头。
- D2: 计划新增最小 `inc/seriona/control/control_contracts.h` 或等价头，仅承载 `PlayerStateSnapshot`、`PlaybackCapabilities`、`MediaControlCommand`、订阅句柄/回调类型；不实现完整 `mediaController`。
- D3: 计划新增 `seriona_metadata` 静态库，内部路径建议 `src/metadata/...`，测试路径 `tests/metadata/...`，文档路径 `docs/metadata-sharing.md`。
- D4: 计划把平台无关同步核心与平台后端拆开：核心负责去重、节流、字段筛选/映射、命令回传；后端接口只接收目标平台真实支持的 `PlatformMediaState`/等价 DTO，而不是完整 `PlayerStateSnapshot`。
- D5: 计划首版必须有 `NoopMetadataBackend` 和 `Recording/FakeMetadataBackend`，确保 Linux/Windows 平台 API 不可用时仍可构建和测试。
- D6: Linux MPRIS 真实适配必须以系统 `sdbus-c++` 依赖落地，配置阶段检测失败时给出清晰错误；自动化测试可使用 fake D-Bus adapter seam，但实现不能降级为仅模型或仅 Noop。
- D7: Windows SMTC 真实适配只在 Windows + WinRT 条件下编译；非 Windows 保留 stub/noop，并用接口测试覆盖命令映射和时间线节流。
- D8: app 首期只做 link-only 集成，不改 CLI 行为、不引入 UI、不启动系统媒体服务，除非后续 mediaController 存在并显式接管生命周期。
- D9: 计划新增明确的“不可传字段”测试：`version`、`sampledAt`、输出格式、错误摘要不得出现在 MPRIS metadata map 或 SMTC display/timeline DTO 中。
- D10: 计划把 timeline 与 metadata 更新拆成两条脏标记路径：`timelineDirty/positionTick` 每 1 秒触发一次位置同步，`metadataDirty/capabilitiesDirty` 只在曲目、封面、展示元数据或控制能力变化时触发。
- D11: Windows 封面适配必须是异步/可失败路径：封面文件不存在、无权限、格式不可读或 WinRT 打开失败时只清空/跳过 Thumbnail 并记录同步失败，不影响播放状态与时间线同步。
- D12: Windows 真实 SMTC 计划必须拆为两层：`WindowsSmtcHostHandle`/等价宿主入口接收 `HWND`，平台适配层通过 `ISystemMediaTransportControlsInterop::GetForWindow` 获取 SMTC；没有 `HWND` 时只启用 Noop，不在后端模块内创建 Qt/QML/Win32 UI。
- D13: Windows timeline、repeat、shuffle、playback-rate 在计划中标为“需原型验证的扩展能力”：若 `ISystemMediaTransportControls2`/C++/WinRT 投影不可用，则降级为只同步基础 playback status、按钮能力、metadata 和 thumbnail。
- D14: 正式计划必须按“功能”而不是“任务”定义 commit 边界；每个功能条目必须写明 commit message 建议、应 stage 的文件范围，以及禁止 stage 的文件范围。
- D15: 正式计划必须为每个 Linux 功能写明 agent-executed QA：最小相关 CTest、真实 `sdbus-c++` 编译/链接门禁、fake D-Bus/MPRIS 行为验证、失败打回规则和 `.omo/evidence/...` evidence 文件路径；Windows 功能只要求编译隔离/静态结构检查，不要求 Windows 运行测试。
- D16: 正式计划必须把并行性显式标注为 `parallel-safe` 或 `sequential-only`；凡会修改同一 CMake、公共契约或同一测试文件的功能默认 `sequential-only`，除非计划中指定冲突消解顺序。

## Scope IN
- 新增纯 C++23 元数据共享公共契约、控制层快照/命令最小契约、字段筛选矩阵、同步核心、Noop/Fake 后端、平台后端接口、构建目标、doctest 测试、中文文档和验证证据。
- Linux MPRIS 规划到强制实现波次：系统 `sdbus-c++` 依赖门禁、DBus 名称/对象/vtable/PropertiesChanged/命令回传/字段单位转换/生命周期异常降级；缺失 `sdbus-c++` 时 metadata 模块配置失败，不降级为 Noop。
- Windows SMTC 规划到编码隔离波次：宿主 `HWND` 注入、`ISystemMediaTransportControlsInterop::GetForWindow` 获取控件、播放状态、display metadata、thumbnail、基础按钮必须按官方路径编码并在 Linux 上证明编译隔离；Windows 运行测试暂不要求，timeline/seek/repeat/shuffle 作为扩展能力需先验证 `ISystemMediaTransportControls2` 或 C++/WinRT 投影可用。
- Linux MPRIS 作为当前阶段首要可用目标：正式计划必须优先完成 Linux 可运行/可验证路径，再推进 Windows 编码占位；Windows 不得改变顶层公共 API。
- 保证所有跨模块状态输入来自控制层快照，所有 OS 控制输出回到控制层命令 sink。
- 保证 OS 输出只包含 MPRIS/SMTC 实际支持字段；内部状态字段不得被塞进 metadata map、MusicProperties 或 timeline 作为私有扩展。
- 保证播放进度同步与静态元数据同步分离：1 秒位置更新不得重复推送歌曲名/封面等静态字段。
- 保证每个 Linux 功能通过 agent 测试后才能进入后续依赖功能；失败必须打回修复，不允许跳过。
- 保证功能级提交：每完成一个可独立回滚的功能就 `git add`/`git commit`，禁止使用除 `git add`、`git commit` 之外的 git 写入操作。
- 保证并行执行只发生在无依赖、无文件提交冲突的功能之间；有依赖或公共文件冲突的功能必须串行。

## Scope OUT (Must NOT have)
- 不实现完整 `mediaController` 播放队列、状态机或 UI/QML 适配。
- 不让元数据共享模块直接依赖或调用 audio/scanner 服务实例。
- 不在 audio 实时回调、扫描线程或平台 API 回调中同步执行业务状态修改。
- 不把 Qt/QML、MPRIS、SMTC、sdbus-c++、WinRT 类型暴露到平台无关公共契约。
- 不改变现有音频播放/扫描行为，不修改 TagReader/SQLite/audio 解码链路。
- 不要求 live 用户 D-Bus session 或 Windows shell 媒体控件才能通过默认测试；但 Linux 必须检测并编译真实 `sdbus-c++` MPRIS 适配，行为测试通过 fake D-Bus adapter seam 执行。
- 不把内部完整 `PlayerStateSnapshot` 当作外部协议载荷直接发布给 MPRIS/SMTC。
- 不在纯后端模块内为了 Windows SMTC 私自创建 UI、Qt/QML、隐藏窗口或消息循环；如需真实 Windows SMTC，由宿主应用显式提供顶层 `HWND` 和线程/消息泵上下文。
- 不因为 Windows 侧尚未测试而改变、拆分或弱化顶层公共 API；Windows 差异只能作为后端能力/运行时降级表达。
- 不在 Linux 功能测试失败时继续推进下一个依赖功能。
- 不使用 `git reset`、`git revert`、`git checkout`、`git restore`、`git clean`、rebase、amend 或任何非 `git add`/`git commit` 的 git 写入操作。

## Open questions
- None blocking. 用户已确认 Linux 必须落真实 `sdbus-c++` MPRIS 后端；若系统缺失 `sdbus-c++`，metadata 模块应配置失败而不是退化为仅模型/Noop。

## Approval gate
status: plan-written
pending-action: await user start-work or plan changes
approval-needed: 用户已要求根据草案编写完整代码编写任务；计划已写入 `.omo/plans/metadata-sharing-module-implementation.md`，等待用户执行或继续修改。
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
