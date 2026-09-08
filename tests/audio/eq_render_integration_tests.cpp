// EQ/限幅 render 级集成单测（任务 28，B2.6 组 seriona.audio.eq_render_integration）。
//
// 被测面：src/audio/device/audio_output_device.{h,cpp} 完整 render 链（fake backend 驱动
// 真实 AudioOutputDevice，全部经公共发布面 + 静态 renderCallback 黑盒驱动，仿
// audio_output_device_dualsource_tests.cpp 的 DualFakeBackend 装置）。本文件是任务 27
// 帧级测试（eq_coefficients/eq_dsp/eq_limiter）的 render 级配套：任务 27 移交清单的
// roundtrip（真实格式换算链）/欠载零尾/muted/停启清理/限幅开关过渡在此以 renderCallback
// 真实路径覆盖；帧级已锁的 DSP 内部语义（平滑推进、状态机逐样本迁移）不在此重复。
//
// 覆盖（计划书 B2.6 四块）：
//   ① 命令→参数→回调生效：setEqualizerConfig 停态窗口应用 → render 输出频响符合
//      （+6dB@1kHz 单频 LSQ，±0.1dB）；started 窗口内仅存储、停态窗口再应用；
//      rebind/加载路径参数回放（rebindQueue 后 EQ 曲线保持、快照不重发）；
//      s32 设备 EQ 关闭整体回既有路径（volume 1.0 下逐位直通；EQ 开 0dB 链是
//      真实 f32 往返——逐位不等、误差有界），锁「EQ 关 ≠ 走 f32 中间路径」。
//   ② 重建后曲线恢复：Direct 逐曲（同格式 re-open = 内容边界 initialize 幂等重放）
//      与输出格式切换（跨格式/采样率 re-open）两入口——初始化后 EQ 从零状态重建，
//      稳态频响恢复配置曲线（允许 ramp 收敛，断言稳态；首段断言无超界/无旧状态
//      残留），eqAppliedSnapshot 的 rate/format/version 随真实应用更新。
//   ③ 格式矩阵抽样：s16/s24/s32/f32 × 44.1k/48k/192k（7 组合，覆盖全部格式与三档
//      采样率）+ 一次运行时格式切换（同实例 f32@48k → s32@192k 重开）；roundtrip
//      量化契约经真实换算链：s16/s24 无增益逐位精确、s32 中幅 ≤1LSB/全幅 ≤64LSB、
//      有增益（preGain −3.5dB）末端单次量化 ≤1LSB。
//   ④ EQ+限幅激活下双源自动前进无缝交接：设备级 T10 completeOverlapHandoff 等价
//      模拟（activateSecondSource 即时 0 → 等功率对双腿 → 归零观测 → deactivate +
//      resetEnvelope + rebindQueue 提升）——全程无空洞（无 ≥4 连零样本、各块 RMS
//      有下界、双队列零欠载）、无爆音（逐帧 delta 有界、峰值在限幅阈内、跨交接
//      帧无阶跃）、EQ 曲线在交接两侧保持（停态快照不变 + 提升后实测 +6dB±0.15）。
//
// 判据纪律（来自冻结实现，勿改实现只约束行为）：
//   - EQ 链进入条件 = DSP 链需要处理本块（无独立发布标志）：eqDsp_ enabled 目标
//     = true（EQ 开，含全 0dB 曲线的真实 f32 链语义）或 !fastBypassActive（平滑/
//     非零收敛/关闭平滑退出期）或 limiter 需要处理（enabled 目标 = true 或非
//     Bypass）；出厂全零稳态不进链（原路径逐位不变）。setEqualizerConfig：
//     停态窗口真实应用（立即生效 + 生效快照发布 + 目标账本消费）；运行中 = 存储 +
//     PENDING 目标层发布，回调块首受理（活跃回调态）投递 DSP（applyTargets）并
//     发布生效快照——播放中调节实时生效；initialize() 经 clearDspState 重建实例 +
//     幂等重放存储配置（快照 version++）。
//   - 格式换算读侧 s16/s24 精确（≤24 位有效位 /2 幂）、s32 有 f32 舍入；写侧
//     double llround+clamp 末端单次量化（s16/s24 逐位、s32 ≤1LSB/≤64LSB 只在此
//     断言——EQ 开 0dB + 限幅关走 fastBypass，链本身不触碰样本；等功率淡入淡出
//     不适用）。limiter 语义：阈值 0.94406、WarmUp 首 D 样本直通、Active 真延迟、
//     开→关 3ms FadingOut → Bypass。
//   - 频响判据：单频 LSQ（非整数窗无偏，双精度递推），误差 <0.1dB；勿断言逐位。
//   - muted/欠载尾/停态窗口语义同 render 实现：muted 整块 memset 先清（跳过链 =
//     平滑冻结）、欠载补零尾不滤波恒零、EQ/限幅状态跨 stop/start 冻结（只允许
//     initialize/clearDspState 停态挂点清零——运行中清空 = 空洞 + 瞬态，违 F5）。
#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace seriona::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- 常量（与既有设备测试装置同构） ----
constexpr std::uint32_t kBlockFrames = 512U;    // 每回调块帧数
constexpr std::uint32_t kRingFrames = 16384U;   // ring 容量（帧）
constexpr std::uint32_t kMeasureFreqHz = 1000U; // 测量单频 = 31 段 EQ idx17 中心
constexpr double kMeasureAmplitude = 0.25;      // 频响测量输入幅值（2^-2 精确）
constexpr float kSettleDb = 6.0F;               // 测量用 band 增益
constexpr std::size_t kBand17 = 17U;            // 31 段 ISO 表 idx17 = 1000Hz
constexpr std::uint32_t kFadeFrames = 2048U;    // 交接等功率双腿总长（帧）

std::uint32_t bpfFor(AudioSampleFormat format, std::uint16_t channels) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U * channels;
  case AudioSampleFormat::Int24:
    return 3U * channels;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U * channels;
  case AudioSampleFormat::Unknown:
    return 0U;
  }
  return 0U;
}

// ---- fake backend（仿 DualFakeBackend 能力声明：格式随 open request 记录）----
class EqFakeBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override {
    return {format};
  }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    format.deviceId = request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    return true;
  }

  [[nodiscard]] bool start() override { return true; }
  [[nodiscard]] bool stop() override { return true; }
  void uninitialize() noexcept override {}

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 4,
                           .actualMode = AudioOutputMode::Mixed,
                           .supportedSampleFormats = {AudioSampleFormat::Float32},
                           .supportedSampleRates = {48000}};
};

// ---- 配置辅助 ----
EqualizerConfig flatEq() {
  EqualizerConfig config{};
  config.mode = EqualizerBandMode::Band31;
  return config; // enabled=false = 出厂直通
}

EqualizerConfig oneBandConfig(float gainDb, bool enabled, bool limiter = false,
                              float preGainDb = 0.0F) {
  EqualizerConfig config = flatEq();
  config.enabled = enabled;
  config.limiterEnabled = limiter;
  config.preGainDb = preGainDb;
  config.bandGainsDb[kBand17] = gainDb; // 31 段 idx17 = 1000Hz（ISO 表，测试装置假设）
  return config;
}

// ---- 内容生成 / 解包（测试侧独立参考实现，与打包写侧同序） ----

// 3 字节小端打包/解包（s24 内容 = 24 位有符号值）。
void packS24(std::int32_t value24, std::uint8_t* bytes) noexcept {
  const auto raw = static_cast<std::uint32_t>(value24) & 0xFFFFFFU;
  bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
}

std::int32_t unpackS24(const std::uint8_t* bytes) noexcept {
  const auto raw24 = static_cast<std::uint32_t>(bytes[0]) |
                     (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16U);
  return raw24 >= 0x800000U ? static_cast<std::int32_t>(raw24) - 0x1000000
                            : static_cast<std::int32_t>(raw24);
}

// 内容编码：幅值 s（[-1,1]）→ 该格式量化样本（环内容域；s24 为 24 位值）。
std::int32_t encodeSample(double s, AudioSampleFormat format) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16:
    return static_cast<std::int32_t>(std::llround(s * 32768.0));
  case AudioSampleFormat::Int24:
    return static_cast<std::int32_t>(std::llround(s * 8388608.0)); // 2^23
  case AudioSampleFormat::Int32:
    return static_cast<std::int32_t>(std::llround(s * 2147483648.0));
  case AudioSampleFormat::Float32:
    return 0; // 不经此路径（f32 直写）
  case AudioSampleFormat::Unknown:
    return 0;
  }
  return 0;
}

