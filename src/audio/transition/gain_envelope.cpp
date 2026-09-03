// 任务 5：worker 侧增益包络账本（GainEnvelopeController）实现。
// 本文件无第三方依赖：控制器只维护纯标量账本并产出快照（纯 POD），
// 发布动作由调用方经 AudioOutputDevice::setMasterEnvelope/setSourceEnvelope 执行，
// 本文件不触碰设备状态、不参与实时回调路径。

#include "gain_envelope.h"

#include <algorithm>
#include <cmath>

namespace seriona::audio {
namespace {

// 增益钳制 [0,1]；NaN 按 1.0 处理（见 GainEnvelopeController::makeRampSnapshot 文档）。
float clampEnvelopeGain(float gain) noexcept {
  return std::isnan(gain) ? 1.0F : std::clamp(gain, 0.0F, 1.0F);
}

} // namespace

GainEnvelopeController::GainEnvelopeController(float initialGain) noexcept
    : currentGain_(clampEnvelopeGain(initialGain)),
      targetGain_(clampEnvelopeGain(initialGain)),
      version_(0U) {}

GainEnvelopeSnapshot GainEnvelopeController::makeRampSnapshot(float targetGain,
                                                              std::chrono::milliseconds durationMs,
                                                              GainEnvelopeCurve curve,
                                                              std::uint32_t sampleRate) noexcept {
  const float clampedTarget = clampEnvelopeGain(targetGain);

  std::uint32_t durationFrames = 0U;
  if (durationMs.count() != 0) {
    // ms→frames = round(ms * sampleRate / 1000)；durationMs==0 → durationFrames=0（即时完成）。
    durationFrames = static_cast<std::uint32_t>(
        std::llround(static_cast<double>(durationMs.count()) * sampleRate / 1000.0));
  }

  GainEnvelopeSnapshot snapshot{};
  snapshot.targetGain = clampedTarget;
  snapshot.startGain = currentGain_; // 账本参考值；回调执行器以自身 currentGain 为 ramp 起点。
  snapshot.durationFrames = durationFrames;
  snapshot.curve = curve;
  snapshot.version = ++version_;
  targetGain_ = clampedTarget;
  // 账本增益不在此推演：ramp 完成后由 worker 经 syncCurrentGain 同步回调真实值。
  return snapshot;
}

GainDipPlan GainEnvelopeController::makeDipPlan(std::chrono::milliseconds totalMs,
                                                GainEnvelopeCurve curve,
                                                std::uint32_t sampleRate,
                                                float dipFloor) noexcept {
  const auto halfMs = totalMs / 2;
  GainDipPlan plan{};
  plan.down = makeRampSnapshot(clampEnvelopeGain(dipFloor), halfMs, curve, sampleRate);
  // 奇数毫秒余量归上行（totalMs - halfMs ≥ halfMs）。
  plan.up = makeRampSnapshot(1.0F, totalMs - halfMs, curve, sampleRate);
  return plan;
}

float GainEnvelopeController::currentGain() const noexcept { return currentGain_; }

void GainEnvelopeController::syncCurrentGain(float gain) noexcept { currentGain_ = gain; }

std::uint32_t GainEnvelopeController::version() const noexcept { return version_; }

} // namespace seriona::audio
