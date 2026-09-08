#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/buffer/pcm_buffer_queue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::audio {

class AudioOutputDevice;
// EQ/限幅 DSP 纯核心前向声明（任务 25 B2.4a）：实现类位于 src/audio/eq_dsp.h 与
// src/audio/limiter.h（内部头，含 miniaudio 类型——公共头不引入内部依赖），本类以
// 不透明 unique_ptr 持有，实例化/销毁在 audio_output_device.cpp（构造函数，非回调路径）。
class EqualizerDspProcessor;
class LimiterDspProcessor;

struct AudioOutputDeviceCounters {
  std::uint64_t callbackCount{0};
  std::uint64_t requestedFrames{0};
  std::uint64_t copiedFrames{0};
  std::uint64_t silenceFrames{0};
};

struct AudioOutputDeviceOpenRequest {
  AudioOutputConfig config{};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Float32};
  std::uint32_t sampleRate{48000};
  std::uint16_t channelCount{2};
  std::uint32_t bufferFrames{512};
  PcmBufferQueue* pcmQueue{nullptr};
  AudioOutputDevice* callbackUserData{nullptr};
};

struct AudioOutputDeviceError {
  PlaybackErrorCode code{PlaybackErrorCode::DeviceUnavailable};
  std::string message;
  std::string detail;
};

class AudioOutputDeviceBackend {
public:
  virtual ~AudioOutputDeviceBackend() = default;

  [[nodiscard]] virtual std::vector<AudioDeviceFormat> enumeratePlaybackDevices() = 0;
  [[nodiscard]] virtual bool initialize(const AudioOutputDeviceOpenRequest& request) = 0;
  [[nodiscard]] virtual bool start() = 0;
  [[nodiscard]] virtual bool stop() = 0;
  virtual void uninitialize() noexcept = 0;
  [[nodiscard]] virtual AudioDeviceFormat currentFormat() const = 0;
  [[nodiscard]] virtual std::optional<AudioOutputDeviceError> lastError() const { return std::nullopt; }
};

// 增益包络曲线（任务 5）。Linear=传送类短淡变；EqualPowerPair=交叉类等功率互补
// （cos 相位插值：1→0 与 0→1 对偶满足 g1²+g2²=1，中点各 -3dB≈0.7071）。
enum class GainEnvelopeCurve : std::uint8_t { Linear = 0, EqualPowerPair = 1 };

// worker→回调 的包络发布快照（纯 POD）。durationFrames==0 = 即时完成（等效关闭）。
// startGain 为 worker 账本参考值（回调执行器始终以自身 currentGain 为 ramp 起点，
// 保证无跳变）；version 每次发布递增，回调侧版本账本据此受理新包络。
struct GainEnvelopeSnapshot {
  float targetGain{1.0F};
  float startGain{1.0F};
  std::uint32_t durationFrames{0};
  GainEnvelopeCurve curve{GainEnvelopeCurve::Linear};
  std::uint32_t version{0};
};

// 单层包络状态（原子），字段分两套角色：
//  - PENDING（worker 写）：version/targetGain/startGain/durationFrames/curve —— worker 最新发布，
//    后写覆盖（last-write-wins），publishEnvelopeLayer 发布序不变（version release 最后写）。
//  - EXEC/LATCHED（回调受理时从 PENDING 一次性拷贝）：execStartGain/execTargetGain/
//    execDurationFrames/execCurve —— 在途执行轨迹的唯一事实源；受理后在轨迹结束前保持稳定，
//    worker 再发布也不覆盖（等轨迹结束后下一块自动受理最新 PENDING）。
//  - 回调推进：currentGain = 回调侧当前增益真值（worker 读回）；rampFramesDone = 进度
//    0..execDurationFrames；latchedVersion = 已受理版本（0 = 未受理过）。
struct GainEnvelopeLayerState {
  std::atomic<std::uint32_t> version{0};
  std::atomic<float> targetGain{1.0F};
  std::atomic<float> startGain{1.0F};
  std::atomic<std::uint32_t> durationFrames{0};
  std::atomic<GainEnvelopeCurve> curve{GainEnvelopeCurve::Linear};
  std::atomic<float> currentGain{1.0F};
  std::atomic<std::uint32_t> rampFramesDone{0};
  std::atomic<std::uint32_t> latchedVersion{0};