// 分格式落盘：把量化样本按格式写入目标字节（每帧每声道 1 样本）。
void writeSampleBytes(std::int32_t value, AudioSampleFormat format, std::uint8_t* dst) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16: {
    const auto v = static_cast<std::int16_t>(value);
    std::memcpy(dst, &v, sizeof(v));
    return;
  }
  case AudioSampleFormat::Int24:
    packS24(value, dst);
    return;
  case AudioSampleFormat::Int32: {
    std::memcpy(dst, &value, sizeof(value));
    return;
  }
  case AudioSampleFormat::Float32: {
    const float v = static_cast<float>(value);
    std::memcpy(dst, &v, sizeof(v));
    return;
  }
  case AudioSampleFormat::Unknown:
    return;
  }
}

// 生成一帧立体声正弦（两声道同值）到 bytes（length = frames×bpf）；phase 计数器
// 保证跨块相位连续（调用方按已写帧数递增）。
void fillSineBytes(AudioSampleFormat format, std::uint16_t channels, std::uint32_t rate,
                   std::uint8_t* bytes, std::uint32_t frames, double freqHz, double amplitude,
                   std::uint64_t& phaseCounter) {
  const double omega = 2.0 * kPi * freqHz / static_cast<double>(rate);
  for (std::uint32_t frame = 0U; frame < frames; ++frame) {
    const double sample = amplitude * std::sin(omega * static_cast<double>(phaseCounter + frame));
    for (std::uint16_t ch = 0U; ch < channels; ++ch) {
      auto* const dst =
          bytes + (static_cast<std::size_t>(frame) * channels + ch) * bpfFor(format, 1);
      if (format == AudioSampleFormat::Float32) {
        const float value = static_cast<float>(sample);
        std::memcpy(dst, &value, sizeof(value));
      } else {
        writeSampleBytes(encodeSample(sample, format), format, dst);
      }
    }
  }
  phaseCounter += frames;
}

// 解码：设备输出域字节 → [-1,1] double（与输入域同尺度：s24 以 24 位值 /2^23）。
double decodeSample(const std::uint8_t* bytes, AudioSampleFormat format) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16: {
    std::int16_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<double>(value) / 32768.0;
  }
  case AudioSampleFormat::Int24:
    return static_cast<double>(unpackS24(bytes)) / 8388608.0;
  case AudioSampleFormat::Int32: {
    std::int32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<double>(value) / 2147483648.0;
  }
  case AudioSampleFormat::Float32: {
    float value;
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<double>(value);
  }
  case AudioSampleFormat::Unknown:
    return 0.0;
  }
  return 0.0;
}

// 解码一块输出：interleaved → 逐声道 double 列（帧序）。
std::vector<double> decodeChannel(const std::uint8_t* bytes, std::uint32_t frames,
                                  std::uint16_t channel, AudioSampleFormat format) {
  std::vector<double> out;
  out.reserve(frames);
  const auto bytesPerFrame = bpfFor(format, 2);
  for (std::uint32_t frame = 0U; frame < frames; ++frame) {
    out.push_back(decodeSample(bytes + static_cast<std::size_t>(frame) * bytesPerFrame +
                                   static_cast<std::size_t>(channel) * bpfFor(format, 1),
                               format));
  }
  return out;
}

// 把 pattern（帧序、interleaved 样本字节）重复铺满一个整块。
std::vector<std::uint8_t> tileBlock(const std::vector<std::uint8_t>& patternBytes,
                                    AudioSampleFormat format, std::uint16_t channels) {
  const std::size_t blockBytes = static_cast<std::size_t>(kBlockFrames) * bpfFor(format, channels);
  REQUIRE(patternBytes.size() <= blockBytes);
  std::vector<std::uint8_t> block(blockBytes);
  for (std::size_t index = 0U; index < blockBytes; ++index) {
    block[index] = patternBytes[index % patternBytes.size()];
  }
  return block;
}

// 单频 LSQ 幅值估计（双精度递推、非整数窗无偏）。
double estimateAmplitude(const std::vector<double>& samples, std::size_t offset,
                         std::size_t windowFrames, double fs, double f0Hz) {
  REQUIRE(offset + windowFrames <= samples.size());
  const double omega = 2.0 * kPi * f0Hz / fs;
  double cosPhase = 1.0;
  double sinPhase = 0.0;
  const double rotC = std::cos(omega);
  const double rotS = std::sin(omega);
  double sumCw = 0.0;
  double sumSw = 0.0;
  double sumCC = 0.0;
  double sumSS = 0.0;
  double sumCS = 0.0;
  for (std::size_t i = offset; i < offset + windowFrames; ++i) {
    const double w = samples[i];
    sumCw += cosPhase * w;
    sumSw += sinPhase * w;
    sumCC += cosPhase * cosPhase;
    sumSS += sinPhase * sinPhase;
    sumCS += cosPhase * sinPhase;
    const double nextC = cosPhase * rotC - sinPhase * rotS;
    sinPhase = cosPhase * rotS + sinPhase * rotC;
    cosPhase = nextC;
  }
  const double denom = sumCC * sumSS - sumCS * sumCS;
  const double a = (sumCw * sumSS - sumSw * sumCS) / denom;
  const double b = (sumSw * sumCC - sumCw * sumCS) / denom;
  return std::hypot(a, b);
}

double linToDb(double ratio) { return 20.0 * std::log10(ratio); }

// 断言一段（连续块拼接的逐声道列）：无 NaN/∞、幅值有界、逐帧跳变有界、无异常零洞。
struct StreamStats {
  double maxAbs{0.0};
  double maxDelta{0.0};
  std::size_t maxZeroRun{0};
  bool hasNan{false};
};

StreamStats analyzeStream(const std::vector<std::vector<double>>& channelStream) {
  StreamStats stats;
  for (const auto& channel : channelStream) {
    double run = 0.0;
    for (std::size_t i = 0; i < channel.size(); ++i) {
      const double value = channel[i];
      if (std::isnan(value) || std::isinf(value)) {
        stats.hasNan = true;
        continue;
      }
      stats.maxAbs = std::max(stats.maxAbs, std::fabs(value));
      if (i > 0U) {
        const double previous = channel[i - 1U];
        if (!std::isnan(previous) && !std::isinf(previous)) {
          stats.maxDelta = std::max(stats.maxDelta, std::fabs(value - previous));
        }
      }
      if (value == 0.0) {
        ++run;
        stats.maxZeroRun = std::max(stats.maxZeroRun, static_cast<std::size_t>(run));
      } else {
        run = 0.0;
      }
    }
  }
  return stats;
}

// ---- 驱动台：单队列 + 单设备（EQ render 用例通用） ----
// PcmBufferQueue 不可拷贝/移动 → queue 以 unique_ptr 持有；重开/换面时换代。
struct EqRenderRig {
  std::unique_ptr<PcmBufferQueue> queue;
  std::unique_ptr<EqFakeBackend> backend;
  AudioOutputDevice device;
  AudioSampleFormat format{AudioSampleFormat::Float32};
  std::uint32_t rate{48000};
  std::uint16_t channels{2};
  std::uint64_t phase{0}; // 内容相位计数器（跨块连续）
  std::vector<std::uint8_t> output;

