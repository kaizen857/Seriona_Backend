#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// 任务 9 双源回调面测试（独立用例文件，编入独立目标 seriona_audio_output_device_dualsource_tests
// 二进制，单独注册以便定向运行；同 TU 白盒约定：全部经公共发布面 + 静态 renderCallback 黑盒驱动）。
// 覆盖（fake backend 帧捕获）：
//   - 双源等功率 / 线性腿逐帧数值（起始纯源 0 → 中点 0.7071×(a+b) → 终点纯源 1）；
//   - 第二源未激活路径与单源逐位一致（Float32 与 Int24，volume 0.75，含激活期负控制）；
//   - deactivate 退役纪律：撤销后输出即刻回到单源逐位一致 + ring 对象可安全销毁；
//   - 双源欠载各自补零不串扰（双向）；边界：第二源先排空 → 无 NaN/无跳变。
// 每个 TEST_CASE 独立建 device（发布/受理账本按实例隔离）。期望值镜像与实现同操作序
// （float 同型同序 / Int24 加宽样本域参考数学），可位级或近位级自洽。
namespace seriona::audio {
namespace {

constexpr std::uint32_t kDualBlockFrames = 10U;   // 每回调块帧数（同 envelope 套件粒度）
constexpr std::uint32_t kDualFadeDuration = 100U; // 淡变总长（帧）
constexpr std::uint32_t kDualQueueFrames = 1024U;
constexpr std::uint32_t kDualF32BytesPerFrame = 8U; // Float32 立体声
constexpr std::uint32_t kDualS24BytesPerFrame = 6U;  // Int24 立体声
constexpr std::uint32_t kDualS16BytesPerFrame = 4U;  // Int16 立体声
constexpr std::uint32_t kDualS32BytesPerFrame = 8U;  // Int32 立体声
constexpr float kDualHalfPi = 1.5707963267948966F;   // 与执行器 kEnvelopeHalfPi 同值同型
constexpr float kDualA = 0.5F;                       // 源 0 常量样本（2^-1，反推/镜像精确）
constexpr float kDualB = 0.25F;                      // 源 1 常量样本（2^-2）
constexpr float kDualMidGain = 0.7071067811865475F;  // cos(π/4)=sin(π/4)（等功率中点）

bool dualCloseTo(float actual, float expected, float tolerance = 1e-6F) noexcept {
  return std::fabs(actual - expected) <= tolerance;
}

GainEnvelopeSnapshot dualEnvelope(float target,
                                  std::uint32_t duration,
                                  GainEnvelopeCurve curve,
                                  std::uint32_t version,
                                  float start = 1.0F) noexcept {
  return GainEnvelopeSnapshot{target, start, duration, curve, version};
}

// 执行器 trajectoryGain 的测试侧镜像（float 同操作序；期望值推导用）。
float dualLinearLeg(float start, float target, std::uint32_t pos, std::uint32_t duration) noexcept {
  if (pos >= duration) {
    return target;
  }
  const float progress = static_cast<float>(pos) / static_cast<float>(duration);
  return start + (target - start) * progress;
}

float dualEqualPowerLeg(float start, float target, std::uint32_t pos, std::uint32_t duration) noexcept {
  if (pos >= duration) {
    return target;
  }
  const float progress = static_cast<float>(pos) / static_cast<float>(duration);
  const float theta = progress * kDualHalfPi;
  return std::cos(theta) * start + std::sin(theta) * target;
}

// 双源 Float32 期望：out = (a·g0 + b·g1)·masterVol；masterVol = master(1.0 恒等)·volume。
// 与 mixDualFloat32Frames 同 float 操作序（a·g0、b·g1、求和、×masterVol）。
float dualF32Expect(float a, float b, float g0, float g1, float masterVol) noexcept {
  const auto from0 = a * g0;
  const auto from1 = b * g1;
  return (from0 + from1) * masterVol;
}

// 双源 Int24 期望（加宽样本域参考数学，同实现操作序）：
// 24 位值左对齐 ×256 → leg = llround(double·double) → int64 求和 → llround(sum·masterVol)
// → 取高 24 位。与 mixDualInt24Frames 逐操作一致（leg 乘是单乘无 FMA 收缩点），可位级相等。
std::int32_t dualS24Expect(std::int32_t valueA24,
                           std::int32_t valueB24,
                           float g0,
                           float g1,
                           float masterVol) noexcept {
  const auto leftA = static_cast<double>(valueA24) * 256.0;
  const auto leftB = static_cast<double>(valueB24) * 256.0;
  const auto legA = std::llround(leftA * static_cast<double>(g0));
  const auto legB = std::llround(leftB * static_cast<double>(g1));
  const auto summed = legA + legB;
  const auto scaled = std::llround(static_cast<double>(summed) * static_cast<double>(masterVol));
  return static_cast<std::int32_t>(scaled >> 8);
}

// 双源 Int16 期望（同实现操作序，可位级相等）：
// leg = lround(float·float) → int32 求和 → lround(sum·masterVol) → clamp int16。
// 与 mixDualInt16Frames 逐操作一致（leg 乘是单乘无 FMA 收缩点）。
std::int32_t dualS16Expect(std::int32_t valueA,
                           std::int32_t valueB,
                           float g0,
                           float g1,
                           float masterVol) noexcept {
  const auto legA = std::lround(static_cast<float>(valueA) * g0);
  const auto legB = std::lround(static_cast<float>(valueB) * g1);
  const auto summed = legA + legB;
  const auto scaled = std::lround(static_cast<float>(summed) * masterVol);
  return static_cast<std::int32_t>(std::clamp<long>(
      scaled, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
}

// 双源 Int32 期望（同实现操作序，可位级相等）：
// leg = llround(double·double) → int64 求和 → llround(sum·masterVol) → clamp int32。
// 与 mixDualInt32Frames 逐操作一致。
std::int32_t dualS32Expect(std::int32_t valueA,
                           std::int32_t valueB,
                           float g0,
                           float g1,
                           float masterVol) noexcept {
  const auto legA = std::llround(static_cast<double>(valueA) * static_cast<double>(g0));
  const auto legB = std::llround(static_cast<double>(valueB) * static_cast<double>(g1));
  const auto summed = legA + legB;
  const auto scaled = std::llround(static_cast<double>(summed) * static_cast<double>(masterVol));
  return static_cast<std::int32_t>(std::clamp<long long>(
      scaled, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}

// 3 字节小端打包/解包（测试侧独立参考，与既有 s24 用例一致）。
void dualPackS24(std::int32_t value24, std::uint8_t* bytes) noexcept {
  const auto raw = static_cast<std::uint32_t>(value24) & 0xFFFFFFU;
  bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
}

std::int32_t dualUnpackS24(const std::uint8_t* bytes) noexcept {
  const auto raw24 = static_cast<std::uint32_t>(bytes[0]) |
                     (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16U);
  return raw24 >= 0x800000U ? static_cast<std::int32_t>(raw24) - 0x1000000
                            : static_cast<std::int32_t>(raw24);
}

class DualFakeBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

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

// Float32 双源驱动台：主队列 + 第二队列各持常量样本；runDual 逐块写双源常量并回调。
class DualFloat32Rig {
public:
  DualFloat32Rig()
      : queue(PcmBufferQueueConfig{kDualQueueFrames, kDualF32BytesPerFrame}),
        second(PcmBufferQueueConfig{kDualQueueFrames, kDualF32BytesPerFrame}),
        backend(std::make_unique<DualFakeBackend>()),
        device(std::move(backend)) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = AudioSampleFormat::Float32,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = 4,
                                                           .pcmQueue = &queue}));
  }

  // 双源各写常量块（两声道同值）并回调；返回原始输出样本。
  std::vector<float> runDual(float mainSample, float secondSample) {
    const std::vector<float> mainBlock(kDualBlockFrames * 2U, mainSample);
    const std::vector<float> secondBlock(kDualBlockFrames * 2U, secondSample);
    REQUIRE(queue.write(mainBlock.data(), kDualBlockFrames));
    REQUIRE(second.write(secondBlock.data(), kDualBlockFrames));
    output.assign(kDualBlockFrames * 2U, 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), kDualBlockFrames);
    return output;
  }

