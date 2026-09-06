#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace seriona::audio {

enum class AudioOutputMode {
  Direct,
  Mixed,
};

enum class AudioSampleFormat {
  Unknown,
  Int16,
  Int24,
  Int32,
  Float32,
};

enum class PlaybackState {
  Idle,
  Loading,
  Ready,
  Playing,
  Paused,
  Draining,
  Stopped,
  Error,
};

enum class PlaybackErrorCode {
  OpenFailed,
  UnsupportedFormat,
  DeviceUnavailable,
  FormatNegotiationFailed,
  DecodeFailed,
  BufferUnderrun,
  SeekFailed,
};

enum class BackendSourceModule {
  AudioPlayer,
  AudioPlaybackService,
};

enum class BackendEventType {
  PlaybackStateChanged,
  TrackChanged,
  PlaybackPositionUpdated,
  PositionDiscontinuity,
  PlaybackEnded,
  OutputFormatChanged,
  OutputModeFallback,
  PlaybackError,
  // T8：预解码预告（服务→控制器）——当前曲目距自然终点剩余 remainingMs（armed 去重，
  // 一次性/臂）。追加末尾保持序数兼容。
  EndApproaching,
  // T10：接管提交（服务→控制器，Metis 缺口 1b）——重叠交叉/无缝直切 handoff 完成、
  // 新曲已作为主源播放时发出，携带新曲 trackId；控制器据 pendingAdvance 账本做
  // advance 提交（不重发 LoadTrack）。无预载的普通自然结束仍走 PlaybackEnded。
  // 发射序 = 提交事件先于新曲的 TrackChanged/状态事件（控制器提交需先于状态漂移）。
  AdvanceCompleted,
};

struct TrackPlaybackRequest {
  std::string trackId;
  std::filesystem::path filePath;
  std::string title;
  std::string artist;
  std::optional<std::chrono::milliseconds> offset;
  std::optional<std::chrono::milliseconds> duration;
  std::optional<std::uint32_t> sampleRate;
  std::optional<std::uint16_t> bitDepth;
  std::optional<std::uint16_t> channels;
  std::optional<std::string> format;
  bool boundedSegment{false};
};

struct AudioOutputConfig {
  AudioOutputMode outputMode{AudioOutputMode::Mixed};
  std::optional<std::uint32_t> targetSampleRate;
  std::optional<AudioSampleFormat> targetSampleFormat;
  std::optional<std::uint16_t> targetChannelCount;
  // 300ms default ring: absorbs decoder-thread scheduling jitter that underran the 100ms ring under CPU load
  // (docs/audio-playback-stutter-and-watcher-reconciliation-analysis.md §二 P0-①).
  std::chrono::milliseconds bufferDuration{300};
  bool keepDeviceOpen{false};
  bool allowFallback{true};
  std::string preferredDeviceId;
};

// 播放过渡参数（淡入淡出/交叉/预加载），用户裁定表 9 项设置的后端值类型。
// 纯 C++23 值类型，不暴露任何第三方类型。默认构造 == 裁定默认 == "旧行为等价"：
// 全部 9 项默认下采样路径与改动前逐位一致（Direct 恒重开+硬切，Mixed 无交叉）。
// 字段声明顺序即跨端契约（前端任务 12 按此顺序组包），勿随意调整。
// 与 AudioOutputConfig 语义隔离：仅描述过渡行为，绝不触发输出重载/设备操作。
enum class AutoAdvanceFadeMode {
  // 设置 1：自动前进（当前曲自然播完）淡入淡出。仅 Mixed 生效。
  Off,               // 无（默认）——除 CUE 无间隙组内尽力无缝外不做交叉
  ExceptGaplessGroup,  // 除 CUE 邻曲/无间隙组外交叉（组内尽力无缝硬切）
  All,               // 全交叉（对 CUE 组也交叉，按字面）
};

enum class ManualAdvanceFadeMode {
  // 设置 8：手动改变音轨淡入淡出。仅 Mixed 生效；Direct 下整项无效。
  Off,           // 无（默认）
  ShortDip,      // 短时交叉渐隐（dip，时长=manualShortCrossfadeMs，对半分解）
  FullCrossfade, // 交叉淡入淡出（时长=crossfadeMs，与自动档共用）
};

