#include "seriona/audio/device/audio_output_device.h"

#include "../eq_dsp.h"
#include "../limiter.h"
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

// EQ 生效快照发布（任务 26 B2.4b；worker，停态窗口调用）：把实际生效配置 + 实际输出率
// 逐字段 relaxed 写入原子镜像层，version release 最后写（发布序同 publishEnvelopeLayer）。
// 调用点 = applyEqualizerDspConfig 每次真实 configure 成功之后（含 clearDspState 重建后
// 重配置）；eqConfig_ 仅存储未应用（未 initialize / started_ 防御存储）时不调用——快照
// 保持上一次真实生效状态，读侧不被告知未生效的新目标。
void publishEqAppliedLayer(AudioOutputDeviceEqLayer& layer,
                           const EqualizerConfig& config,
                           const AudioDeviceFormat& format,
                           std::uint32_t version) noexcept {
  layer.enabled.store(config.enabled, std::memory_order_relaxed);
  layer.mode.store(config.mode, std::memory_order_relaxed);
  layer.preGainDb.store(config.preGainDb, std::memory_order_relaxed);
  for (std::size_t band = 0; band < config.bandGainsDb.size(); ++band) {
    layer.bandGainsDb[band].store(config.bandGainsDb[band], std::memory_order_relaxed);
  }
  layer.limiterEnabled.store(config.limiterEnabled, std::memory_order_relaxed);
  layer.sampleRate.store(format.sampleRate, std::memory_order_relaxed);
  layer.channelCount.store(format.channelCount, std::memory_order_relaxed);
  layer.version.store(version, std::memory_order_release);
}

// 生效快照「config 系字段」发布（运行期回调受理专用，不写 sampleRate/channelCount）：
// 受理发生在设备运行中——输出格式不变，采样率/声道数字段保持停态真实应用的回填值恒
// 正确（格式变化必须经停态 initialize 重建，回调受理永不触碰格式）。回调红线：
// 零分配/零锁/零日志；逐字段 relaxed + version release 最后写同 publishEqAppliedLayer。
void publishEqAppliedConfigLayer(AudioOutputDeviceEqLayer& layer,
                                 const EqualizerConfig& config,
                                 std::uint32_t version) noexcept {
  layer.enabled.store(config.enabled, std::memory_order_relaxed);
  layer.mode.store(config.mode, std::memory_order_relaxed);
  layer.preGainDb.store(config.preGainDb, std::memory_order_relaxed);
  for (std::size_t band = 0; band < config.bandGainsDb.size(); ++band) {
    layer.bandGainsDb[band].store(config.bandGainsDb[band], std::memory_order_relaxed);
  }
  layer.limiterEnabled.store(config.limiterEnabled, std::memory_order_relaxed);
  layer.version.store(version, std::memory_order_release);
}

// EQ 运行期目标 PENDING 发布（worker；setEqualizerConfig 无条件调用——含停态与未
// initialize）：目标配置逐字段 relaxed 写，version release 最后写（回调读到 version
// 变化时字段必然已就位；发布序同 publishEnvelopeLayer/publishEqAppliedLayer）。
void publishEqTargetLayer(AudioOutputDeviceEqTargetLayer& layer,
                          const EqualizerConfig& config) noexcept {
  layer.enabled.store(config.enabled, std::memory_order_relaxed);
  layer.mode.store(config.mode, std::memory_order_relaxed);
  layer.preGainDb.store(config.preGainDb, std::memory_order_relaxed);
  for (std::size_t band = 0; band < config.bandGainsDb.size(); ++band) {
    layer.bandGainsDb[band].store(config.bandGainsDb[band], std::memory_order_relaxed);
  }
  layer.limiterEnabled.store(config.limiterEnabled, std::memory_order_relaxed);
  const auto nextVersion = layer.version.load(std::memory_order_relaxed) + 1U;
  layer.version.store(nextVersion, std::memory_order_release);
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

// --- EQ 格式转换辅助（任务 24 B2.3；任务 25 EQ 分支接线）------------------------------
// EQ 激活时处理链在 f32 域进行：int 设备格式逐样本转 f32 进 f32Scratch_（读侧三函数），
// 链尾一次量化回设备格式（写侧三函数）——末端是 EQ 链唯一 round+clamp 点（单次量化，
// 无中间域往返）。尺度与既有打包同源防双标：s16 以 2^15 为 1.0（±32768 ↔ ±1.0）；
// s24 按 76-95 的「左对齐 S32」中间域以 2^31 为 1.0——读用 unpackS24ToLeftAlignedS32
// 还原后 /2^31，写先量化左对齐 S32 再 packLeftAlignedS32ToS24 落 3 字节，与打包写侧
// （ffmpeg_filter_pipeline.cpp packS32ToS24）同尺度；s32 直接以 2^31 为 1.0。读侧
// s16/s24 内容 ≤ 24 位有效位：int→float 精确且除数为 2 的幂 → 转换零舍入；s32 高位
// 内容按 f32 尾数（24 位）正确舍入，误差 ≤ 该幅值浮点 ULP（f32 域固有精度）。
// 写侧量化数学沿用 40-49（double 加宽域 llround + clamp）：有限样本先 clamp 到
// [-1,1] 再乘尺度——与直接 round 后 clamp 输出等价（|x|>1 的量化结果经 clamp 恒收敛
// 到同一满幅值），且使 llround 永在 long long 域内（无实现相关溢出值）；NaN/±Inf
// 显式量化 0（40-49 输入恒 int 无此通道；llround(NaN) 结果实现相关，落满幅会放大
// 爆音）。全零分配、零间接。[[maybe_unused]]：任务 25 接线前无调用点（抑制
// -Wunused-function），接线后随调用点移除。

// 读侧：int 设备样本 → f32（source = 设备输出缓冲 int 布局，destination = f32Scratch_）。
[[maybe_unused]] void int16ToFloat32(const void* source, float* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto const* in = static_cast<std::int16_t const*>(source);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    destination[index] = static_cast<float>(in[index]) / 32768.0F;
  }
}

