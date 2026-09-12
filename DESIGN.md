# Seriona 后端设计文档

> 本文档完全基于当前源码（根 `CMakeLists.txt`、`app/`、`src/`、`inc/`、`tests/`）重建，只描述源码可证明的现状。
> 事实优先级：CMake 配置与源码 > 测试注册 > 本文件；本文件与源码冲突时以源码为准。

## 1. 项目简介

Seriona 是一个独立的 C++23 音乐库后端：接收一个音乐根目录（或单个文件），扫描目录中的音频文件与 CUE 索引，构建播放列表树，并提供"命令→状态→事件"式的播放控制接口。当前仓库内唯一的前端是终端控制器（`app/`），它以单行状态栏的方式驱动后端；后端本身与 UI 解耦，通过订阅机制对外发布播放状态与曲库快照。

关键选型：

- 构建：CMake 3.27+，C++23，仅 CXX；`CMakePresets.json` 提供 `release` 预设（输出 `build/release`）；无 CI、无格式化配置。
- 音频解码：FFmpeg（`libavformat/libavcodec/libavutil/libavfilter/libswresample`）；输出：miniaudio（vendored 单头文件）。
- 标签读取：外部仓库 TagReader（`TagReaderCore`），经适配层接入。
- 元数据缓存：SQLite（固定 v3 schema）；哈希：libxxhash（XXH3/XXH64）。
- 平台媒体集成：Linux 上经 sdbus-c++ 发布 MPRIS 2.x 对象；Windows 仅有占位实现。
- 日志：spdlog（默认 logger `seriona`，滚动文件 5MB×3）；测试：doctest（vendored）。
- 并发：`bshoshany/thread-pool` v4.1.0（FetchContent 固定版本）。
- 文件监视：efsw（FetchContent 固定 commit，仅静态库；Linux inotify / Windows / macOS FSEvents 由上游提供）。

## 2. 整体架构

分层结构（依赖方向自上而下，`inc/seriona/` 是稳定契约边界）：

```
seriona（可执行文件，app/）
  └─ 直接编译 main / terminal_controller / terminal_io / runtime_paths / logging
      └─ seriona_control（编排枢纽：命令、状态归约、事件分发）
          ├─ PRIVATE 链接 seriona_audio   （播放管线 + 波形生成）
          ├─ PRIVATE 链接 seriona_scanner （扫描管线 + SQLite 缓存 + watcher）
          └─ PRIVATE 链接 seriona_metadata（平台媒体集成：MPRIS）
seriona_app（静态库，仅 application_logging/runtime_paths/logging，当前无内部消费者）
```

五个静态库：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`、`seriona_app`；别名目标 `SerionaBackend::{audio,scanner,metadata,control,app}`。

线程模型概览（源码可证明）：

| 线程 | 归属 | 职责 |
|---|---|---|
| 音频工作线程（单） | `seriona_audio` | 解码填充、状态机、seek、无缝交接、进度发布与频谱节流分析（全部串行化） |
| miniaudio 回调线程 | `seriona_audio` | 无锁读 PCM 环 + 增益/混音；EQ 激活时走 f32 中间域处理链（EQ→音量→限幅→量化）；频谱摘录零分配（实时约束，见 §11） |
| 扫描线程（单） | `seriona_scanner` | 串行执行扫描请求（容量 16 的队列） |
| 扫描 worker 池 | `seriona_scanner` | BS::thread_pool 并发读取元数据（TagReader 有信号量限流） |
| watcher 线程 | `seriona_scanner` | efsw 文件系统事件经适配层映射为内部事件（跨平台后端；根自身移出/删除由存活轮询兜底，事件队列溢出经 missed 消息上报）→ 完整事件入队 → 50ms 去抖归并 → 分类器精准增量更新（目录移入/移出、CUE/歌词/封面走精确或 scoped 对账；无法精准定位/事件丢失/周期探测走 Reconcile，永不回落全量重扫）；60s 周期对账兜底 |
| 控制事件循环（单） | `seriona_control` | 所有命令归约与后端事件处理（串行化点） |
| 订阅投递线程（每订阅类型一个） | `seriona_control` | 快照拷贝后异步回调订阅者 |
| 封面解析线程 | `seriona_control` | TagReader 封面提取（有界 latest-wins 队列） |
| metadata worker（单） | `seriona_metadata` | 播放状态异步转发到平台后端 |

## 3. 项目目录

```
app/                 可执行文件 seriona：main、terminal_controller、terminal_io
inc/seriona/         稳定公共契约（按模块分组；实现导向头不属稳定边界）
src/audio/           播放核心（service 与状态机、device/buffer/clock/events 子目录）、
                     ffmpeg 解码与滤镜、waveform 波形生成（scalar/simd/avx2/strategy_a/b）
src/scanner/         扫描服务/编排器/worker 池、cache/（SQLite v3）、哈希与身份、
                     路径分类、播放列表树、文件夹缩略图解析、TagReader 适配
src/metadata/        平台元数据分享（backend 抽象、MPRIS Linux 实现、Windows 占位）
src/control/         媒体控制器、事件循环、状态归约器、播放上下文构建、封面解析、订阅存储、
                     设置存储（SQLite 文件夹排序/应用设置）
src/app/             应用日志与运行时路径（亦编入 seriona_app）
src/logging/         内部日志模块（无公共头，编入式复用）
src/thumbnail/       缩略图服务（Qt/QImage，未接入任何构建目标，生产禁用）
third_party/         vendored：doctest、miniaudio（efsw 经 FetchContent 固定 commit，非 vendored）
tests/               doctest 测试（70+ 目标，见 §9）
tools/               SERIONA_BUILD_TOOLS=ON 才构建：scanner_cold_perf、miniaudio_platform_probe、
                     watch_root_move_audit