struct TransitionConfig {
  AutoAdvanceFadeMode autoAdvanceFadeMode{AutoAdvanceFadeMode::Off};
  // 设置 2：播放/暂停/停止淡入淡出开关（全局，含 Direct）。
  bool fadeOnTransport{false};
  // 设置 3：调整播放进度（seek）淡入淡出开关（全局）。
  bool fadeOnSeek{false};
  // 设置 4：无间隙音轨预解码触发提前量（仅 Mixed）。
  std::chrono::milliseconds gaplessPreloadMs{0};
  // 设置 5：交叉淡入淡出长度（自动交叉与手动档 3 共用，仅 Mixed）。
  std::chrono::milliseconds crossfadeMs{3000};
  // 设置 6：播放/暂停/停止淡变长度（全局）。
  std::chrono::milliseconds transportFadeMs{300};
  // 设置 7：seek 淡变长度（全局）。
  std::chrono::milliseconds seekFadeMs{300};
  ManualAdvanceFadeMode manualAdvanceFadeMode{ManualAdvanceFadeMode::Off};
  // 设置 9：短时手动交叉长度（档 2 dip 用，仅 Mixed）。
  std::chrono::milliseconds manualShortCrossfadeMs{500};

  // 语义：0 时长 = 该淡变即时完成（等效关闭），不做下界钳制。
  // 相等比较供任务 7 免重开短路判据与 L1 实变判定复用；默认构造 == 裁定默认。
  friend bool operator==(const TransitionConfig&, const TransitionConfig&) = default;
};