[[maybe_unused]] void int32ToFloat32(const void* source, float* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto const* in = static_cast<std::int32_t const*>(source);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    destination[index] = static_cast<float>(in[index]) / 2147483648.0F;
  }
}

[[maybe_unused]] void int24ToFloat32(const void* source, float* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto const* bytes = static_cast<std::uint8_t const*>(source);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    // 3 字节小端 S24 → 左对齐 S32（76-95 unpack，|值| ≤ 2^31）→ /2^31（精确，见区注释）。
    destination[index] = static_cast<float>(unpackS24ToLeftAlignedS32(bytes + index * 3U)) / 2147483648.0F;
  }
}

// 写侧（末端量化）：f32 → int 设备样本（source = f32 处理域，destination = 设备输出缓冲）。
[[maybe_unused]] void float32ToInt16(const float* source, void* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto* out = static_cast<std::int16_t*>(destination);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    const double value = static_cast<double>(source[index]);
    const auto scaled =
        std::isfinite(value) ? std::llround(std::clamp(value, -1.0, 1.0) * 32768.0) : 0LL;
    out[index] = static_cast<std::int16_t>(std::clamp<long long>(
        scaled, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
  }
}

[[maybe_unused]] void float32ToInt32(const float* source, void* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto* out = static_cast<std::int32_t*>(destination);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    const double value = static_cast<double>(source[index]);
    const auto scaled =
        std::isfinite(value) ? std::llround(std::clamp(value, -1.0, 1.0) * 2147483648.0) : 0LL;
    out[index] = static_cast<std::int32_t>(std::clamp<long long>(
        scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
  }
}

[[maybe_unused]] void float32ToInt24(const float* source, void* destination, std::uint32_t frameCount,
                                     std::uint16_t channelCount) noexcept {
  auto* bytes = static_cast<std::uint8_t*>(destination);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0U; index < sampleCount; ++index) {
    const double value = static_cast<double>(source[index]);
    const auto scaled =
        std::isfinite(value) ? std::llround(std::clamp(value, -1.0, 1.0) * 2147483648.0) : 0LL;
    const auto quantized = static_cast<std::int32_t>(std::clamp<long long>(
        scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    packLeftAlignedS32ToS24(quantized, bytes + index * 3U);
  }
}

// --- EQ 链第二腿累加辅助（任务 25 B2.4a；EQ 激活分支专用）----------------------------
// 双源 EQ 链的读侧混合：腿 0（output）先经任务 24 读侧辅助逐样本转 f32 进
// f32Scratch_，腿 1（mixScratch_，设备域）经本族逐样本转 f32 后**就地累加**进
// f32Scratch_（destination += 腿1 × 腿1包络增益——仅双腿增益+求和，master×volume
// 推迟到 EQ 后统一应用）。尺度和数学与任务 24 读侧辅助一致（s16 / 2^15；s24 经
// unpackS24ToLeftAlignedS32 / 2^31；s32 / 2^31），单次量化仍在链尾（见 533-547
// 区注释）。leg 增益每帧每声道相同（trajectoryGain 按帧取）；未受理层 = 恒等 1.0。
// 零分配、零锁、零日志；Float32 设备不经过本族（就地混音，见 eqMixFloat32Dual）。

template <typename IntSample>
void eqAccumulateIntLegToFloat32(IntSample const* secondSrc,
                                 float* destination,
                                 std::uint32_t frameCount,
                                 std::uint16_t channelCount,
                                 float intScale,
                                 const ExecutedTrajectory& legTrajectory) noexcept {
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = trajectoryGain(legTrajectory, frame);
    auto const* const frameSrc = secondSrc + static_cast<std::size_t>(frame) * channels;
    auto* const frameDst = destination + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      frameDst[ch] += static_cast<float>(frameSrc[ch]) * intScale * gain;
    }
  }
}

void eqAccumulateS24LegToFloat32(const void* secondSrc,
                                 float* destination,
                                 std::uint32_t frameCount,
                                 std::uint16_t channelCount,
                                 const ExecutedTrajectory& legTrajectory) noexcept {
  auto const* bytes = static_cast<std::uint8_t const*>(secondSrc);
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float gain = trajectoryGain(legTrajectory, frame);
    auto const* const frameSrc = bytes + static_cast<std::size_t>(frame) * channels * 3U;
    auto* const frameDst = destination + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      // 3 字节小端 S24 → 左对齐 S32（unpackS24ToLeftAlignedS32，|值| ≤ 2^31）→
      // /2^31（与 int24ToFloat32 同尺度；f32 域相加，链尾单次量化）。
      frameDst[ch] +=
          static_cast<float>(unpackS24ToLeftAlignedS32(frameSrc + ch * 3U)) / 2147483648.0F * gain;
    }
  }
}

// f32 设备双源 EQ 链的腿求和（就地）：output 兼腿 0 与和缓冲，secondSrc = 腿 1
// （mixScratch_，f32 设备域字节）。逐帧：out = out×g0 + second×g1——同上方累加
// 纪律（双腿增益+求和；master×volume 推迟 EQ 后；单源 f32 走 applyFloat32FrameGains，
// 不经本函数）。未受理层增益恒等 1.0。
void eqMixFloat32Dual(float* output,
                      const float* secondSrc,
                      std::uint32_t frameCount,
                      std::uint16_t channelCount,
                      const ExecutedTrajectory& source0,
                      const ExecutedTrajectory& source1) noexcept {
  const auto channels = static_cast<std::size_t>(channelCount);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    const float g0 = trajectoryGain(source0, frame);
    const float g1 = trajectoryGain(source1, frame);
    auto* const frameOut = output + static_cast<std::size_t>(frame) * channels;
    auto const* const frameSrc = secondSrc + static_cast<std::size_t>(frame) * channels;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      frameOut[ch] = frameOut[ch] * g0 + frameSrc[ch] * g1;
    }
  }
}

