# Seriona 后端前端媒体控制集成说明

本文只描述前端应该如何对接 Seriona 的媒体控制边界。前端的唯一集成面是 `seriona::control::MediaController` 和 `inc/seriona/control/control_contracts.h` 里的契约类型。

前端媒体控制目标的 CMake 消费契约是链接 `SerionaBackend::control`。只要前端使用 `MediaController` 或控制契约类型，就应该把自己的目标链接到这个 alias，不要直接链接或实例化音频、扫描、metadata 的内部服务目标。

## 1. 集成边界

前端只应该把媒体控制看成一个控制面和两个快照面。

1. 控制面，`MediaController::start()`，`shutdown()`，`submitCommand()`，`scanLibrary()`。
2. 状态面，`subscribePlayerState()`，`subscribeLibraryState()`，`subscribeDomainNotifications()`。
3. 读快照面，`playerStateSnapshot()`，`libraryStateSnapshot()`。

前端不应该直接碰这些后端内部模块，也不应该绕过 `MediaController` 去调用它们。

1. `AudioPlaybackService`
2. `FileScannerService`
3. `MetadataSharingService`
4. 这些服务的 facade 或 factory
5. `audioEventSink()`
6. `scannerEventSink()`

这些都是后端装配和内部编排细节，不是前端集成边界。

从当前仓库代码看，没有可见的 HTTP 或 IPC 协议层。以后如果要加协议适配层，应该只做很薄的映射，把外部协议翻译成 `MediaControlCommand`、`scanLibrary()`、以及各类快照和订阅回调，不要把协议逻辑下沉到音频、扫描或 metadata 模块。

## 2. 你能依赖的公开对象

`MediaController` 的公开 API 很直接。

1. `start()` 启动控制器。
2. `shutdown()` 停止控制器。
3. `submitCommand(const MediaControlCommand&)` 提交播放控制命令。
4. `scanLibrary(std::vector<scanner::ScannerRoot>, scanner::ScanMode)` 触发扫描。
5. `subscribePlayerState(...)` 订阅播放器状态。
6. `subscribeLibraryState(...)` 订阅库状态。
7. `subscribeDomainNotifications(...)` 订阅控制域通知。
8. `playerStateSnapshot()` 读取当前播放器快照。
9. `libraryStateSnapshot()` 读取当前库快照。

`MediaControllerDependencies` 里虽然能看到 `audio`，`scanner`，`metadata` 三个依赖，但这只是后端装配输入。前端不应该自己拼这些依赖，也不应该自己持有它们的实现对象。

## 3. 生命周期

正确顺序是先构建，再订阅，再启动，再扫描，再提交命令，最后停止。

1. 先创建好 `MediaController`。
2. 先注册三类订阅，播放器，库，通知。
3. 再调用 `start()`。
4. 再调用 `scanLibrary()` 发起库扫描。
5. 运行中通过订阅和快照刷新 UI。
6. 退出时先提交一次 `Stop`，再 `shutdown()`，再释放订阅句柄。

当前实现里，`start()` 会启动事件循环，并把 metadata 命令回调注册进去。`shutdown()` 会断开音频和扫描事件源，取消 metadata 命令订阅，停止 metadata，同步停掉事件循环。`submitCommand()` 在控制器未启动或已停止时会返回 `ControllerStopped`。

## 4. 订阅和快照

前端应当把订阅当作主刷新通道，把快照当作补偿通道。

1. `PlayerStateSnapshot` 用来描述播放状态，当前曲目，显示元数据，封面，进度，音量，静音，循环和随机状态，还有能力位。
2. `LibraryStateSnapshot` 用来描述库状态，扫描状态，`libraryTree`，扫描进度和最近错误。
3. `ControlDomainNotification` 用来发出事件性提示，比如扫描开始，扫描完成，播放结束，播放错误，命令被拒绝，输出模式回退。

`PlayerStateSnapshot` 里的 `freshness.version` 可以用来判断是不是新状态。`freshness.sampledAt` 记录了采样时间。`LibraryStateSnapshot` 也有自己的 `version`。

订阅句柄是 `SubscriptionHandle`，里面有 `subscriptionId` 和 `unsubscribe`。前端应当保存这个句柄，并在页面销毁或视图退出时主动调用 `unsubscribe()`。