struct AudioDeviceFormat {
  std::string deviceId;
  std::string deviceName;
  std::string backendName;
  std::uint32_t sampleRate{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::uint16_t channelCount{0};
  std::uint32_t bufferFrames{0};
  AudioOutputMode actualMode{AudioOutputMode::Mixed};
  bool fallbackApplied{false};
  // 设备能力枚举结果（ma_context_get_device_info 的 nativeDataFormats 提取）。
  // 空列表表示"未枚举或全支持"：miniaudio 中 ma_format_unknown / sampleRate==0 即全支持，
  // 不产生条目；查询失败时同样留空并置 fallbackApplied。前端设置窗口据此过滤
  // 采样率/位深下拉（空=显示全部）。
  std::vector<AudioSampleFormat> supportedSampleFormats;
  std::vector<std::uint32_t> supportedSampleRates;
  // 是否系统默认播放设备（miniaudio ma_device_info.isDefault 透传）。
  // 语义随后端而异：PulseAudio/PipeWire-pulse 下可靠（服务器默认 sink）；ALSA 下仅
  // id=="default" 的伪设备为 true，硬件设备恒 false（miniaudio 源码注释自述无法判定）。
  // 前端只应将其用于 UI 标记/默认高亮，不得作为"必须存在"的假设。
  bool isDefaultDevice{false};
};

struct PlaybackClockSnapshot {
  std::string trackId;
  std::chrono::milliseconds position{0};
  std::chrono::steady_clock::time_point sampledAt{};
  std::uint64_t version{0};
  bool continuous{false};
};

struct PlaybackStateChanged {
  PlaybackState state{PlaybackState::Idle};
};

struct TrackChanged {
  TrackPlaybackRequest request{};
};

struct PlaybackPositionUpdated {
  PlaybackClockSnapshot clock{};
};

struct PositionDiscontinuity {
  PlaybackClockSnapshot before{};
  PlaybackClockSnapshot after{};
  std::string reason;
};

struct PlaybackEnded {
  TrackPlaybackRequest request{};
  PlaybackClockSnapshot finalClock{};
};

struct OutputFormatChanged {
  AudioOutputConfig requestedConfig{};
  AudioDeviceFormat deviceFormat{};
};

struct OutputModeFallback {
  AudioOutputConfig requestedConfig{};
  AudioOutputConfig effectiveConfig{};
  AudioDeviceFormat effectiveFormat{};
  std::string reason;
};

struct PlaybackError {
  PlaybackErrorCode code{PlaybackErrorCode::OpenFailed};
  std::string message;
  std::string detail;
  std::optional<PlaybackClockSnapshot> clock;
};

// T8：预解码预告载荷。remainingMs = 距自然终点（endPosition−clock）的毫秒估计；
// 控制器据此决定是否 PrepareNext 预解码（Metis 缺口 1a 选曲侧）。
struct EndApproaching {
  std::chrono::milliseconds remainingMs{0};
};

// T10：接管提交载荷（服务→控制器）。trackId = 已接管的下一曲标识（与控制器
// EndApproaching 时经 PrepareNext 下发的选定曲一致）。无预载的普通自然结束不发本事件。
struct AdvanceCompleted {
  std::string trackId;
};

// T8：控制器在 EndApproaching 时选定的预解码交接方式（PrepareNext 元数据）。
// 字段声明顺序即跨端契约，追加字段只能放末尾。
enum class PrepareNextKind : std::uint8_t {
  SeamlessDirect,  // 就绪即无缝直切（handoff，无交叉）：RepeatOne 自身重播 / CUE 组内 / 自动档=无+预加载
  Crossfade,       // 交叉淡入淡出（任务 9 双源重叠启用；T8 期间槽就绪时自然终点仍按直切兜底）
};

struct PrepareNextMeta {
  PrepareNextKind kind{PrepareNextKind::SeamlessDirect};
  // 同一 .cue 文件相邻轨道（无间隙组内）：中间档据此豁免交叉（组内尽力无缝）。
  bool isGaplessGroup{false};
};

using PlaybackEvent = std::variant<
    PlaybackStateChanged,
    TrackChanged,
    PlaybackPositionUpdated,
    PositionDiscontinuity,
    PlaybackEnded,
    OutputFormatChanged,
    OutputModeFallback,
    PlaybackError,
    EndApproaching,
    AdvanceCompleted>;

struct BackendEvent {
  BackendEventType type{BackendEventType::PlaybackStateChanged};
  BackendSourceModule sourceModule{BackendSourceModule::AudioPlaybackService};
  std::uint64_t monotonicVersion{0};
  std::chrono::steady_clock::time_point timestamp{};
  PlaybackEvent payload{};
};

using BackendEventSink = std::function<void(BackendEvent)>;

// 均衡器/频谱契约（EQ 追加）：图形均衡器参数、均衡器状态快照与实时频谱快照的
// 跨端值类型。纯 C++23 值类型，不暴露任何第三方类型；默认构造 == 出厂默认 ==
// "均衡器关闭"的旧行为等价（enabled=false 全链路直通，音频处理与改动前逐位一致）。
// 字段声明顺序即跨端契约（前端按此组包），追加字段只能放末尾。
// 频段模式：决定 bandGainsDb 的有效前缀长度与均衡器曲线解析粒度。
enum class EqualizerBandMode {
  Band10,  // 10 段图示均衡（bandGainsDb 仅前 10 项生效）
  Band31,  // 31 段图示均衡（bandGainsDb 全部 31 项生效）
};

// 均衡器参数（用户 EQ 设置的后端值类型，前端经 setEqualizer 下发）。
// 与 AudioOutputConfig / TransitionConfig 语义隔离：仅描述均衡处理，绝不触发输出重载。
struct EqualizerConfig {
  bool enabled = false;                                // 均衡器总开关（默认关 = 直通）
  EqualizerBandMode mode = EqualizerBandMode::Band10;  // 频段模式（默认 10 段）
  float preGainDb = 0.0f;                              // 前置增益（dB，整体放大/衰减）
  std::array<float, 31> bandGainsDb{};                 // 频段增益（dB，默认全 0 = 平直；按 mode 取前缀）
  bool limiterEnabled = false;                         // 输出限幅器开关（默认关）
};

// 均衡器状态快照（setEqualizer 生效后的同步读回：生效配置 + 按当前采样率/频段
// 解析出的增益曲线）。curvePointsDb 为 181 点、按 20–20k Hz 对数频率轴采样；
// 频率轴点由 reducer 填充到 curveFrequenciesHz（本类型只声明形状），对端可按
// (curveFrequenciesHz[i], curvePointsDb[i]) 逐点绘制。
struct EqualizerStateSnapshot {
  std::uint64_t generation = 0;                // 单调代数：每次均衡器参数生效递增（0 = 从未生效的空快照）
  EqualizerConfig config;                      // 当前生效配置
  std::uint32_t sampleRate = 0;                // 快照对应的输出采样率
  std::array<float, 181> curvePointsDb{};      // 增益曲线采样点（dB）
  std::array<float, 181> curveFrequenciesHz{}; // 与 curvePointsDb 逐点对应的频率轴（Hz）
};

// 频谱快照（实时分析输出，60 段频带电平，供前端频谱可视化；默认全 0 = 空快照）。
struct SpectrumSnapshot {
  std::uint64_t generation = 0;   // 单调代数：每次频谱更新递增（0 = 空快照）
  std::uint32_t sampleRate = 0;   // 快照对应的输出采样率
  std::array<float, 60> binsDb{}; // 60 段频带电平（dB）
  std::uint64_t timestampMs = 0;  // 快照生成时刻（毫秒时间戳）
};

class AudioPlaybackService {
public:
  virtual ~AudioPlaybackService() = default;

