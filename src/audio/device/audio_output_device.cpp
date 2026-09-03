#include "seriona/audio/device/audio_output_device.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace seriona::audio {
namespace {

std::uint32_t bytesPerSample(AudioSampleFormat format) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U;
  case AudioSampleFormat::Int24:
    return 3U;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U;
  case AudioSampleFormat::Unknown:
    return 0U;
  }
  return 0U;
}

void applyInt16Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<std::int16_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const auto scaled = std::lround(static_cast<float>(samples[index]) * volume);
    samples[index] = static_cast<std::int16_t>(std::clamp<long>(scaled,
                                                               std::numeric_limits<std::int16_t>::min(),
                                                               std::numeric_limits<std::int16_t>::max()));
  }
}

void applyInt32Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<std::int32_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const auto scaled = std::llround(static_cast<double>(samples[index]) * static_cast<double>(volume));
    samples[index] = static_cast<std::int32_t>(std::clamp<long long>(scaled,
                                                                    std::numeric_limits<std::int32_t>::min(),
                                                                    std::numeric_limits<std::int32_t>::max()));
  }
}

void applyFloat32Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<float*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    samples[index] *= volume;
  }
}

// --- Int24（3 字节小端打包）实时增益 -------------------------------------------------
// 队列中的 S24 内容由 ffmpeg_filter_pipeline.cpp 的 packS32ToS24 生成：FFmpeg aformat
// 输出的左对齐 S32（24 位内容占用高 24 位）取高 24 位打包为 3 字节小端（LSB 在前，
// 第 3 字节高位为符号位），即 ma_format_s24 的字节序约定。下述解包/打包辅助与其
// 字节序一致，且自写实现（不搬 miniaudio 内部 3 字节代码，任务 4 裁定）。
//
// 中间域策略：解包还原为「左对齐 S32」后，增益数学与 applyInt32Gain 完全同构
// （double 乘积 + llround + ±2^31 clamp），因此同一 24 位内容在 Int24 与 Int32
// 路径下增益输出逐样本一致（等价验收基准）。值域检查：|样本| ≤ 2^23，volume 被
// clamp 到 [0,1]，故增益后 |y| ≤ 2^31 - 256，永不触碰 clamp 边界（clamp 保留仅为
// 与 Int32 参考路径语义一致）。
//
// 任务 9 预留入口：双源混音在「加宽样本域」执行——两路样本各自解包（unpackS24…）
// 后在 Int64 域求和（两路 |x| ≤ 2^31-256，和 ≤ 2^32-512，Int64 无溢出），clamp 回
// ±2^31 后经 packLeftAlignedS32ToS24 逐样本落回 3 字节。本函数与上述打包辅助同处
// 本 TU 匿名命名空间，任务 9 混音器直接复用，零分配。

std::int32_t unpackS24ToLeftAlignedS32(const std::uint8_t* bytes) noexcept {
  // 3 字节小端（bytes[0] 最低位）拼出无符号 24 位值。
  const auto raw24 = static_cast<std::uint32_t>(bytes[0]) |
                     (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16U);
  // 符号扩展到有符号 24 位（int32 表示范围 -2^23..2^23-1 内，减 0x1000000 无溢出）。
  const auto signed24 = raw24 >= 0x800000U ? static_cast<std::int32_t>(raw24) - 0x1000000
                                           : static_cast<std::int32_t>(raw24);
  // 左移 8 位左对齐到 S32 域：与 packS32ToS24 的输入布局一致（|signed24| ≤ 2^23，移位无溢出）。
  return signed24 << 8;
}

