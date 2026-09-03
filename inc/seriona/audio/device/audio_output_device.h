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
  std::array<GainEnvelopeLayerState, 2> sourceEnvelopes;          // 源增益层；槽 0 激活、槽 1 预留（任务 9）
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
  // slot >= kActiveSourceEnvelopeSlots 的发布被忽略（槽 1 预留任务 9）。
  void setSourceEnvelope(std::size_t slot, const GainEnvelopeSnapshot& snapshot) noexcept;
  // 清 PENDING 与 EXEC 全部 12 字段（含 currentGain→1.0 与 exec* 锁存；stop 后不滞留末次淡变值，
  // 防陈旧快照与陈旧起点）。stop()/uninitialize() 内调用（设备已停、无活跃回调，无竞争）。
  void resetEnvelopes() noexcept;
  // 回调线程写回值的读回（worker 轮询/账本同步用）。
  float masterEnvelopeGain() const noexcept;
  float sourceEnvelopeGain(std::size_t slot) const noexcept;
  // 当前激活的源包络槽数（槽 1 编译期关闭）。
  static constexpr std::size_t kActiveSourceEnvelopeSlots = 1;

  static void renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept;
  [[nodiscard]] AudioOutputDeviceCounters counters() const noexcept;

private:
  void publishCallbackQueue(PcmBufferQueue& queue, const AudioDeviceFormat& format) noexcept;
  void deactivateCallbackQueue() noexcept;

  std::unique_ptr<AudioOutputDeviceBackend> backend_;
  AudioOutputDeviceCallbackState callbackState_{};
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