  EqRenderRig(AudioSampleFormat fmt, std::uint32_t sampleRate, std::uint16_t ch = 2U)
      : queue(std::make_unique<PcmBufferQueue>(
            PcmBufferQueueConfig{kRingFrames, bpfFor(fmt, ch)})),
        backend(std::make_unique<EqFakeBackend>()),
        device(std::move(backend)),
        format(fmt),
        rate(sampleRate),
        channels(ch) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = fmt,
                                                           .sampleRate = sampleRate,
                                                           .channelCount = ch,
                                                           .bufferFrames = kBlockFrames,
                                                           .pcmQueue = queue.get()}));
    output.resize(static_cast<std::size_t>(kBlockFrames) * bpfFor(fmt, ch));
  }

  // 内容边界重开（Direct 逐曲 / 格式切换同实例重开 = initialize 幂等重放）。
  void reopen(AudioSampleFormat fmt, std::uint32_t sampleRate, std::uint16_t ch = 2U) {
    queue = std::make_unique<PcmBufferQueue>(
        PcmBufferQueueConfig{kRingFrames, bpfFor(fmt, ch)});
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = fmt,
                                                           .sampleRate = sampleRate,
                                                           .channelCount = ch,
                                                           .bufferFrames = kBlockFrames,
                                                           .pcmQueue = queue.get()}));
    format = fmt;
    rate = sampleRate;
    channels = ch;
    phase = 0;
    output.resize(static_cast<std::size_t>(kBlockFrames) * bpfFor(fmt, ch));
  }

  // 同格式换面（rebind 路径；不重建 DSP、不重放快照）。
  void rebindNew() {
    queue = std::make_unique<PcmBufferQueue>(
        PcmBufferQueueConfig{kRingFrames, bpfFor(format, channels)});
    device.rebindQueue(*queue);
    phase = 0;
    output.resize(static_cast<std::size_t>(kBlockFrames) * bpfFor(format, channels));
  }

  // 写一块正弦（默认 1kHz amp 0.25）并回调；返回解码后 L/R 帧序。
  std::vector<std::vector<double>> renderSine(double freqHz, double amplitude) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kBlockFrames) * bpfFor(format, channels));
    fillSineBytes(format, channels, rate, bytes.data(), kBlockFrames, freqHz, amplitude, phase);
    REQUIRE(queue->write(bytes.data(), kBlockFrames));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kBlockFrames) * bpfFor(format, channels), 0);
    AudioOutputDevice::renderCallback(&device, out.data(), kBlockFrames);
    std::vector<std::vector<double>> decoded;
    for (std::uint16_t ch = 0U; ch < channels; ++ch) {
      decoded.push_back(decodeChannel(out.data(), kBlockFrames, ch, format));
    }
    return decoded;
  }

  // 写原始模式字节（roundtrip/逐位用例，恰一整块）并回调；返回原始输出字节。
  std::vector<std::uint8_t> renderRawBlock(const std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() == static_cast<std::size_t>(kBlockFrames) * bpfFor(format, channels));
    REQUIRE(queue->write(bytes.data(), kBlockFrames));
    std::vector<std::uint8_t> out(bytes.size(), 0x5A);
    AudioOutputDevice::renderCallback(&device, out.data(), kBlockFrames);
    return out;
  }
};

// 渲染 warmup+measure 并返回稳定尾段每声道 LSQ 幅值（双声道）。
std::vector<double> settleAndMeasure(EqRenderRig& rig, std::uint32_t warmBlocks,
                                     std::uint32_t measureBlocks, double freqHz,
                                     double amplitude) {
  for (std::uint32_t block = 0U; block < warmBlocks; ++block) {
    static_cast<void>(rig.renderSine(freqHz, amplitude));
  }
  std::vector<std::vector<double>> window(rig.channels);
  const auto measureFrames = static_cast<std::size_t>(measureBlocks) * kBlockFrames;
  for (std::uint32_t block = 0U; block < measureBlocks; ++block) {
    const auto decoded = rig.renderSine(freqHz, amplitude);
    for (std::uint16_t ch = 0U; ch < rig.channels; ++ch) {
      window[ch].insert(window[ch].end(), decoded[ch].begin(), decoded[ch].end());
    }
  }
  std::vector<double> amps;
  for (std::uint16_t ch = 0U; ch < rig.channels; ++ch) {
    amps.push_back(estimateAmplitude(window[ch], 0, measureFrames, rig.rate, freqHz));
  }
  return amps;
}

// 断言 eqAppliedSnapshot 与期望一致（config 字段 + 实际输出率/声道 + 已发布）。
void requireSnapshotEq(const AudioOutputDeviceEqSnapshot& snap, const EqualizerConfig& expectedConfig,
                       std::uint32_t rate, std::uint16_t channels) {
  CHECK(snap.config.enabled == expectedConfig.enabled);
  CHECK(snap.config.mode == expectedConfig.mode);
  CHECK(snap.config.preGainDb == expectedConfig.preGainDb);
  CHECK(snap.config.limiterEnabled == expectedConfig.limiterEnabled);
  for (std::size_t band = 0U; band < snap.config.bandGainsDb.size(); ++band) {
    CHECK(snap.config.bandGainsDb[band] == expectedConfig.bandGainsDb[band]);
  }
  CHECK(snap.sampleRate == rate);
  CHECK(snap.channelCount == channels);
  CHECK(snap.version != 0U);
}

// ---- 双队列/双源驱动台（T10 交接用）：主源 + 第二源各自独立相位/写游标 ----
struct DualRig {
  PcmBufferQueue main;
  PcmBufferQueue second;
  std::unique_ptr<EqFakeBackend> backend;
  AudioOutputDevice device;
  std::uint64_t mainPhase{0};
  std::uint64_t secondPhase{0};
  std::vector<std::vector<double>> stream; // 全程拼接（逐声道）

  DualRig()
      : main(PcmBufferQueueConfig{kRingFrames, 8U}),
        second(PcmBufferQueueConfig{kRingFrames, 8U}),
        backend(std::make_unique<EqFakeBackend>()),
        device(std::move(backend)) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = AudioSampleFormat::Float32,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = kBlockFrames,
                                                           .pcmQueue = &main}));
  }

  void feedMain(double freqHz, double amplitude) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kBlockFrames) * 8U);
    fillSineBytes(AudioSampleFormat::Float32, 2U, 48000, bytes.data(), kBlockFrames, freqHz,
                  amplitude, mainPhase);
    REQUIRE(main.write(bytes.data(), kBlockFrames));
  }

  void feedSecond(double freqHz, double amplitude) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kBlockFrames) * 8U);
    fillSineBytes(AudioSampleFormat::Float32, 2U, 48000, bytes.data(), kBlockFrames, freqHz,
                  amplitude, secondPhase);
    REQUIRE(second.write(bytes.data(), kBlockFrames));
  }

  // 回调一块，输出追加进 stream（内容需先经 feed* 入 ring）。
  void render() {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kBlockFrames) * 8U, 0);
    AudioOutputDevice::renderCallback(&device, out.data(), kBlockFrames);
    if (stream.empty()) {
      stream.resize(2U);
    }
    const auto left = decodeChannel(out.data(), kBlockFrames, 0U, AudioSampleFormat::Float32);
    const auto right = decodeChannel(out.data(), kBlockFrames, 1U, AudioSampleFormat::Float32);
    stream[0].insert(stream[0].end(), left.begin(), left.end());
    stream[1].insert(stream[1].end(), right.begin(), right.end());
  }
};

GainEnvelopeSnapshot instantZeroEnvelope(std::uint32_t version) {
  return GainEnvelopeSnapshot{0.0F, 0.0F, 0U, GainEnvelopeCurve::Linear, version};
}

GainEnvelopeSnapshot equalPowerLeg(float target, std::uint32_t duration, std::uint32_t version,
                                   float start) {
  return GainEnvelopeSnapshot{target, start, duration, GainEnvelopeCurve::EqualPowerPair, version};
}

} // namespace

// ==================== ① 命令→参数→回调生效 ====================

TEST_CASE("eq render: EQ 配置经设备公共面下达后 render 频响成形并可迁移到其它 band") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);

  // 出厂态：EQ 未配置 → 逐位直通（volume 1.0、无包络 → 既有快速路径不触碰样本）。
  const auto passthrough = rig.renderSine(1000.0, kMeasureAmplitude);
  CHECK(passthrough[0].size() == kBlockFrames);
  CHECK(std::fabs(passthrough[0][100]) <= kMeasureAmplitude * 1.001); // 未放大

  // 命令下达（停态窗口——设备未 start）：+6dB@1kHz idx17。
  const auto config = oneBandConfig(kSettleDb, true);
  rig.device.setEqualizerConfig(config);
  auto snap = rig.device.eqAppliedSnapshot();
  requireSnapshotEq(snap, config, 48000, 2);

  // 频响成形：warm 后实测 +6dB ±0.1（两声道）。
  const auto amps = settleAndMeasure(rig, 90U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }

  // 配置迁移到 idx20（2kHz）：1kHz 回落 ≈0dB（裙边 +0.148dB 容差 0.3）、2kHz 成形 +6dB。
  EqualizerConfig moved = flatEq();
  moved.enabled = true;
  moved.bandGainsDb[20U] = kSettleDb; // idx20 = 2000Hz
  rig.device.setEqualizerConfig(moved);
  snap = rig.device.eqAppliedSnapshot();
  requireSnapshotEq(snap, moved, 48000, 2);
  static_cast<void>(settleAndMeasure(rig, 90U, 12U, 1000.0, kMeasureAmplitude)); // 迁移期排空
  const auto amps1k = settleAndMeasure(rig, 10U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps1k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude)) <= 0.3); // 旧 band 已排空（仅裙边）
  }
  const auto amps2k = settleAndMeasure(rig, 10U, 12U, 2000.0, kMeasureAmplitude);
  for (const double amp : amps2k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
}