void packLeftAlignedS32ToS24(std::int32_t sample32, std::uint8_t* bytes) noexcept {
  // 与 ffmpeg_filter_pipeline.cpp packS32ToS24 同序：取 S32 高 24 位（对负数等价于
  // 算术右移后取低 24 位），写为 3 字节小端。本函数即任务 9 的「累加后打包」入口。
  const auto high24 = static_cast<std::uint32_t>(sample32) >> 8U;
  bytes[0] = static_cast<std::uint8_t>(high24 & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((high24 >> 8U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((high24 >> 16U) & 0xFFU);
}

void applyInt24Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* bytes = static_cast<std::uint8_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    auto* const sample = bytes + index * 3U;
    // 解包（符号扩展 → 左对齐 S32）→ 加宽样本域增益（与 applyInt32Gain 同数学）
    // → 打包回 3 字节小端。输出字节序/布局与输入一致，仅补全增益语义。
    const auto sample32 = unpackS24ToLeftAlignedS32(sample);
    const auto scaled = std::llround(static_cast<double>(sample32) * static_cast<double>(volume));
    const auto gained = static_cast<std::int32_t>(std::clamp<long long>(
        scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    packLeftAlignedS32ToS24(gained, sample);
  }
}

void applyGain(void* output,
               std::uint32_t frameCount,
               std::uint16_t channelCount,
               AudioSampleFormat sampleFormat,
               float volume,
               bool muted) noexcept {
  if (output == nullptr || frameCount == 0U || channelCount == 0U) {
    return;
  }
  if (muted || volume <= 0.0F) {
    std::memset(output, 0, static_cast<std::size_t>(frameCount) * channelCount * bytesPerSample(sampleFormat));
    return;
  }
  if (volume == 1.0F) {
    return;
  }
  switch (sampleFormat) {
  case AudioSampleFormat::Int16:
    applyInt16Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Int24:
    // 补全 S24 增益缺口：队列内容为 3 字节小端打包样本，音量在回调内以加宽样本域
    // 完成（解包→增益→打包）。静音/音量 0 与 volume==1.0 已在上方早退覆盖。
    // （旧注释声称"音量由 miniaudio 设备侧控制"，核查确认后端从未调用
    // ma_device_set_master_volume，设备侧恒 1.0——S24 实际无任何音量源，此为补全。）
    applyInt24Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Int32:
    applyInt32Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Float32:
    applyFloat32Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Unknown:
    return;
  }
}

// --- 逐帧增益应用族（任务 5-B2 活动路径）------------------------------------------------
// gainAt(f) = 第 f 帧的组合增益（帧内各声道同增益），帧外层 / 声道内层循环。各格式数学与
// 上方单值族逐项对应（Int16/Float32：float 域；Int32/Int24：double 加宽域），因此包络静止
// 于 1.0 时活动路径与单值族逐位一致（IEEE 乘 1.0 精确，round/clamp/打包往返恒等）。
// gainAt 为模板形参、调用点内联展开：零 std::function、零分配、零间接。
constexpr float kEnvelopeHalfPi = 1.5707963267948966F; // π/2：等功率 cos/sin 相位插值步进角

template <typename GainAt>
void applyInt16FrameGains(void* output, std::uint32_t frameCount, std::uint16_t channelCount, GainAt gainAt) noexcept {
  auto* samples = static_cast<std::int16_t*>(output);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = gainAt(frame);
    auto* const frameBase = samples + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      auto& sample = frameBase[ch];
      const auto scaled = std::lround(static_cast<float>(sample) * gain);
      sample = static_cast<std::int16_t>(
          std::clamp<long>(scaled, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }
  }
}

template <typename GainAt>
void applyInt24FrameGains(void* output, std::uint32_t frameCount, std::uint16_t channelCount, GainAt gainAt) noexcept {
  auto* bytes = static_cast<std::uint8_t*>(output);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = gainAt(frame);
    auto* const frameBase = bytes + static_cast<std::size_t>(frame) * channels * 3U;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      auto* const sample = frameBase + ch * 3U;
      // 解包（符号扩展 → 左对齐 S32）→ 加宽样本域增益 → 打包回 3 字节小端（同 applyInt24Gain 数学）。
      const auto sample32 = unpackS24ToLeftAlignedS32(sample);
      const auto scaled = std::llround(static_cast<double>(sample32) * static_cast<double>(gain));
      const auto gained = static_cast<std::int32_t>(std::clamp<long long>(
          scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
      packLeftAlignedS32ToS24(gained, sample);
    }
  }
}

template <typename GainAt>
void applyInt32FrameGains(void* output, std::uint32_t frameCount, std::uint16_t channelCount, GainAt gainAt) noexcept {
  auto* samples = static_cast<std::int32_t*>(output);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = gainAt(frame);
    auto* const frameBase = samples + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      auto& sample = frameBase[ch];
      const auto scaled = std::llround(static_cast<double>(sample) * static_cast<double>(gain));
      sample = static_cast<std::int32_t>(std::clamp<long long>(
          scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    }
  }
}

template <typename GainAt>
void applyFloat32FrameGains(void* output, std::uint32_t frameCount, std::uint16_t channelCount, GainAt gainAt) noexcept {
  auto* samples = static_cast<float*>(output);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = gainAt(frame);
    auto* const frameBase = samples + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      frameBase[ch] *= gain;
    }
  }
}

template <typename GainAt>
void applyFrameGains(void* output,
                     std::uint32_t frameCount,
                     std::uint16_t channelCount,
                     AudioSampleFormat sampleFormat,
                     GainAt gainAt) noexcept {
  if (output == nullptr || frameCount == 0U || channelCount == 0U) {
    return;
  }
  switch (sampleFormat) {
  case AudioSampleFormat::Int16:
    applyInt16FrameGains(output, frameCount, channelCount, gainAt);
    return;
  case AudioSampleFormat::Int24:
    applyInt24FrameGains(output, frameCount, channelCount, gainAt);
    return;
  case AudioSampleFormat::Int32:
    applyInt32FrameGains(output, frameCount, channelCount, gainAt);
    return;
  case AudioSampleFormat::Float32:
    applyFloat32FrameGains(output, frameCount, channelCount, gainAt);
    return;
  case AudioSampleFormat::Unknown:
    return;
  }
}

void fillSilence(void* output, std::uint32_t frameCount, std::uint32_t bytesPerFrame) noexcept {
  if (output == nullptr || frameCount == 0U || bytesPerFrame == 0U) {
    return;
  }

  std::memset(output, 0, static_cast<std::size_t>(frameCount) * bytesPerFrame);
}

// 包络发布（worker 线程）：防御性校验后写字段，version 最后 release 发布。
// 回调侧受理规则见 renderCallback 执行器（任务 5-B2）；startGain 是 worker 账本参考，
// 回调受理时会覆写为自身 currentGain（连续性保证）。
void publishEnvelopeLayer(GainEnvelopeLayerState& layer, const GainEnvelopeSnapshot& snapshot) noexcept {
  if (std::isnan(snapshot.targetGain)) return; // NaN 防御：整次发布忽略
  const float target = std::clamp(snapshot.targetGain, 0.0F, 1.0F);
  const float start = std::clamp(snapshot.startGain, 0.0F, 1.0F);
  const std::uint32_t duration = snapshot.durationFrames;
  layer.targetGain.store(target, std::memory_order_relaxed);
  layer.startGain.store(start, std::memory_order_relaxed);
  layer.durationFrames.store(duration, std::memory_order_relaxed);
  layer.curve.store(snapshot.curve, std::memory_order_relaxed);
  layer.version.store(snapshot.version, std::memory_order_release);
}

// 单层全字段复位（resetEnvelopes 与 resetSourceEnvelope 共用）：清 PENDING 与 EXEC
// （含 currentGain→1.0、latchedVersion→0、exec* 锁存）——设备已停时无竞争；设备运行中
// 的槽级复位与回调并发窗口见 resetSourceEnvelope 头注释（块首快照已锁存，下块自愈）。
void clearEnvelopeLayer(GainEnvelopeLayerState& layer) noexcept {
  layer.version.store(0, std::memory_order_release);
  layer.targetGain.store(1.0F, std::memory_order_relaxed);
  layer.startGain.store(1.0F, std::memory_order_relaxed);
  layer.durationFrames.store(0, std::memory_order_relaxed);
  layer.curve.store(GainEnvelopeCurve::Linear, std::memory_order_relaxed);
  layer.currentGain.store(1.0F, std::memory_order_relaxed);
  layer.rampFramesDone.store(0, std::memory_order_relaxed);
  layer.latchedVersion.store(0, std::memory_order_relaxed);
  layer.execStartGain.store(1.0F, std::memory_order_relaxed);
  layer.execTargetGain.store(1.0F, std::memory_order_relaxed);
  layer.execDurationFrames.store(0U, std::memory_order_relaxed);
  layer.execCurve.store(GainEnvelopeCurve::Linear, std::memory_order_relaxed);
}

// --- 包络执行器账本（任务 5-B2；仅回调线程调用，EXEC/进度域字段回调独占写）----------------
// 块内执行轨迹的一次性快照（受理/拒绝后稳定，块内逐帧复用；回调线程独占写 exec*，块内无竞态）。
struct ExecutedTrajectory {
  bool latched{false};          // latchedVersion != 0（本层受理过包络）
  float start{1.0F};            // execStartGain（受理时自 currentGain 锁存）
  float target{1.0F};           // execTargetGain
  std::uint32_t duration{0U};   // execDurationFrames（0 = 即时）
  GainEnvelopeCurve curve{GainEnvelopeCurve::Linear};
  std::uint32_t basePos{0U};    // 块首 rampFramesDone
};

// 受理/拒绝账本：version != latched 且无在途轨迹 → 把 PENDING 锁存到 EXEC（起点取回调真值
// currentGain——连续性保证，绝不取 snapshot.startGain），复位进度并记 latchedVersion；
// 否则原样继承执行轨迹（拒绝时字段不动，在途轨迹结束后下一块自动受理最新 PENDING）。
ExecutedTrajectory planEnvelopeLayer(GainEnvelopeLayerState& layer) noexcept {
  ExecutedTrajectory trajectory{};
  const auto version = layer.version.load(std::memory_order_acquire);
  const auto latched = layer.latchedVersion.load(std::memory_order_relaxed);
  const auto framesDone = layer.rampFramesDone.load(std::memory_order_relaxed);
  const auto execDuration = layer.execDurationFrames.load(std::memory_order_relaxed);
  const bool inFlight = latched != 0U && framesDone < execDuration;
  if (version != latched && !inFlight) {
    trajectory.latched = version != 0U;
    trajectory.start = std::clamp(layer.currentGain.load(std::memory_order_relaxed), 0.0F, 1.0F);
    trajectory.target = layer.targetGain.load(std::memory_order_relaxed);
    trajectory.duration = layer.durationFrames.load(std::memory_order_relaxed);
    trajectory.curve = layer.curve.load(std::memory_order_relaxed);
    layer.execStartGain.store(trajectory.start, std::memory_order_relaxed);
    layer.execTargetGain.store(trajectory.target, std::memory_order_relaxed);
    layer.execDurationFrames.store(trajectory.duration, std::memory_order_relaxed);
    layer.execCurve.store(trajectory.curve, std::memory_order_relaxed);
    layer.rampFramesDone.store(0U, std::memory_order_relaxed);
    layer.latchedVersion.store(version, std::memory_order_relaxed);
  } else {
    trajectory.latched = latched != 0U;
    trajectory.start = layer.execStartGain.load(std::memory_order_relaxed);
    trajectory.target = layer.execTargetGain.load(std::memory_order_relaxed);
    trajectory.duration = execDuration;
    trajectory.curve = layer.execCurve.load(std::memory_order_relaxed);
    trajectory.basePos = framesDone;
  }
  return trajectory;
}

// 执行轨迹第 frameIndex 帧的层增益：未受理 → 恒等 1.0；即时 → 直落目标；轨迹外 → 持于目标。
float trajectoryGain(const ExecutedTrajectory& t, std::uint32_t frameIndex) noexcept {
  if (!t.latched) {
    return 1.0F;
  }
  if (t.duration == 0U) {
    return t.target;
  }
  const auto pos = t.basePos + frameIndex;
  if (pos >= t.duration) {
    return t.target;
  }
  const float progress = static_cast<float>(pos) / static_cast<float>(t.duration);
  if (t.curve == GainEnvelopeCurve::EqualPowerPair) {
    // 等功率对：θ = p·(π/2)，g = cos(θ)·start + sin(θ)·target；与对偶层满足 g1²+g2²=1，
    // 中点各 -3dB≈0.7071（同头文件注释）。
    const float theta = progress * kEnvelopeHalfPi;
    return std::cos(theta) * t.start + std::sin(theta) * t.target;
  }
  return t.start + (t.target - t.start) * progress;
}

// 块末一次性写回（relaxed；每块一次；进度封顶 duration，防 32 位进度回绕造成假在途）。
// copiedFrames == 0 的纯静音块不推进（无出声帧，轨迹保持原位，恢复出声后续跑）。
void finalizeTrajectory(GainEnvelopeLayerState& layer,
                        const ExecutedTrajectory& t,
                        std::uint32_t copiedFrames) noexcept {
  if (!t.latched) {
    return;
  }
  if (t.duration == 0U) {
    // 即时包络：无轨迹推进，currentGain 立即落目标。
    layer.currentGain.store(t.target, std::memory_order_relaxed);
    return;
  }
  if (copiedFrames == 0U) {
    return;
  }
  layer.rampFramesDone.store(std::min(t.basePos + copiedFrames, t.duration), std::memory_order_relaxed);
  layer.currentGain.store(trajectoryGain(t, copiedFrames - 1U), std::memory_order_relaxed);
}

// --- 双源逐帧混音（任务 9；D1 渲染序）------------------------------------------------------
// 输入就位：output = 源 0 整块（readIfGeneration 结果，欠载尾已补零）；secondSrc = 源 1 整块
// （同纪律）。逐帧：两腿各按自身包络轨迹缩放 → 加宽样本域求和 → master×volume → 落回
// output。执行器三件套（planEnvelopeLayer/trajectoryGain/finalizeTrajectory）原样复用，
// 腿增益在帧循环内联求值（零分配、零锁、零间接）。求和域按格式加宽：
//   Int16：leg 数学同 applyInt16FrameGains（float+lround），int32 域求和（|sum| ≤ 2^16）；
//   Int32：leg 数学同 applyInt32FrameGains（double+llround），int64 域求和（|sum| ≤ 2^32）；
//   Int24：解包左对齐 → leg 数学同 applyInt24FrameGains → int64 域求和（两路 |x| ≤ 2^31-256，
//     和 ≤ 2^32-512 无溢出；见 unpackS24ToLeftAlignedS32 区注释）→ clamp ±2^31 →
//     packLeftAlignedS32ToS24 落回（任务 4 预留的累加后打包入口）；
//   Float32：float 域直接求和。
// 混音覆盖整块 frameCount（含两路补零尾：零 × 腿增益 = 零——整数域 round(0)=0 逐位中性；
// float 域零尾贡献 +0.0×g≥0=+0.0，除 ±0.0F 符号边缘外逐位中性：(-0.0·g0)+(+0.0·g1)=+0.0
// ≠ 单源路径的 -0.0 原样保留）；muted/音量 0 走调用方 memset 早退，与单源活动路径同纪律。
void mixDualInt16Frames(void* output,
                        const void* secondSrc,
                        std::uint32_t frameCount,
                        std::uint16_t channelCount,
                        const ExecutedTrajectory& source0,
                        const ExecutedTrajectory& source1,
                        const ExecutedTrajectory& master,
                        float volume) noexcept {
  auto* out = static_cast<std::int16_t*>(output);
  auto const* src1 = static_cast<std::int16_t const*>(secondSrc);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float leg0 = trajectoryGain(source0, frame);
    const float leg1 = trajectoryGain(source1, frame);
    const float masterVol = trajectoryGain(master, frame) * volume;
    auto* const frameOut = out + static_cast<std::size_t>(frame) * channels;
    auto const* const frame1 = src1 + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      const auto from0 = std::lround(static_cast<float>(frameOut[ch]) * leg0);
      const auto from1 = std::lround(static_cast<float>(frame1[ch]) * leg1);
      const auto summed = from0 + from1;
      const auto scaled = std::lround(static_cast<float>(summed) * masterVol);
      frameOut[ch] = static_cast<std::int16_t>(
          std::clamp<long>(scaled, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
    }
  }
}

void mixDualInt24Frames(void* output,
                        const void* secondSrc,
                        std::uint32_t frameCount,
                        std::uint16_t channelCount,
                        const ExecutedTrajectory& source0,
                        const ExecutedTrajectory& source1,
                        const ExecutedTrajectory& master,
                        float volume) noexcept {
  auto* outBytes = static_cast<std::uint8_t*>(output);
  auto const* src1Bytes = static_cast<std::uint8_t const*>(secondSrc);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float leg0 = trajectoryGain(source0, frame);
    const float leg1 = trajectoryGain(source1, frame);
    const float masterVol = trajectoryGain(master, frame) * volume;
    auto* const frameOut = outBytes + static_cast<std::size_t>(frame) * channels * 3U;
    auto const* const frame1 = src1Bytes + static_cast<std::size_t>(frame) * channels * 3U;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      auto* const sampleOut = frameOut + ch * 3U;
      auto const* const sample1 = frame1 + ch * 3U;
      const auto sample32 = unpackS24ToLeftAlignedS32(sampleOut);
      const auto other32 = unpackS24ToLeftAlignedS32(sample1);
      const auto from0 = std::llround(static_cast<double>(sample32) * static_cast<double>(leg0));
      const auto from1 = std::llround(static_cast<double>(other32) * static_cast<double>(leg1));
      const auto summed = from0 + from1;  // |sum| ≤ 2^32-512，int64 无溢出
      const auto scaled = std::llround(static_cast<double>(summed) * static_cast<double>(masterVol));
      const auto clamped = static_cast<std::int32_t>(std::clamp<long long>(
          scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
      packLeftAlignedS32ToS24(clamped, sampleOut);
    }
  }
}

void mixDualInt32Frames(void* output,
                        const void* secondSrc,
                        std::uint32_t frameCount,
                        std::uint16_t channelCount,
                        const ExecutedTrajectory& source0,
                        const ExecutedTrajectory& source1,
                        const ExecutedTrajectory& master,
                        float volume) noexcept {
  auto* out = static_cast<std::int32_t*>(output);
  auto const* src1 = static_cast<std::int32_t const*>(secondSrc);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float leg0 = trajectoryGain(source0, frame);
    const float leg1 = trajectoryGain(source1, frame);
    const float masterVol = trajectoryGain(master, frame) * volume;
    auto* const frameOut = out + static_cast<std::size_t>(frame) * channels;
    auto const* const frame1 = src1 + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      const auto from0 = std::llround(static_cast<double>(frameOut[ch]) * static_cast<double>(leg0));
      const auto from1 = std::llround(static_cast<double>(frame1[ch]) * static_cast<double>(leg1));
      const auto summed = from0 + from1;
      const auto scaled = std::llround(static_cast<double>(summed) * static_cast<double>(masterVol));
      frameOut[ch] = static_cast<std::int32_t>(std::clamp<long long>(
          scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    }
  }
}

void mixDualFloat32Frames(void* output,
                          const void* secondSrc,
                          std::uint32_t frameCount,
                          std::uint16_t channelCount,
                          const ExecutedTrajectory& source0,
                          const ExecutedTrajectory& source1,
                          const ExecutedTrajectory& master,
                          float volume) noexcept {
  auto* out = static_cast<float*>(output);
  auto const* src1 = static_cast<float const*>(secondSrc);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float leg0 = trajectoryGain(source0, frame);
    const float leg1 = trajectoryGain(source1, frame);
    const float masterVol = trajectoryGain(master, frame) * volume;
    auto* const frameOut = out + static_cast<std::size_t>(frame) * channels;
    auto const* const frame1 = src1 + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      const auto from0 = frameOut[ch] * leg0;
      const auto from1 = frame1[ch] * leg1;
      frameOut[ch] = (from0 + from1) * masterVol;
    }
  }
}