  // 单源（第二源未激活）：仅主队列写常量并回调。
  std::vector<float> runSingle(float mainSample) {
    const std::vector<float> mainBlock(kDualBlockFrames * 2U, mainSample);
    REQUIRE(queue.write(mainBlock.data(), kDualBlockFrames));
    output.assign(kDualBlockFrames * 2U, 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), kDualBlockFrames);
    return output;
  }

  // 仅写主队列常量并回调（第二队列保持现状——欠载/排空场景用）。
  std::vector<float> runMainOnly(float mainSample) {
    const std::vector<float> mainBlock(kDualBlockFrames * 2U, mainSample);
    REQUIRE(queue.write(mainBlock.data(), kDualBlockFrames));
    output.assign(kDualBlockFrames * 2U, 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), kDualBlockFrames);
    return output;
  }

  // 一次性向第二队列写 count 帧常量（frameCount 帧 × 2 样本）。
  void fillSecond(float sample, std::uint32_t frameCount) {
    const std::vector<float> block(static_cast<std::size_t>(frameCount) * 2U, sample);
    REQUIRE(second.write(block.data(), frameCount));
  }

  // 写主队列自定义样本（长度 = 帧数×2）并回调（第二源不写）；返回原始输出样本。
  std::vector<float> runMainPattern(const std::vector<float>& samples) {
    REQUIRE(samples.size() % 2U == 0U);
    REQUIRE(queue.write(samples.data(), static_cast<std::uint32_t>(samples.size() / 2U)));
    output.assign(samples.size(), 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size() / 2U));
    return output;
  }

  PcmBufferQueue queue;
  PcmBufferQueue second;
  std::unique_ptr<DualFakeBackend> backend;
  AudioOutputDevice device;
  std::vector<float> output;
};