docs/、*.md          项目演进记录文档，非事实来源
```

## 4. 模块说明

### 4.1 seriona_audio（播放与波形）

- `AudioPlaybackService`（接口，`audio_contracts.h`）+ 唯一实现 `SingleTrackAudioPlaybackService`：13 个异步控制方法 + 1 个同步 `queryPlaybackClock`（合计 14，勿与测试专用 `AudioPlayer` 的 13 个方法混淆）；所有操作入命令队列由单音频工作线程执行。
- 播放状态机 `PlaybackStateMachine`：Idle → Loading → Ready → Playing ⇄ Paused，另有瞬时 Draining、Stopped、Error；每次迁移发 `PlaybackStateChanged`。seek 为 begin/cancel/complete 三阶段，带 generation 防过期完成。
- `AudioOutputDevice` + 后端接口 `AudioOutputDeviceBackend`：生产后端为 `MiniaudioOutputDeviceBackend`（`MINIAUDIO_IMPLEMENTATION` 仅在该 TU 实例化）；回调经 `renderCallback` 只做无锁读队、补静音、增益、原子计数。输出格式协商（`AudioSampleFormat`，含 `Int24`）与设备枚举/选择：`enumeratePlaybackDevices` 上报设备能力（nativeDataFormats 提取）与 `isDefaultDevice`（miniaudio `isDefault` 透传），`deviceId` 为按 `context.backend` 编码的稳定文本 id（`miniaudio_device_id_encoding.h`，可持久化跨枚举复用；历史"枚举索引字符串"值在 `resolvePreferredDevice` 按纯数字兼容回退），`AudioOutputConfig.preferredDeviceId` 经 `resolvePreferredDevice` 精确匹配绑定对应设备，空串 = 系统默认，选错格式自动回退并通知。
- `PcmBufferQueue`：无锁 SPSC 字节环 + generation 失效机制（seek 防竞态）；`PlaybackClock`：帧计数驱动（非墙钟）。
- `AudioEventDispatcher`：锁内取 sink 副本、锁外回调；`BackendEvent` 信封带 monotonicVersion/timestamp；事件面含过渡域 `EndApproaching{remainingMs}`（终点预告）、`AdvanceCompleted{trackId}`（交接提交）与频谱显示域 `SpectrumUpdated{snapshot}`（R2：频谱快照周期外发）等载荷（追加于 `BackendEventType` 枚举末尾，见下）。
- 播放过渡域（淡入淡出/交叉/预加载，整体自任务组 B1-B4 落地）：
  - 过渡契约：`TransitionConfig`（`audio_contracts.h`，9 项，字段声明顺序即跨端契约）描述自动前进淡变（Off / 除 CUE 无间隙组外 / 全交叉三档）、手动切歌淡变（Off / 短时 dip / 全交叉三档）、传送与 seek 淡变、无间隙预解码提前量。默认构造即裁定默认，全部 9 项默认下采样路径与过渡引擎引入前基线逐位一致（仓库内 17 键哈希回归总闸持续锁定，Direct 恒重开+硬切、Mixed 无交叉的旧行为不变）。控制面经 `SetTransitionConfig` 命令（§4.4）到达 `AudioPlaybackService::configureTransition`，与 `ConfigureOutput` 语义隔离：只更新配置，不触发 LoadTrack、设备重开或任何事件。
  - 增益包络引擎（过渡的物理执行面）：`GainEnvelopeController`（`src/audio/transition/`，音频 worker 侧账本）产出轨迹快照（Linear / EqualPowerPair 两族，version 单调递增），经 `AudioOutputDevice::setMasterEnvelope`/`setSourceEnvelope` 发布。master 包络承载传送层淡变（pause/stop/seek/play），`sourceEnvelopes[2]` 承载源级淡变（槽 0 主源、槽 1 交叉第二源）。`renderCallback` 在块首按版本受理、逐帧推进增益：整数格式在加宽样本域做增益数学，S24 解包为左对齐 S32 后与 Int32 路径同构再打包；静音/零音量块照常推进包络，音量/静音与包络正交（回调内相乘）。`resetEnvelopes`/`resetSourceEnvelope` 复位层账本。
  - 传送收尾（finishing）语义：pause/stop 命令到达即翻转逻辑态并发布状态事件（Paused/Stopped），物理淡出由音频 worker 监督（`FinishingAction`：PauseFreeze / StopCleanup / SeekDipDown / ManualDipDown）；淡出期间解码继续喂队列，包络归零后设备才停止，冷启动/恢复经先即时落零再 0→1 的淡入握手，避免切尾爆音与残余出声。暂停淡出（PauseFreeze）期间到达的 seek 只更新冻结目标、收尾后补做物理 seek；停止收尾（StopCleanup）中 seek 为非法（照发 SeekFailed、收尾监督保留）。
  - Mixed 同目标切歌免重开：输出协商短路。请求曲目标格式与设备已协商参数一致时设备保持 initialized，`rebindQueue` 原子换代原地换队列（旧环退役，下次停设备时析构）；参数变化或 Direct 才拆卸重开。`OutputFormatChanged` 仅在实变时发布。
  - 自动前进事件链（EndApproaching/PrepareNext/AdvanceCompleted）：Mixed 下自动前进侧启用预解码提前量或交叉档位时，曲目进入终点阈值窗由服务发一次性 `EndApproaching{remainingMs}`（armed 去重；Direct 与默认全关路径零发射、自然结束仍走 PlaybackEnded）。控制层按档位/RepeatOne/无间隙组语义决策选曲并定 `PrepareNextKind`（SeamlessDirect / Crossfade，`PrepareNextMeta` 带无间隙组标记），经 `prepareNext` 两参重载下发；服务把下一曲预解码入槽，自然终点按就绪槽直切（handoff）或双源交叉重叠提升。交接完成先发 `AdvanceCompleted{trackId}` 再发新曲 TrackChanged/状态事件，控制层据 pendingAdvance 账本提交（不重发 LoadTrack）；窗口内被判定失效的命令（选曲/seek/stop/重配过渡等）先经 `abortTransition` 中止（弃槽、第二源去活、预告重新武装）再执行本体。
  - 双源交叉重叠面：第二源 ring 由 `AudioOutputDevice::activateSecondSource` 发布（指针+代次+源包络），回调按代次双读、在加宽样本域混音（等功率对 g0²+g1²≈1），代次失配整块补零，杜绝陈旧混音；主源排空按 drain-promote 语义即时完成交接，无静音缺口。
  - 手动切歌三档（Mixed 且播放中生效；Direct 恒瞬时硬切并重开设备，档位与预解码不生效）：Off 为现状瞬时体；ShortDip 先把目标解码（复用匹配预载槽或临时解码，失败走错误路径），master 对半 dip 归零后原地采纳（rebind + 状态机 Loading/TrackChanged/Ready/Playing 背靠背，无位置跳变、设备不重开）；FullCrossfade 在预载槽匹配且就绪时走真重叠（腿长取交叉长度与剩余时长的较小值，等功率换腿，提升或排空提升后采纳且不发射 AdvanceCompleted，selectTrack 批尾 Play 在过渡在途被吞掉），未就绪则临时解码降级 dip、时长不足则硬切，永不等待。播放中 seek 在 fadeOnSeek 下走同类 dip（归零点集中发 Loading/PositionDiscontinuity/Playing）；暂停/就绪态 seek 保持瞬时。
  - 早期设计稿（6.1.5/6.1.8/6.1.11）承诺的「混音模式必须支持无缝切歌」「切歌时不应因下一首源格式不同而重开设备」「同一输出格式的无缝切歌衔接处没有明显静音、爆音或设备重开」现均已落地：无缝直切与交叉重叠见上，免重开为协商短路，无爆音由等功率交叉腿、代次补零与归零收尾保证，并由默认值等价回归与帧级断言套件持续锁定。Direct 模式按承诺不承诺无缝（每次切歌重开设备、瞬时硬切）。
- 均衡器/频谱域（音频侧处理面与频谱通道已落地；控制面见 §4.4）：`EqualizerConfig`（总开关、`EqualizerBandMode{Band10,Band31}`、前置增益、31 段 `bandGainsDb`、限幅器开关）、`EqualizerStateSnapshot`（generation/config/sampleRate/181 点 `curvePointsDb`+`curveFrequenciesHz`，对数频率轴 20–20k）、`SpectrumSnapshot`（generation/sampleRate/120 段 `binsDb`/timestampMs）为 `audio_contracts.h` 纯 C++23 值类型；`AudioPlaybackService` 虚表尾部追加非纯虚 `setEqualizer`/`equalizerState`/`setSpectrumEnabled`/`spectrumEnabled`（默认空实现/默认关，同 `configureTransition` 先例——4 个实现者免逐个覆写）。频段中心频率与 Q 常量单点定义于 `inc/seriona/audio/equalizer_tables.h`；生效快照（generation 单调递增 + RBJ peaking 增益曲线）由控制层 reducer 纯函数先行产生（控制层无输出采样率知识——快照 sampleRate 未回填、曲线按全轴计算、越界守卫不参与）。音频侧处理面与接线现状：
  - 实时回调链两态：EQ 关闭 = 既有输出域快速路径，逐位直通保留（Direct 逐曲重开与 EQ 关的出厂行为同处理链引入前基线一致，render 级逐位回归锁定）；EQ 激活 = f32 中间域链——读环 → int 设备逐样本转 f32（f32 设备就地）→ 双源包络增益/混音（master×volume 推迟）→ EQ 级联（单实例逐 band 滤波、含 preGain；增益爬坡按块小步平滑，时间常数 ~20ms 量级，低采样率按块长钳制防大步进）→ 音量/包络（master×volume）→ 限幅器 → 末端单次量化写回输出域。补零欠载尾不喂链；muted/零音量与纯静音块跳过链（平滑与滤波/延迟线原位冻结，恢复出声续跑）。
  - 配置投递与应用语义：eqConfig_ 存储恒执行（兼容/回读面）且 PENDING 目标层无条件发布（version 递增）——停态窗口（设备未启动/无活跃回调）配置入口即时真实应用（configure 允许分配，含生效快照发布并把目标受理账本消费到当前 version）；运行中（设备活跃回调）到达由 renderCallback 块首受理，经 DSP 实时目标更新（eq_dsp/limiter `applyTargets`，零分配/零锁/零日志）即时投递——播放中调节实时生效，平滑爬坡与限幅开关过渡在各自 process 内逐块/逐样本执行，无咔哒、无设备重载；受理后发布生效快照（config 系字段 + version 递增，sampleRate/channelCount 不回写——运行中格式不变，停态回填值恒正确）。链激活无独立发布标志——回调按 DSP 目标/平滑状态实时判定进入。initialize 幂等重放——格式重协商与 Direct 逐曲重开 = 重建（滤波历史/限幅延迟线/平滑清零，生效快照代际推进）；新曲加载协商成功与瞬时 seek 重启为内容边界，经设备清理入口显式清零。rebindQueue、T10 运行中交接、stop/pause/resume 非重建不清零——滤波/延迟线状态冻结跨越、线性精确续接（免 ~5ms 延迟线空洞与滤波阶跃瞬态）。
  - 越界守卫：中心频率 ≥ 0.95×（采样率/2）的频段整体硬直通（不进滤波链，奈奎斯特外的增益命令零影响）。
  - 限幅器：f32 域阈值限幅 + attack/release 平滑 + lookahead 延迟线（按采样率换算、>192k 封顶），开关过渡短时淡入淡出防爆音；与 EQ 同受配置投递语义（停态即时应用、运行中 `applyTargets` 实时投递）与重建语义约束。
  - 生效快照：设备持原子镜像层（version 单调递增、停态真实应用与运行期回调受理双写侧低频发布——两写者不并发；跨线程一致读回，含实际输出率回填点），供 service 同步读回（见接线现状）。
  - 频谱通道：实时回调内零分配摘录（预分配双缓冲 + 重建纪元防混率帧；链激活态摘录点 = EQ 后/音量前 f32、链关闭态 = 输出域最终，两态相对电平语义由域标注区分，激活态静音块补零帧）→ 音频 worker 播放中按 ~10ms 轮询节流取最新摘录块（512 帧；发布上界 ~40 Hz@44.1/48k，采样率越高随窗档递减——率依赖节奏契约见 spectrum_analyzer.h）→ av_tx FFT（Hann 周期窗，窗长按采样率分档 2048/4096/8192/16384，hop = 窗长/4 推进、每 hop 产出一份）→ 120 对数桶（20–20k），桶功率按范围重叠比例分摊（Σ可测桶 = Σ参与 FFT bin，能量守恒；窄桶由相邻 bin 共享）→ 驻留快照（采样率 <40k 按奈奎斯特截断、不可测桶与静音同置 −120 dB 地板；独立单调代数；默认关）。
  - 接线现状：控制命令链已贯通——意图经 setEqualizer 进入 worker 串行域（worker 侧目标存储 + 设备配置入口转发：配置存储恒执行、PENDING 目标层无条件发布——运行中（设备活跃回调）由回调块首受理并经 DSP 实时目标更新（applyTargets，零分配）投递，播放中调节实时生效；停态窗口即时真实应用、initialize/内容边界幂等重放，重建/交接不丢），生产经控制命令可激活 DSP。service 同步读回 equalizerState 直读设备生效面（generation = 设备单调代数——每次真实应用 +1，含停态应用与运行期回调受理，config/sampleRate = 生效真值；181 点增益曲线由控制层 reducer 先行产生并经订阅发布，读回面不编造、消费绘制以 reducer 快照曲线为准）。仍属后续接线：生效快照经 service 读回通道向控制层的重发布（控制器订阅现仍用 reducer 先行快照、sampleRate 未回填）。频谱驻留快照推送已接线（R2 频谱显示链路）：分析产出即经 `SpectrumUpdated` 事件外发（同事件通道、dispatcher 版本计数）→ 控制器直写驻留槽并推送频谱订阅（不落 reducer 状态面）；开关经 `SetSpectrumEnabled` 命令/意图直转服务原子位（纯门控、无 reducer 镜像）。
- FFmpeg：`FfmpegAudioSource`（解复用+解码，含 MP3 尾部 ID3v1 净化与损坏尾部截断）、`FfmpegFilterPipeline`（libavfilter 图：abuffer→aformat→abuffersink，输入签名变化时惰性重建）；两者均 pimpl，公共头不暴露任何 AV 类型。
- 波形生成：公共入口 `buildAudioWaveform`；按容器选择策略——MP4 族走 PacketBatches（单输入、250 包一批、克隆解码器）、其余走 SeekChunks（每 chunk 独立解码器 + 1 秒 preroll）；能量核运行时按 CPUID 选择 AVX2（仅 `waveform_simd_avx2.cpp` 编译期加 `-mavx2;-mfma`）/ 标量。波形生成当前仓库内无生产调用方（面向未来可视化消费）。
- 测试专用：`AudioPlayer` 类（`src/audio/audio_player.cpp`）不在库内，仅测试目标直接编译。

### 4.2 seriona_scanner（扫描与缓存）

- `FileScannerService`（接口）+ `FileScanner` 门面 + 工厂 `makeFileScannerService([deps])`；依赖注入经 `FileScannerServiceDependencies{metadataReader, watcherFactory, databasePath, coverExportDir, folderThumbnailSeam, watcherDebounce, reconcileInterval}`。接口另提供显式根移除 `removeRoot`（清空该根索引与缓存并停止监视）；根暂时性丢失（目录根）保留既有索引与缓存等待恢复，仅显式 `removeRoot` 清空；已索引的单文件根是例外——根自身经存在性复核确认 Destroyed 时按精准删除收敛（索引+缓存行），复核不通过（路径仍在/stat 出错）则保留索引并请求 Reconcile。
- 扫描主流程（`file_scanner_orchestrator.cpp`）：入队（容量 16）→ 单扫描线程 `runScan` → 逐 root `decideScanMode`（目录树哈希 vs 缓存比对，决定 Full/Incremental）→ `reconcileRoot` 四阶段（发现 → 增量计划/任务准备 → worker 并发元数据读取 → 歌词协调；末段计时为空）→ 返回后由 `recordScanRootDecision` 做缓存写回（单事务，失败整体回滚）→ 按 root 合并进长期索引（缺失/不可用/不可读的 root 保留既有条目与缓存：根不可打开时按 `rootUnavailable` 上报，空枚举绝不当作磁盘真相合并）→ 聚合构建 `PlaylistTreeSnapshot` → `resolveFolderThumbnails`（扫描收尾：为非根 Directory 节点解析 node-level 缩略图）→ 发布事件。
- 事件顺序：ScanStarted →（每 root：ScanError/FileScanned）→ ProgressUpdated（worker 阶段按 `progressInterval`（默认 250ms）节流中途发布，结束后必发一次最终汇总；`filesScanned` 为"已完成节点数"（含内联 CUE 曲目/容器与失败任务），结束恒有 `filesScanned + filesSkipped == filesDiscovered`）→ PlaylistSnapshotUpdated → ScanCompleted；取消路径先发一条 code=Cancelled 的 ScanError 再发 ScanStopped。
- 缓存：`SQLiteCache`，固定 v3 schema（`user_version=0` 初始化、非 0 非 3 抛 unsupported，无迁移桥）；5 张表 content/locations/lyrics/scan_roots/scan_errors + 8 个索引；WAL + `synchronous=NORMAL` + 64MB 页缓存；写事务 `BEGIN IMMEDIATE`、读写各持独立互斥锁（并发依赖 SQLite busy timeout 500ms）。**实际读写的是 `<databasePath>.scan-roots.sqlite` 独立文件**；传入的主库文件仅被打开初始化，扫描流程不读写。缓存另提供路径级精准写接口（按路径前缀删除、子树改名改写既有行、scoped 删除+写入单事务合并、歌词批量单事务对账、显式根删除），供事件驱动的精准增量更新使用，不读取元数据。
- 身份哈希：`computeContentId`（duration/title/artist 链式 XXH64）与 `computeLocationId`（路径/大小/mtime，CUE 轨道追加 offset/index）——实现经 `hash_utils.cpp` 文本包含 `song_identity.cpp` 进入生产库。目录树哈希：XXH3_128bits 流式 Merkle（只含文件名/类型/子哈希，跳过 `.lrc`）；单文件根（常规文件）没有目录树可递归，改由规范化路径 + size + mtime 合成稳定身份哈希，同样参与 Full/Incremental/Reconcile 的模式判定与脏标记收敛，只有路径缺失/不可读才无哈希（非常规文件仍按 UnsupportedPath 处理）。身份判据不含 ctime/inode（Windows 无 inode 且裸 POSIX API 违反平台边界），因此"大小与 mtime 均未变"的原地编辑不会被 Reconcile 发现，需用户显式 `forceRescan` 兜底。
- 并发配置：worker 数默认 `hardware_concurrency`，TagReader 并发默认同 worker 数；环境变量 `SERIONA_SCANNER_WORKERS`、`SERIONA_SCANNER_TAGREADER_CONCURRENCY` 覆盖，`SERIONA_SCANNER_DISABLE_CONCURRENCY=1` 强制串行。
- 自动更新（事件驱动精准增量 + Reconcile 对账兜底）：efsw watcher（FetchContent 固定 commit 的跨平台库：Linux inotify / Windows / macOS FSEvents；适配层把 efsw 动作映射为内部 `WatchEvent`，同目录与跨目录 rename 统一为 Renamed 对，根自身移出/删除无后端事件时由存活轮询兜底，事件队列溢出经 missed 消息上报）→ 完整 `WatchEvent` 入队 → 50ms 去抖归并 → 事件分类器按 rename 对 / create / modify / destroy / 自移动 × 文件 / 目录 归并去重 → 可精准定位时走路径级精准更新（树补丁 + SQLite 精准 API，不触发全根扫描）；目录移入走 scoped 子树对账（只枚举该子树并并入树/缓存，scope 内增量计划命中缓存时不重读标签，发布仅快照）；目录移出与 CUE 交叉（cue 删而源在 / 源删而 cue 在）走精确前缀删除 + 孤儿源重 upsert / cueRefresh scoped 重解析；`.lrc` 走 T0 歌词对账（按 sidecar 建条目索引，hash 每批一次、parse 仅在确有变化时执行一次，同一 `.lrc` 不重复读取）；封面走父目录 scoped 并强制重读该目录歌曲标签（封面增删不改音频 size/mtime，缓存直灌会让歌曲级 `artworkPath`/`thumbnailPath` 陈旧）；根级封面（父目录 == root）改用整根 force-reread scope，仍受 T1 成本门约束（超阈值回落 Reconcile）；非递归根没有可枚举子树，整根重读退化为普通扫描、直接 Reconcile——两条回落路径都不强制重读标签，歌曲级 `artworkPath`/`thumbnailPath` 在该场景不刷新（已记录的接受限制）。**watcher 事件路径（含无法分类兜底、watcher 消息、队列溢出、周期探测）绝不进入全量重扫**：这些场景提交内部 Reconcile（全根 stat 遍历 + locationId 比对 + 仅变化文件重读；哈希缺失/缓存不可读时中止并保留既有索引），并置 per-root 脏标记供周期探测无条件收敛（脏标记仅在 Reconcile/Full 成功收敛后清除，用户 Incremental 的哈希命中不清除；子项遍历/分类错误导致枚举不完整的根同样置脏，由周期 Reconcile 重读缓存缺失文件）；另有 60s 周期对账兜底。根自身移出/删除（目录根）保留既有索引等待恢复；已索引单文件根经复核确认的 Destroyed 按精准删除收敛，显式根清理走 `removeRoot`。
- TagReader 适配：`TagReader::Read`/`ReadCueSheet`（全局命名空间外部库）；适配头直接包含 `<TagReader.hpp>` 并暴露其类型，属实现导向头。
- 文件夹缩略图解析（scanner-internal `folder_thumbnail_resolver.{h,cpp}`，不进 `inc/seriona/` 稳定边界）：扫描收尾阶段为非根 Directory 节点解析 node-level 缩略图，回填 `PlaylistNode::thumbnailPath` 随快照下发（Track 节点该字段留空，歌曲缩略图走 `SongMetadata::thumbnailPath`）。case 1 经导出 seam（生产侧由 `exportFolderCoverThumbnail` 装配 `TagReader::ExportFolderCover`，ThumbnailOnly+Ignore，只查目录自身）导出封面；case 2 回退取后代歌曲中已解析缩略图的第一首（(filename, relativeDirectory) 字节序升序）；根目录恒空；确定性全序比较，seam 异常被吞掉，单文件夹失败不阻断扫描。watcher 局部批次只重解析 touched 子树 + 祖先链，未受影响目录回填上次解析结果（builder 每次重建，不回填会让文件夹封面在任意事件后消失）；缩略图缓存键含物理根（长度前缀编码），不存在跨根回填/擦除；rel 前缀命中多根（多根同 rel 合并为同一节点）时，代表值确定性地取自第一个物理根，各候选根的缓存行全部保留。
- 测试专用：`scan_scheduler.{h,cpp}`（通用任务调度器）不编译进任何库，仅测试目标直接编译。

### 4.3 seriona_metadata（平台媒体集成）

- `MetadataSharingService`（接口，`metadata_contracts.h`）+ `MetadataSharingServiceImpl`：`update()` 投递到内部 worker 线程异步转发给后端。
- 后端抽象 `MetadataServiceBackend`，按 `MetadataBackendKind`（Noop/Linux/Windows）选择；Linux 下经 sdbus-c++ 在 session bus 发布 `org.mpris.MediaPlayer2.seriona`（Root + Player 两接口 vtable、PropertiesChanged 信号；仅位置变化的更新不发信号）。
- MPRIS 内部以 `IMprisBus`/`IMprisObject` 抽象隔离 sdbus（测试可注入假总线）；命令（Play/Pause/Seek/SetPosition 等）经 `registerCommandCallback` 回传控制层，带能力门禁。
- Windows 后端为占位（接受但不发布）；`platformExtension` 为不透明 `shared_ptr<void>`。
- 未接线：`MetadataSynchronizer`（同步计划器）编译进库但生产管线未使用；`metadataServiceSynchronize`/`metadataServiceDefaultResult`/`metadataMprisSmokeResult` 无调用点。
- 条件编译：`UNIX AND NOT APPLE` 追加 mpris 实现并链接 sdbus-c++；`WIN32` 追加 windows 实现。公共契约不暴露任何平台类型。

### 4.4 seriona_control（编排核心）

- `MediaController`（pimpl 门面）：`submitCommand`（24 种命令，同步阻塞直到执行完成）、`enumeratePlaybackDevices`（设备枚举）、`scanLibrary`、五路订阅（playerState/libraryState/equalizerState/spectrum/domainNotifications）、快照查询、`start/shutdown`。命令面含播放/扫描/排序、输出配置（`ConfigureOutput`，携带 `AudioOutputConfig`）、播放过渡配置（`SetTransitionConfig`，携带 `TransitionConfig`，仅更新过渡配置、无整轨重载/设备副作用）、删除（`DeleteTrack`/`DeleteFolder`，直接删原文件，目标经 `targetPath` 传入）、临时队列（`PlayNextTrack`/`ClearPlayQueue`/`RemoveFromQueue`）、均衡器参数配置（`SetEqualizerConfig`，携带 `EqualizerConfig`，校验模式/±15 dB/NaN 后生效快照 generation++ 经 equalizerState 订阅发布；音频侧处理面与接线现状见 §4.1）、频谱开关（`SetSpectrumEnabled`，携带 bool 载荷，纯门控直转音频服务原子位，无 reducer 镜像——频谱数据通道见 §4.1 接线现状）；播放快照含 `queueEntries`（`[{trackId, nodeId}]`）临时队列字段。另提供应用设置键值读写（`getAppSetting`/`setAppSetting`/`removeAppSetting`，经 `AppSettingsStore` 落库，供前端设置/导航/播放统计三控制器持久化）。
- 命令与后端事件共用单事件循环线程：命令 → `ControlStateReducer`（纯函数归约，含 shuffle 历史、seek 状态抑制、版本去重、PlaybackEnded 自动下一曲/Repeat One、EndApproaching 决策与 AdvanceCompleted 提交账本，窗口内失效命令先 abort 再执行本体）→ `ControlReduction{result, intents, notifications}` → 提交快照 → 发布订阅者 → `executeIntents` 翻译为 audio 调用。
- 播放上下文：`buildPlaybackContextOrder` 从播放列表树快照 DFS 收集轨道 + 多规则排序（缺失值 First/Last）+ 锚点定位；Root/Folder 两种作用域。
- 依赖注入：`MediaControllerDependencies`（audio/scanner/metadata/folderSortSettingsStore/appSettingsStore/artworkResolver），缺失自动回退 noop；生产工厂接线 miniaudio 后端、带 databasePath/coverExportDir 的 scanner、Linux metadata、SQLite 文件夹排序存储（databasePath 非空时）、SQLite 应用设置存储（与排序存储共享 databasePath）。
- 封面解析：`ArtworkResolver`（有界 latest-wins 队列 + 结果 epoch 失效）+ 归约器 generation 校验，结果回填 `player_.artwork.localPath`。
- 文件夹排序：`FolderSortSettingsStore` 抽象（Noop/SQLite 实现，手写 JSON 解析）；`ApplyFolderSortRules` 命令持久化、扫描启动时重放、播放上下文构建时回填。
- 应用设置：`AppSettingsStore` 抽象（Noop/SQLite 实现，接口含 `set`/`get`/`remove`/`listByGroup`（按 key 排序）），`app_settings` 表 `(group_name, key, value, updated_at_ms)` 主键 `(group_name, key)`；`getAppSetting` 失败返回 nullopt（未启动/未存储），值以不透明字符串存储（后端不解释，前端负责编解码）。
- 订阅分发：每订阅类型一个独立投递线程，快照拷贝后异步回调，避免阻塞归约线程。

### 4.5 seriona_app 与入口层

- `seriona` 可执行文件直接编译 `main.cpp`、`terminal_controller.cpp`、`terminal_io.cpp`、`runtime_paths.cpp`、`logging.cpp`，链接 `seriona_control`（不链接 `seriona_app`），仅 Release 追加 FFmpeg（用于压制 FFmpeg 库日志）。
- `seriona_app` 静态库（application_logging/runtime_paths/logging）当前无内部消费者，作为对外分发单元存在。
- `src/logging/`：内部日志模块（无公共头），编入式复用（scanner、seriona_app、可执行文件各编译一份）；`createDedicatedLogger` 供 scanner 创建 TagReader 独立日志器。
- 公共入口：`initializeApplicationLogging(RuntimePaths)` 与 `resolveRuntimePaths(executablePath)`（`inc/seriona/app/`）。

## 5. 模块关系

- 库链接（根 CMakeLists）：`seriona_control` PRIVATE → audio/scanner/metadata/SQLite3/spdlog；`seriona_audio` PUBLIC → FFmpeg + third_party 头、PRIVATE → BS::thread_pool/spdlog；`seriona_scanner` PUBLIC → SQLite3/xxhash/TagReaderCore/thread_pool/spdlog、PRIVATE → efsw-static；`seriona_metadata` PRIVATE → spdlog（Linux 追加 sdbus-c++）。
- 数据流：`terminal_io`（按键）→ `MediaControlCommand` → 事件循环 → 归约 → audio 调用 → `BackendEvent` → 归约 → 订阅者；快照同时驱动 metadata 分享。
- 扫描数据流：`FileScannerService` 事件（含 `PlaylistTreeSnapshot`）→ control 归约更新曲库 → 播放上下文重建（当前曲消失自动续播）。
- 头级循环依赖：`metadata_contracts.h` 包含 `control_contracts.h`，control 侧前置声明 `MetadataSharingService` 打破环。
- 跨模块内部头耦合：`media_controller_module.cpp` 包含 scanner 私有头 `file_scanner_service_internal.h`（使用 `FileScannerServiceDependencies`）。
- 公共契约边界：audio 看 `audio_contracts.h`，scanner 看 `scanner_contracts.h`/`file_scanner_service.h`，metadata 看 `metadata_contracts.h`，control 看 `control_contracts.h`/`media_controller.h`；新增稳定契约不得暴露 TagReader、SQLite、watcher、FFmpeg、MPRIS/sdbus、Windows 类型。

## 6. 启动流程

```
main(argc=2, 路径存在)                       main.cpp
  └─ runTerminalController(musicPath)        terminal_controller.cpp
      ├─ TerminalMode 检查（非 tty 退出）
      ├─ resolveRuntimePaths → ensureDirectoriesExist     SerionaData/（便携）或 XDG 目录（安装模式）
      ├─ (Release) av_log_set_level(AV_LOG_QUIET)
      ├─ prepareLogFile(logs/) → 生成时间戳日志名 → logging::initialize(console=off, file, level)
      ├─ makeProductionMediaController({}, library.sqlite, artwork)
      ├─ runTerminalControllerSession
      │   ├─ 订阅 playerState/libraryState/notifications 三路
      │   ├─ controller.start()
      │   ├─ scanLibrary({musicPath, recursive}, Full)
      │   └─ 命令循环：readAction(100ms) → 按键映射命令 → submitCommand；q 退出
      └─ Stop → controller.shutdown() → 退订 → spdlog::shutdown()