  virtual void setEventSink(BackendEventSink sink) = 0;
  virtual void configureOutput(const AudioOutputConfig& config) = 0;
  // 过渡参数配置（淡入淡出/交叉/预加载）。与 configureOutput 语义隔离：仅更新
  // 过渡配置，绝不触发重载/设备生命周期操作/事件。非纯虚 + 默认空实现：实现者
  // 共 4 个（SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），纯虚会同时打破
  // 两仓库编译；no-op 也是 Noop/Fake 的既有机制，无需逐个覆写（同下方
  // enumeratePlaybackDevices 先例）。
  virtual void configureTransition(const TransitionConfig& config) { (void)config; }
  virtual void loadTrack(const TrackPlaybackRequest& request) = 0;
  virtual void prepareNext(const TrackPlaybackRequest& request) = 0;
  // T8：预解码交接方式重载。非纯虚 + 默认转发单参版（= SeamlessDirect 语义）：实现者共
  // 4 个（SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），纯虚/改签单参版会同时打破
  // 两仓库编译——同 configureTransition / enumeratePlaybackDevices 先例。
  virtual void prepareNext(const TrackPlaybackRequest& request, const PrepareNextMeta& meta) {
    (void)meta;
    prepareNext(request);
  }
  // T10：中止在途过渡（重叠交叉/预解码槽退役 + 预解码预告重新武装）。非纯虚 +
  // 默认空实现：实现者共 4 个（SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），
  // 同 configureTransition 先例（Noop/Fake 靠默认空实现维持，无需逐个覆写）。
  virtual void abortTransition() {}
  virtual void play() = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;
  virtual void seek(std::chrono::milliseconds position) = 0;
  virtual void setVolume(float linearGain) = 0;
  virtual void setMuted(bool muted) = 0;
  virtual void selectOutputDevice(const std::string& deviceId) = 0;
  [[nodiscard]] virtual PlaybackClockSnapshot queryPlaybackClock() const = 0;
  // 枚举当前可用的输出设备。非纯虚 + 默认返回空列表：实现者共 4 个
  // （SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），纯虚会同时打破
  // 两仓库编译；默认空也是 Noop/Fake 的既有机制，无需逐个覆写。
  [[nodiscard]] virtual std::vector<AudioDeviceFormat> enumeratePlaybackDevices() const { return {}; }
  // EQ：均衡器参数下发（频段增益/模式 + 前置增益 + 限幅器）。与 configureOutput
  // 语义隔离：仅更新均衡器配置，绝不触发输出重载/设备生命周期操作/事件。非纯虚 +
  // 默认空实现：实现者共 4 个（SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），
  // 纯虚会同时打破两仓库编译；no-op 也是 Noop/Fake 的既有机制，无需逐个覆写
  // （同 configureTransition 先例）。
  virtual void setEqualizer(const EqualizerConfig& config) { (void)config; }
  // EQ：同步读取当前均衡器状态快照（生效配置 + 增益曲线 + 单调代数）。非纯虚 +
  // 默认返回空快照（generation=0）：实现者共 4 个，纯虚会同时打破两仓库编译，
  // 同 setEqualizer / enumeratePlaybackDevices 先例。
  virtual EqualizerStateSnapshot equalizerState() const { return {}; }
  // 频谱：实时频谱分析开关（任务 30 B3.2）。默认关——worker 不取摘录帧/不分析/
  // 零成本；开启且逻辑态 Playing 时以 ~50ms 节流产出频谱快照。非纯虚 + 默认空
  // 实现：实现者共 4 个（SingleTrack 业务实现、Noop、后端 Fake、前端 Fake），
  // 纯虚会同时打破两仓库编译；no-op 也是 Noop/Fake 的既有机制（同 setEqualizer
  // / configureTransition 先例）。
  virtual void setSpectrumEnabled(bool enabled) { (void)enabled; }
  [[nodiscard]] virtual bool spectrumEnabled() const { return false; }
};

class AudioPlayer {
public:
  AudioPlayer();
  explicit AudioPlayer(std::shared_ptr<AudioPlaybackService> service);

  void setPlaybackService(std::shared_ptr<AudioPlaybackService> service);
  void setEventSink(BackendEventSink sink);
  void configureOutput(const AudioOutputConfig& config);
  void loadTrack(const TrackPlaybackRequest& request);
  void prepareNext(const TrackPlaybackRequest& request);
  void play();
  void pause();
  void resume();
  void stop();
  void seek(std::chrono::milliseconds position);
  void setVolume(float linearGain);
  void setMuted(bool muted);
  void selectOutputDevice(const std::string& deviceId);
  [[nodiscard]] PlaybackClockSnapshot queryPlaybackClock() const;

private:
  std::shared_ptr<AudioPlaybackService> service_;
};

}