void mixDualFrames(void* output,
                   const void* secondSrc,
                   std::uint32_t frameCount,
                   std::uint16_t channelCount,
                   AudioSampleFormat sampleFormat,
                   const ExecutedTrajectory& source0,
                   const ExecutedTrajectory& source1,
                   const ExecutedTrajectory& master,
                   float volume) noexcept {
  if (output == nullptr || secondSrc == nullptr || frameCount == 0U || channelCount == 0U) {
    return;
  }
  switch (sampleFormat) {
  case AudioSampleFormat::Int16:
    mixDualInt16Frames(output, secondSrc, frameCount, channelCount, source0, source1, master, volume);
    return;
  case AudioSampleFormat::Int24:
    mixDualInt24Frames(output, secondSrc, frameCount, channelCount, source0, source1, master, volume);
    return;
  case AudioSampleFormat::Int32:
    mixDualInt32Frames(output, secondSrc, frameCount, channelCount, source0, source1, master, volume);
    return;
  case AudioSampleFormat::Float32:
    mixDualFloat32Frames(output, secondSrc, frameCount, channelCount, source0, source1, master, volume);
    return;
  case AudioSampleFormat::Unknown:
    return;
  }
}


}

AudioOutputDevice::AudioOutputDevice(std::unique_ptr<AudioOutputDeviceBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_) {
    backend_ = makeMiniaudioOutputDeviceBackend();
  }
}