  // 执行轨迹锁存（回调受理时从 pending 拷贝；在途期间稳定，worker 再发布不覆盖）。
  std::atomic<float> execStartGain{1.0F};
  std::atomic<float> execTargetGain{1.0F};
  std::atomic<std::uint32_t> execDurationFrames{0};
  std::atomic<GainEnvelopeCurve> execCurve{GainEnvelopeCurve::Linear};
};

// EQ 生效快照（任务 26 B2.4b）——读侧 POD：实际生效的均衡器配置 + 实际输出率/声道数
// + 单调发布代数。仿 GainEnvelopeSnapshot 的「发布→读回」定位；读侧 eqAppliedSnapshot()
// 返回一致副本（原子镜像层 AudioOutputDeviceEqLayer + version seqlock，见下）。
struct AudioOutputDeviceEqSnapshot {
  EqualizerConfig config{};        // 实际生效配置（最后一次真实应用；非「仅存储」值）
  std::uint32_t sampleRate = 0;    // 实际输出采样率（设备格式确定后才随应用发布；0 = 未应用）
  std::uint16_t channelCount = 0;  // 实际输出声道数
  std::uint32_t version = 0;       // 单调发布代数（每次真实应用 +1；0 = 从未应用）
};

// EQ 生效快照原子镜像层（任务 26 B2.4b + 本任务实时受理；结构仿 GainEnvelopeLayerState
// / publishEnvelopeLayer 发布序）：字段按角色分两套——
//  - 写侧两处（低频互斥，不并发）：
//     · worker 停态窗口（applyEqualizerDspConfig/clearDspState 真实应用后）；
//     · renderCallback 块首受理（本任务：运行期目标经 applyTargets 投递 DSP 后发布
//       config 系字段；sampleRate/channelCount 不回写——运行中格式不变，停态应用
//       已回填，恒正确）。
//   均逐字段 relaxed 写、version release 最后写（发布序同 publishEnvelopeLayer：
//   回调/读侧读到 version 变化时字段必然已就位）；
//  - 读侧（eqAppliedSnapshot()，可为 worker 或控制线程）：version acquire 读后取一致
//    字段视图，再复核 version——发布可发生于读取期间，字段为原子镜像无撕裂；版本变更
//    仅表示读到相邻两代混合，低频读 + 低频写下重读一次自愈（同 planEnvelopeLayer 语义）。
// 与包络层差异：EQ 停态应用只发生在停态窗口（无活跃回调），运行期受理发生在回调线程
// （此时 worker 不应用）——镜像层同时服务 worker 与未来控制线程
// （service.equalizerState 回读）的无锁一致读，并容忍双写侧低频交错。
struct AudioOutputDeviceEqLayer {
  std::atomic<std::uint32_t> version{0};
  std::atomic<bool> enabled{false};
  std::atomic<EqualizerBandMode> mode{EqualizerBandMode::Band10};
  std::atomic<float> preGainDb{0.0F};
  std::array<std::atomic<float>, 31> bandGainsDb{};
  std::atomic<bool> limiterEnabled{false};
  std::atomic<std::uint32_t> sampleRate{0};
  std::atomic<std::uint16_t> channelCount{0};
};

// EQ 运行期目标 PENDING 层（播放中实时调节的 worker→回调 发布面；仿
// GainEnvelopeLayerState 的 PENDING 角色）。字段 = setEqualizerConfig 收到的目标
// 配置快照（enabled/mode/preGainDb/31 段 bandGainsDb/limiterEnabled）：
//  - 写侧 = worker（setEqualizerConfig 无条件发布，含停态与未 initialize）；
//    逐字段 relaxed 写、version release 最后写（回调读到 version 变化时字段必然
//    已就位；回调以 version 与受理账本 latchedEqTargetVersion_ 比较判定新目标）；
//  - 读侧 = renderCallback 块首受理（device 活跃回调态）：逐字段 relaxed 读组包
//    EqualizerConfig → eqDsp_/limiter_ applyTargets（零分配投递）→ 发布生效快照。
// version 每次发布 +1（0 = 从未发布）。停态真实应用（applyEqualizerDspConfig）
// 不依赖本层，但会在应用后把账本消费到当前 version（防停态应用 + 停态直渲/重启
// 首块重复受理重放——受理对同目标无操作，仅避免生效快照重复发布）。
struct AudioOutputDeviceEqTargetLayer {
  std::atomic<std::uint32_t> version{0};
  std::atomic<bool> enabled{false};
  std::atomic<EqualizerBandMode> mode{EqualizerBandMode::Band10};
  std::atomic<float> preGainDb{0.0F};
  std::array<std::atomic<float>, 31> bandGainsDb{};
  std::atomic<bool> limiterEnabled{false};
};