### 4.1 `PlayerStateSnapshot` 字段

`PlayerStateSnapshot` 是播放页的主数据源。

| 字段 | 用途 |
|---|---|
| `freshness.version` | 播放器状态版本，前端可用它跳过重复渲染。 |
| `freshness.sampledAt` | 后端采样时间点。 |
| `currentTrack` | 当前选中或正在播放的 `TrackIdentity`。 |
| `display` | 标题、艺术家、专辑、专辑艺术家、流派。 |
| `artwork` | 本地封面路径、URI、内容哈希。 |
| `playback` | 播放状态和错误文本。 |
| `repeatMode` | `Off`、`One`、`All`。 |
| `shuffle` | 是否随机播放。 |
| `capabilities` | 当前能力位。reducer 初始化时这些能力目前都为 `true`。 |
| `timeline.position` | 当前曲目内进度。CUE 子曲目会折算成子曲目内位置。 |
| `timeline.duration` | 当前曲目时长；可能为空。 |
| `timeline.buffered` | 缓冲进度；当前契约预留，可为空。 |
| `timeline.seekableFrom` / `seekableTo` | 可 seek 范围；当前契约预留，可为空。 |
| `volume` | 线性音量，默认 `1.0F`。 |
| `muted` | 是否静音，默认 `false`。 |

### 4.2 `LibraryStateSnapshot` 字段

`LibraryStateSnapshot` 是音乐库页和播放列表树的主数据源。

| 字段 | 用途 |
|---|---|
| `version` | 库状态版本。 |
| `scanStatus` | `Idle`、`Scanning`、`Completed`、`Stopped`、`Error`。 |
| `libraryTree` | 当前库树快照；扫描出结果前可能为空。 |
| `scanProgress` | 扫描进度；扫描中或文件扫描事件里更新。 |
| `lastError` | 最近一次扫描错误。 |

### 4.3 通知与快照的关系

快照是当前事实，通知是事件提示。前端不要只依赖通知来维护完整状态；通知丢失或视图重建后，应以 `playerStateSnapshot()` 和 `libraryStateSnapshot()` 补齐当前状态。

## 5. 播放控制命令

控制命令由 `MediaControlCommand` 表达，`kind` 决定动作，其他字段只在对应动作里使用。

| `MediaControlCommandKind` | 必要字段 | 成功效果 | 参数缺失/非法时的结果 |
|---|---|---|---|
| `Play` | 无 | 播放已选曲目，或选择第一首可播放曲目并播放。 | 没有可播放曲目时返回 `NoPlayableTrack`。 |
| `Pause` | 无 | 进入暂停状态并下发暂停 intent。 | reducer 不额外校验参数。 |
| `Stop` | 无 | 停止播放并把位置归零。 | reducer 不额外校验参数。 |
| `TogglePlayPause` | 无 | 播放中变暂停，暂停中恢复，其它状态走播放。 | 没有可播放曲目时沿用播放逻辑的拒绝结果。 |
| `SeekTo` | `position` | seek 到绝对位置。 | 缺 `position` 返回 `InvalidCommand`。 |
| `SeekBy` | `delta` | 按相对位移 seek。 | 缺 `delta` 返回 `InvalidCommand`。 |
| `SetVolume` | `volume` | 设置线性音量，并夹到 `[0,1]`。 | 缺值或 NaN 返回 `InvalidCommand`。 |
| `SetMuted` | `muted` | 设置静音状态。 | 缺 `muted` 返回 `InvalidCommand`。 |
| `SetRepeatMode` | `repeatMode` | 设置循环模式。 | 缺 `repeatMode` 返回 `InvalidCommand`。 |
| `SetShuffle` | `shuffle` | 设置随机播放，并清空随机历史。 | 缺 `shuffle` 返回 `InvalidCommand`。 |
| `SkipNext` | 无 | 下一首、随机下一首、RepeatOne 重播或 RepeatAll 回绕。 | 无下一首且不能回绕时停播。 |
| `SkipPrevious` | 无 | 超过 5 秒先回到当前曲目开头，否则上一首或回绕。 | 无上一首且不能回绕时 seek 到 0。 |
| `SelectTrack` | `track` | 选择库中曲目并立即播放。 | 缺 `track` 返回 `InvalidCommand`；不在库中返回 `TrackNotInLibrary`。 |