AudioOutputDevice::~AudioOutputDevice() { uninitialize(); }

std::vector<AudioDeviceFormat> AudioOutputDevice::enumeratePlaybackDevices() {
  return backend_->enumeratePlaybackDevices();
}

bool AudioOutputDevice::initialize(const AudioOutputDeviceOpenRequest& request) {
  lastError_.reset();
  if (request.pcmQueue == nullptr || request.sampleRate == 0U || request.channelCount == 0U ||
      request.bufferFrames == 0U) {
    spdlog::error("device init failed: invalid request (rate={} ch={} bufFrames={} queue={})",
                  request.sampleRate, request.channelCount, request.bufferFrames,
                  static_cast<bool>(request.pcmQueue));
    lastError_ = AudioOutputDeviceError{PlaybackErrorCode::FormatNegotiationFailed,
                                        "audio output device request is invalid",
                                        "pcm queue, sample rate, channel count, and buffer frames must be set"};
    return false;
  }

  if (initialized_) {
    uninitialize();
  }

  auto backendRequest = request;
  backendRequest.callbackUserData = this;
  if (!backend_->initialize(backendRequest)) {
    spdlog::error("device init failed: backend init returned false");
    lastError_ = backend_->lastError();
    return false;
  }

  currentFormat_ = backend_->currentFormat();
  currentQueue_ = request.pcmQueue;
  // 任务 9：双源混音暂存容量 = 主队列容量 × 帧字节。只在此处（initialize，无活跃回调）
  // 调整——rebind/start 的重发布在设备运行期可能发生，绝不边跑边改回调工作区。
  mixScratch_.resize(static_cast<std::size_t>(request.pcmQueue->capacityFrames()) *
                     bytesPerSample(currentFormat_.sampleFormat) * currentFormat_.channelCount);
  publishCallbackQueue(*request.pcmQueue, currentFormat_);
  callbackCount_.store(0U, std::memory_order_relaxed);
  requestedFrames_.store(0U, std::memory_order_relaxed);
  copiedFrames_.store(0U, std::memory_order_relaxed);
  silenceFrames_.store(0U, std::memory_order_relaxed);
  initialized_ = true;
  started_ = false;
  spdlog::info("device initialized ({}Hz {}ch fmt={})", currentFormat_.sampleRate,
               currentFormat_.channelCount,
               static_cast<int>(currentFormat_.sampleFormat));
  return true;
}