// Int24 双源驱动台：两队列各持常量 24 位值；runDual 写双源并回调，返回解包样本（帧×2）。
class DualInt24Rig {
public:
  DualInt24Rig(std::int32_t mainValue24, std::int32_t secondValue24)
      : queue(PcmBufferQueueConfig{kDualQueueFrames, kDualS24BytesPerFrame}),
        second(PcmBufferQueueConfig{kDualQueueFrames, kDualS24BytesPerFrame}),
        backend(std::make_unique<DualFakeBackend>()),
        device(std::move(backend)),
        mainValue(mainValue24),
        secondValue(secondValue24) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = AudioSampleFormat::Int24,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = 4,
                                                           .pcmQueue = &queue}));
  }

  std::vector<std::int32_t> runDual() {
    std::vector<std::uint8_t> mainBytes(kDualBlockFrames * kDualS24BytesPerFrame);
    std::vector<std::uint8_t> secondBytes(kDualBlockFrames * kDualS24BytesPerFrame);
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      dualPackS24(mainValue, mainBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame);
      dualPackS24(mainValue, mainBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame + 3U);
      dualPackS24(secondValue, secondBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame);
      dualPackS24(secondValue, secondBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame + 3U);
    }
    REQUIRE(queue.write(mainBytes.data(), kDualBlockFrames));
    REQUIRE(second.write(secondBytes.data(), kDualBlockFrames));
    outputBytes.assign(kDualBlockFrames * kDualS24BytesPerFrame, 0x55);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), kDualBlockFrames);
    std::vector<std::int32_t> unpacked;
    unpacked.reserve(kDualBlockFrames * 2U);
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      unpacked.push_back(dualUnpackS24(outputBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame));
      unpacked.push_back(
          dualUnpackS24(outputBytes.data() + static_cast<std::size_t>(frame) * kDualS24BytesPerFrame + 3U));
    }
    return unpacked;
  }

  std::vector<std::uint8_t> runPattern(const std::vector<std::int32_t>& samples) {
    REQUIRE(samples.size() % 2U == 0U);
    std::vector<std::uint8_t> inputBytes(samples.size() * 3U);
    for (std::size_t index = 0; index < samples.size(); ++index) {
      dualPackS24(samples[index], inputBytes.data() + index * 3U);
    }
    REQUIRE(queue.write(inputBytes.data(), static_cast<std::uint32_t>(samples.size() / 2U)));
    outputBytes.assign(inputBytes.size(), 0x55);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), static_cast<std::uint32_t>(samples.size() / 2U));
    return outputBytes;
  }

  PcmBufferQueue queue;
  PcmBufferQueue second;
  std::unique_ptr<DualFakeBackend> backend;
  AudioOutputDevice device;
  std::int32_t mainValue{0};
  std::int32_t secondValue{0};
  std::vector<std::uint8_t> outputBytes;
};

// Int16 双源驱动台：两队列各持常量 16 位样本（双声道同值）；runDual 写双源并回调，
// 返回原始输出样本（帧×2）。与 DualInt24Rig 同构。
class DualInt16Rig {
public:
  DualInt16Rig(std::int16_t mainValue, std::int16_t secondValue)
      : queue(PcmBufferQueueConfig{kDualQueueFrames, kDualS16BytesPerFrame}),
        second(PcmBufferQueueConfig{kDualQueueFrames, kDualS16BytesPerFrame}),
        backend(std::make_unique<DualFakeBackend>()),
        device(std::move(backend)),
        mainValue(mainValue),
        secondValue(secondValue) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = AudioSampleFormat::Int16,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = 4,
                                                           .pcmQueue = &queue}));
  }

  std::vector<std::int16_t> runDual() {
    const std::vector<std::int16_t> mainBlock(kDualBlockFrames * 2U, mainValue);
    const std::vector<std::int16_t> secondBlock(kDualBlockFrames * 2U, secondValue);
    REQUIRE(queue.write(mainBlock.data(), kDualBlockFrames));
    REQUIRE(second.write(secondBlock.data(), kDualBlockFrames));
    output.assign(kDualBlockFrames * 2U, 0);
    AudioOutputDevice::renderCallback(&device, output.data(), kDualBlockFrames);
    return output;
  }

  PcmBufferQueue queue;
  PcmBufferQueue second;
  std::unique_ptr<DualFakeBackend> backend;
  AudioOutputDevice device;
  std::int16_t mainValue{0};
  std::int16_t secondValue{0};
  std::vector<std::int16_t> output;
};

// Int32 双源驱动台：两队列各持常量 32 位样本（双声道同值）；runDual 写双源并回调，
// 返回原始输出样本（帧×2）。与 DualInt24Rig 同构。
class DualInt32Rig {
public:
  DualInt32Rig(std::int32_t mainValue, std::int32_t secondValue)
      : queue(PcmBufferQueueConfig{kDualQueueFrames, kDualS32BytesPerFrame}),
        second(PcmBufferQueueConfig{kDualQueueFrames, kDualS32BytesPerFrame}),
        backend(std::make_unique<DualFakeBackend>()),
        device(std::move(backend)),
        mainValue(mainValue),
        secondValue(secondValue) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = AudioSampleFormat::Int32,
                                                           .sampleRate = 48000,
                                                           .channelCount = 2,
                                                           .bufferFrames = 4,
                                                           .pcmQueue = &queue}));
  }

  std::vector<std::int32_t> runDual() {
    const std::vector<std::int32_t> mainBlock(kDualBlockFrames * 2U, mainValue);
    const std::vector<std::int32_t> secondBlock(kDualBlockFrames * 2U, secondValue);
    REQUIRE(queue.write(mainBlock.data(), kDualBlockFrames));
    REQUIRE(second.write(secondBlock.data(), kDualBlockFrames));
    output.assign(kDualBlockFrames * 2U, 0);
    AudioOutputDevice::renderCallback(&device, output.data(), kDualBlockFrames);
    return output;
  }

  PcmBufferQueue queue;
  PcmBufferQueue second;
  std::unique_ptr<DualFakeBackend> backend;
  AudioOutputDevice device;
  std::int32_t mainValue{0};
  std::int32_t secondValue{0};
  std::vector<std::int32_t> output;
};

}  // namespace