### 5.1 播放，暂停，切换

`MediaControlCommandKind` 支持 `Play`，`Pause`，`Stop`，`TogglePlayPause`。

`ControlStateReducer` 的当前行为是，

1. `Play` 会在已有选中曲目时尽量从该曲目开始，没有选中曲目时会找第一首可播放曲目。
2. `Pause` 会进入暂停。
3. `Stop` 会停止并把播放位置回到零。
4. `TogglePlayPause` 会在播放和暂停之间切换。

`PlaybackStatus` 的状态值是 `Stopped`，`Playing`，`Paused`，`Loading`，`Seeking`，`Buffering`，`Error`。

### 5.2 上一首，下一首

`SkipNext` 和 `SkipPrevious` 都在控制契约里存在，也在 reducer 里有完整处理。

1. `SkipNext` 会优先考虑重复单曲和随机模式，再找下一首。
2. `SkipPrevious` 在播放位置大于 5 秒时更像是回到当前曲目开头。
3. `RepeatMode` 只有 `Off`，`One`，`All`。
4. `shuffle` 是单独的布尔值，不是 `RepeatMode` 的一部分。

播放自然结束时也会走同一套策略。`PlaybackEnded` 到来后，控制层会先发 `PlaybackEnded` 通知，然后在 `RepeatMode::One` 下重播当前曲目，在有下一首时自动选择下一首，否则停播。

### 5.3 定位进度

`SeekTo` 和 `SeekBy` 都支持，但参数不同。

1. `SeekTo` 使用 `position`，它是绝对位置。
2. `SeekBy` 使用 `delta`，它是相对位移。
3. 这两个命令都会把目标位置夹到当前曲目时长范围内。
4. 如果当前状态是播放或暂停，reducer 会在 seek 期间暂存可见状态，避免状态抖动。

前端发 seek 以后，不要等它立刻改变所有 UI，应该还是以订阅回调和快照为准。

### 5.4 音量和静音

`SetVolume` 使用 `volume`，类型是 `float`。

1. reducer 会把它夹在 `0.0F` 到 `1.0F`。
2. `SetMuted` 使用 `muted`，类型是 `bool`。
3. 终端消费者里，音量是按 `0.05F` 步进调节的，但那只是一个示例消费方式，不是协议要求。

### 5.5 循环和随机

`SetRepeatMode` 使用 `repeatMode`，类型是 `RepeatMode`。

`SetShuffle` 使用 `shuffle`，类型是 `bool`。

当前 reducer 在设置随机时会清空随机历史。`MediaControllerOptions` 里还有 `shuffleSeed` 和 `shuffleHistorySize`，这是后端行为配置，不是前端 UI 必填字段。

### 5.6 选择曲目

`SelectTrack` 使用 `track`，类型是 `TrackIdentity`。

`TrackIdentity` 由 `trackId`，`filePath`，`sourceId`，`libraryId` 组成。当前实现里，真正匹配时至少要有 `trackId`，`filePath` 为空时会按 `trackId` 继续匹配。

这意味着前端选曲时最稳妥的做法是直接保留控制层返回的 `TrackIdentity`，不要自己拼一个看起来像的对象。

## 6. 扫描和库树

扫描入口就是 `scanLibrary(std::vector<scanner::ScannerRoot>, scanner::ScanMode)`。

1. `ScannerRoot` 只有 `path` 和 `recursive`。
2. `ScanMode` 只有 `Incremental` 和 `Full`。
3. `scanLibrary()` 当前实现只是把请求交给扫描器，并立刻返回一个接受结果。
4. 扫描完成与否，不是靠这个返回值判断，而是靠订阅和快照判断。

`LibraryStateSnapshot` 里的 `libraryTree` 是前端看库和播放列表的唯一树形入口。它的类型是 `std::optional<scanner::PlaylistTreeSnapshot>`。

`PlaylistTreeSnapshot` 的形状很明确。

1. `version`
2. `generatedAt`
3. `nodes`
4. `rootNodeId`

每个 `PlaylistNode` 包含 `nodeId`，`parentNodeId`，`kind`，`displayName`，`song`，`childNodeIds`。节点类型是 `PlaylistNodeKind::Root`，`Directory`，`Album`，`Disc`，`Track`。