bool AudioOutputDevice::start() {
  lastError_.reset();
  if (!initialized_) {
    spdlog::error("device start failed: not initialized");
    lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                        "audio output device is not initialized",
                                        "start requires a successful initialize call"};
    return false;
  }

  if (started_) {
    return true;
  }

  if (currentQueue_ != nullptr) {
    publishCallbackQueue(*currentQueue_, currentFormat_);
  }

  if (!backend_->start()) {
    spdlog::error("device start failed: backend start returned false");
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                        "failed to start audio output device",
                                                                        "AudioOutputDeviceBackend::start returned false"});
    return false;
  }

  started_ = true;
  spdlog::info("device started");
  return true;
}

bool AudioOutputDevice::stop() {
  lastError_.reset();
  if (!initialized_ || !started_) {
    return true;
  }

  if (!backend_->stop()) {
    spdlog::error("device stop failed: backend stop returned false");
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                        "failed to stop audio output device",
                                                                        "AudioOutputDeviceBackend::stop returned false"});
    return false;
  }

  deactivateCallbackQueue();
  deactivateSecondSource();  // 任务 9：停 = 双回调面全清（含第二源；防重启后陈旧指针复活）
  resetEnvelopes(); // 任务 5：清包络目标与版本防陈旧快照
  started_ = false;
  spdlog::info("device stopped");
  return true;
}