TEST_CASE("eq render: started 窗口内 setEqualizerConfig 实时受理——下块回调即生效、停态再应用幂等") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  const auto versionAtInit = rig.device.eqAppliedSnapshot().version; // initialize 已重放出厂配置
  REQUIRE(rig.device.start()); // 模拟运行中（回调可能活跃）

  // 运行期下达 EQ：存储 + PENDING 目标层发布——受理前生效快照保持上次真实生效
  // （出厂直通，version 冻结）；首个 render 块首受理投递 DSP 后版本前进并真实生效。
  const auto config = oneBandConfig(kSettleDb, true);
  rig.device.setEqualizerConfig(config);
  auto snap = rig.device.eqAppliedSnapshot();
  CHECK(snap.version == versionAtInit);   // 未受理：快照仍 = 上次真实生效（出厂）
  CHECK(snap.config.enabled == false);

  // 首块 render（受理点）：块首 applyTargets → 平滑起点 0dB——第 1 帧尚未爬坡
  // （advanceSmoothing 在本块内推进；首块首帧 ≈ 0dB，尾帧 ≈ +2dB）。对比出厂直通
  // 断言：本块内增益已开始爬坡（0→+2dB），末 128 帧幅值 ≈ +2dB（0.25×1.259≈0.315
  // > 1.001×0.25——若仍仅存储/直通则 ≤0.2503）。快照在受理后发布（config 前进）。
  const auto first = rig.renderSine(1000.0, kMeasureAmplitude);
  snap = rig.device.eqAppliedSnapshot();
  CHECK(snap.version == versionAtInit + 1U); // 回调受理 = 真实应用一代
  CHECK(snap.config.enabled == true);
  CHECK(snap.sampleRate == 48000U);         // 运行期受理不回写率/声道（停态值恒正确）
  CHECK(snap.channelCount == 2U);
  const double tailAmp =
      estimateAmplitude(first[0], kBlockFrames - 128U, 128U, 48000, 1000.0);
  CHECK(std::fabs(linToDb(tailAmp / kMeasureAmplitude) - 2.0) <= 0.5); // 首块爬坡 ~+2dB

  // 平滑收敛 + 稳态：运行期调节的曲线成形 +6dB（不依赖任何停态窗口/内容边界）。
  const auto amps = settleAndMeasure(rig, 90U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }

  // 停态窗口（stop）同配置再应用：真实应用一代（幂等，生效面 config 不变）。
  const auto versionWhileRunning = rig.device.eqAppliedSnapshot().version;
  CHECK(rig.device.stop());
  rig.device.setEqualizerConfig(config);
  snap = rig.device.eqAppliedSnapshot();
  requireSnapshotEq(snap, config, 48000, 2);
  CHECK(snap.version == versionWhileRunning + 1U);
  CHECK(rig.device.start());

  // 生效后 render：频响成形 +6dB（停态应用后 DSP 已在目标态，无重爬坡）。
  const auto ampsAfter = settleAndMeasure(rig, 90U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : ampsAfter) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
}

TEST_CASE("eq render: 出厂关 → 运行中开总开关——回调从原路径切入链并实时成形") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  REQUIRE(rig.device.start());

  // 出厂态（从未配置 EQ，DSP 已被 initialize 预配置为全零目标）：render 直通。
  const auto passthrough = rig.renderSine(1000.0, kMeasureAmplitude);
  CHECK(std::fabs(passthrough[0][100]) <= kMeasureAmplitude * 1.001);

  // 运行中开启（enabled false→true +6dB@1kHz）：PENDING → 下块受理激活链。
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));
  // 受理块尾帧已爬坡（非直通）——链从「出厂不进链」实时切入。
  const auto activating = rig.renderSine(1000.0, kMeasureAmplitude);
  double maxAbs = 0.0;
  for (const double sample : activating[0]) {
    maxAbs = std::max(maxAbs, std::fabs(sample));
  }
  CHECK(maxAbs > kMeasureAmplitude * 1.001); // 本块已处理（直通会恒 ≤1.001×）
  // 稳态成形 +6dB。
  const auto amps = settleAndMeasure(rig, 90U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }

  // 运行中关闭（enabled true→false）：平滑退出（fade 期仍进链，≤ramp 时长 = 512
  // 帧块下 3 块收敛）→ 收敛后整链旁路 → 回调回落原路径——f32 出厂直通逐位回归。
  rig.device.setEqualizerConfig(flatEq());
  for (int block = 0; block < 4; ++block) { // 退出期块（含受理块）：余块直通断言
    const auto out = rig.renderSine(1000.0, kMeasureAmplitude);
    if (block >= 3) {
      for (const double sample : out[0]) {
        CHECK(std::fabs(sample) <= kMeasureAmplitude * 1.001);
      }
    }
  }
  for (int block = 0; block < 3; ++block) { // 收敛后稳态直通
    const auto out = rig.renderSine(1000.0, kMeasureAmplitude);
    for (const double sample : out[0]) {
      CHECK(std::fabs(sample) <= kMeasureAmplitude * 1.001);
    }
  }
  CHECK(rig.device.eqAppliedSnapshot().config.enabled == false);
}

TEST_CASE("eq render: 运行中调参（band 迁移）无咔哒实时成形、停启冻结保持") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  REQUIRE(rig.device.start());
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));
  static_cast<void>(settleAndMeasure(rig, 90U, 6U, 1000.0, kMeasureAmplitude)); // +6@1k 稳态
  const auto versionBefore = rig.device.eqAppliedSnapshot().version;

  // 运行中迁移到 idx20（2kHz）：受理后 1kHz 平滑排空、2kHz 成形；全程无爆音
  // （逐帧跳变有界——实时受理若跳变目标会端点阶跃 >0.25）。
  EqualizerConfig moved = flatEq();
  moved.enabled = true;
  moved.bandGainsDb[20U] = kSettleDb;
  rig.device.setEqualizerConfig(moved);
  std::vector<std::vector<double>> stream(2U);
  double maxDelta = 0.0;
  for (int block = 0; block < 6; ++block) {
    const auto decoded = rig.renderSine(1000.0, kMeasureAmplitude); // 旧频段观察面
    for (std::size_t ch = 0U; ch < 2U; ++ch) {
      for (std::size_t i = 0U; i < decoded[ch].size(); ++i) {
        stream[ch].push_back(decoded[ch][i]);
        if (stream[ch].size() > 1U) {
          maxDelta = std::max(maxDelta,
                              std::fabs(stream[ch][stream[ch].size() - 1U] - stream[ch][stream[ch].size() - 2U]));
        }
      }
    }
  }
  CHECK(maxDelta <= 0.25); // 无爆音/无端点阶跃（1kHz 0.25 内容斜率上界 ~0.065）
  static_cast<void>(settleAndMeasure(rig, 60U, 6U, 2000.0, kMeasureAmplitude));
  const auto amps2k = settleAndMeasure(rig, 10U, 12U, 2000.0, kMeasureAmplitude);
  for (const double amp : amps2k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
  const auto amps1k = settleAndMeasure(rig, 10U, 12U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps1k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude)) <= 0.3); // 旧 band 已排空（仅裙边）
  }
  CHECK(rig.device.eqAppliedSnapshot().version > versionBefore); // 运行期受理已发布

  // 运行中调参后 stop/start：冻结续接（曲线跨停启保持——运行期目标已在 DSP，
  // 无重爬坡无重放；停态无新 set 不重发快照）。首块全增益（同 895 用例首块判据
  // 0.44–0.56 域），稳态 ±0.15。
  CHECK(rig.device.stop());
  CHECK(rig.device.start());
  const auto resumed = rig.renderSine(2000.0, kMeasureAmplitude);
  const double ampResumed = estimateAmplitude(resumed[0], 0, kBlockFrames, 48000, 2000.0);
  CAPTURE(ampResumed);
  CHECK(ampResumed > 0.44);
  CHECK(ampResumed < 0.56);
  const auto settled = settleAndMeasure(rig, 6U, 12U, 2000.0, kMeasureAmplitude);
  for (const double amp : settled) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
}