// --- 摘录单声道均值下混辅助（任务 29 B3.1；回调线程专用）------------------------
// 帧内按声道序 0..N-1 固定顺序累加（f32；|Σ| ≤ N×幅值，EQ 增益后幅值有界、无溢出
// 可能；累加顺序固定 = 可复现），再乘 1/N。int 源逐样本先按任务 24 冻结读侧尺度
// 转 f32（s16 /2^15；s24 经 unpackS24ToLeftAlignedS32 左对齐后 /2^31；s32 /2^31，
// 与 int16ToFloat32/int24ToFloat32/int32ToFloat32 同尺度）；N=1 单声道直取（无
// 均值运算，scale 乘/拷贝恒等）。零分配、零锁、零日志。
template <typename IntSample>
void captureDownmixIntToMono(IntSample const* src, float* mono, std::uint32_t frameCount,
                             std::uint16_t channelCount, float intScale) noexcept {
  const auto channels = static_cast<std::size_t>(channelCount);
  if (channels == 1U) {
    for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
      mono[frame] = static_cast<float>(src[frame]) * intScale;
    }
    return;
  }
  const float invChannels = 1.0F / static_cast<float>(channels);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    auto const* const frameSrc = src + static_cast<std::size_t>(frame) * channels;
    float sum = 0.0F;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      sum += static_cast<float>(frameSrc[ch]) * intScale;
    }
    mono[frame] = sum * invChannels;
  }
}

void captureDownmixS24ToMono(const void* src, float* mono, std::uint32_t frameCount,
                             std::uint16_t channelCount) noexcept {
  auto const* bytes = static_cast<std::uint8_t const*>(src);
  const auto channels = static_cast<std::size_t>(channelCount);
  if (channels == 1U) {
    for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
      mono[frame] =
          static_cast<float>(unpackS24ToLeftAlignedS32(bytes + static_cast<std::size_t>(frame) * 3U)) /
          2147483648.0F;
    }
    return;
  }
  const float invChannels = 1.0F / static_cast<float>(channels);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    auto const* const frameSrc = bytes + static_cast<std::size_t>(frame) * channels * 3U;
    float sum = 0.0F;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      sum += static_cast<float>(unpackS24ToLeftAlignedS32(frameSrc + ch * 3U)) / 2147483648.0F;
    }
    mono[frame] = sum * invChannels;
  }
}

void captureDownmixFloatToMono(const float* src, float* mono, std::uint32_t frameCount,
                               std::uint16_t channelCount) noexcept {
  const auto channels = static_cast<std::size_t>(channelCount);
  if (channels == 1U) {
    std::memcpy(mono, src, static_cast<std::size_t>(frameCount) * sizeof(float));
    return;
  }
  const float invChannels = 1.0F / static_cast<float>(channels);
  for (std::uint32_t frame = 0U; frame < frameCount; ++frame) {
    auto const* const frameSrc = src + static_cast<std::size_t>(frame) * channels;
    float sum = 0.0F;
    for (std::size_t ch = 0U; ch < channels; ++ch) {
      sum += frameSrc[ch];
    }
    mono[frame] = sum * invChannels;
  }
}

}

AudioOutputDevice::AudioOutputDevice(std::unique_ptr<AudioOutputDeviceBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_) {
    backend_ = makeMiniaudioOutputDeviceBackend();
  }
  // EQ/限幅 DSP 实例（任务 25）：构造函数内一次性分配（非回调路径），保证首次
  // setEqualizerConfig（未 initialize、格式未定）等停态调用永不触碰空指针；configure
  // 在停态窗口（setEqualizerConfig/initialize），process 在 renderCallback EQ 分支。
  // 任务 26：每次 initialize() 经 clearDspState() 重建实例（内容边界清零），此处仅兜底。
  eqDsp_ = std::make_unique<EqualizerDspProcessor>();
  limiter_ = std::make_unique<LimiterDspProcessor>();
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
  // 任务 24：EQ f32 暂存容量 = 主队列容量 × 4B（float32 字节宽）× 声道数。与 mixScratch_
  // 同纪律（仅此处调整）；按固定 4B/f32 分配，不依赖 mixScratch_ 的帧字节（后随设备
  // 位深字节宽变化）。EQ 处理帧数 ≤ 后端 period ≤ 主队列容量 ⇒ 4×帧×声道 ≤ 容量恒成立。
  f32Scratch_.resize(static_cast<std::size_t>(request.pcmQueue->capacityFrames()) * sizeof(float) *
                     currentFormat_.channelCount);
  // 任务 29 B3.1：频谱摘录双缓冲按主队列容量（= 最大回调帧数上界）重分配 + 纪元
  // 推进（双槽内容清零防混率帧被消费；无活跃回调窗口，同 f32Scratch_ resize 纪律；
  // rebindQueue/T10 交接与 stop() 不调用——交接不是重建，暂停冻结续接）。
  captureReset(request.pcmQueue->capacityFrames());
  // 任务 25/26：EQ/限幅 DSP 按生效格式（重新）应用存储配置（幂等——negotiateOutput
  // 失败候选反复 initialize 安全；backend init 已成功 = 格式确定）。挂点 ①（任务 26
  // 内容边界清理）：initialize() 内经 clearDspState() 重建 DSP 单元实例——滤波历史/
  // limiter 延迟线/检测/平滑全清零后按存储配置重配置并发布 EQ 生效快照（含实际输出率
  // sampleRate 回填）。configure 允许分配/重建，此处无活跃回调（结构性无竞态，
  // 同 f32Scratch_ resize 纪律）；EQ 重建语义在 eq_dsp configure 内。
  clearDspState();
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