void AudioOutputDevice::rebindQueue(PcmBufferQueue& queue) noexcept {
  spdlog::debug("device rebind queue (generation={})", queue.generation());
  currentQueue_ = &queue;
  if (initialized_) {
    publishCallbackQueue(queue, currentFormat_);
  }
}

void AudioOutputDevice::uninitialize() noexcept {
  if (!initialized_) {
    return;
  }

  if (started_) {
    static_cast<void>(backend_->stop());
    started_ = false;
  }

  deactivateCallbackQueue();
  deactivateSecondSource();  // 任务 9：uninitialize 同样清第二源回调面
  resetEnvelopes(); // 任务 5：清包络目标与版本防陈旧快照
  backend_->uninitialize();
  currentFormat_ = {};
  currentQueue_ = nullptr;
  lastError_.reset();
  initialized_ = false;
  spdlog::info("device uninitialized");
}

bool AudioOutputDevice::initialized() const noexcept { return initialized_; }

bool AudioOutputDevice::started() const noexcept { return started_; }

AudioDeviceFormat AudioOutputDevice::currentFormat() const { return currentFormat_; }

std::optional<AudioOutputDeviceError> AudioOutputDevice::lastError() const { return lastError_; }

void AudioOutputDevice::setVolume(float linearGain) noexcept {
  if (std::isnan(linearGain)) {
    return;
  }
  const auto clamped = std::clamp(linearGain, 0.0F, 1.0F);
        spdlog::debug("device volume set to {:.2f}", clamped);
  volume_.store(clamped, std::memory_order_release);
}

void AudioOutputDevice::setMuted(bool muted) noexcept {
    spdlog::debug("device mute set to {}", muted);
  muted_.store(muted, std::memory_order_release);
}

void AudioOutputDevice::setMasterEnvelope(const GainEnvelopeSnapshot& snapshot) noexcept {
  publishEnvelopeLayer(callbackState_.masterEnvelope, snapshot);
}

void AudioOutputDevice::setSourceEnvelope(std::size_t slot, const GainEnvelopeSnapshot& snapshot) noexcept {
  if (slot >= kActiveSourceEnvelopeSlots) return; // 槽 ≥ kActiveSourceEnvelopeSlots（≥2）的发布被忽略（槽 1 自任务 9 起随第二源激活参与执行）
  publishEnvelopeLayer(callbackState_.sourceEnvelopes[slot], snapshot);
}

// 清 PENDING 与 EXEC 全字段（防陈旧快照）。含 currentGain/exec*：stop() 若不清 currentGain，
// 最后一次淡变值会滞留，下一次 start() 将从陈旧点受理包络（任务 5-B1 缺陷的修复点）。
// 在 stop()/uninitialize() 内调用（设备已停、无活跃回调，无竞争）。
void AudioOutputDevice::resetEnvelopes() noexcept {
  clearEnvelopeLayer(callbackState_.masterEnvelope);
  for (auto& layer : callbackState_.sourceEnvelopes) {
    clearEnvelopeLayer(layer);
  }
}

float AudioOutputDevice::masterEnvelopeGain() const noexcept {
  return callbackState_.masterEnvelope.currentGain.load(std::memory_order_acquire);
}