这也说明一件事，库和播放列表在当前契约里不是一个单独的编辑 API，而是同一棵库树快照。前端应当把它当成只读的树模型来展示和选择，不要期待有单独的 playlist 新建，重命名，移动，删除接口。

### 6.1 曲目节点和 `SongMetadata`

只有 `PlaylistNodeKind::Track` 且带有 `song` 的节点才是可播放曲目候选。`SongMetadata` 是前端展示库树和构造选曲请求时最重要的数据来源。

| 字段 | 用途 |
|---|---|
| `trackId` | 曲目的稳定控制 ID。 |
| `filePath` | 曲目节点路径；普通文件通常就是媒体路径。 |
| `title` / `artist` / `album` / `albumArtist` / `genre` | 展示元数据。 |
| `trackNumber` / `discNumber` / `year` | 可选排序/展示元数据。 |
| `sampleRate` / `bitDepth` / `channels` | 可选音频格式展示信息。 |
| `fileSizeBytes` / `fileMtime` | 可选文件信息。 |
| `contentHash` | 同音频字节跨路径去重用的稳定内容身份。 |
| `effectiveLyricsSource` / `effectiveLyrics` | 歌词来源和歌词行。 |
| `externalLyricsPath` / `externalLyricsHash` / `externalLyricsMtime` | 外部歌词文件信息。 |
| `sourceFilePath` | CUE 派生轨道的真实音频文件；普通文件时等于 `filePath`。 |
| `offset` / `duration` | CUE 子曲目在真实音频文件里的时间窗。 |
| `logicalTrackId` | 一源多轨时用于播放列表/UI 状态的稳定逻辑 ID。 |
| `artworkPath` | 可选封面导出路径。 |

控制层会从 `SongMetadata` 生成 `TrackIdentity`、`DisplayMetadata`、`ArtworkRef` 和底层播放请求。CUE 曲目播放时，底层音频读取使用 `sourceFilePath` 和 `offset/duration`；前端不需要自己把 CUE 时间窗换算给 audio 层，只需要继续通过 `MediaController` 选择曲目。

### 6.2 自动选择第一首曲目

当扫描事件带来 `PlaylistSnapshotUpdated`，且当前没有选中曲目、没有正在播放或加载时，reducer 会自动选择第一首可播放曲目，但不会立刻下发 `loadTrack()` 或 `play()`。这就是为什么扫描后 UI 可能在停止态看到第一首曲目的标题。

## 7. 通知

`ControlDomainNotificationKind` 目前有这些值。

1. `LibrarySnapshotUpdated`
2. `LibraryScanStarted`
3. `LibraryScanProgressUpdated`
4. `LibraryScanCompleted`
5. `LibraryScanStopped`
6. `LibraryScanError`
7. `PlaybackEnded`
8. `PlaybackError`
9. `OutputModeFallback`
10. `CommandRejected`

`ControlDomainNotification` 里还有 `errorCode`，`message`，`scanStatus`。

前端可以把它当成三种用途。

1. 给用户展示扫描和播放的异步事件。
2. 给用户展示命令拒绝原因。
3. 给用户展示输出模式回退这类平台级提示。

扫描事件到通知的大致映射如下。

| 扫描事件 | 库状态变化 | 通知 |
|---|---|---|
| `ScanStarted` | `scanStatus = Scanning`，清空 `lastError` | `LibraryScanStarted` |
| `ProgressUpdated` / `FileScanned` | 更新 `scanProgress` | `LibraryScanProgressUpdated` |
| `PlaylistSnapshotUpdated` | 更新 `libraryTree` 和版本 | `LibrarySnapshotUpdated` |
| `ScanCompleted` | `scanStatus = Completed` | `LibraryScanCompleted` |
| `ScanStopped` | `scanStatus = Stopped` | `LibraryScanStopped` |
| `ScanError` | `scanStatus = Error`，更新 `lastError` | `LibraryScanError`，`errorCode = BackendRejected` |

播放侧的错误和结束事件会分别映射到 `PlaybackError` 和 `PlaybackEnded`。输出模式回退会映射到 `OutputModeFallback`。

## 8. 错误处理

`MediaControllerCommandResult` 是命令提交结果，包含 `accepted`，`code`，`message`。