// ============================ 1. 双源等功率交叉逐帧数值 ============================

TEST_CASE("audio_output_device dualsource equal power crossfade frames match pair math") {
  DualFloat32Rig rig;

  // 阶段 0：激活第二源（槽 1 即时落 0.0，v1）+ 尚未发布源 0 包络 → 本块源 0 腿恒等 1.0、
  // 源 1 腿 0：输出 = 纯源 0 常量（与单源值一致，等功率窗口前的现状保持）。
  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  auto out = rig.runDual(kDualA, kDualB);
  REQUIRE(out.size() == kDualBlockFrames * 2U);
  for (const float sample : out) {
    CHECK(sample == kDualA);  // 0.5·1.0 + 0.25·0.0 = 0.5（float 精确）
  }
  CHECK(rig.device.secondSourceActive());
  CHECK(dualCloseTo(rig.device.sourceEnvelopeGain(1U), 0.0F));

  // 发布互补对：源 0 腿 1→0 等功率（v1）、源 1 腿 0→1 等功率（v2，自 currentGain=0 起跑）。
  // 两腿同块受理 → 全局帧 f（自本块起）= 两腿公共 pos → 严格互补（g0²+g1²≈1）。
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 11U; ++block) {  // pos 0..109（含轨迹外持于目标段）
    const auto samples = rig.runDual(kDualA, kDualB);
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
      const auto g1 = dualEqualPowerLeg(0.0F, 1.0F, position, kDualFadeDuration);
      const auto expected = dualF32Expect(kDualA, kDualB, g0, g1, 1.0F);
      CAPTURE(position);
      CAPTURE(expected);
      CHECK(dualCloseTo(samples[frame * 2U], expected));
      CHECK(dualCloseTo(samples[frame * 2U + 1U], expected));
      // 等功率对性质：g0² + g1² = 1（容差内）——腿确实是互补对而非独立曲线。
      CHECK(dualCloseTo(g0 * g0 + g1 * g1, 1.0F, 1e-4F));
    }
    globalFrame += kDualBlockFrames;
  }

  // 锚点（QA 场景）：起点纯源 0（pos 0：g0=cos0=1、g1=sin0=0 → out=a）；
  // 中点 pos 50：g0=g1=cos(π/4) → out = 0.7071·(a+b)（独立锚点用例精确断言）；
  // pos ≥ 100 持于目标：g0=0、g1=1 → out = b（float 精确）。轨迹外读回 = 目标值。
  const auto endSamples = rig.runDual(kDualA, kDualB);  // pos 110..119
  for (const float sample : endSamples) {
    CHECK(sample == kDualB);
  }
  CHECK(rig.device.sourceEnvelopeGain(0U) == 0.0F);
  CHECK(rig.device.sourceEnvelopeGain(1U) == 1.0F);
}

TEST_CASE("audio_output_device dualsource equal power midpoint anchor equals root half times sum") {
  // 独立的锚点用例：只跑一段精确覆盖中点 pos 50 的窗口，直接断言 0.7071×(a+b)。
  DualFloat32Rig rig;
  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  static_cast<void>(rig.runDual(kDualA, kDualB));  // 阶段块：槽 1 落 0、源 0 未发布（恒等腿）
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  for (std::uint32_t block = 0U; block < 5U; ++block) {  // pos 0..49
    static_cast<void>(rig.runDual(kDualA, kDualB));
  }
  const auto samples = rig.runDual(kDualA, kDualB);  // pos 50..59；帧 0 = pos 50
  const auto expected = kDualMidGain * (kDualA + kDualB);  // 0.7071×(0.5+0.25) = 0.53033…
  CHECK(dualCloseTo(samples[0], expected, 1e-4F));
  CHECK(dualCloseTo(samples[1], expected, 1e-4F));
  // 相邻帧逐镜像核对（pos 51..59），无跳变。
  for (std::uint32_t frame = 1U; frame < kDualBlockFrames; ++frame) {
    const auto position = 50U + frame;
    const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
    const auto g1 = dualEqualPowerLeg(0.0F, 1.0F, position, kDualFadeDuration);
    CHECK(dualCloseTo(samples[frame * 2U], dualF32Expect(kDualA, kDualB, g0, g1, 1.0F), 1e-4F));
  }
}

// ============================ 2. 线性腿双源逐帧 ============================

TEST_CASE("audio_output_device dualsource linear legs crossfade frame exact") {
  DualFloat32Rig rig;
  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  static_cast<void>(rig.runDual(kDualA, kDualB));  // 阶段块：槽 1 落 0
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::Linear, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::Linear, 2U));

  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 10U; ++block) {  // pos 0..99
    const auto samples = rig.runDual(kDualA, kDualB);
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      const auto g0 = dualLinearLeg(1.0F, 0.0F, position, kDualFadeDuration);
      const auto g1 = dualLinearLeg(0.0F, 1.0F, position, kDualFadeDuration);
      CHECK(dualCloseTo(g0 + g1, 1.0F, 1e-4F));  // 线性互补腿
      const auto expected = dualF32Expect(kDualA, kDualB, g0, g1, 1.0F);
      CAPTURE(position);
      CHECK(dualCloseTo(samples[frame * 2U], expected));
      CHECK(dualCloseTo(samples[frame * 2U + 1U], expected));
    }
    globalFrame += kDualBlockFrames;
  }
  // 线性中点 pos 50 = (a+b)/2 = 0.375（两腿各 0.5）。
  CHECK(dualCloseTo(rig.device.sourceEnvelopeGain(0U), dualLinearLeg(1.0F, 0.0F, 99U, kDualFadeDuration)));
  const auto tail = rig.runDual(kDualA, kDualB);  // pos ≥100：持于目标
  for (const float sample : tail) {
    CHECK(sample == kDualB);
  }
}