```

运行时路径规则（`runtime_paths.cpp`）分两种构建模式（编译期宏 `SERIONA_INSTALLED_MODE` 选择，默认便携）：
- 便携模式：可执行文件目录（Linux 经 `/proc/self/exe`）下的 `SerionaData/` 为 data root。
- 安装模式（Linux 安装版，`cmake -DSERIONA_INSTALLED_MODE=ON`）：遵循 XDG Base Directory，应用 ID 为 `org.kaizen857.Seriona`——数据 `$XDG_DATA_HOME/org.kaizen857.Seriona`、日志 `$XDG_STATE_HOME/org.kaizen857.Seriona/logs`、封面缓存 `$XDG_CACHE_HOME/org.kaizen857.Seriona/artwork`；`XDG_*` 未设置时回退 `$HOME` 默认值，相对路径忽略（规范要求）。
- 两种模式下布局一致：日志实际文件为 `logs/seriona-<时间戳>.log`（`RuntimePaths.logFile` 中的 `seriona.log` 仅为逻辑位），数据库 `library.sqlite`，封面目录 `artwork`；`resolvePortableRuntimePaths`/`resolveInstalledRuntimePaths` 均无条件编译，`resolveRuntimePaths` 依宏选择，测试直接以环境变量注入覆盖 installed 分支。

## 7. 核心运行流程

### 7.1 播放控制链路

`submitCommand(Play/Seek/...)` → 控制事件循环 → 归约器产出意图 → `executeIntents` 调 `AudioPlaybackService` → 音频工作线程执行（状态机迁移、解码填充、时钟推进）→ `BackendEvent`（100ms 节流的进度事件、状态变更、seek 不连续事件、错误）→ 控制事件循环 → 归约器按 monotonicVersion 去重 → 快照发布 → 订阅投递线程回调；同一快照同时传给 metadata 服务发布到平台。

自动前进（自然播完）与过渡域（详见 §4.1）：Mixed 下自动前进侧启用预解码提前量或交叉档位时，终点阈值窗内服务发 `EndApproaching`，控制器决策下一曲与交接方式（直切/交叉）后经 `prepareNext` 预解码；交接或交叉重叠完成以 `AdvanceCompleted` 提交（先于新曲 TrackChanged），控制层按账本落曲、不重发 LoadTrack；窗口内失效命令先经 `AbortTransition` 中止。无预载路径（Direct、默认全关配置、预解码失败）维持 PlaybackEnded 触发控制层自动下一曲的既有流程。

### 7.2 扫描流程

扫描存在两条路径：**全量/增量重扫**（手动 `scanLibrary`、首次索引）与**事件驱动的精准增量 + Reconcile 对账**（watcher 事件主路径），两者共用扫描队列串行化。

**全量/增量重扫**：入队 → 单扫描线程逐 root 计算目录树哈希，与 `scan-roots.sqlite` 中 `CachedScanRoot` 比对决定全量/增量；增量时逐文件用 `computeLocationId`（路径/大小/mtime）判定 added/changed/unchanged/deleted，unchanged 走缓存直灌（含 CUE 轨道与歌词），changed/added 构造 worker 任务并发读元数据；结束后歌词协调（外置 LRC 哈希比对/重解析/清除）→ 缓存写回（单事务，失败整体回滚）→ 按 root 合并进长期索引（缺失/不可用 root 保留既有条目与缓存）→ `PlaylistTreeBuilder` 聚合 → `resolveFolderThumbnails` 为非根 Directory 节点解析 node-level 缩略图（`folder_thumbnail_resolver`：生产 seam 经 `exportFolderCoverThumbnail` 接 `TagReader::ExportFolderCover` 导出，回退取后代歌曲已解析缩略图，根目录恒空）→ `PlaylistSnapshotUpdated`。

**事件驱动精准增量**（watcher 事件主路径）：efsw 文件系统事件经适配层映射为完整 `WatchEvent` 入队 → 50ms 去抖归并 → 事件分类器按 rename 对 / create / modify / destroy / 自移动 × 文件 / 目录 归并同批去重 → 可精准定位的批次执行路径级操作：长生命周期 `PlaylistTreeBuilder` 成员的子树删除 / 子树改名 / 单歌 upsert（树补丁）+ SQLite 精准 API（路径前缀删除、子树改名改写既有行），不读取元数据、不触发扫描；每批处理后发布完整快照（控制层整树替换假设不变）。移入含未扫描文件的新目录走 scoped 子树对账（子树枚举 + 前缀清理后并入，scope 内增量计划命中缓存时不重读标签，发布契约只发 `PlaylistSnapshotUpdated`）；目录移出 / CUE 交叉 / `.lrc` / 封面各有专用精确或 scoped 处理；无法分类的事件 / watcher 消息 / 队列溢出标记 / 周期探测 → 提交 **Reconcile**（全根 stat + locationId 比对 + 仅变化文件重读；不因哈希缺失/不一致升级全量，哈希缺失或缓存不可读时中止并保留既有索引）；根自身移出/删除（目录根）保留既有索引等待恢复（已索引单文件根经复核确认的 Destroyed 精准删除）。

**周期对账兜底**：事件驱动之外，以可注入周期（默认 60s）对全部根做目录树哈希探测；哈希变化或该根有脏标记（批次被丢弃、队列满折叠）时提交 Reconcile，无变化且无脏标记零发布；覆盖事件丢失、队列溢出等静默失效，保证极端情况下最终收敛。

### 7.3 元数据分享（Linux MPRIS）

快照发布 → `metadata->update` → metadata worker → Linux 后端 `publishCurrentSnapshot`：映射为 MPRIS 属性（trackid/length/artUrl/xesam:url 等）→ 若相对上次仅 Playing 状态下的位置变化则不发 `PropertiesChanged` → 否则发信号；外部控制命令（如 MPRIS 客户端 Play/Seek）经 sdbus 方法 → 能力门禁 → `MediaControlCommandSink` → 控制层归约。

### 7.4 封面解析

`ResolveArtwork` 意图 → `ArtworkResolver`（1 in-flight + 1 pending，最新请求覆盖）→ TagReader 读取封面并导出到 `artwork/` 目录 → 结果回控制事件循环 → 归约器按 generation + trackId 校验 → 回填 `artwork.localPath`。

## 8. 配置方式

- 构建期（CMake 选项）：`SERIONA_BUILD_APP`（默认 ON）、`SERIONA_BUILD_TESTS`（默认 ON）、`SERIONA_BUILD_TOOLS`（默认 OFF）、`SERIONA_TAGREADER_SOURCE_DIR`（TagReader 源码路径）；三个 `SERIONA_*_SIMULATE_MISSING_*` 选项会故意令配置失败（依赖门禁演示）。
- 运行时：**无配置文件**。路径全部由可执行文件位置推导（§6）；scanner 并发由环境变量 `SERIONA_SCANNER_WORKERS`、`SERIONA_SCANNER_TAGREADER_CONCURRENCY`、`SERIONA_SCANNER_DISABLE_CONCURRENCY` 调节（非法值警告并忽略）。
- 命令行：单参数（音乐根目录或文件），必须存在。
- 日志级别：Release 构建 logger 级别 info、Debug 构建 trace；控制台 sink 在终端 UI 下恒关闭。运行时可经 `setLogLevel`（`inc/seriona/app/application_logging.h`）调整（前端设置窗口接线）。

## 9. 测试

- 构建：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N>`；运行 `build/seriona <音乐根目录或文件>`。
- 发现/运行：`ctest --test-dir build -N`；`ctest --test-dir build --output-on-failure`；聚焦 `ctest --test-dir build -R '<regex>' --output-on-failure`（常用：`seriona\.audio`、`seriona\.scanner`、`seriona\.metadata`、`seriona\.control`、`seriona\.logging`、`seriona\.runtime_paths`、`seriona\.application_logging`）。
- doctest 二进制必须恰有一个 main：多数目标由 CMake 注入 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`（含 `seriona_tests`，其 tests/main.cpp 不定义 main）；真正自带 main、未注入宏的是 `seriona_audio_fixture_tests`、`seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test`。注意 `seriona_scanner_cache_schema_tests` 链接裸 `sqlite3`。
- 特殊注册：cancellation 状态机测试单独注册为 `seriona.playback_state_machine_cancellation`（普通状态机测试排除它）；`seriona.audio.waveform.perf` 超时 900s；`seriona.control_artwork_resolver` 超时 300s；`seriona.scanner.efsw_integration` 超时 300s。
- 禁用目标：`seriona_scanner_cache_tests`、`seriona_scanner_cache_content_tests`、v2→v3 migration、backup rollback、phase1 integration 在 `tests/CMakeLists.txt` 中整段注释，不要假设可运行。
- 性能目标：`seriona_scanner_perf_test`、`seriona_scanner_detailed_perf_test` 只构建不注册 CTest，直接运行 `build/tests/<目标>`；`tools/scanner_cold_perf`（-DSERIONA_BUILD_TOOLS=ON）为独立冷扫描基准。
- 测试隔离：音频测试使用 fake `AudioOutputDeviceBackend` 或测试现场生成的短音频 fixture；扫描测试使用 `scanner_test_harness` 与测试接缝（`file_scanner_orchestrator_test_access.h` 的全局观察者，仅测试包含）。

## 10. 扩展方式

- 音频后端：实现 `AudioOutputDeviceBackend` 并经 `makeAudioPlaybackService(backend)` 注入。
- 元数据后端：实现 `MetadataServiceBackend` 或复用 MPRIS 抽象（`IMprisBus`/`IMprisObject`）；通过 `MetadataSharingOptions.backendKind` + `platformExtension` 选择。
- 扫描依赖：`FileScannerServiceDependencies` 注入自定义元数据读取器（`TagMetadataReader`）与 watcher 工厂。
- 排序存储：实现 `FolderSortSettingsStore` 抽象。
- 应用设置：实现 `AppSettingsStore` 抽象（Noop 或 SQLite 落库）。
- 控制器依赖：`MediaControllerDependencies` 全量注入，缺失项自动回退 noop——可用于无 UI/无真实硬件的场景。
- 对外消费：订阅 `PlayerStateSnapshot`/`LibraryStateSnapshot`/`EqualizerStateSnapshot`（生效配置 + 181 点增益曲线 + 单调代数）/`SpectrumSnapshot`（频谱：订阅即收当前驻留快照，随后经 `SpectrumUpdated` 事件增量推送 120 段电平）/领域通知即可构建新前端；`AudioPlaybackService` 与 `FileScannerService` 也可独立使用。
- 前端集成：终端控制器是 `TerminalActionReader` 抽象之上的唯一实现，新 UI 可替换入口层而保持 control 不变。

## 11. 开发建议

- 实时路径红线：`AudioOutputDevice::renderCallback()` 内禁止 FFmpeg、事件回调、日志、动态分配、阻塞锁、设备生命周期操作。
- schema 红线：`SQLiteCache` schema 固定 v3，`user_version=0` 直接初始化、非 0 非 3 报错；不存在迁移桥，改 schema 必须同步 `sqlite_cache_connection.cpp` 内嵌 SQL 与 `cache/schema.sql`（一致性仅靠测试校验）。
- 编译归属陷阱：`audio_player.cpp`、`scan_scheduler.cpp` 只被测试目标编译；`song_identity.cpp` 经 `hash_utils.cpp` 文本包含进库——生产代码不要依赖这些文件的独立编译单元身份。AVX2/FMA 参数只允许施加于 `waveform_simd_avx2.cpp`（根 CMake 仅对该文件施加 `-mavx2;-mfma`，无专门守卫；FATAL_ERROR 守卫只针对 `BS::thread_pool` 链接）。
- 未接线代码（勿假设生效）：`MetadataSynchronizer`、`buildScalarWaveformBars`、scanner `ProgressThrottle` 类（orchestrator 自持轻量时间节流，未用此类）、`platformExtension`（Windows）。设备选择已接线（§4.1：`preferredDeviceId` 解析绑定，非纯标签）。
- 新稳定契约不得暴露第三方类型（TagReader/SQLite/watcher/FFmpeg/MPRIS/sdbus/Windows）。
- 文档优先级：本文件低于 CMake 配置、源码与测试注册；README 仅有标题，`docs/` 为演进记录。

## 12. 维护建议

- 启动日志：`SerionaData/logs/seriona-<时间戳>.log`（5MB×3 滚动，总量超 50MB 自动清理最旧）。
- 缓存重置：删除 `SerionaData/` 下 `*.scan-roots.sqlite`（与 `library.sqlite`）即可强制下次全量扫描并重建状态。
- 常见故障：配置失败时按 FATAL_ERROR 提示安装缺失系统库（sqlite、xxhash、sdbus-c++）；`ctest` 中波形/封面相关目标超时较长，聚焦时用 `-R` 正则。
- 提交与文档语言：中文（与仓库既有约定一致）。