// 频谱摘录帧电平语义标注（任务 29 B3.1）——两态电平语义不同、显示须按相对电平注明：
//  · ChainInactiveOutput：链关闭态（EQ 链未激活）——帧取自设备输出域，volume/
//    muted/混音/包络已应用（逐格式转换拷贝：f32 设备直取，int 设备逐样本按读侧
//    冻结尺度 /2^15、/2^31、s24 左对齐语义 int→f32）；
//  · ChainActiveF32：链激活态（EQ 链进入）——帧取自 EQ(含 preGain) 后、volume
//    前的 f32 中间域，不含用户 volume/限幅/包络淡变——与输出域帧不可同标定比较。
enum class AudioOutputDeviceCaptureDomain : std::uint8_t {
  ChainInactiveOutput = 0,
  ChainActiveF32 = 1,
};

// 摘录帧元数据（任务 29 B3.1；latestCaptureFrame 回填，消费契约）：
//  - generation：设备重建纪元——initialize()（重新协商格式/容量）时 ++ 并清空帧
//    内容；消费方发现 generation 变化必须丢弃全部跨代状态（FFT 窗缓冲/率标定），
//    防混率帧被消费；
//  - sequence：帧序（同纪元内每发布帧 +1，单调）；消费方据此判断是否出现新帧；
//  - sampleRate：帧采样率（= 设备实际输出率；与 generation 双保险）；
//  - frameCount：帧样本数（单声道 f32；= 整回调块帧数，含欠载静音尾）；
//  - domain：电平语义来源（见 AudioOutputDeviceCaptureDomain）。
struct AudioOutputDeviceCaptureMeta {
  std::uint32_t generation{0};
  std::uint32_t sequence{0};
  std::uint32_t sampleRate{0};
  std::uint32_t frameCount{0};
  AudioOutputDeviceCaptureDomain domain{AudioOutputDeviceCaptureDomain::ChainInactiveOutput};
};

struct AudioOutputDeviceCallbackState {
  std::atomic<PcmBufferQueue*> pcmQueue{nullptr};
  std::atomic<PcmBufferQueueGeneration> queueGeneration{0};
  std::atomic<std::uint32_t> bytesPerFrame{0};
  std::atomic<std::uint16_t> channelCount{0};
  std::atomic<AudioSampleFormat> sampleFormat{AudioSampleFormat::Unknown};
  std::atomic<bool> active{false};

  // ---- 增益包络状态（任务 5）：worker 发布 / 回调推进，见 GainEnvelopeLayerState 注释 ----
  GainEnvelopeLayerState masterEnvelope;                          // 传送层（pause/stop/seek/play 淡变）
  std::array<GainEnvelopeLayerState, 2> sourceEnvelopes;          // 源增益层；槽 0 = 主源（单源活动路径），
                                                                  // 槽 1 = 第二源（任务 9：仅 secondActive 时参与执行）

  // ---- 第二源状态（任务 9）：worker 发布 / 回调读取，发布纪律同 pcmQueue 三件套 ----
  // activateSecondSource 先写 ring 指针 + 代次 + 包络，最后置 secondActive=true（release）；
  // deactivateSecondSource 先清 active、代次递增、指针置空。回调按 secondActive 门控：
  // 撤销窗口内至多再持有一个 block 的旧指针（secondGeneration 校验即弃，绝不触碰新指针）。
  std::atomic<PcmBufferQueue*> secondQueue{nullptr};
  std::atomic<PcmBufferQueueGeneration> secondGeneration{0};
  std::atomic<bool> secondActive{false};
};

class AudioOutputDevice {
public:
  explicit AudioOutputDevice(std::unique_ptr<AudioOutputDeviceBackend> backend = nullptr);
  ~AudioOutputDevice();