// ============================ 3. 未激活逐位一致（含负控制） ============================

TEST_CASE("audio_output_device dualsource inactive path bitwise equals single source f32") {
  DualFloat32Rig rig;
  rig.device.setVolume(0.75F);
  const std::vector<float> pattern{0.5F,   -0.25F, 0.125F, -0.0625F, 0.999F,  -0.999F, 0.75F,  -0.5F,
                                   0.25F,  0.0F,   -0.875F, 0.375F,  1.0F,    -1.0F,   0.001F, -0.001F,
                                   0.99F,  -0.33F, 0.66F,   -0.77F};

  // 基线：从未调用第二源发布面（改动前路径）。
  const auto baseline = rig.runMainPattern(pattern);

  // 阶段 1：激活（第二源带数据）期间输出必须与基线不同（负控制——证明阶段 2 的逐位
  // 一致不是「第二源被忽略」的空转）。第二源内容 = 恒定 0.25 混合到全部样本。
  rig.device.activateSecondSource(rig.second, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  {
    const std::vector<float> secondBlock(pattern.size(), 0.25F);
    REQUIRE(rig.second.write(secondBlock.data(), static_cast<std::uint32_t>(secondBlock.size() / 2U)));
    const auto activeOutput = rig.runMainPattern(pattern);
    REQUIRE(activeOutput.size() == baseline.size());
    bool differs = false;
    for (std::size_t index = 0; index < baseline.size(); ++index) {
      differs = differs || (activeOutput[index] != baseline[index]);
    }
    CHECK(differs);
    CHECK(activeOutput[0] != baseline[0]);  // 首样本 0.5 混入 0.25 → (0.5+0.25)×0.75 = 0.5625 ≠ 单源 0.375
  }

  // 阶段 2：deactivate → 输出即刻回到单源逐位一致（无陈旧指针/无残留混音）。
  rig.device.deactivateSecondSource();
  CHECK_FALSE(rig.device.secondSourceActive());
  const auto retiredOutput = rig.runMainPattern(pattern);
  CHECK(retiredOutput == baseline);

  // 阶段 3：第二源从未激活的设备再跑一遍 = 基线（对照）。
  DualFloat32Rig freshRig;
  freshRig.device.setVolume(0.75F);
  const auto freshBaseline = freshRig.runMainPattern(pattern);
  CHECK(freshBaseline == baseline);
}

TEST_CASE("audio_output_device dualsource inactive path bitwise equals single source int24") {
  DualInt24Rig rig(0, 0);
  rig.device.setVolume(0.75F);
  const std::vector<std::int32_t> pattern{-8388608, 8388607, -1,       1,      0,        123456,
                                          -654321,  4194303, -4194304, 2,      -3,       3,
                                          262143,   -262144, 1000000,  -1000000};
  const auto baseline = rig.runPattern(pattern);

  // 激活期负控制：第二源带恒定 500000 → 输出必须不同。
  rig.device.activateSecondSource(rig.second, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  {
    std::vector<std::uint8_t> secondBlock(pattern.size() * 3U);
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      dualPackS24(500000, secondBlock.data() + index * 3U);
    }
    REQUIRE(rig.second.write(secondBlock.data(), static_cast<std::uint32_t>(pattern.size() / 2U)));
    const auto activeOutput = rig.runPattern(pattern);
    bool differs = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      differs = differs || (activeOutput[index * 3U] != baseline[index * 3U]);
    }
    CHECK(differs);
  }

  rig.device.deactivateSecondSource();
  const auto retiredOutput = rig.runPattern(pattern);
  CHECK(retiredOutput == baseline);
}

// ============================ 4. deactivate 退役纪律 ============================

