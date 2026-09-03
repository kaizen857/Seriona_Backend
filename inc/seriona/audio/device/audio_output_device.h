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
