#pragma once

#include "seriona/audio/device/audio_output_device.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace seriona::audio {

// dip 对半计划：下行（当前增益 → dipFloor）+ 上行（dipFloor → 1.0）。两段时长 = 总/2。
// 任务 11 手动 dip 使用；本结构只携带参数，发布时序由调用方（worker）执行。
struct GainDipPlan {
  GainEnvelopeSnapshot down;
  GainEnvelopeSnapshot up;
};

// worker 侧包络账本（任务 5）：唯一真源，工作于音频 worker 线程。产出供
// AudioOutputDevice::setMasterEnvelope/setSourceEnvelope 发布的快照；
// version 每次产出递增（回调侧版本账本据此受理新包络）。
class GainEnvelopeController {
public:
  explicit GainEnvelopeController(float initialGain = 1.0F) noexcept;

  // 产出 ramp 快照：targetGain 钳制 [0,1]（NaN 按 1.0 处理）；durationMs==0 → durationFrames=0
  // （即时）；ms→frames = round(ms * sampleRate / 1000)；startGain=当前账本增益；version++。
  [[nodiscard]] GainEnvelopeSnapshot makeRampSnapshot(float targetGain,
                                                      std::chrono::milliseconds durationMs,
                                                      GainEnvelopeCurve curve,
                                                      std::uint32_t sampleRate) noexcept;
  // 产出 dip 对半计划：下行 startGain=账本当前 → dipFloor；上行 dipFloor → 1.0；时长各 totalMs/2
  // （向下取整毫秒）。版本连续（down 用 n、up 用 n+1）。
  [[nodiscard]] GainDipPlan makeDipPlan(std::chrono::milliseconds totalMs,
                                        GainEnvelopeCurve curve,
                                        std::uint32_t sampleRate,
                                        float dipFloor = 0.0F) noexcept;

  // 账本当前增益（worker 侧参考；回调真实值经 AudioOutputDevice::*EnvelopeGain 读回后 sync）。
  float currentGain() const noexcept;
  void syncCurrentGain(float gain) noexcept;
  std::uint32_t version() const noexcept;

private:
  float currentGain_;
  float targetGain_; // 最近一次 makeRampSnapshot 的目标（dip 上行回 1.0 不依赖它，保留以备后续任务）
  std::uint32_t version_;
};

} // namespace seriona::audio