TEST_CASE("audio_output_device dualsource deactivate mid stream retires without stale reads") {
  DualFloat32Rig rig;
  rig.device.setVolume(0.75F);
  const std::vector<float> pattern{0.5F, -0.25F, 0.125F, -0.0625F, 0.999F, -0.999F, 0.75F, -0.5F,
                                   0.25F, 0.0F,   -0.875F, 0.375F,  1.0F,  -1.0F,   0.001F, -0.001F};
  const auto baseline = rig.runMainPattern(pattern);

  // 第二源 ring 在独立作用域持有：deactivate 后立即销毁对象，后续块不得触碰陈旧指针。
  {
    PcmBufferQueue scopedSecond(PcmBufferQueueConfig{kDualQueueFrames, kDualF32BytesPerFrame});
    rig.device.activateSecondSource(scopedSecond, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
    CHECK(rig.device.secondSourceActive());
    const std::vector<float> secondBlock(pattern.size(), 0.25F);
    REQUIRE(scopedSecond.write(secondBlock.data(), static_cast<std::uint32_t>(secondBlock.size() / 2U)));
    const auto activeOutput = rig.runMainPattern(pattern);
    REQUIRE(activeOutput.size() == baseline.size());
    bool differs = false;
    for (std::size_t index = 0; index < baseline.size(); ++index) {
      differs = differs || (activeOutput[index] != baseline[index]);
    }
    CHECK(differs);
    rig.device.deactivateSecondSource();
    CHECK_FALSE(rig.device.secondSourceActive());
  }  // scopedSecond 在 deactivate 之后销毁（回调单线程同步——无跨块陈旧指针窗口）

  // 撤销后连续拉多个块：输出与单源基线逐位一致（无陈旧引用/无崩溃）。
  for (int block = 0; block < 8; ++block) {
    const auto out = rig.runMainPattern(pattern);
    CHECK(out == baseline);
  }
  // 主源再次空队列 → 纯静音（回调路径健康，计数正常）。
  const auto silent = rig.runSingle(0.0F);
  for (const float sample : silent) {
    CHECK(sample == 0.0F);
  }
}

// ============================ 5. 双源欠载各自补零不串扰 ============================

TEST_CASE("audio_output_device dualsource second underrun does not mask main source") {
  DualFloat32Rig rig;
  rig.device.setVolume(0.75F);
  const std::vector<float> pattern{0.5F,  -0.25F, 0.125F, -0.0625F, 0.999F, -0.999F, 0.75F, -0.5F,
                                   0.25F, 0.0F,   -0.875F, 0.375F,  1.0F,  -1.0F,   0.001F, -0.001F};
  // 基线 = 单源路径对同一样本的输出（第二源欠载补零贡献 0 时应与之一致）。
  const auto baseline = rig.runMainPattern(pattern);

  rig.device.activateSecondSource(rig.second, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  // 第二队列为空：双源激活但源 1 欠载 → 输出 = 源 0 贡献（腿恒等 1.0 × volume 0.75）。
  // 数学上 = (s0·1.0 + 0.0·1.0)·(1.0·0.75)，逐位同单源（IEEE 乘/加 1.0 与 +0.0 恒等）。
  const auto dualOutput = rig.runMainPattern(pattern);
  CHECK(dualOutput == baseline);
  // 第二队列独立补零记账：本块 8 帧全部静音、零消费；主队列消费 = 基线 8 + 本块 8。
  CHECK(rig.second.counters().consumedFrames == 0U);
  CHECK(rig.second.counters().silenceFrames == 8U);
  CHECK(rig.queue.counters().consumedFrames == 16U);
  rig.device.deactivateSecondSource();
}

TEST_CASE("audio_output_device dualsource main underrun does not mask second source") {
  DualFloat32Rig rig;
  rig.device.setVolume(0.75F);
  const std::vector<float> pattern{0.5F,  -0.25F, 0.125F, -0.0625F, 0.999F, -0.999F, 0.75F, -0.5F,
                                   0.25F, 0.0F,   -0.875F, 0.375F,  1.0F,  -1.0F,   0.001F, -0.001F};

  rig.device.activateSecondSource(rig.second, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  // 主队列空、第二队列满：输出 = 源 1 贡献单独（主源欠载补零不掩盖第二源）。
  std::vector<float> secondBlock(pattern.size(), 0.25F);
  // 期望 = 逐样本 0.25×0.75（腿恒等 1.0；与单值增益同 float 乘 → 逐位可断言）。
  std::vector<float> expected(pattern.size());
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    expected[index] = 0.25F * 0.75F;
  }
  REQUIRE(rig.second.write(secondBlock.data(), static_cast<std::uint32_t>(secondBlock.size() / 2U)));
  rig.output.assign(pattern.size(), 0.0F);
  AudioOutputDevice::renderCallback(&rig.device, rig.output.data(),
                                    static_cast<std::uint32_t>(rig.output.size() / 2U));
  CHECK(rig.output == expected);
  CHECK(rig.queue.counters().underrunCount == 1U);
  CHECK(rig.queue.counters().silenceFrames == 8U);
  CHECK(rig.second.counters().consumedFrames == 8U);
  rig.device.deactivateSecondSource();
}

// ============================ 6. 边界：第二源先排空 ============================

TEST_CASE("audio_output_device dualsource second drains first no nan no jump") {
  DualFloat32Rig rig;

  // 等功率淡变进行中，第二队列内容先耗尽（预载 45 帧 = 4.5 块，之后不再供数）：
  // 源 1 读取逐块从「满块」退化到「部分 + 补零」再到「全补零」——补零是源 1 自己的
  // 贡献（×腿增益），不得以 NaN/异常跳变污染主源输出。
  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  rig.fillSecond(kDualB, 45U);
  static_cast<void>(rig.runMainOnly(kDualA));  // 阶段块：槽 1 v1 即时落 0（消费 10 帧）
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  // 受理时序：淡变块 0 = 槽 0 v1（1→0 EQ，pos 0 起）+ 槽 1 v2（0→1 EQ，自 0 起）——
  // 两腿同块受理、pos 对齐。第二队列内容覆盖淡变 pos 0..34（阶段块已消费 10 帧），
  // pos 35 起补零 → 内容消失帧的跳变上界 = b·g1(25) ≤ b。
  std::vector<float> allFrames;  // 全程帧样本（判定无 NaN/跳变用）
  float prevSample = 0.0F;
  bool havePrev = false;
  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 10U; ++block) {  // pos 0..99
    const auto samples = rig.runMainOnly(kDualA);
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      CAPTURE(position);
      for (const float sample : {samples[frame * 2U], samples[frame * 2U + 1U]}) {
        CHECK_FALSE(std::isnan(sample));
        CHECK_FALSE(std::isinf(sample));
        REQUIRE(sample >= -1.0F);  // 全程无越界（混音 clamp/值域正确）
        REQUIRE(sample <= 1.0F);
        if (havePrev) {
          const auto delta = std::fabs(sample - prevSample);
          if (position == 35U) {
            // 内容消失帧：跳变 ≤ b·g1(25) + 余量（源 1 腿未归零，其贡献瞬时消失）。
            CHECK(delta <= kDualB + 0.05F);
          } else {
            // 其余帧：腿斜率上界 (a+b)·π/2/D ≈ 0.0118 → 断言 0.02（浮点噪声覆盖）。
            CHECK(delta <= 0.02F);
          }
        }
        prevSample = sample;
        havePrev = true;
        allFrames.push_back(sample);
      }
    }
    globalFrame += kDualBlockFrames;
  }

  // 队列记账：第二队列共消费 45 帧（阶段 10 + 淡变区 35）；淡变区 100 次读取中
  // 补零 65 帧（块 3 部分 5 + 块 4..9 全 60），underrun 只记在第二队列自身。
  CHECK(rig.second.counters().consumedFrames == 45U);
  CHECK(rig.second.counters().silenceFrames == 65U);
  CHECK(allFrames.size() == 200U);
  // 排空后的尾部锚点（global 90..99，块 9）：源 1 内容全零 → 输出 = 纯主源贡献镜像
  // （源 0 腿 g0 = cos(0.90..0.99·π/2) 单调缓降，无异常）。
  const auto tailStart = 90U * 2U;
  for (std::uint32_t frame = 0U; frame < 10U; ++frame) {
    const auto position = 90U + frame;
    const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
    const auto expected = dualF32Expect(kDualA, 0.0F, g0, 0.0F, 1.0F);
    CHECK(dualCloseTo(allFrames[tailStart + frame * 2U], expected, 1e-4F));
    CHECK(dualCloseTo(allFrames[tailStart + frame * 2U + 1U], expected, 1e-4F));
  }
}