bool AudioOutputDevice::f32ScratchFits(std::uint32_t frameCount, std::uint16_t channelCount) const noexcept {
  // EQ f32 暂存容量守卫：frameCount×4B/f32×channelCount ≤ 分配容量（f32Scratch_）。
  // 正常路径恒成立（EQ 处理帧数 ≤ 后端 period ≤ 主队列容量，容量在 initialize 按主
  // 队列容量分配）；装不下时调用方（任务 25 EQ 分支）退回既有路径，同 dualMix 守卫语义。
  return static_cast<std::size_t>(frameCount) * sizeof(float) * channelCount <= f32Scratch_.size();
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

void AudioOutputDevice::setEqualizerConfig(const EqualizerConfig& config) noexcept {
  // EQ 配置存储 + 实时投递入口（任务 25 B2.4a；头注释为线程契约）。语义：
  //  - eqConfig_ 存储恒执行（停态应用/幂等重放读它，兼容/回读面）；
  //  - PENDING 目标层无条件发布（publishEqTargetLayer，version 递增）——运行中
  //    （started_，回调可能活跃）由 renderCallback 块首受理投递到 DSP（applyTargets
  //    零分配，播放中调节实时生效——用户实测缺陷修复点；不再 started_ 门控仅存储）；
  //  - 停态窗口（未 started_：无活跃回调）仍即时真实应用 applyEqualizerDspConfig
  //    （configure 允许分配；暂停中调整 = 立即生效 + 生效快照发布，同现状零回归）。
  eqConfig_ = config;
  publishEqTargetLayer(eqTargetLayer_, config);
  if (!started_) {
    applyEqualizerDspConfig();
  }
}

void AudioOutputDevice::applyEqualizerDspConfig() noexcept {
  // 停态应用：按当前设备格式 configure 两 DSP 单元并发布生效快照。格式未定
  // （未 initialize，sampleRate==0）时仅存储语义成立（initialize() 在格式确定后
  // 调用本方法补应用——幂等）。发布序：先 configure 后发布 EQ 生效快照（任务 26：
  // version 在原子镜像层 release 最后写，读侧一致取用——含实际输出率 sampleRate
  // 回填点）。DSP 链进入条件不再由本方法发布独立标志——renderCallback 按 DSP
  // 目标/平滑状态实时判定（见 renderCallback 区注释）；本方法同步把目标受理账本
  // 消费到当前 PENDING version：停态真实应用已把目标 configure 进 DSP，回调无需
  // 再受理重放（受理对同目标本无操作，消费仅为避免生效快照重复发布——测试装置
  // 停态直渲与重启首块均不发重复发布）。
  const auto rate = currentFormat_.sampleRate;
  if (rate == 0U || currentFormat_.channelCount == 0U) {
    return;
  }
  static_cast<void>(eqDsp_->configure(eqConfig_, rate, currentFormat_.channelCount));
  static_cast<void>(limiter_->configure(eqConfig_, rate, currentFormat_.channelCount));
  const auto nextVersion = eqAppliedLayer_.version.load(std::memory_order_relaxed) + 1U;
  publishEqAppliedLayer(eqAppliedLayer_, eqConfig_, currentFormat_, nextVersion);
  latchedEqTargetVersion_.store(eqTargetLayer_.version.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
}

void AudioOutputDevice::clearDspState() noexcept {
  // 内容边界清理（任务 26 B2.4b；仅停态窗口调用，见头注释三挂点与不清零纪律）：
  // eq_dsp/limiter 无公开复位 API 且同参数 configure 不清态（fs/声道变化才重建），
  // 故重建实例（滤波历史/limiter 延迟线/检测/平滑全清零）+ 按当前存储配置重配置
  // （幂等；格式未定 = 仅清空待下次应用）达成清零，零单元改动。DSP 实例不透明持有，
  // 此处分配不在回调路径（调用点设备已停，结构性无竞态）。
  eqDsp_ = std::make_unique<EqualizerDspProcessor>();
  limiter_ = std::make_unique<LimiterDspProcessor>();
  applyEqualizerDspConfig();
}

AudioOutputDeviceEqSnapshot AudioOutputDevice::eqAppliedSnapshot() const noexcept {
  // 一致读（跨线程安全）：version acquire 取代 → 逐字段 relaxed 读 → version 复核。
  // 发布可发生于读取期间：原子镜像字段无撕裂；版本变更仅表示读到相邻两代混合，
  // 重读一次自愈（发布 = 停态应用（worker）与回调受理（运行期）双写侧低频发布，
  // 两写者不并发；实际重读几乎不发生——同 planEnvelopeLayer「读侧受理/自愈」
  // 语义；不循环等待，避免读侧饥饿）。
  AudioOutputDeviceEqSnapshot snapshot;
  std::uint32_t version = eqAppliedLayer_.version.load(std::memory_order_acquire);
  for (std::uint32_t attempt = 0; attempt < 3U; ++attempt) {
    snapshot.config.enabled = eqAppliedLayer_.enabled.load(std::memory_order_relaxed);
    snapshot.config.mode = eqAppliedLayer_.mode.load(std::memory_order_relaxed);
    snapshot.config.preGainDb = eqAppliedLayer_.preGainDb.load(std::memory_order_relaxed);
    for (std::size_t band = 0; band < snapshot.config.bandGainsDb.size(); ++band) {
      snapshot.config.bandGainsDb[band] =
          eqAppliedLayer_.bandGainsDb[band].load(std::memory_order_relaxed);
    }
    snapshot.config.limiterEnabled = eqAppliedLayer_.limiterEnabled.load(std::memory_order_relaxed);
    snapshot.sampleRate = eqAppliedLayer_.sampleRate.load(std::memory_order_relaxed);
    snapshot.channelCount = eqAppliedLayer_.channelCount.load(std::memory_order_relaxed);
    const auto current = eqAppliedLayer_.version.load(std::memory_order_acquire);
    if (current == version) {
      break;
    }
    version = current;  // 读取期间发布推进：用新代重读。
  }
  snapshot.version = version;
  return snapshot;
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
  // ---- EQ 运行期目标受理（播放中实时调节的投递点；每块一次，零分配/零锁/零日志）----
  // worker 在 setEqualizerConfig（无条件发布 PENDING，见发布面注释）写目标层；本块首
  // 把未受理的新目标经 applyTargets 投递到 DSP（eq_dsp/limiter 的实时目标更新——
  // 平滑/开关过渡在各自 process 内执行，无咔哒）。受理前置：
  //  - 设备处于活跃回调态（state.active）：停态直渲（stop 后测试/异常直渲）不投递
  //    ——停态窗口的真实应用走 worker 侧 applyEqualizerDspConfig，不经本路径；且
  //    停态应用已把账本消费到当前 version（见 applyEqualizerDspConfig），此处不重放；
  //  - DSP 已 configure（accepted_）：initialize() 停态窗口无条件预配置（含出厂默认
  //    配置）→ 活跃态恒成立；applyTargets 返回 false（防御分支，不可达）→ 跳过
  //    受理与生效发布（音频走既有原路径逐位不变，下块重试自愈）。
  // 受理后发布生效快照（config 系字段 + version 递增——服务读回曲线 = 用户目标，
  // 受理即发布；sampleRate/channelCount 不回写：运行中格式不变，停态回填值恒正确）。
  // 已受理账本 latchedEqTargetVersion_：回调线程写 + 停态应用消费（worker）——两写者
  // 不并发（停态应用时无活跃回调；受理仅活跃回调态），原子化仅为跨线程读写模型卫生。
  const std::uint32_t eqTargetVersion = device->eqTargetLayer_.version.load(std::memory_order_acquire);
  if (state.active.load(std::memory_order_acquire) &&
      eqTargetVersion != device->latchedEqTargetVersion_.load(std::memory_order_relaxed)) {
    // 从 PENDING 原子层逐字段 relaxed 读组包（version 已 acquire 确认就位）。
    EqualizerConfig pending;
    pending.enabled = device->eqTargetLayer_.enabled.load(std::memory_order_relaxed);
    pending.mode = device->eqTargetLayer_.mode.load(std::memory_order_relaxed);
    pending.preGainDb = device->eqTargetLayer_.preGainDb.load(std::memory_order_relaxed);
    for (std::size_t band = 0; band < pending.bandGainsDb.size(); ++band) {
      pending.bandGainsDb[band] =
          device->eqTargetLayer_.bandGainsDb[band].load(std::memory_order_relaxed);
    }
    pending.limiterEnabled = device->eqTargetLayer_.limiterEnabled.load(std::memory_order_relaxed);
    if (device->eqDsp_->applyTargets(pending)) {
      static_cast<void>(device->limiter_->applyTargets(pending));
      device->latchedEqTargetVersion_.store(eqTargetVersion, std::memory_order_relaxed);
      const auto nextVersion = device->eqAppliedLayer_.version.load(std::memory_order_relaxed) + 1U;
      publishEqAppliedConfigLayer(device->eqAppliedLayer_, pending, nextVersion);
    }
  }
  // ---- EQ 激活处理链（任务 25 B2.4a + 播放中实时调节）----
  // 进入条件 = DSP 链「需要处理本块」（无独立发布标志——运行期受理与停态真实应用
  // 统一收敛到 DSP 目标/平滑状态，本块首受理后同线程直读 DSP 可观测态）：
  //   · eqDsp_ enabled 目标 = true（EQ 开——含全 0dB 曲线的真实 f32 链语义，
  //     既有 render 测试锁定「EQ 开 0dB ≠ 逐位直通」）；或
  //   · eqDsp_ 平滑/收敛态需要处理（!fastBypassActive：非零曲线稳态、开→爬坡、
  //     enabled=false 的平滑退出期——收敛后自动回落原路径）；或
  //   · limiter_ 需要处理（enabled 目标 = true = 待 WarmUp，或非 Bypass =
  //     WarmUp/Active/FadingOut 进行中）。
  // 出厂全零稳态（从未开 EQ/已关且收敛、限幅关）三条件恒 false → 本块不进入：
  // 下方既有三分支原样执行，逐位不变（既有测试锁定；本分支为纯新增）。
  // int 设备额外要求 f32Scratch_ 容量守卫（同 dualMix 守卫语义：装不下退回既有
  // 路径，生产不可达——EQ 处理帧数 ≤ 后端 period ≤ 主队列容量）。
  // 读取 = 回调线程对 DSP 成员的同线程读：运行中 DSP 成员仅本线程写（applyTargets/
  // process）；worker configure/重建只在停态窗口（设备已停，无活跃回调——同下方
  // EQ 分支内 process 读取 DSP 成员的既有信任边界）。
  //
  // 链序（f32 域，见 534-548 区注释；EQ 级联 = eqDsp_ 单实例 = 任务 22 单元：
  // 块级平滑推进 + preGain（在单元内）+ 逐 band ma_peak2，非 band×ch 实例）：
  //   读环已就位 output（设备域）→ int 设备逐样本转 f32 进 f32Scratch_（任务 24
  //   读侧辅助；f32 设备就地，不占暂存）→ 双腿包络增益 + f32 求和（第二源经
  //   eqAccumulate*/eqMixFloat32Dual；master×volume 推迟）→ EQ 级联 process →
  //   master 包络 × volume（f32 乘）→ 限幅器 process（任务 23 单元；limiterEnabled
  //   门控与开关过渡在单元内，关闭收敛态走旁路快速路径）→ 末端单次 f32→int 量化
  //   写回（任务 24 写侧辅助；f32 设备无量化）。
  //   [频谱摘录（任务 29 B3.1 已落地）：摘录点 = 链激活时 preGain 后/volume 前
  //   f32，即 EQ process 之后、master×volume 之前；muted/静音帧该点不发帧、由
  //   分支尾部补发全零帧（「muted 下摘静音帧」语义）。]
  //
  // 欠载语义：级联只喂 copiedFrames（补零尾不滤波——readIfGeneration 已补零的
  // 尾部原样保留，EQ 链不触碰、量化也不写回尾部）。muted/volume==0：先 memset
  // 输出整块再跳过链（既有语义；跳过 = 不转换/不 EQ/不限幅——EQ 平滑与限幅延迟
  // 线原位冻结，恢复出声从当前位置续跑，无跳变；包络轨迹照常按 copiedFrames
  // 推进，同既有活动路径 muted 块纪律，见下方 1181-1183 区注释）。
  // current-dB 爬坡（eq_dsp 平滑）按块推进且与块长相关（eq_dsp.h 头注释）：
  // 欠载块也按实际喂入帧数照常推进（步进随帧长比例缩小）= 设计意图——平滑沿
  // 真实出声时间走，不因欠载冻结；唯 copiedFrames==0 的纯静音块无帧可喂，与
  // muted 跳过块同列原位保持（同包络 finalize 的 0 帧冻结纪律，恢复后续跑）。
  // 进入判定（本块首受理已执行——若存在新目标，DSP 目标/平滑态已按新目标更新）。
  const bool eqChainEnter =
      (device->eqDsp_->enabled() || !device->eqDsp_->fastBypassActive() ||
       device->limiter_->enabled() || !device->limiter_->bypassActive()) &&
      (sampleFormat == AudioSampleFormat::Float32 || device->f32ScratchFits(frameCount, channelCount));
  if (eqChainEnter) {
    const bool chainDual = dualMix;
    // B3.1 摘录（链激活态）：EQ process 后发布有效帧；未处理块（muted/纯静音）
    // 在分支尾部补发全零帧——两者互斥，每回调至多发布一帧。
    bool eqCaptureDone = false;
    if (chainDual) {
      // 第二源读入 mixScratch_（设备域；同既有双源路径 1192-1194——muted 也读，
      // ring 消费进度与静音无关）。代次/指针由上方门控保证非空且容量已守卫。
      const auto secondGeneration = state.secondGeneration.load(std::memory_order_acquire);
      static_cast<void>(
          secondQueue->readIfGeneration(device->mixScratch_.data(), frameCount, secondGeneration));
    }
    const auto masterTrajectory = planEnvelopeLayer(masterLayer);
    const auto sourceTrajectory = planEnvelopeLayer(sourceLayer);
    const auto secondTrajectory =
        chainDual ? planEnvelopeLayer(state.sourceEnvelopes[1]) : ExecutedTrajectory{};
    if (muted || volume <= 0.0F) {
      if (output != nullptr && frameCount != 0U && bytesPerFrame != 0U) {
        std::memset(output, 0, static_cast<std::size_t>(frameCount) * bytesPerFrame);
      }
    } else if (copiedFrames != 0U && channelCount != 0U) {
      float* work = nullptr;  // f32 处理缓冲：f32 设备 = output 就地；int 设备 = f32Scratch_
      if (sampleFormat == AudioSampleFormat::Float32) {
        work = static_cast<float*>(output);
        if (chainDual) {
          // 双腿增益 + f32 求和（就地；output 兼腿 0 与和缓冲）。
          eqMixFloat32Dual(work, reinterpret_cast<const float*>(device->mixScratch_.data()), copiedFrames,
                           channelCount, sourceTrajectory, secondTrajectory);
        } else {
          applyFloat32FrameGains(work, copiedFrames, channelCount,
                                 [&](std::uint32_t frame) noexcept {
                                   return trajectoryGain(sourceTrajectory, frame);
                                 });
        }
      } else {
        work = reinterpret_cast<float*>(device->f32Scratch_.data());
        // 读侧转 f32（任务 24 辅助；单源即腿 0）→ 腿 0 包络增益 → 第二腿转 f32
        // 就地累加（双腿增益+求和；均不含 master×volume——推迟到 EQ 后）。
        switch (sampleFormat) {
        case AudioSampleFormat::Int16:
          int16ToFloat32(output, work, copiedFrames, channelCount);
          applyFloat32FrameGains(work, copiedFrames, channelCount,
                                 [&](std::uint32_t frame) noexcept {
                                   return trajectoryGain(sourceTrajectory, frame);
                                 });
          if (chainDual) {
            eqAccumulateIntLegToFloat32(reinterpret_cast<const std::int16_t*>(device->mixScratch_.data()), work,
                                        copiedFrames, channelCount, 1.0F / 32768.0F, secondTrajectory);
          }
          break;
        case AudioSampleFormat::Int24:
          int24ToFloat32(output, work, copiedFrames, channelCount);
          applyFloat32FrameGains(work, copiedFrames, channelCount,
                                 [&](std::uint32_t frame) noexcept {
                                   return trajectoryGain(sourceTrajectory, frame);
                                 });
          if (chainDual) {
            eqAccumulateS24LegToFloat32(device->mixScratch_.data(), work, copiedFrames, channelCount,
                                        secondTrajectory);
          }
          break;
        case AudioSampleFormat::Int32:
          int32ToFloat32(output, work, copiedFrames, channelCount);
          applyFloat32FrameGains(work, copiedFrames, channelCount,
                                 [&](std::uint32_t frame) noexcept {
                                   return trajectoryGain(sourceTrajectory, frame);
                                 });
          if (chainDual) {
            eqAccumulateIntLegToFloat32(reinterpret_cast<const std::int32_t*>(device->mixScratch_.data()), work,
                                        copiedFrames, channelCount, 1.0F / 2147483648.0F, secondTrajectory);
          }
          break;
        case AudioSampleFormat::Float32:
          break;  // f32 设备走上方就地分支，不到达本 switch
        case AudioSampleFormat::Unknown:
          break;
        }
      }
      if (sampleFormat != AudioSampleFormat::Unknown) {
        device->eqDsp_->process(work, copiedFrames);
        // ---- B3.1 摘录（链激活态）：EQ(含 preGain) 后、master×volume 前 f32 中间域 ----
        // 帧 = 最新可听信号（不含 volume/限幅/包络淡变——电平语义由 domain 标注，
        // 显示按相对电平）；只取 copiedFrames 有效帧，欠载尾在摘录槽内补零（work
        // 尾部为陈旧内容，不外泄）。muted/无有效帧块由分支尾部补发全零帧。
        device->capturePublishF32(work, copiedFrames, frameCount, channelCount,
                                  AudioOutputDeviceCaptureDomain::ChainActiveF32);
        eqCaptureDone = true;
        // 音量/包络：master×volume 逐帧 f32 乘（既有音量数学照搬 f32 域；组合
        // 增益 = master(i) × volume，与既有路径 1176-1178 的 master×volume 同构）。
        applyFloat32FrameGains(work, copiedFrames, channelCount, [&](std::uint32_t frame) noexcept {
          return trajectoryGain(masterTrajectory, frame) * volume;
        });
        device->limiter_->process(work, copiedFrames);
        // 末端单次量化写回（int 设备；f32 设备 work == output 就地，无需写回）。
        switch (sampleFormat) {
        case AudioSampleFormat::Int16:
          float32ToInt16(work, output, copiedFrames, channelCount);
          break;
        case AudioSampleFormat::Int24:
          float32ToInt24(work, output, copiedFrames, channelCount);
          break;
        case AudioSampleFormat::Int32:
          float32ToInt32(work, output, copiedFrames, channelCount);
          break;
        case AudioSampleFormat::Float32:
          break;
        case AudioSampleFormat::Unknown:
          break;
        }
      }
    }
    // B3.1 摘录（链激活态静音帧）：muted/volume==0/无有效帧等跳过链的块——输出
    // 整块静音，补发全零帧（「muted 下摘静音帧」语义）；标注仍为链激活态域
    // （恢复出声后同域采集，显示不跳标定）。
    if (!eqCaptureDone) {
      device->capturePublishF32(nullptr, 0U, frameCount, channelCount,
                                AudioOutputDeviceCaptureDomain::ChainActiveF32);
    }
    // 轨迹推进同既有路径纪律（muted/音量 0 块照常按 copiedFrames 推进；纯静音块
    // 0 帧冻结）；EQ/限幅状态已在上方按「是否喂帧」推进/冻结（注释见链说明）。
    finalizeTrajectory(masterLayer, masterTrajectory, copiedFrames);
    finalizeTrajectory(sourceLayer, sourceTrajectory, copiedFrames);
    if (chainDual) {
      finalizeTrajectory(state.sourceEnvelopes[1], secondTrajectory, copiedFrames);
    }
    // EQ 分支提前返回：自行推进计数器（同下方既有尾部）。
    device->callbackCount_.fetch_add(1U, std::memory_order_relaxed);
    device->requestedFrames_.fetch_add(result.requestedFrames, std::memory_order_relaxed);
    device->copiedFrames_.fetch_add(result.copiedFrames, std::memory_order_relaxed);
    device->silenceFrames_.fetch_add(result.silenceFrames, std::memory_order_relaxed);
    return;
  }
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
  // ---- B3.1 摘录（链关闭态）：EQ 链未激活 → 输出域样本转换拷贝 ----
  // 帧 = 设备输出域最终可听信号（volume/muted/混音已应用；int 设备逐样本按任务
  // 24 冻结读侧尺度 int→f32，f32 设备直取；均值下混单声道；欠载尾在摘录槽内
  // 补零——单源路径只处理 copiedFrames（其后为读环补零静音），双源混音路径整块
  // frameCount 有效（mixDualFrames 覆盖全块）。
  const auto outputValidFrames = dualMix ? frameCount : copiedFrames;
  device->capturePublishOutputDomain(output, outputValidFrames, frameCount, channelCount, sampleFormat);
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

void AudioOutputDevice::captureReset(std::uint32_t capacityFrames) noexcept {
  // 摘录纪元推进（任务 29 B3.1；initialize 停态窗口调用，无活跃回调）：按新协商
  // 容量重分配双槽（assign 即清零内容）+ 元数据/帧序复位 + 纪元 ++（release 发布
  // 于清零之后）——任何跨代帧（旧率/旧容量/旧内容）不可再被消费（frameCount=0
  // 即无帧）；rebindQueue/T10 交接与 stop() 不调用本方法（交接不是重建）。
  captureCapacityFrames_ = capacityFrames;
  captureBuffers_.assign(static_cast<std::size_t>(capacityFrames) * 2U, 0.0F);
  captureNextSequence_ = 0U;
  capturePublishedSlot_.store(0U, std::memory_order_relaxed);
  for (auto& slotMeta : captureSlotMeta_) {
    slotMeta.sequence.store(0U, std::memory_order_relaxed);
    slotMeta.frameCount.store(0U, std::memory_order_relaxed);
    slotMeta.sampleRate.store(0U, std::memory_order_relaxed);
    slotMeta.domain.store(AudioOutputDeviceCaptureDomain::ChainInactiveOutput, std::memory_order_relaxed);
  }
  captureGeneration_.store(captureGeneration_.load(std::memory_order_relaxed) + 1U, std::memory_order_release);
}

void AudioOutputDevice::captureCommitSlot(std::uint32_t slot, std::uint32_t frameCount,
                                          AudioOutputDeviceCaptureDomain domain) noexcept {
  // 发布序尾（仿 publishCallbackQueue）：样本/元数据先就绪，索引 release 翻转——
  // 读侧 acquire 取槽后必然看到完整帧。
  auto& slotMeta = captureSlotMeta_[slot];
  slotMeta.sequence.store(++captureNextSequence_, std::memory_order_relaxed);
  slotMeta.frameCount.store(frameCount, std::memory_order_relaxed);
  slotMeta.sampleRate.store(currentFormat_.sampleRate, std::memory_order_relaxed);
  slotMeta.domain.store(domain, std::memory_order_relaxed);
  capturePublishedSlot_.store(slot, std::memory_order_release);
}

void AudioOutputDevice::capturePublishF32(const float* interleaved, std::uint32_t validFrames,
                                          std::uint32_t blockFrames, std::uint16_t channelCount,
                                          AudioOutputDeviceCaptureDomain domain) noexcept {
  if (blockFrames == 0U || channelCount == 0U || blockFrames > captureCapacityFrames_) {
    return;  // 无帧可发 / 容量守卫（回调帧数 ≤ 主队列容量 = 槽容量，超限生产不可达）
  }
  const auto slot = capturePublishedSlot_.load(std::memory_order_relaxed) ^ 1U;  // 写侧独占，写非发布槽
  float* const dst = captureBuffers_.data() + static_cast<std::size_t>(slot) * captureCapacityFrames_;
  const auto valid = std::min(validFrames, blockFrames);
  if (interleaved == nullptr || valid == 0U) {
    std::memset(dst, 0, static_cast<std::size_t>(blockFrames) * sizeof(float));
  } else {
    captureDownmixFloatToMono(interleaved, dst, valid, channelCount);
    if (valid < blockFrames) {
      std::memset(dst + valid, 0, static_cast<std::size_t>(blockFrames - valid) * sizeof(float));
    }
  }
  captureCommitSlot(slot, blockFrames, domain);
}

void AudioOutputDevice::capturePublishOutputDomain(const void* output, std::uint32_t validFrames,
                                                   std::uint32_t blockFrames, std::uint16_t channelCount,
                                                   AudioSampleFormat sampleFormat) noexcept {
  if (output == nullptr || blockFrames == 0U || channelCount == 0U || blockFrames > captureCapacityFrames_ ||
      sampleFormat == AudioSampleFormat::Unknown) {
    return;
  }
  const auto slot = capturePublishedSlot_.load(std::memory_order_relaxed) ^ 1U;
  float* const dst = captureBuffers_.data() + static_cast<std::size_t>(slot) * captureCapacityFrames_;
  const auto valid = std::min(validFrames, blockFrames);
  switch (sampleFormat) {
  case AudioSampleFormat::Int16:
    captureDownmixIntToMono(reinterpret_cast<const std::int16_t*>(output), dst, valid, channelCount,
                            1.0F / 32768.0F);
    break;
  case AudioSampleFormat::Int24:
    captureDownmixS24ToMono(output, dst, valid, channelCount);
    break;
  case AudioSampleFormat::Int32:
    captureDownmixIntToMono(reinterpret_cast<const std::int32_t*>(output), dst, valid, channelCount,
                            1.0F / 2147483648.0F);
    break;
  case AudioSampleFormat::Float32:
    captureDownmixFloatToMono(reinterpret_cast<const float*>(output), dst, valid, channelCount);
    break;
  case AudioSampleFormat::Unknown:
    return;
  }
  if (valid < blockFrames) {
    std::memset(dst + valid, 0, static_cast<std::size_t>(blockFrames - valid) * sizeof(float));
  }
  captureCommitSlot(slot, blockFrames, AudioOutputDeviceCaptureDomain::ChainInactiveOutput);
}

bool AudioOutputDevice::latestCaptureFrame(float* samplesOut, std::uint32_t capacityFrames,
                                           AudioOutputDeviceCaptureMeta& meta) const noexcept {
  if (samplesOut == nullptr || captureCapacityFrames_ == 0U) {
    return false;
  }
  // 有界重试 ≤3（同 eqAppliedSnapshot 先例；不循环等待——读侧低频、写侧块率发布，
  // 首轮即一致，重试仅覆盖「拷贝期间写者连续两轮发布回绕到本槽」的不可达窗口）。
  for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
    const auto generation = captureGeneration_.load(std::memory_order_acquire);
    const auto slot = capturePublishedSlot_.load(std::memory_order_acquire);
    const auto frameCount = captureSlotMeta_[slot].frameCount.load(std::memory_order_relaxed);
    if (frameCount == 0U) {
      return false;  // 尚无完整帧（从未摘录 / 重建清零后）
    }
    if (frameCount > capacityFrames) {
      return false;  // 调用方缓冲不足（应按 captureCapacityFrames() 分配）
    }
    AudioOutputDeviceCaptureMeta candidate;
    candidate.generation = generation;
    candidate.sequence = captureSlotMeta_[slot].sequence.load(std::memory_order_relaxed);
    candidate.sampleRate = captureSlotMeta_[slot].sampleRate.load(std::memory_order_relaxed);
    candidate.frameCount = frameCount;
    candidate.domain = captureSlotMeta_[slot].domain.load(std::memory_order_relaxed);
    std::memcpy(samplesOut,
                captureBuffers_.data() + static_cast<std::size_t>(slot) * captureCapacityFrames_,
                static_cast<std::size_t>(frameCount) * sizeof(float));
    // 复核：拷贝期间写者未把本槽翻转回发布位（连续两轮发布才可能覆写本槽）。
    if (generation == captureGeneration_.load(std::memory_order_acquire) &&
        slot == capturePublishedSlot_.load(std::memory_order_acquire) &&
        captureSlotMeta_[slot].sequence.load(std::memory_order_relaxed) == candidate.sequence &&
        captureSlotMeta_[slot].frameCount.load(std::memory_order_relaxed) == frameCount) {
      meta = candidate;
      return true;
    }
  }
  return false;  // 写者连续发布：本轮未取得一致视图，调用方稍后重试
}

std::uint32_t AudioOutputDevice::captureCapacityFrames() const noexcept {
  return captureCapacityFrames_;
}

}