TEST_CASE("eq render: s32 设备 EQ 关闭整体回既有路径逐位直通（EQ 开 0dB = 真实 f32 往返有界）") {
  EqRenderRig rig(AudioSampleFormat::Int32, 48000);

  // 内容模式：覆盖中幅/大幅值——含 f32 不可精确域（>2^24 带低位）样本，EQ 开 0dB
  // 时读侧 f32 舍入必然逐位不等（否则 (b) 的 differs 断言失效）。
  std::vector<std::int32_t> values{
      std::int32_t{0},            std::int32_t{1},             std::int32_t{-1},
      std::int32_t{1000000},      std::int32_t{-1000000},      std::int32_t{16777216}, // 2^24（f32 精确）
      std::int32_t{-16777216},    std::int32_t{16777217},      // 2^24+1 → f32 舍入
      std::int32_t{-16777217},    std::int32_t{536870913},     // 2^29+1 → f32 舍入
      std::int32_t{1073741888},   // 2^30+64 → f32 舍入 64LSB
      std::int32_t{-1073741888},  std::int32_t{2147483647},    // 全幅
      std::int32_t{-2147483647 - 1}}; // 全幅（-2^31）
  std::vector<std::uint8_t> patternBytes;
  patternBytes.reserve(values.size() * 2U * 4U);
  for (const std::int32_t v : values) {
    for (int ch = 0; ch < 2; ++ch) { // 立体声
      std::uint8_t raw[4];
      std::memcpy(raw, &v, sizeof(v));
      patternBytes.insert(patternBytes.end(), raw, raw + sizeof(v));
    }
  }
  const auto blockBytes = tileBlock(patternBytes, AudioSampleFormat::Int32, 2U);

  // (a) EQ 出厂关：legacy 快速路径（volume 1.0 不触碰样本）→ 逐字节一致。
  const auto outOff = rig.renderRawBlock(blockBytes);
  CHECK(outOff == blockBytes);

  // (b) EQ 开 + 全 0dB + 限幅关：真实 f32 换算链往返——s32 不是逐位（f32 中间域固有
  //     舍入），但误差有界（f32 精确域 ≤1LSB、其余 ≤64LSB）。若实现误把 EQ 关也走
  //     f32 中间路径，则 (a) 的逐位一致会 FAIL——本断言锁「EQ 关 ≠ 走 f32 路径」。
  auto zeroConfig = flatEq();
  zeroConfig.enabled = true;
  rig.device.setEqualizerConfig(zeroConfig);
  CHECK(rig.device.eqAppliedSnapshot().config.enabled == true);
  const auto outOn = rig.renderRawBlock(blockBytes);
  REQUIRE(outOn.size() == blockBytes.size());
  bool differs = false;
  for (std::size_t i = 0U; i < blockBytes.size(); ++i) {
    differs = differs || (outOn[i] != blockBytes[i]);
  }
  CHECK(differs);
  for (std::size_t index = 0U; index + 4U <= blockBytes.size(); index += 4U) {
    std::int32_t inputValue = 0;
    std::int32_t outputValue = 0;
    std::memcpy(&inputValue, blockBytes.data() + index, sizeof(inputValue));
    std::memcpy(&outputValue, outOn.data() + index, sizeof(outputValue));
    const auto delta = static_cast<std::int64_t>(outputValue) - static_cast<std::int64_t>(inputValue);
    const auto magnitude = std::llabs(static_cast<long long>(inputValue));
    CAPTURE(inputValue);
    if (magnitude <= (1LL << 24)) {
      CHECK(std::llabs(delta) <= 1LL); // f32 精确域 → 往返逐位
    } else {
      CHECK(std::llabs(delta) <= 64LL); // f32 半 ULP ≤ 64 LSB @2^31 域
    }
  }

  // (c) EQ 关闭恢复：整体回既有路径 → 逐字节一致（无残留 f32 处理）。
  rig.device.setEqualizerConfig(flatEq());
  const auto outOffAgain = rig.renderRawBlock(blockBytes);
  CHECK(outOffAgain == blockBytes);
}

// ==================== ③ 格式矩阵（EQ 频响在真实格式换算链上） ====================

TEST_CASE("eq render: 格式矩阵——s16/s24/s32/f32 × 44.1k/48k/192k 中心增益 0.1dB") {
  struct MatrixEntry {
    AudioSampleFormat format;
    std::uint32_t rate;
    const char* name;
  };
  // 7 组合：覆盖全部 4 格式 + 全部 3 档采样率。
  constexpr std::array<MatrixEntry, 7> kMatrix{{
      {AudioSampleFormat::Int16, 44100, "s16@44.1k"},
      {AudioSampleFormat::Int16, 192000, "s16@192k"},
      {AudioSampleFormat::Int24, 48000, "s24@48k"},
      {AudioSampleFormat::Int24, 192000, "s24@192k"},
      {AudioSampleFormat::Int32, 44100, "s32@44.1k"},
      {AudioSampleFormat::Float32, 48000, "f32@48k"},
      {AudioSampleFormat::Float32, 192000, "f32@192k"},
  }};

  for (const auto& entry : kMatrix) {
    CAPTURE(entry.name);
    EqRenderRig rig(entry.format, entry.rate);
    const auto config = oneBandConfig(kSettleDb, true);
    rig.device.setEqualizerConfig(config);
    requireSnapshotEq(rig.device.eqAppliedSnapshot(), config, entry.rate, 2);

    // 每组合逐台独立测量：warm 100 块（≥ EQ 收敛 + 滤波器振铃）+ 末 12 块 LSQ。
    const auto amps = settleAndMeasure(rig, 100U, 12U, kMeasureFreqHz, kMeasureAmplitude);
    for (const double amp : amps) {
      CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
    }
    // 内容全程无越界（若增益被误加倍则 >0.9 → FAIL）。
    CHECK(amps[0] < 0.5 * 1.05);
  }
}

// ==================== ⑤ 输出格式运行时切换（重建入口：跨格式/采样率重开） ====================

TEST_CASE("eq render: 输出格式运行时切换 f32@48k→s32@192k 重开——曲线恢复、快照率更新、首段无瞬态") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  const auto config = oneBandConfig(kSettleDb, true);
  rig.device.setEqualizerConfig(config);
  const auto amps48 = settleAndMeasure(rig, 90U, 12U, kMeasureFreqHz, kMeasureAmplitude);
  for (const double amp : amps48) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
  const auto versionBefore = rig.device.eqAppliedSnapshot().version;

  // 运行时切换 = 同实例重开：initialize 内部 uninitialize + 新格式（s32@192k）重协商
  // → clearDspState 重建 DSP + 幂等重放存储配置（f32→s32、48k→192k 不丢配置）。
  rig.reopen(AudioSampleFormat::Int32, 192000);
  auto snap = rig.device.eqAppliedSnapshot();
  requireSnapshotEq(snap, config, 192000, 2);
  CHECK(snap.version == versionBefore + 1U); // 真实重放发布一代

  // 首段（重建后第一块）：内容从 0 相位起播，EQ 从零状态爬坡（192k 512 帧块
  // ≈2.7ms → ramp≥20ms → 每块 ~0.8dB）→ 远小于终值 0.499；无 NaN、无旧状态阶跃。
  const auto first = rig.renderSine(kMeasureFreqHz, kMeasureAmplitude);
  for (const double sample : first[0]) {
    CHECK(std::isfinite(sample));
    CHECK(std::fabs(sample) <= 0.35); // 首块 ~0.8dB（0.25×1.096≈0.274）；残留阶跃会 >0.45
  }

  // 稳态恢复：s32@192k 域中心增益回到 +6dB（192k 系数、s32 量化链）。
  const auto amps192 = settleAndMeasure(rig, 100U, 12U, kMeasureFreqHz, kMeasureAmplitude);
  for (const double amp : amps192) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
}

// ==================== roundtrip 量化契约（真实换算链） ====================