float AudioOutputDevice::sourceEnvelopeGain(std::size_t slot) const noexcept {
  if (slot >= kActiveSourceEnvelopeSlots) return 1.0F;
  return callbackState_.sourceEnvelopes[slot].currentGain.load(std::memory_order_acquire);
}

void AudioOutputDevice::resetSourceEnvelope(std::size_t slot) noexcept {
  if (slot >= kActiveSourceEnvelopeSlots) {
    return;
  }
  clearEnvelopeLayer(callbackState_.sourceEnvelopes[slot]);
}

void AudioOutputDevice::activateSecondSource(PcmBufferQueue& queue,
                                             const GainEnvelopeSnapshot& sourceEnvelope) noexcept {
  // N8（任务 9 评审潜伏项，T10 生产激活首调用方）：第二 ring 帧字节必须与设备当前格式
  // 一致——双源路径把第二源读入 mixScratch_（容量按主队列 × 主 bpf 在 initialize 时一次性
  // 调整，见头注释），ring 自身 bpf 更大的失配环会在 frameCount×ringBpf 写入时溢出暂存。
  // 守卫失败 = 激活请求作废（状态零变更：代次/指针/包络/激活标志均未动），调用方（调度器）
  // 应走无重叠降级路径。currentFormat_ 未初始化（bpf=0）时同样拒绝。
  const auto deviceBpf =
      bytesPerSample(currentFormat_.sampleFormat) * currentFormat_.channelCount;
  if (deviceBpf == 0U || queue.bytesPerFrame() != deviceBpf) {
    spdlog::error("second source activation rejected: ring bpf {} != device bpf {}",
                  queue.bytesPerFrame(), deviceBpf);
    return;
  }
  // 发布序同 publishCallbackQueue 纪律：ring 内容先就绪（代次 → 指针 → 包络），
  // secondActive=true 最后 release——回调看到激活标志时全部字段已发布完毕。
  callbackState_.secondGeneration.store(queue.generation(), std::memory_order_release);
  callbackState_.secondQueue.store(&queue, std::memory_order_release);
  publishEnvelopeLayer(callbackState_.sourceEnvelopes[1], sourceEnvelope);
  callbackState_.secondActive.store(true, std::memory_order_release);
}

void AudioOutputDevice::deactivateSecondSource() noexcept {
  // 撤销序同 deactivateCallbackQueue：active 先清（后续块不再混入），代次递增 + 指针置空。
  // 回调撤销窗口内至多再持有一个 block 的旧指针：代次对旧队列仍匹配时混入该块数据
  // （ring 对象此时仍存活——销毁归 worker 延迟回收），之后一律按代次校验即弃。
  callbackState_.secondActive.store(false, std::memory_order_release);
  callbackState_.secondGeneration.fetch_add(1U, std::memory_order_acq_rel);
  callbackState_.secondQueue.store(nullptr, std::memory_order_release);
}

bool AudioOutputDevice::secondSourceActive() const noexcept {
  return callbackState_.secondActive.load(std::memory_order_acquire);
}