  AudioOutputDevice(const AudioOutputDevice&) = delete;
  AudioOutputDevice& operator=(const AudioOutputDevice&) = delete;
  AudioOutputDevice(AudioOutputDevice&&) = delete;
  AudioOutputDevice& operator=(AudioOutputDevice&&) = delete;

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices();
  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request);
  [[nodiscard]] bool start();
  [[nodiscard]] bool stop();
  void uninitialize() noexcept;
  void rebindQueue(PcmBufferQueue& queue) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] AudioDeviceFormat currentFormat() const;
  [[nodiscard]] std::optional<AudioOutputDeviceError> lastError() const;
  void setVolume(float linearGain) noexcept;
  void setMuted(bool muted) noexcept;

  // ---- 增益包络发布面（任务 5；worker 线程调用，acquire/release 语义同 publishCallbackQueue）----
  void setMasterEnvelope(const GainEnvelopeSnapshot& snapshot) noexcept;
  // slot >= kActiveSourceEnvelopeSlots 的发布被忽略。
  void setSourceEnvelope(std::size_t slot, const GainEnvelopeSnapshot& snapshot) noexcept;
  // 清 PENDING 与 EXEC 全部 12 字段（含 currentGain→1.0 与 exec* 锁存；stop 后不滞留末次淡变值，
  // 防陈旧快照与陈旧起点）。stop()/uninitialize() 内调用（设备已停、无活跃回调，无竞争）。
  void resetEnvelopes() noexcept;
  // T10：仅复位单个源层（语义同 resetEnvelopes 内 clearLayer，作用于 sourceEnvelopes[slot]；
  // version→0 使后续发布直接可受理）。worker 在重叠中止/主源换代时调用——中断在途轨迹
  // （任务 9 双源腿）后该层立即回到"未受理常量 1.0"，打断即瞬时（裁定）。与回调并发的
  // 窗口：块内执行器快照在块首已锁存，本复位至多让该块末次写回与复位竞态（下块重受理
  // 自愈），无越界/无陈旧轨迹残留。版本清零与发布侧 GainEnvelopeController 单调版本并存：
  // 发布版本 > 0 恒被受理（A5 语义），不会重放旧版本。
  void resetSourceEnvelope(std::size_t slot) noexcept;
  // 回调线程写回值的读回（worker 轮询/账本同步用）。
  float masterEnvelopeGain() const noexcept;
  float sourceEnvelopeGain(std::size_t slot) const noexcept;

  // ---- 第二源发布面（任务 9；worker 线程调用）----
  // 发布第二源 ring（指针 + 代次）+ 源包络（落 sourceEnvelopes[1]，version 由调用方递增），
  // secondActive=true 最后 release —— 内容先就绪、激活标志殿后（同 publishCallbackQueue 序）。
  // 激活后 renderCallback 对两源独立 readIfGeneration（欠载各自补零），D1 渲染序混音。
  // ring 对象生命周期归 worker：本方法不取得所有权；撤销后回调至多再持有一个 block 的
  // 旧指针（代次校验即弃），ring 销毁由 worker 侧延迟回收（本类永不销毁 ring）。
  void activateSecondSource(PcmBufferQueue& queue, const GainEnvelopeSnapshot& sourceEnvelope) noexcept;
  // 撤销第二源：active=false（release）→ 代次递增 → 指针置空。与 deactivateCallbackQueue
  // 同纪律；sourceEnvelopes[1] 的账本保持冻结（下次 activate 发布新包络覆盖 PENDING）。
  void deactivateSecondSource() noexcept;
  // 第二源是否处于激活态（worker 轮询/退役决策用；与回调读取同序）。
  [[nodiscard]] bool secondSourceActive() const noexcept;
  // 当前激活的源包络槽数（任务 9：槽 1 随第二源激活参与执行）。
  static constexpr std::size_t kActiveSourceEnvelopeSlots = 2;

  // ---- EQ 处理链发布面（任务 25 B2.4a + 任务 26 B2.4b + 播放中实时调节）----
  // 均衡器配置存储 + 应用入口（worker 线程调用）。语义：
  //   · eqConfig_ 存储恒执行（停态应用/幂等重放读它）+ PENDING 目标层无条件发布
  //     （eqTargetLayer_，见结构注释）——**运行中（started_）仅发布 PENDING，
  //     由 renderCallback 块首受理**（本任务：受理把目标经 eqDsp_/limiter_ 的
  //     applyTargets 零分配投递到 DSP 平滑，播放中调节实时生效）；
  //   · 停态窗口（未 started_，无活跃回调）：仍即时真实应用
  //     applyEqualizerDspConfig（configure 允许分配——不得与活跃回调并发），
  //     暂停中调整 = 立即生效 + 生效快照发布（既有语义零回归）；
  //   · 未 initialize（设备格式未定）：仅存储 + PENDING 照发，待 initialize() 按
  //     生效格式应用（幂等）。
  // 运行期参数变更不再「仅存储等下次内容边界」——用户实测缺陷（播放中调 EQ 无
  // 声音变化）的根因（started_ 门控仅存储）由 PENDING + 回调受理链修复。
  void setEqualizerConfig(const EqualizerConfig& config) noexcept;
  // 内容边界清理（任务 26 B2.4b）：重建 EQ/限幅 DSP 单元实例（滤波历史/limiter 延迟线/
  // 检测/平滑全清零）并按当前设备格式重新应用存储配置（幂等）。eq_dsp/limiter 无公开
  // 复位 API 且同参数 configure 不清态（fs/声道变化才重建），故以「重建实例 + 重配置」
  // 达成清零，零单元改动。**仅限停态窗口调用**：①initialize() 内、②service 加载路径
  // rebind/协商成功后、③瞬时 seek 重启点——设备已停（无活跃回调，结构性无竞态）。
  // 禁止在 stop()/start()/rebindQueue() 及 T10 运行中交接（completeOverlapHandoff——
  // 设备运行中 rebind）调用：pause/resume/同曲续接保留冻结状态；运行中清空 lookahead
  // 延迟线 = ~5ms 空洞 + EQ 阶跃瞬态，违 F5「切换无爆音」。
  void clearDspState() noexcept;
  // EQ 生效快照一致读回（可跨线程；见 AudioOutputDeviceEqLayer 注释的锁存语义）。
  [[nodiscard]] AudioOutputDeviceEqSnapshot eqAppliedSnapshot() const noexcept;

  // ---- 频谱摘录读侧（任务 29 B3.1；FFT worker（任务 30）/测试调用，无锁）----
  // 最新完整摘录帧的一致拷贝：把发布槽单声道 f32 样本（frameCount 帧）memcpy 到
  // samplesOut（容量 ≥ captureCapacityFrames() 帧），meta 回填该帧元数据。与
  // renderCallback（唯一写侧）并发安全（双缓冲 + 发布序 + 有界重试 ≤3；写侧在
  // 回调内零分配/零锁/零自旋，见私有区注释）。与设备生命周期（initialize/
  // uninitialize）须同线程（与 currentFormat() 同纪律——生命周期调用方 = 音频
  // worker 线程）。返回 false = 尚无完整帧（从未摘录/重建后）或本次未取到一致
  // 视图（写者连续发布/输出缓冲不足）——调用方下轮重试或沿用上一帧；帧的新旧
  // 用 meta.sequence/meta.generation 判断（重复调用同一帧返回相同 sequence）。
  [[nodiscard]] bool latestCaptureFrame(float* samplesOut, std::uint32_t capacityFrames,
                                        AudioOutputDeviceCaptureMeta& meta) const noexcept;
  // 摘录槽每帧样本容量（latestCaptureFrame 输出缓冲最小容量；0 = 尚未 initialize）。
  [[nodiscard]] std::uint32_t captureCapacityFrames() const noexcept;

  static void renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept;
  [[nodiscard]] AudioOutputDeviceCounters counters() const noexcept;