TEST_CASE("eq render: roundtrip——EQ 开 0dB 真实换算链 s16/s24 逐位、有增益末端单次量化 ≤1LSB") {
  // EQ 链激活（enabled + 全 0dB）时 int 设备逐样本经 f32 中间域再单次量化回设备域；
  // eqDsp 全零配置走 fastBypass（不触碰样本）→ 读侧精确 + 写侧 llround → s16/s24 逐位。
  // 有增益（preGain −3.5dB）时末端为链上唯一量化点：误差 ≤1LSB（防二次量化）。
  auto zeroConfig = flatEq();
  zeroConfig.enabled = true;

  // --- s16：无增益逐位 + 有增益 ≤1LSB ---
  {
    EqRenderRig rig(AudioSampleFormat::Int16, 48000);
    rig.device.setEqualizerConfig(zeroConfig);

    std::vector<std::int16_t> pattern{
        std::int16_t{0},        std::int16_t{-32768},  std::int16_t{32767},
        std::int16_t{1},        std::int16_t{-1},      std::int16_t{16384},
        std::int16_t{-8192},    std::int16_t{12345},   std::int16_t{-23456},
        std::int16_t{4096},     std::int16_t{-16384},  std::int16_t{20000}};
    std::vector<std::uint8_t> patternBytes;
    for (std::int16_t v : pattern) {
      for (int ch = 0; ch < 2; ++ch) { // 立体声
        patternBytes.push_back(static_cast<std::uint8_t>(v & 0xFF));
        patternBytes.push_back(static_cast<std::uint8_t>((static_cast<std::uint16_t>(v) >> 8U) & 0xFFU));
      }
    }
    const auto block = tileBlock(patternBytes, AudioSampleFormat::Int16, 2U);
    const auto out = rig.renderRawBlock(block);
    CHECK(out == block); // EQ 开 0dB + s16：真实换算链逐位往返

    // 有增益 −3.5dB：常量 DC 经链（读精确 f32 → preGain 平滑收敛 → 末端 llround 单次量化）。
    EqualizerConfig gainConfig = flatEq();
    gainConfig.enabled = true;
    gainConfig.preGainDb = -3.5F;
    rig.device.setEqualizerConfig(gainConfig);
    static_cast<void>(settleAndMeasure(rig, 10U, 1U, 1000.0, kMeasureAmplitude)); // preGain 收敛
    const double gainLin = std::pow(10.0, -3.5 / 20.0);
    for (const std::int16_t dc : {std::int16_t{8192}, std::int16_t{-8192}, std::int16_t{16384},
                                  std::int16_t{-16384}, std::int16_t{4096}}) {
      std::vector<std::uint8_t> dcBlock(static_cast<std::size_t>(kBlockFrames) * 4U);
      for (std::uint32_t frame = 0U; frame < kBlockFrames; ++frame) {
        for (std::uint16_t ch = 0U; ch < 2U; ++ch) {
          const auto v = static_cast<std::uint16_t>(dc);
          dcBlock[static_cast<std::size_t>(frame) * 4U + ch * 2U] = static_cast<std::uint8_t>(v & 0xFFU);
          dcBlock[static_cast<std::size_t>(frame) * 4U + ch * 2U + 1U] =
              static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
        }
      }
      const auto dcOut = rig.renderRawBlock(dcBlock);
      const auto expected = std::llround(static_cast<double>(dc) * gainLin);
      for (std::uint32_t frame = 0U; frame < kBlockFrames; ++frame) {
        for (std::uint16_t ch = 0U; ch < 2U; ++ch) {
          std::int16_t outValue = 0;
          std::memcpy(&outValue, dcOut.data() + static_cast<std::size_t>(frame) * 4U + ch * 2U,
                      sizeof(outValue));
          CHECK(std::llabs(static_cast<long long>(outValue) - expected) <= 1LL);
        }
      }
    }
  }

  // --- s24：无增益逐位（24 位值全范围代表 + 左对齐 <<8 语义锁） ---
  {
    EqRenderRig rig(AudioSampleFormat::Int24, 48000);
    rig.device.setEqualizerConfig(zeroConfig);

    std::vector<std::int32_t> pattern{
        std::int32_t{0},           std::int32_t{8388607}, // 2^23−1
        std::int32_t{-8388608},    std::int32_t{1},       std::int32_t{-1},
        std::int32_t{2097152},     std::int32_t{-4194304}, std::int32_t{1234567},
        std::int32_t{-7654321},    std::int32_t{262144},  std::int32_t{-262145},
        std::int32_t{4194304}};
    std::vector<std::uint8_t> patternBytes;
    for (const std::int32_t v24 : pattern) {
      for (int ch = 0; ch < 2; ++ch) { // 立体声
        const std::size_t offset = patternBytes.size();
        patternBytes.resize(offset + 3U);
        packS24(v24, patternBytes.data() + offset);
      }
    }
    const auto block = tileBlock(patternBytes, AudioSampleFormat::Int24, 2U);
    const auto out = rig.renderRawBlock(block);
    CHECK(out == block); // EQ 开 0dB + s24：真实换算链逐位往返
  }
}

// ==================== 移交清单：欠载补零尾 / muted / 停启 / 无残留 ====================

TEST_CASE("eq render: EQ 稳态欠载补零尾恒零无尾音、恢复后相位连续") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));
  static_cast<void>(settleAndMeasure(rig, 90U, 1U, 1000.0, kMeasureAmplitude)); // 稳态 +6

  // 欠载块：ring 仅 200 帧 → 回调请求 512：尾 312 帧由读侧补零。EQ 链只喂
  // copiedFrames（200）——零尾不滤波（若误把补零喂进滤波链会振出尾音）。
  std::vector<std::uint8_t> partial(static_cast<std::size_t>(200U) * 8U);
  fillSineBytes(AudioSampleFormat::Float32, 2U, 48000, partial.data(), 200U, 1000.0,
                kMeasureAmplitude, rig.phase);
  REQUIRE(rig.queue->write(partial.data(), 200U));
  std::vector<std::uint8_t> out(static_cast<std::size_t>(kBlockFrames) * 8U, 0x5A);
  AudioOutputDevice::renderCallback(&rig.device, out.data(), kBlockFrames);
  auto left = decodeChannel(out.data(), kBlockFrames, 0U, AudioSampleFormat::Float32);
  auto right = decodeChannel(out.data(), kBlockFrames, 1U, AudioSampleFormat::Float32);
  for (std::uint32_t frame = 0U; frame < 200U; ++frame) {
    CHECK(std::fabs(left[frame]) <= 0.6); // 有内容段：EQ 成形正弦（≈0.499 峰）
    CHECK(std::fabs(right[frame]) <= 0.6);
  }
  for (std::uint32_t frame = 200U; frame < kBlockFrames; ++frame) {
    CHECK(left[frame] == 0.0); // 补零尾恒零（EQ 稳态不染色）
    CHECK(right[frame] == 0.0);
  }
  CHECK(rig.queue->counters().silenceFrames == 312U);

  // 恢复：满块续播——EQ 状态跨欠载冻结，相位连续，稳态幅值立即回到 +6dB 附近。
  const auto recovered = settleAndMeasure(rig, 6U, 6U, 1000.0, kMeasureAmplitude);
  for (const double amp : recovered) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.15);
  }
}

TEST_CASE("eq render: muted 先清后跳——静音块整块清零、解冻后从冻结增益点续跑") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));

  // 块 0（爬坡起点）：平滑在块首推进一次（0→+2dB，512 帧 @48k 步进 1/3 段）。
  static_cast<void>(rig.renderSine(1000.0, kMeasureAmplitude));

  // muted 期间（块 1-5）：内容继续供流但相位不推进（每块写同一段 512 帧——mute
  // 只清输出，DSP 冻结与内容相位无关；解冻时内容相位与冻结的滤波状态连续，无相位
  // 失配瞬态污染平滑判定）。输出整块 memset 清零（先清）；EQ process 跳过 → 平滑
  // 冻结（块内无推进；若 muted 期状态错误推进，解冻块会跳到 +6dB）。
  rig.device.setMuted(true);
  std::vector<std::uint8_t> muteContent(static_cast<std::size_t>(kBlockFrames) * 8U, 0);
  std::uint64_t frozenPhase = rig.phase; // = 512（块 0 末）；mute 期不推进
  fillSineBytes(AudioSampleFormat::Float32, 2U, 48000, muteContent.data(), kBlockFrames, 1000.0,
                kMeasureAmplitude, frozenPhase);
  std::vector<std::uint8_t> mutedOut(static_cast<std::size_t>(kBlockFrames) * 8U, 0);
  for (int block = 0; block < 5; ++block) {
    REQUIRE(rig.queue->write(muteContent.data(), kBlockFrames));
    AudioOutputDevice::renderCallback(&rig.device, mutedOut.data(), kBlockFrames);
    const auto mutedChannel =
        decodeChannel(mutedOut.data(), kBlockFrames, 0U, AudioSampleFormat::Float32);
    for (const double sample : mutedChannel) {
      CHECK(sample == 0.0);
    }
  }
  rig.device.setMuted(false);

  // 解冻块（块 6）：从冻结点（平滑停在 2dB）继续推进到 +4dB——实测 4dB 增益
  // （幅值 0.25×10^(4/20)=0.397）；若 mute 期误推进/重置则落 +6dB/+2dB，均出界。
  // 系数 2→4dB 切换含瞬态（peaking 极点半径 ~0.992 → τ≈133 帧），故只测末段
  // 128 帧（残余 <0.05dB），容差 ±0.3dB 仍区分 2dB/6dB 各 2dB 偏差。
  const auto unfrozen = rig.renderSine(1000.0, kMeasureAmplitude);
  const std::vector<double> ch0(unfrozen[0].begin(), unfrozen[0].end());
  const std::vector<double> ch1(unfrozen[1].begin(), unfrozen[1].end());
  constexpr std::size_t kTailFrames = 128U;
  const double ampCh0 = estimateAmplitude(ch0, kBlockFrames - kTailFrames, kTailFrames, 48000, 1000.0);
  const double ampCh1 = estimateAmplitude(ch1, kBlockFrames - kTailFrames, kTailFrames, 48000, 1000.0);
  CAPTURE(ampCh0);
  CHECK(std::fabs(linToDb(ampCh0 / kMeasureAmplitude) - 4.0) <= 0.3);
  CHECK(std::fabs(linToDb(ampCh1 / kMeasureAmplitude) - 4.0) <= 0.3);

  // 再一块收敛到 +6（平滑正常收尾，无过冲）。
  const auto amps = settleAndMeasure(rig, 8U, 6U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.15);
  }
}