// ============================ 7. Int24 双源加宽域累加（任务 4 入口） ============================

TEST_CASE("audio_output_device dualsource int24 equal power widened domain accumulate exact") {
  // Int24 双源数值锁定「解包 → 各自 llround 增益 → Int64 求和 → llround(masterVol) → 打包」
  // 契约（unpackS24ToLeftAlignedS32 区注释预留的任务 9 累加入口）；期望 = dualS24Expect
  // 镜像（同 double 操作序，单乘无 FMA 收缩点 → 位级相等）。
  constexpr std::int32_t kMainValue = 1000000;   // 偶数常量（镜像/折半精确）
  constexpr std::int32_t kSecondValue = 500000;
  DualInt24Rig rig(kMainValue, kSecondValue);

  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  static_cast<void>(rig.runDual());  // 阶段块：槽 1 即时落 0
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 10U; ++block) {  // pos 0..99
    const auto unpacked = rig.runDual();
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
      const auto g1 = dualEqualPowerLeg(0.0F, 1.0F, position, kDualFadeDuration);
      const auto expected = dualS24Expect(kMainValue, kSecondValue, g0, g1, 1.0F);
      CAPTURE(position);
      CAPTURE(expected);
      CHECK(unpacked[frame * 2U] == expected);       // L
      CHECK(unpacked[frame * 2U + 1U] == expected);  // R
    }
    globalFrame += kDualBlockFrames;
  }
  // 中点锚点：pos 50 两腿 = cos(π/4)=sin(π/4) → 左右对齐值按加宽域精确折半/累加。
  // 轨迹外：g0=0、g1=1 → 输出 = 纯源 1 常量（llround 恒等往返）。
  const auto tail = rig.runDual();
  for (const std::int32_t sample : tail) {
    CHECK(sample == kSecondValue);
  }
  CHECK(rig.device.sourceEnvelopeGain(0U) == 0.0F);
  CHECK(rig.device.sourceEnvelopeGain(1U) == 1.0F);
}