void AudioOutputDevice::renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept {
  auto* device = static_cast<AudioOutputDevice*>(userData);
  if (device == nullptr) {
    return;
  }

  auto& state = device->callbackState_;
  const auto bytesPerFrame = state.bytesPerFrame.load(std::memory_order_acquire);
  const auto channelCount = state.channelCount.load(std::memory_order_acquire);
  const auto sampleFormat = state.sampleFormat.load(std::memory_order_acquire);
  const auto generation = state.queueGeneration.load(std::memory_order_acquire);
  auto* queue = state.pcmQueue.load(std::memory_order_acquire);

  PcmBufferReadResult result{};
  result.requestedFrames = frameCount;
  if (queue != nullptr && state.active.load(std::memory_order_acquire)) {
    result = queue->readIfGeneration(output, frameCount, generation);
  } else {
    result.silenceFrames = frameCount;
    fillSilence(output, frameCount, bytesPerFrame);
  }

  const auto copiedFrames = result.copiedFrames;
  const float volume = device->volume_.load(std::memory_order_acquire);
  const bool muted = device->muted_.load(std::memory_order_acquire);

  // ---- 增益包络执行器（任务 5-B2 / 任务 9 双源）----
  // 活动层：masterEnvelope、sourceEnvelopes[0]（主源）；sourceEnvelopes[1] 仅第二源激活时参与。
  // 默认快速路径：本设备代从未发布包络（master/source0 版本均 0）且第二源未激活 → 调任务 4
  // 单值 applyGain，逐字节不变。
  auto& masterLayer = state.masterEnvelope;
  auto& sourceLayer = state.sourceEnvelopes[0];
  const bool envelopePublished = masterLayer.version.load(std::memory_order_acquire) != 0U ||
                                 sourceLayer.version.load(std::memory_order_acquire) != 0U;
  // 任务 9 第二源门控：secondActive=false → 下述 dualMix=false，本块严格走既有单源路径
  // （不读第二队列、不碰暂存、不执行槽 1 层——逐位回归由测试锁定）。撤销竞态窗口内
  // （active 已清、指针未空）读到的旧指针+旧代次仍匹配 → 至多再混一个块；指针已空即视同
  // 无第二源。暂存容量守卫：frameCount 超出 mixScratch_ 时本块退回单源路径（生产不可达）。
  const bool secondActive = state.secondActive.load(std::memory_order_acquire);
  PcmBufferQueue* secondQueue = nullptr;
  if (secondActive) {
    secondQueue = state.secondQueue.load(std::memory_order_acquire);
  }
  const bool dualMix = secondQueue != nullptr &&
                       static_cast<std::size_t>(frameCount) * bytesPerFrame <= device->mixScratch_.size();
  if (!envelopePublished && !dualMix) {
    applyGain(output, copiedFrames, channelCount, sampleFormat, volume, muted);
  } else if (!dualMix) {
    // 受理账本先于增益应用（每块每活动层一次，master 后 source0）。
    const auto masterTrajectory = planEnvelopeLayer(masterLayer);
    const auto sourceTrajectory = planEnvelopeLayer(sourceLayer);
    // 组合增益 = master(i) × source0(i) × volume；muted/volume<=0 保持既有 memset 早退。
    if (muted || volume <= 0.0F) {
      if (output != nullptr && copiedFrames != 0U && channelCount != 0U) {
        std::memset(output, 0, static_cast<std::size_t>(copiedFrames) * channelCount * bytesPerSample(sampleFormat));
      }
    } else {
      const auto blockGain = [&](std::uint32_t frameIndex) noexcept -> float {
        return trajectoryGain(masterTrajectory, frameIndex) * trajectoryGain(sourceTrajectory, frameIndex) * volume;
      };
      applyFrameGains(output, copiedFrames, channelCount, sampleFormat, blockGain);
    }
    // 块末写回一次：rampFramesDone = 块末进度，currentGain = 末帧增益。muted/音量 0 块
    // 按 copiedFrames 照常推进（包络随输出帧时间走，worker 读回同步）；copiedFrames==0
    // 的纯静音块保持原位（finalizeTrajectory 内不推进，恢复出声后续跑）。
    finalizeTrajectory(masterLayer, masterTrajectory, copiedFrames);
    finalizeTrajectory(sourceLayer, sourceTrajectory, copiedFrames);
  } else {
    // ---- 双源活动路径（任务 9；D1 渲染序）----
    // 两源各自独立 readIfGeneration（代次各自校验、欠载各自补零，互不串扰）；随后
    // source0×腿0 + source1×腿1 → master×volume（mixDualFrames，见匿名命名空间注释）。
    // 主源读已就位于 output；第二源读入 mixScratch_（块内瞬态，内容不跨块保留）。
    auto& secondLayer = state.sourceEnvelopes[1];
    const auto secondGeneration = state.secondGeneration.load(std::memory_order_acquire);
    static_cast<void>(
        secondQueue->readIfGeneration(device->mixScratch_.data(), frameCount, secondGeneration));
    const auto masterTrajectory = planEnvelopeLayer(masterLayer);
    const auto sourceTrajectory = planEnvelopeLayer(sourceLayer);
    const auto secondTrajectory = planEnvelopeLayer(secondLayer);
    if (muted || volume <= 0.0F) {
      if (output != nullptr && frameCount != 0U && channelCount != 0U) {
        std::memset(output, 0, static_cast<std::size_t>(frameCount) * channelCount * bytesPerSample(sampleFormat));
      }
    } else {
      mixDualFrames(output, device->mixScratch_.data(), frameCount, channelCount, sampleFormat, sourceTrajectory,
                    secondTrajectory, masterTrajectory, volume);
    }
    // 轨迹推进同单源纪律：按主源 copiedFrames（第二源欠载不冻结淡变——出声帧以主源计）。
    finalizeTrajectory(masterLayer, masterTrajectory, copiedFrames);
    finalizeTrajectory(sourceLayer, sourceTrajectory, copiedFrames);
    finalizeTrajectory(secondLayer, secondTrajectory, copiedFrames);
  }
  device->callbackCount_.fetch_add(1U, std::memory_order_relaxed);
  device->requestedFrames_.fetch_add(result.requestedFrames, std::memory_order_relaxed);
  device->copiedFrames_.fetch_add(result.copiedFrames, std::memory_order_relaxed);
  device->silenceFrames_.fetch_add(result.silenceFrames, std::memory_order_relaxed);
}

void AudioOutputDevice::publishCallbackQueue(PcmBufferQueue& queue, const AudioDeviceFormat& format) noexcept {
  callbackState_.active.store(false, std::memory_order_release);
  callbackState_.bytesPerFrame.store(bytesPerSample(format.sampleFormat) * format.channelCount, std::memory_order_release);
  callbackState_.channelCount.store(format.channelCount, std::memory_order_release);
  callbackState_.sampleFormat.store(format.sampleFormat, std::memory_order_release);
  callbackState_.queueGeneration.store(queue.generation(), std::memory_order_release);
  callbackState_.pcmQueue.store(&queue, std::memory_order_release);
  callbackState_.active.store(true, std::memory_order_release);
}

void AudioOutputDevice::deactivateCallbackQueue() noexcept {
  callbackState_.active.store(false, std::memory_order_release);
  callbackState_.queueGeneration.fetch_add(1U, std::memory_order_acq_rel);
  callbackState_.pcmQueue.store(nullptr, std::memory_order_release);
}

AudioOutputDeviceCounters AudioOutputDevice::counters() const noexcept {
  return AudioOutputDeviceCounters{callbackCount_.load(std::memory_order_relaxed),
                                   requestedFrames_.load(std::memory_order_relaxed),
                                   copiedFrames_.load(std::memory_order_relaxed),
                                   silenceFrames_.load(std::memory_order_relaxed)};
}

}