TEST_CASE("eq render: stop/start 冻结续接——EQ 稳态跨停启保留 + 限幅开关过渡无 pop") {
  // (a) EQ +6 稳态 → stop（停态窗口）→ start：DSP 状态跨停启冻结（stop/pause 不清
  //     DSP），恢复后第一块即全增益——无重爬坡（重爬坡 = 块 0 仅 ~+2dB → 幅值
  //     ~0.315）无瞬态。限幅器开但内容阈下（0.4989 < 0.9441 → 增益恒 1）。
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true, /*limiter=*/true));
  REQUIRE(rig.device.start()); // stop() 只在 started_ 时解绑回调队列（否则 no-op）
  static_cast<void>(settleAndMeasure(rig, 40U, 1U, 1000.0, kMeasureAmplitude)); // 稳态
  const auto versionBefore = rig.device.eqAppliedSnapshot().version;
  CHECK(rig.device.stop());
  std::vector<std::uint8_t> stopOut(static_cast<std::size_t>(kBlockFrames) * 8U, 0);
  for (int block = 0; block < 3; ++block) { // 停态直渲（无内容供流）：回调面无队列 → 静音
    AudioOutputDevice::renderCallback(&rig.device, stopOut.data(), kBlockFrames);
    const auto silent = decodeChannel(stopOut.data(), kBlockFrames, 0U, AudioSampleFormat::Float32);
    for (const double sample : silent) {
      CHECK(sample == 0.0);
    }
  }
  CHECK(rig.device.start());
  const auto resumed = rig.renderSine(1000.0, kMeasureAmplitude);
  const double ampResumed = estimateAmplitude(resumed[0], 0, kBlockFrames, 48000, 1000.0);
  CAPTURE(ampResumed);
  CHECK(ampResumed > 0.44); // ≈0.499 峰（+6dB 全增益首块即达；重爬坡会 ≤0.35）
  CHECK(ampResumed < 0.56);
  CHECK(rig.device.eqAppliedSnapshot().version == versionBefore); // 停启不重放/不清零

  // (b) 限幅开→关（停态窗口换配置，同实例）：3ms 交叉淡化在进程内执行——内容全程
  //     有界（≤ 输入峰 0.9977 附近）、无爆音（逐帧跳变有界）；淡毕全旁路（限幅解除）。
  auto limiterOffConfig = oneBandConfig(kSettleDb, true, /*limiter=*/false);
  CHECK(rig.device.stop());
  rig.device.setEqualizerConfig(limiterOffConfig);
  CHECK(rig.device.eqAppliedSnapshot().config.limiterEnabled == false);
  CHECK(rig.device.start());
  std::vector<std::vector<double>> transitionStream(2U);
  double maxAbs = 0.0;
  double maxDelta = 0.0;
  bool hasNan = false;
  for (int block = 0; block < 10; ++block) {
    const auto decoded = rig.renderSine(1000.0, 0.5); // 热内容（EQ +6 → 峰 ~0.9977）
    for (std::size_t ch = 0U; ch < 2U; ++ch) {
      for (const double sample : decoded[ch]) {
        hasNan = hasNan || std::isnan(sample) || std::isinf(sample);
        maxAbs = std::max(maxAbs, std::fabs(sample));
        transitionStream[ch].push_back(sample);
      }
    }
  }
  for (const auto& channel : transitionStream) {
    for (std::size_t i = 1U; i < channel.size(); ++i) {
      maxDelta = std::max(maxDelta, std::fabs(channel[i] - channel[i - 1U]));
    }
  }
  CHECK_FALSE(hasNan);
  CHECK(maxAbs <= 1.0);   // 淡化/旁路全程无越界
  CHECK(maxAbs >= 0.97);  // 淡毕限幅解除：热内容恢复到 ~0.9977（若限幅未解除则 ≤0.955）
  CHECK(maxDelta <= 0.25); // 无爆音/无端点阶跃（1kHz 0.5 内容斜率上界 ~0.131）
}

// ==================== ② Direct 逐曲重建（initialize 幂等重放 + 无残留） ====================

TEST_CASE("eq render: Direct 逐曲重建——initialize 幂等重放曲线恢复、旧滤波状态零残留") {
  // 阶段 A：配置在未 initialize 时仅存储（格式未定，快照不发布），initialize 后
  // 幂等重放（version 1 发布 + eqChainActive 置位）→ 频响立即成形。
  {
    PcmBufferQueue queue(PcmBufferQueueConfig{kRingFrames, 8U});
    auto backend = std::make_unique<EqFakeBackend>();
    AudioOutputDevice device(std::move(backend));
    const auto config = oneBandConfig(kSettleDb, true);
    device.setEqualizerConfig(config); // 未 initialize：仅存储
    CHECK(device.eqAppliedSnapshot().version == 0U);
    AudioOutputConfig openConfig{};
    openConfig.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = openConfig,
                                                           .sampleFormat = AudioSampleFormat::Float32,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = kBlockFrames,
                                                           .pcmQueue = &queue}));
    auto snap = device.eqAppliedSnapshot();
    requireSnapshotEq(snap, config, 48000, 2);
    CHECK(snap.version == 1U);
    device.uninitialize();
  }

  // 阶段 B：已生效设备（1kHz +6dB 稳态）模拟 Direct 逐曲重开——同格式 re-open =
  // initialize（新 ring、新内容 3kHz）。内容边界重建 = 旧滤波状态清零（无 1kHz
  // 残留振铃）：首块幅值有界 + 3kHz 稳态 ≈0dB（+6dB@1kHz band 裙外 +0.047dB）。
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));
  const auto amps1k = settleAndMeasure(rig, 90U, 6U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps1k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1); // 1kHz 成形中
  }
  const auto versionBefore = rig.device.eqAppliedSnapshot().version;

  // Direct 逐曲：新 queue + 新内容（3kHz；与旧内容不同频 → 若滤波状态残留，首块
  // 会叠加旧 1kHz 振铃（稳态 ~0.5 幅值量级）→ 首块 max 超界即 FAIL）。
  rig.reopen(AudioSampleFormat::Float32, 48000);
  CHECK(rig.device.eqAppliedSnapshot().version == versionBefore + 1U); // 重建 = 真实重放
  double firstMax = 0.0;
  bool firstNan = false;
  {
    // 首块：3kHz amp 0.25；fresh 链无旧状态（爬坡第一档 ~2dB 对 3kHz 裙外无增益）。
    const auto decoded = rig.renderSine(3000.0, kMeasureAmplitude);
    for (const double sample : decoded[0]) {
      firstNan = firstNan || std::isnan(sample);
      firstMax = std::max(firstMax, std::fabs(sample));
    }
  }
  CHECK_FALSE(firstNan);
  CHECK(firstMax <= 0.42); // 残留（1kHz ~0.5 振铃）会 >0.55；干净首块 ≤ ~0.27

  // 稳态：3kHz 在 1kHz band（Q4.32，裙 +0.047dB）外 → 实测 ≈0dB（±0.15）。
  const auto amps3k = settleAndMeasure(rig, 90U, 10U, 3000.0, kMeasureAmplitude);
  for (const double amp : amps3k) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude)) <= 0.15);
  }
  // 快照保持重放后的配置（rate/format/config 正确回填）。
  requireSnapshotEq(rig.device.eqAppliedSnapshot(), oneBandConfig(kSettleDb, true), 48000, 2);
}

// ==================== rebind 路径参数回放 ====================

TEST_CASE("eq render: rebind 路径参数回放——同格式换面（T7 免重开）后 EQ 曲线保持、快照不重发") {
  EqRenderRig rig(AudioSampleFormat::Float32, 48000);
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true));
  static_cast<void>(settleAndMeasure(rig, 90U, 6U, 1000.0, kMeasureAmplitude));
  const auto versionBefore = rig.device.eqAppliedSnapshot().version;
  const auto snapshotBefore = rig.device.eqAppliedSnapshot();

  // rebind：ring 换代（同格式、同 bpf），EQ 链/状态原样跨面（无停态窗口，无重建）。
  rig.rebindNew();
  CHECK(rig.device.eqAppliedSnapshot().version == versionBefore); // 不重放（快照不发布）

  // 换面后曲线保持：稳态仍是 +6dB@1kHz（若换面误清 EQ → 平直 0dB）。
  const auto amps = settleAndMeasure(rig, 90U, 10U, 1000.0, kMeasureAmplitude);
  for (const double amp : amps) {
    CHECK(std::fabs(linToDb(amp / kMeasureAmplitude) - kSettleDb) <= 0.1);
  }
  const auto snapshotAfter = rig.device.eqAppliedSnapshot();
  CHECK(snapshotAfter.version == snapshotBefore.version);
  CHECK(snapshotAfter.config.enabled == snapshotBefore.config.enabled);
}