`MediaControllerErrorCode` 目前有这些值。

1. `None`
2. `ControllerStopped`
3. `NoPlayableTrack`
4. `TrackNotInLibrary`
5. `InvalidCommand`
6. `BackendRejected`

前端应该把它们当成用户可见状态，而不是异常路径。

1. `ControllerStopped`，控制器没在运行。
2. `NoPlayableTrack`，当前库里没有可播曲目。
3. `TrackNotInLibrary`，选中的曲目不在当前库里。
4. `InvalidCommand`，命令参数缺失或不合法。
5. `BackendRejected`，后端或底层事件报告了失败。

同时，`CommandRejected` 通知会把命令被拒绝这件事广播出去，所以前端最好把命令结果和通知一起看。

## 9. 终端消费者说明

现有 `app/terminal_controller.cpp` 是一个现成的消费样例，不是新的前端协议。

它的模式很清楚。

1. 先创建 `MediaController`。
2. 先订阅播放器，库，通知。
3. 再 `start()`。
4. 再 `scanLibrary()`。
5. 循环里根据用户按键拼 `MediaControlCommand`，然后 `submitCommand()`。
6. 退出时先提交 `Stop`，再 `shutdown()`。

这个样例还说明了一个重要限制，终端 UI 目前只暴露了这些动作。

1. 播放和暂停切换
2. 下一首
3. 上一首
4. 停止
5. 音量加减
6. 静音切换

它没有暴露 `SelectTrack`，`SetRepeatMode`，`SetShuffle`，`SeekTo`，`SeekBy`，虽然这些命令在控制契约里已经存在。

这不是控制层缺功能，而是当前终端消费端还没把这些动作映射出来。

## 10. 不支持和禁止路径

下面这些路径前端不要碰。

1. 不要直接调用 `AudioPlaybackService`。
2. 不要直接调用 `FileScannerService`。
3. 不要直接调用 `MetadataSharingService`。
4. 不要直接调用它们的 facade 或 factory。
5. 不要直接调用 `audioEventSink()`。
6. 不要直接调用 `scannerEventSink()`。
7. 不要把库编辑逻辑做成绕过 `MediaController` 的私有通道。

还有一个明确的缺口要记住。

1. 控制 API 里没有输出设备选择命令。
2. 也就是说，前端目前不能通过 `MediaController` 做设备切换。
3. 这和 `AudioPlaybackService` 里有 `selectOutputDevice()` 是两回事，那个接口属于音频后端，不是前端控制边界。

## 11. 波形生成是唯一的工具类例外

`inc/seriona/audio/waveform_generator.h` 暴露的 `buildAudioWaveform()` 是前端可以直接用的例外。

1. 它是纯计算型工具，不是媒体控制状态机。
2. 它不属于 `AudioPlaybackService`，也不属于 `MediaController` 的控制面。
3. 前端可以直接把它用在波形条绘制上。
4. 前端负责决定 `barCount`，`totalWidth`，`maxHeight`，以及时间窗口。
5. 后端负责把这些参数转换成波形高度数组。

它的公开形状是 `buildAudioWaveform(const std::string& filepath, int barCount, int totalWidth, int& barWidth, int maxHeight, std::int64_t startTimeUS = 0, std::int64_t endTimeUS = 0, const WaveformConfig& config = WaveformConfig{})`。

`WaveformConfig` 目前有 `dbFloor`，`dbCeiling`，`enableSIMD`，`threadCount`。

源码和测试都能确认这些行为。

1. `barCount <= 0` 时返回空向量，`barWidth` 置零。
2. 非法尺寸时返回等长零数组，`barWidth` 置零。
3. 归一化后的空时间窗会返回零数组。
4. 读不到音频文件，或者输入不是可读音频容器时，会抛出运行时错误。
5. 有效输入时会返回与 `barCount` 一致的高度数组，并把条宽算出来。
6. 高度计算是纯函数式的输入输出关系，不依赖播放状态。

所以，波形生成可以作为前端绘制工具直接使用，但它不是媒体控制通道的一部分。

直接工具 API 当前只白名单 `buildAudioWaveform()` 这一项。前端如果直接调用它，除了媒体控制目标链接 `SerionaBackend::control` 之外，还必须显式链接 `SerionaBackend::audio`；`SerionaBackend::control` 对音频库的后端内部依赖不是波形工具的传递消费契约。