private:
  void publishCallbackQueue(PcmBufferQueue& queue, const AudioDeviceFormat& format) noexcept;
  void deactivateCallbackQueue() noexcept;

  std::unique_ptr<AudioOutputDeviceBackend> backend_;
  AudioOutputDeviceCallbackState callbackState_{};
  // 任务 9：第二源混音暂存（块内瞬态工作区，内容不跨块保留）。容量按「主队列容量 ×
  // 帧字节」在 initialize()（无活跃回调）一次性调整，运行期不再改动；容量 < 当前块
  // 需求时该块退回单源路径（守卫；生产不可达——回调帧数 ≤ 后端 period ≤ 主队列容量）。
  std::vector<std::uint8_t> mixScratch_;
  // ---- EQ f32 暂存（任务 24 B2.3）----
  // EQ 激活路径的转换暂存：int 设备格式样本逐样本转 f32 后先落此（EQ/音量/限幅在
  // f32 域处理，块末一次量化回设备格式；f32 设备格式就地处理，不占用本缓冲）。
  // 内容不跨块保留；容量 = 主队列容量 × 4B（float32）× 声道数，在 initialize()
  // （无活跃回调）一次性调整、运行期不再改动——与 mixScratch_ 同纪律，但不依赖/
  // 不复用 mixScratch_（后者是双源 int 域混音暂存，尺寸随设备位深字节宽变化；
  // 本暂存按固定 4B/f32 分配）。EQ 分支（任务 25 接线）处理帧数 ≤ 后端 period ≤
  // 主队列容量，frameCount×4×ch ≤ 容量恒成立；f32ScratchFits 为未来集成提供越界
  // 防御（装不下 = 该块退回既有路径，同 dualMix 守卫语义）。
  std::vector<std::uint8_t> f32Scratch_;
  // 守卫查询：EQ 块（frameCount 帧 × channelCount 声道 × 4B/f32）能否装入 f32Scratch_。
  [[nodiscard]] bool f32ScratchFits(std::uint32_t frameCount, std::uint16_t channelCount) const noexcept;
  // ---- EQ 处理链状态（任务 25 B2.4a + 任务 26 B2.4b）----
  // DSP 纯核心实例（不透明持有，见头前向声明）：configure（可分配/重建）只发生在
  // setEqualizerConfig()/initialize()（经 clearDspState 重建实例 + applyEqualizerDspConfig
  // 重配置）——无活跃回调窗口（worker 侧契约，见发布面注释）；process（noexcept，
  // 零分配/锁/日志）只发生在 renderCallback EQ 分支。filter/限幅状态跨 stop()/start()
  // 保留（pause/resume 冻结续接纪律）；内容边界经 clearDspState() 显式清零。
  std::unique_ptr<EqualizerDspProcessor> eqDsp_;
  std::unique_ptr<LimiterDspProcessor> limiter_;
  // renderCallback EQ 链进入条件 = DSP 链「需要处理本块」态（无独立发布标志——
  // 运行期目标受理与停态真实应用统一收敛到 DSP 目标/平滑状态，见 .cpp renderCallback
  // 区注释）：eqDsp_ enabled 目标为 true（EQ 开，含全 0dB 曲线的真实 f32 链语义）
  // 或 !fastBypassActive（平滑中/收敛于非零——含 enabled=false 的平滑退出期）或
  // limiter_ enabled 目标为 true / 非旁路（限幅开关/淡化/压限进行中）。出厂全零
  // 稳态（从未开 EQ）三条件恒 false → 回调走既有原路径逐位不变（既有测试锁定）。
  // 读取 = 回调线程对 DSP 成员的同线程读（运行中 DSP 成员仅回调线程写：
  // applyTargets/process；worker configure 只在停态窗口——设备已停无活跃回调）。
  // worker 侧存储的均衡器配置（默认出厂直通）；无回调窗口写，render 不读（只经
  // PENDING 目标层受理）。setEqualizerConfig 恒存储本值——下个 initialize/
  // 内容边界（clearDspState 重配置）与停态即时应用读它。
  EqualizerConfig eqConfig_{};
  // EQ 运行期目标 PENDING 层（本任务）：setEqualizerConfig 无条件发布
  // （publishEqTargetLayer，发布序见结构注释）；renderCallback 块首受理
  // （活跃回调态 + version ≠ 账本 → applyTargets 投递 + 发布生效快照）。
  AudioOutputDeviceEqTargetLayer eqTargetLayer_{};
  // 目标受理账本（已受理/已停态消费的 PENDING version；0 = 从未受理）。写者 =
  // renderCallback 受理（回调线程）+ applyEqualizerDspConfig 停态应用后消费
  // （worker）——两写者不并发（停态应用时无活跃回调；受理仅活跃回调态发生），
  // 原子化仅为跨线程读写的模型卫生。回调侧每块一次 relaxed 读，无额外成本。
  std::atomic<std::uint32_t> latchedEqTargetVersion_{0};
  // EQ 生效快照原子镜像层（任务 26 B2.4b + 实时受理）：停态真实应用
  // （applyEqualizerDspConfig，每次 configure 两单元成功后 publishEqAppliedLayer）
  // 与回调受理（运行期，见 renderCallback 受理注释）双写侧；eqAppliedSnapshot()
  // 跨线程一致读（结构注释为契约）。
  AudioOutputDeviceEqLayer eqAppliedLayer_{};
  // 停态应用辅助：按 currentFormat_ configure 两单元 + 发布生效快照 + 消费目标受理
  // 账本（格式未定 = 仅存储语义，initialize() 在格式确定后调用本方法幂等应用）。
  void applyEqualizerDspConfig() noexcept;
  // ---- 频谱摘录状态（任务 29 B3.1；写 = renderCallback，读 = latestCaptureFrame）----
  // 双缓冲 + 纪元：写侧（设备线程）每回调至多一帧——把最新可听信号均值下混为
  // 单声道 f32 写入非发布槽 → captureCommitSlot 填元数据后 release 翻转
  // capturePublishedSlot_（发布序仿 publishCallbackQueue：内容/元数据先就绪、
  // 索引翻转殿后）。读侧（worker）：acquire 取槽 → 拷贝样本 + 元数据 → 复核翻转
  // 未回绕到本槽（写者须连续两轮发布才可能覆写正在拷贝的槽：µs 级拷贝 vs
  // ~10ms 块周期实际不可达；复核失败有界重试 ≤3）。槽样本为普通 float——读/
  // 写潜在重叠仅发生在读侧被抢占 ≥2 块周期（不可达），复核即弃。
  // captureGeneration_ 纪元：initialize()（格式/容量重协商）时 captureReset() ++
  // 并双槽清零（防混率帧被消费）；rebindQueue/T10 交接、stop()/seek 重启不清
  // （交接不是重建；暂停冻结续接纪律）。
  struct CaptureSlotMeta {
    std::atomic<std::uint32_t> sequence{0};
    std::atomic<std::uint32_t> frameCount{0};
    std::atomic<std::uint32_t> sampleRate{0};
    std::atomic<AudioOutputDeviceCaptureDomain> domain{AudioOutputDeviceCaptureDomain::ChainInactiveOutput};
  };
  std::vector<float> captureBuffers_;            // 2 × captureCapacityFrames_（槽 0/1 连续）
  std::uint32_t captureCapacityFrames_{0};       // 每槽帧容量（initialize 按主队列容量定）
  std::uint32_t captureNextSequence_{0};         // 写侧帧序（回调线程独占；每发布帧 +1）
  std::atomic<std::uint32_t> capturePublishedSlot_{0};  // 0/1：最新完整帧所在槽；翻转 = 发布
  std::atomic<std::uint32_t> captureGeneration_{0};     // 纪元（initialize ++；跨代帧禁混用）
  CaptureSlotMeta captureSlotMeta_[2]{};         // 槽元数据（发布前 relaxed 写、acquire 后读）
  // 摘录纪元推进（initialize 停态窗口调用，无活跃回调；见上注释）。
  void captureReset(std::uint32_t capacityFrames) noexcept;
  // 槽提交（发布序尾：元数据 relaxed 写全 → 索引 release 翻转）。
  void captureCommitSlot(std::uint32_t slot, std::uint32_t frameCount,
                         AudioOutputDeviceCaptureDomain domain) noexcept;
  // 回调线程专用发布（零分配/零锁/零日志；blockFrames==0/容量不足 → 直接返回）：
  // f32 交织源前 validFrames 帧均值下混 + 尾部清零至 blockFrames（静音帧传
  // interleaved=nullptr、validFrames=0——全零帧照常发布）。
  void capturePublishF32(const float* interleaved, std::uint32_t validFrames, std::uint32_t blockFrames,
                         std::uint16_t channelCount, AudioOutputDeviceCaptureDomain domain) noexcept;
  // 链关闭态发布：设备输出域前 validFrames 帧逐格式 int→f32（任务 24 冻结读侧
  // 尺度）均值下混 + 尾部清零（f32 设备直取）。
  void capturePublishOutputDomain(const void* output, std::uint32_t validFrames, std::uint32_t blockFrames,
                                  std::uint16_t channelCount, AudioSampleFormat sampleFormat) noexcept;
  std::atomic<std::uint64_t> callbackCount_{0};
  std::atomic<std::uint64_t> requestedFrames_{0};
  std::atomic<std::uint64_t> copiedFrames_{0};
  std::atomic<std::uint64_t> silenceFrames_{0};
  std::atomic<float> volume_{1.0F};
  std::atomic<bool> muted_{false};
  AudioDeviceFormat currentFormat_{};
  PcmBufferQueue* currentQueue_{nullptr};
  std::optional<AudioOutputDeviceError> lastError_{};
  bool initialized_{false};
  bool started_{false};
};

std::unique_ptr<AudioOutputDeviceBackend> makeMiniaudioOutputDeviceBackend();

}