// ==================== ④ EQ+限幅激活下双源自动前进无缝交接（T10 等价模拟） ====================

TEST_CASE("eq render: EQ+限幅激活下双源自动前进交接——无空洞无爆音、EQ 曲线交接两侧保持") {
  // 设备级 T10 completeOverlapHandoff 等价序列（服务典序 N7/N8）：
  //   1) 预载第二源（B 内容），activateSecondSource(B, 即时 0 包络 v1)；
  //   2) 发布主源下行腿（source0: 1→0 等功率，时长 kFadeFrames）+ 第二源上行腿
  //      （source1: 0→1 等功率，同长——起点 = 回调 currentGain=0 归零观测后）；
  //   3) 腿完（g0==0、g1==1）→ deactivateSecondSource + resetSourceEnvelope(0/1)
  //      + rebindQueue(B ring)（= completeOverlapHandoff 提升）；
  //   4) 提升后 B ring 继续为主源（其读游标与回调消费位置连续——无空洞）。
  // 全程 EQ(+6dB@1kHz)+限幅器激活（内容跨阈前限幅已过 WarmUp→Active，交接在
  // Active 稳态内进行）。断言：无空洞（无 ≥4 连零、逐块 RMS 下界、双队列零欠载）、
  // 无爆音（逐帧 delta 有界、峰值贴限幅阈、跨交接帧无阶跃）、EQ 曲线在交接两侧
  // 保持（快照版本/配置不变 + 提升后实测 B 内容 +6dB）。
  constexpr double kAmplitudeA = 0.5; // 主源：EQ +6 后 0.9977（超阈 → 限幅活跃）
  constexpr double kAmplitudeB = 0.3; // 第二源：EQ +6 后 0.5987（阈下 → 无压限）
  DualRig rig;
  rig.device.setEqualizerConfig(oneBandConfig(kSettleDb, true, /*limiter=*/true));
  auto snapshotBefore = rig.device.eqAppliedSnapshot();
  requireSnapshotEq(snapshotBefore, oneBandConfig(kSettleDb, true, /*limiter=*/true), 48000, 2);

  // ---- 阶段 0：主源独占预热（EQ 平滑收敛 + 限幅器 WarmUp→Active）----
  for (std::uint32_t block = 0U; block < 40U; ++block) {
    rig.feedMain(1000.0, kAmplitudeA);
    rig.render();
  }
  // 预热后：主源内容（0.9977 峰）被限幅贴阈（稳态出峰 ≈0.9441+0.06dB≈0.951）——
  // 无空洞无爆音（EQ 爬坡 2dB/块使内容在块 2 才跨阈，限幅器那时已 Active）。
  {
    StreamStats stats = analyzeStream(rig.stream);
    CHECK_FALSE(stats.hasNan);
    CHECK(stats.maxAbs <= 0.955); // 限幅稳态上界（≈0.951；未限幅内容会 0.9977）
    CHECK(stats.maxAbs >= 0.93);  // 内容在阈附近流动（限幅未哑音/未越界）
    CHECK(stats.maxDelta <= 0.25); // 1kHz 0.951 内容斜率上界 ~0.125
    CHECK(stats.maxZeroRun <= 4U); // 无 ≥4 连零空洞（正弦过零单点）
  }

  // ---- 阶段 1：第二源激活 + 等功率双腿（T10 交接窗）----
  rig.device.activateSecondSource(rig.second, instantZeroEnvelope(1U));
  CHECK(rig.device.secondSourceActive());
  for (std::uint32_t block = 0U; block < 2U; ++block) { // 激活后阶段块：槽 1 即时落 0
    rig.feedMain(1000.0, kAmplitudeA);
    rig.feedSecond(1000.0, kAmplitudeB);
    rig.render();
  }
  CHECK(rig.device.sourceEnvelopeGain(1U) == 0.0F); // 即时 0 已执行（归零观测）
  rig.device.setSourceEnvelope(0U, equalPowerLeg(0.0F, kFadeFrames, 1U, 1.0F));
  rig.device.setSourceEnvelope(1U, equalPowerLeg(1.0F, kFadeFrames, 2U, 0.0F));
  for (std::uint32_t block = 0U; block < 8U; ++block) { // 4 块腿长 + 4 块持于目标
    rig.feedMain(1000.0, kAmplitudeA);
    rig.feedSecond(1000.0, kAmplitudeB);
    rig.render();
  }
  CHECK(rig.device.sourceEnvelopeGain(0U) == 0.0F); // 腿毕 + 持块后精确到位
  CHECK(rig.device.sourceEnvelopeGain(1U) == 1.0F);

  // ---- 阶段 2：提升（deactivate + 包络复位 + rebindQueue 到 B ring）----
  rig.device.deactivateSecondSource();
  CHECK_FALSE(rig.device.secondSourceActive());
  rig.device.resetSourceEnvelope(0U);
  rig.device.resetSourceEnvelope(1U);
  rig.device.rebindQueue(rig.second);
  // 注意：main 队列从此不再被读（B ring 提升为主源）；主源对象延迟退役（对象存活）。
  for (std::uint32_t block = 0U; block < 90U; ++block) { // 提升后稳态（B 内容续播）
    rig.feedSecond(1000.0, kAmplitudeB);
    rig.render();
  }

  // ---- 全程断言 ----
  StreamStats stats = analyzeStream(rig.stream);
  CHECK_FALSE(stats.hasNan);
  CHECK(stats.maxAbs <= 1.0);
  CHECK(stats.maxAbs >= 0.93); // 全程峰值贴阈（阶段 0/1 限幅活跃；B 段 ≤0.6）
  CHECK(stats.maxDelta <= 0.25); // 全程逐帧跳变有界（跨交接帧无阶跃）
  CHECK(stats.maxZeroRun <= 4U); // 无 ≥4 连零空洞
  // 提升后 tail RMS 下界（无空洞：内容 0.5987 峰 → RMS ≈0.42）：最后 8 块抽查。
  {
    const std::size_t totalFrames = rig.stream[0].size();
    for (std::size_t ch = 0U; ch < 2U; ++ch) {
      double rms = 0.0;
      const std::size_t tailStart = totalFrames - 8U * kBlockFrames;
      for (std::size_t i = tailStart; i < totalFrames; ++i) {
        rms += rig.stream[ch][i] * rig.stream[ch][i];
      }
      rms = std::sqrt(rms / (8.0 * kBlockFrames));
      CHECK(rms > 0.3);
    }
  }
  // 双队列全程零欠载（内容不断供 → 交接两侧无空洞结构性证明）。
  CHECK(rig.main.counters().silenceFrames == 0U);
  CHECK(rig.main.counters().underrunCount == 0U);
  CHECK(rig.second.counters().silenceFrames == 0U);
  CHECK(rig.second.counters().underrunCount == 0U);

  // EQ 配置/生效快照在交接全程保持（无重建、无重放——停态快照链不参与运行中交接）。
  auto snapshotAfter = rig.device.eqAppliedSnapshot();
  CHECK(snapshotAfter.version == snapshotBefore.version);
  requireSnapshotEq(snapshotAfter, oneBandConfig(kSettleDb, true, /*limiter=*/true), 48000, 2);

  // EQ 曲线在交接后仍生效：B 内容（阈下、限幅增益已释放到 1.0）稳态实测 +6dB±0.15。
  // （A 侧输出被限幅贴阈不可测频响——B 侧是曲线保持的干净观测面。）
  const std::size_t totalFrames = rig.stream[0].size();
  const auto tailAmp0 = estimateAmplitude(rig.stream[0], totalFrames - 12U * kBlockFrames,
                                          12U * kBlockFrames, 48000, 1000.0);
  const auto tailAmp1 = estimateAmplitude(rig.stream[1], totalFrames - 12U * kBlockFrames,
                                          12U * kBlockFrames, 48000, 1000.0);
  CHECK(std::fabs(linToDb(tailAmp0 / kAmplitudeB) - kSettleDb) <= 0.15);
  CHECK(std::fabs(linToDb(tailAmp1 / kAmplitudeB) - kSettleDb) <= 0.15);
}

} // namespace seriona::audio