## 12. 实际接线建议

如果前端和后端同进程，前端直接持有一个 `MediaController` 就够了。

如果前端未来要跨进程，那就加一个很薄的适配层，把外部请求翻译成以下三类动作。

1. 订阅快照。
2. 提交 `MediaControlCommand`。
3. 触发 `scanLibrary()`。

不要把适配层做成第二套业务逻辑。所有播放、扫描、选择、循环、随机、错误和通知语义，都应该继续由 `MediaController` 和控制契约定义。

## 13. 前端接入检查清单

接入前逐项确认。

1. UI 事件是否只被转换成 `MediaControlCommand` 或 `scanLibrary()` 调用。
2. 播放器 UI 是否只读 `PlayerStateSnapshot`。
3. 音乐库和播放列表 UI 是否只读 `LibraryStateSnapshot.libraryTree`。
4. 提示、toast、错误横幅是否来自 `MediaControllerCommandResult` 和 `ControlDomainNotification`。
5. 是否保存并释放了 `SubscriptionHandle`。
6. 是否没有直接持有或调用 `AudioPlaybackService`、`FileScannerService`、`MetadataSharingService`。
7. 是否没有直接调用 `audioEventSink()` 或 `scannerEventSink()`。
8. 是否把 waveform 作为独立计算工具使用，而不是当成播放控制通道。
9. 如果存在协议适配层，它是否只是薄映射，没有重新实现播放列表、随机、循环或错误策略。

## 14. 来源参考

1. `inc/seriona/control/control_contracts.h:20-28`，`PlaybackStatus`。
2. `inc/seriona/control/control_contracts.h:30-34`，`RepeatMode`。
3. `inc/seriona/control/control_contracts.h:89-114`，播放能力和 `PlayerStateSnapshot`。
4. `inc/seriona/control/control_contracts.h:124-130`，`LibraryStateSnapshot`。
5. `inc/seriona/control/control_contracts.h:132-165`，通知和命令结果。
6. `inc/seriona/control/control_contracts.h:167-204`，控制器选项、依赖和 `MediaControlCommand`。
7. `inc/seriona/control/control_contracts.h:206-223`，订阅回调和 `SubscriptionHandle`。
8. `inc/seriona/control/media_controller.h:11-45`，`MediaController` 公开 API。
9. `src/control/media_controller.cpp:47-89`，启动和停止。
10. `src/control/media_controller.cpp:92-135`，提交命令、扫描、订阅、快照和事件 sink。
11. `src/control/media_controller.cpp:249-321`，提交快照、发布通知、执行底层 intent。
12. `src/control/control_state_reducer.cpp:269-463`，各类控制命令处理。
13. `src/control/control_state_reducer.cpp:465-585`，音频事件处理。
14. `src/control/control_state_reducer.cpp:588-656`，扫描事件处理。
15. `src/control/control_state_reducer.cpp:659-858`，可播放曲目、上一首、下一首、随机播放和 seek 夹取。
16. `src/control/control_state_reducer.cpp:888-936`，扫描后自动选择首曲、选曲、停止播放。
17. `app/terminal_controller.cpp:109-152`，终端动作到控制命令的映射。
18. `app/terminal_controller.cpp:227-317`，现有消费者的订阅、启动、扫描、事件循环和退出流程。
19. `inc/seriona/audio/audio_contracts.h:64-187`，内部音频播放请求、事件和服务接口。
20. `inc/seriona/scanner/scanner_contracts.h:16-169`，扫描和播放列表树结构。
21. `inc/seriona/scanner/file_scanner_service.h:10-28`，扫描 facade 和 factory。
22. `inc/seriona/metadata/metadata_contracts.h:14-60`，metadata 状态和服务接口。
23. `inc/seriona/audio/waveform_generator.h:9-23`，波形生成公开 API。
24. `src/audio/waveform_generator.cpp:116-210`，波形生成公开函数行为、形状和时间窗归一化、策略选择。
25. `src/audio/waveform_generator.cpp:287-316`，波形高度映射。
26. `tests/audio/waveform_generator_tests.cpp:779-947`，波形公开行为验证。