TEST_CASE("audio_output_device dualsource stop clears second face so restart is clean") {
  // 停 = 双回调面全清（任务 9 设备纪律）：第二源激活期间 stop()，随后 ring 销毁 +
  // start() 重启 → 回调不得触碰陈旧第二源指针（复活 = ASan/数值脏读）。
  DualFloat32Rig rig;
  rig.device.setVolume(0.75F);
  const std::vector<float> pattern{0.5F, -0.25F, 0.125F, -0.0625F, 0.999F, -0.999F, 0.75F, -0.5F,
                                   0.25F, 0.0F,   -0.875F, 0.375F,  1.0F,  -1.0F,   0.001F, -0.001F};
  const auto baseline = rig.runMainPattern(pattern);
  CHECK(rig.device.start());

  {
    PcmBufferQueue scopedSecond(PcmBufferQueueConfig{kDualQueueFrames, kDualF32BytesPerFrame});
    rig.device.activateSecondSource(scopedSecond, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
    CHECK(rig.device.secondSourceActive());
    // stop()：设备停 → 第二源回调面必须一并清空（防陈旧指针复活）。
    CHECK(rig.device.stop());
    CHECK_FALSE(rig.device.started());
    CHECK_FALSE(rig.device.secondSourceActive());
  }  // scopedSecond 在 stop 之后销毁——重启后任何读取都不得触碰它

  // start() 重启：回调重新发布主队列；输出与单源基线逐位一致（第二源未复活）。
  CHECK(rig.device.start());
  const auto afterRestart = rig.runMainPattern(pattern);
  CHECK(afterRestart == baseline);
  CHECK_FALSE(rig.device.secondSourceActive());

  // 再激活 + uninitialize 同样清面（对象拆除路径同纪律）。
  {
    PcmBufferQueue scopedSecond(PcmBufferQueueConfig{kDualQueueFrames, kDualF32BytesPerFrame});
    rig.device.activateSecondSource(scopedSecond, dualEnvelope(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
    CHECK(rig.device.secondSourceActive());
    rig.device.uninitialize();
    CHECK_FALSE(rig.device.secondSourceActive());
  }
}

// ============================ 8. Int16 / Int32 双源加宽域累加 ============================

TEST_CASE("audio_output_device dualsource int16 equal power widened domain accumulate exact") {
  // Int16 双源数值锁定「int16 → float+lround 各自腿 → int32 求和 → lround(masterVol) →
  // clamp int16」契约；期望 = dualS16Expect 镜像（同 float 操作序，单乘无 FMA 收缩点 → 位级相等）。
  constexpr std::int16_t kMainValue = 16384;  // 偶数常量（镜像/折半精确）
  constexpr std::int16_t kSecondValue = 8192;
  DualInt16Rig rig(kMainValue, kSecondValue);

  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto stage = rig.runDual();  // 阶段块：槽 1 即时落 0、源 0 未发布（腿恒等 1.0）
  for (const std::int16_t sample : stage) {
    CHECK(sample == kMainValue);  // lround(16384·1.0 + 8192·0.0)·1.0 = 16384（float 精确）
  }
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 10U; ++block) {  // pos 0..99
    const auto samples = rig.runDual();
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
      const auto g1 = dualEqualPowerLeg(0.0F, 1.0F, position, kDualFadeDuration);
      const auto expected = dualS16Expect(kMainValue, kSecondValue, g0, g1, 1.0F);
      CAPTURE(position);
      CAPTURE(expected);
      CHECK(samples[frame * 2U] == expected);       // L
      CHECK(samples[frame * 2U + 1U] == expected);  // R
    }
    globalFrame += kDualBlockFrames;
  }
  // 轨迹外：g0=0、g1=1 → 输出 = 纯源 1 常量（lround 恒等往返，位级精确）。
  const auto tail = rig.runDual();
  for (const std::int16_t sample : tail) {
    CHECK(sample == kSecondValue);
  }
  CHECK(rig.device.sourceEnvelopeGain(0U) == 0.0F);
  CHECK(rig.device.sourceEnvelopeGain(1U) == 1.0F);
}

TEST_CASE("audio_output_device dualsource int32 equal power widened domain accumulate exact") {
  // Int32 双源数值锁定「int32 → double+llround 各自腿 → int64 求和 → llround(masterVol) →
  // clamp int32」契约；期望 = dualS32Expect 镜像（同 double 操作序，单乘无 FMA 收缩点 → 位级相等）。
  constexpr std::int32_t kMainValue = 1000000;  // 偶数常量（镜像/折半精确）
  constexpr std::int32_t kSecondValue = 500000;
  DualInt32Rig rig(kMainValue, kSecondValue);

  rig.device.activateSecondSource(rig.second, dualEnvelope(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto stage = rig.runDual();  // 阶段块：槽 1 即时落 0、源 0 未发布（腿恒等 1.0）
  for (const std::int32_t sample : stage) {
    CHECK(sample == kMainValue);  // llround(1000000·1.0 + 500000·0.0)·1.0 = 1000000（double 精确）
  }
  rig.device.setSourceEnvelope(0U, dualEnvelope(0.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
  rig.device.setSourceEnvelope(1U, dualEnvelope(1.0F, kDualFadeDuration, GainEnvelopeCurve::EqualPowerPair, 2U));

  std::uint32_t globalFrame = 0U;
  for (std::uint32_t block = 0U; block < 10U; ++block) {  // pos 0..99
    const auto samples = rig.runDual();
    for (std::uint32_t frame = 0U; frame < kDualBlockFrames; ++frame) {
      const auto position = globalFrame + frame;
      const auto g0 = dualEqualPowerLeg(1.0F, 0.0F, position, kDualFadeDuration);
      const auto g1 = dualEqualPowerLeg(0.0F, 1.0F, position, kDualFadeDuration);
      const auto expected = dualS32Expect(kMainValue, kSecondValue, g0, g1, 1.0F);
      CAPTURE(position);
      CAPTURE(expected);
      CHECK(samples[frame * 2U] == expected);       // L
      CHECK(samples[frame * 2U + 1U] == expected);  // R
    }
    globalFrame += kDualBlockFrames;
  }
  // 轨迹外：g0=0、g1=1 → 输出 = 纯源 1 常量（llround 恒等往返，位级精确）。
  const auto tail = rig.runDual();
  for (const std::int32_t sample : tail) {
    CHECK(sample == kSecondValue);
  }
  CHECK(rig.device.sourceEnvelopeGain(0U) == 0.0F);
  CHECK(rig.device.sourceEnvelopeGain(1U) == 1.0F);
}

}  // namespace seriona::audio
