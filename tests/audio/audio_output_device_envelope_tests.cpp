#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// 增益包络执行器测试（任务 5-B2 测试轮）：
//   - 与既有 audio_output_device_tests.cpp 相同的驱动模式：fake backend + 手工
//     renderCallback；被测音频_output_device.cpp 直接编入本二进制（白盒同 TU 约定见
//     tests/CMakeLists.txt 说明——匿名命名空间助手不跨 TU 可见，故全部经公共发布面 +
//     renderCallback 黑盒驱动，断言可复现真实执行器行为）。
//   - 轨迹精度测试固定 Float32 立体声常量 0.5 输入：乘/除 2 的幂均精确，反推帧增益与
//     执行器帧增益逐位一致；Int24 端到端用「加宽样本域参考数学」（与既有 s24 用例同构）。
//   - 每个 TEST_CASE 独立建 device（发布/受理账本按实例隔离，无跨用例污染）。
namespace seriona::audio {
namespace {

constexpr std::uint32_t kEnvBlockFrames = 10U;        // 每回调块 10 帧（轨迹推进粒度）
constexpr std::uint32_t kEnvDuration = 100U;         // 默认轨迹总长（帧）
constexpr std::uint32_t kEnvQueueFrames = 1024U;
constexpr std::uint32_t kEnvF32BytesPerFrame = 8U;   // Float32 立体声
constexpr std::uint32_t kEnvS24BytesPerFrame = 6U;   // Int24 立体声
constexpr float kEnvInputSample = 0.5F;              // Float32 输入常量（2^-1 → 反推精确）
constexpr float kTestHalfPi = 1.5707963267948966F;   // 与执行器 kEnvelopeHalfPi 同值同型

// 执行器 trajectoryGain 的测试侧镜像（逐操作一致，位级相等；仅作期望值推导）。
float linearTrajectoryMirror(float start, float target, std::uint32_t pos, std::uint32_t duration) noexcept {
  if (pos >= duration) {
    return target;
  }
  const float progress = static_cast<float>(pos) / static_cast<float>(duration);
  return start + (target - start) * progress;
}

float equalPowerTrajectoryMirror(float start, float target, std::uint32_t pos, std::uint32_t duration) noexcept {
  if (pos >= duration) {
    return target;
  }
  const float progress = static_cast<float>(pos) / static_cast<float>(duration);
  const float theta = progress * kTestHalfPi;
  return std::cos(theta) * start + std::sin(theta) * target;
}

bool closeTo(float actual, float expected, float tolerance = 1e-6F) noexcept {
  return std::fabs(actual - expected) <= tolerance;
}

GainEnvelopeSnapshot envelopeSnapshot(float target,
                                      std::uint32_t duration,
                                      GainEnvelopeCurve curve,
                                      std::uint32_t version,
                                      float start = 1.0F) noexcept {
  return GainEnvelopeSnapshot{target, start, duration, curve, version};
}

class FakeAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
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

  [[nodiscard]] bool start() override {
    ++startCalls;
    return true;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    return true;
  }

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
  int startCalls{0};
  int stopCalls{0};
};

AudioOutputDeviceOpenRequest f32OpenRequest(PcmBufferQueue& queue) {
  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  return AudioOutputDeviceOpenRequest{.config = config,
                                      .sampleFormat = AudioSampleFormat::Float32,
                                      .sampleRate = 48000,
                                      .channelCount = 2,
                                      .bufferFrames = 4,
                                      .pcmQueue = &queue};
}

AudioOutputDeviceOpenRequest s24OpenRequest(PcmBufferQueue& queue) {
  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  return AudioOutputDeviceOpenRequest{.config = config,
                                      .sampleFormat = AudioSampleFormat::Int24,
                                      .sampleRate = 48000,
                                      .channelCount = 2,
                                      .bufferFrames = 4,
                                      .pcmQueue = &queue};
}

// Float32 立体声驱动台：每块写 10 帧常量 0.5 → renderCallback → 反推逐帧增益。
// runPattern 返回原始输出样本（B7 位级比较用），runBlock 返回逐帧左声道反推增益。
class Float32Rig {
public:
  Float32Rig()
      : queue(PcmBufferQueueConfig{kEnvQueueFrames, kEnvF32BytesPerFrame}),
        backend(std::make_unique<FakeAudioOutputDeviceBackend>()),
        device(std::move(backend)) {
    REQUIRE(device.initialize(f32OpenRequest(queue)));
  }

  // 写常量 0.5 块并回调；返回该块逐帧增益（L 声道，R 同值）。
  std::vector<float> runBlock() {
    const auto output = runPattern(std::vector<float>(kEnvBlockFrames * 2U, kEnvInputSample));
    std::vector<float> gains;
    gains.reserve(kEnvBlockFrames);
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      gains.push_back(output[frame * 2U] / kEnvInputSample);
    }
    return gains;
  }

  // 写自定义样本（长度 = 帧数×2）并回调；返回原始输出样本。
  std::vector<float> runPattern(const std::vector<float>& samples) {
    REQUIRE(samples.size() % 2U == 0U);
    REQUIRE(queue.write(samples.data(), static_cast<std::uint32_t>(samples.size() / 2U)));
    output.assign(samples.size(), 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size() / 2U));
    return output;
  }

  // 空队列静音块（copiedFrames==0，轨迹不推进路径）。
  void runEmptyBlock() {
    output.assign(kEnvBlockFrames * 2U, 0.0F);
    AudioOutputDevice::renderCallback(&device, output.data(), kEnvBlockFrames);
  }

  PcmBufferQueue queue;
  std::unique_ptr<FakeAudioOutputDeviceBackend> backend;
  AudioOutputDevice device;
  std::vector<float> output;
};

// Int24 立体声驱动台：块内容为常量 value24（两声道同值，6 字节/帧）。
class Int24Rig {
public:
  explicit Int24Rig(std::int32_t value24)
      : queue(PcmBufferQueueConfig{kEnvQueueFrames, kEnvS24BytesPerFrame}),
        backend(std::make_unique<FakeAudioOutputDeviceBackend>()),
        device(std::move(backend)),
        value(value24) {
    REQUIRE(device.initialize(s24OpenRequest(queue)));
  }

  // 写 10 帧常量值并回调；返回该块逐帧逐声道解包结果（帧×2）。
  std::vector<std::int32_t> runBlock() {
    std::vector<std::uint8_t> inputBytes(kEnvBlockFrames * kEnvS24BytesPerFrame);
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      packS24(value, inputBytes.data() + static_cast<std::size_t>(frame) * kEnvS24BytesPerFrame);
      packS24(value, inputBytes.data() + static_cast<std::size_t>(frame) * kEnvS24BytesPerFrame + 3U);
    }
    REQUIRE(queue.write(inputBytes.data(), kEnvBlockFrames));
    outputBytes.assign(kEnvBlockFrames * kEnvS24BytesPerFrame, 0x55);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), kEnvBlockFrames);
    std::vector<std::int32_t> unpacked;
    unpacked.reserve(kEnvBlockFrames * 2U);
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      unpacked.push_back(unpackS24(outputBytes.data() + static_cast<std::size_t>(frame) * kEnvS24BytesPerFrame));
      unpacked.push_back(
          unpackS24(outputBytes.data() + static_cast<std::size_t>(frame) * kEnvS24BytesPerFrame + 3U));
    }
    return unpacked;
  }

  // 写自定义 24 位样本序列（长度 = 帧数×2）并回调；返回原始输出字节（位级比较用）。
  std::vector<std::uint8_t> runPattern(const std::vector<std::int32_t>& samples) {
    REQUIRE(samples.size() % 2U == 0U);
    std::vector<std::uint8_t> inputBytes(samples.size() * 3U);
    for (std::size_t index = 0; index < samples.size(); ++index) {
      packS24(samples[index], inputBytes.data() + index * 3U);
    }
    REQUIRE(queue.write(inputBytes.data(), static_cast<std::uint32_t>(samples.size() / 2U)));
    outputBytes.assign(inputBytes.size(), 0x55);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), static_cast<std::uint32_t>(samples.size() / 2U));
    return outputBytes;
  }

  // 3 字节小端打包/解包（测试侧独立参考，与既有 s24 用例一致）。
  static void packS24(std::int32_t value24, std::uint8_t* bytes) noexcept {
    const auto raw = static_cast<std::uint32_t>(value24) & 0xFFFFFFU;
    bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    bytes[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
  }

  static std::int32_t unpackS24(const std::uint8_t* bytes) noexcept {
    const auto raw24 = static_cast<std::uint32_t>(bytes[0]) |
                       (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                       (static_cast<std::uint32_t>(bytes[2]) << 16U);
    return raw24 >= 0x800000U ? static_cast<std::int32_t>(raw24) - 0x1000000
                              : static_cast<std::int32_t>(raw24);
  }

  PcmBufferQueue queue;
  std::unique_ptr<FakeAudioOutputDeviceBackend> backend;
  AudioOutputDevice device;
  std::int32_t value{0};
  std::vector<std::uint8_t> outputBytes;
};

// 与设备内实现同构的加宽样本域参考数学（24 位值左对齐 ×256，double 增益 + llround，取高 24 位）。
std::int32_t expectedS24Gain(std::int32_t value24, float gain) noexcept {
  const auto scaled = std::llround(static_cast<double>(value24) * 256.0 * static_cast<double>(gain));
  return static_cast<std::int32_t>(scaled >> 8);
}

}  // namespace

// ============================ Group A：发布面（B1 缺口） ============================

TEST_CASE("audio_output_device envelope instant publish master and source read back target") {
  Float32Rig rig;

  SUBCASE("master instant 0.25") {
    rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 1U));
    const auto gains = rig.runBlock();
    for (const float gain : gains) {
      CHECK(closeTo(gain, 0.25F));
    }
    CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));
  }

  SUBCASE("source0 instant 0.3") {
    rig.device.setSourceEnvelope(0U, envelopeSnapshot(0.3F, 0U, GainEnvelopeCurve::Linear, 1U));
    const auto gains = rig.runBlock();
    for (const float gain : gains) {
      CHECK(closeTo(gain, 0.3F));
    }
    CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 0.3F));
  }

  SUBCASE("master 0.25 then source0 0.3 composite 0.075") {
    rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 1U));
    static_cast<void>(rig.runBlock());
    rig.device.setSourceEnvelope(0U, envelopeSnapshot(0.3F, 0U, GainEnvelopeCurve::Linear, 1U));
    const auto gains = rig.runBlock();
    for (const float gain : gains) {
      CHECK(closeTo(gain, 0.25F * 0.3F));
    }
    CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));
    CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 0.3F));
  }
}

TEST_CASE("audio_output_device envelope source slot one publish ignored") {
  Float32Rig rig;

  // 槽 1 发布被编译期关闭：输出不缩放、读回恒 1.0（发布面无副作用可观测）。
  rig.device.setSourceEnvelope(1U, envelopeSnapshot(0.5F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 1.0F));
  }
  CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 1.0F));
  CHECK(closeTo(rig.device.sourceEnvelopeGain(1U), 1.0F));

  // 对照：槽 0 同参数发布生效——证明上面的 1.0 确因槽 1 被忽略而非管线失效。
  rig.device.setSourceEnvelope(0U, envelopeSnapshot(0.5F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto gainsAfterSlot0 = rig.runBlock();
  for (const float gain : gainsAfterSlot0) {
    CHECK(closeTo(gain, 0.5F));
  }
  CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 0.5F));
}

TEST_CASE("audio_output_device envelope nan target publish ignored then valid publish works") {
  Float32Rig rig;

  const auto nan = std::numeric_limits<float>::quiet_NaN();
  rig.device.setMasterEnvelope(envelopeSnapshot(nan, 0U, GainEnvelopeCurve::Linear, 1U));
  auto gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 1.0F));  // 整次发布（含 version）被忽略 → 仍走快速路径
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));

  rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 2U));
  gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 0.25F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));
}

TEST_CASE("audio_output_device envelope stop resets current gain and fresh start runs fast path") {
  Float32Rig rig;
  CHECK(rig.device.start());

  rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 1U));
  auto gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 0.25F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));

  // stop() → resetEnvelopes 清全部 12 字段（含 currentGain——B1 缺陷修复的回归锁）。
  CHECK(rig.device.stop());
  CHECK_FALSE(rig.device.started());
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));

  // start() 后设备回到「从未发布」语义：快速路径，包络零影响。
  CHECK(rig.device.start());
  gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 1.0F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));
}

TEST_CASE("audio_output_device envelope restart clears version ledger so stale nothing accepted and publish acts fresh") {
  Float32Rig rig;
  CHECK(rig.device.start());

  rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 1U));
  static_cast<void>(rig.runBlock());
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));
  CHECK(rig.device.stop());

  // 重启后不发布：version==0==latched → 无受理、无陈旧执行轨迹残留，读回与输出恒 1.0。
  CHECK(rig.device.start());
  auto gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 1.0F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));

  // 版本账本已清零（v==0==latched==0）：重新发布旧版本号 1 会被当作全新包络受理
  // （执行器只按 version != latched 判定，无跨会话单调记忆——这是裁定语义，非缺陷）。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.25F, 0U, GainEnvelopeCurve::Linear, 1U));
  gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 0.25F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.25F));

  // 新版本号照常受理。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.5F, 0U, GainEnvelopeCurve::Linear, 2U));
  gains = rig.runBlock();
  for (const float gain : gains) {
    CHECK(closeTo(gain, 0.5F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.5F));
}

// ============================ Group B：执行轨迹（精确数学） ============================

TEST_CASE("audio_output_device envelope linear ramp per frame exact") {
  Float32Rig rig;
  rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));

  // 块 0..9：全局帧 f = 块×10+j，期望 g = 1 − f/100（与执行器 float 逐位一致）。
  for (std::uint32_t block = 0U; block < 10U; ++block) {
    const auto gains = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = block * kEnvBlockFrames + frame;
      CAPTURE(position);
      CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration)));
    }
  }
  // 含第 99 帧的块写回 currentGain = g(99) = 0.01（非 0.0——轨迹末帧值，裁定语义）。
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.01F));

  // 块 10..11：轨迹外持于目标 0.0。
  for (std::uint32_t block = 0U; block < 2U; ++block) {
    const auto gains = rig.runBlock();
    for (const float gain : gains) {
      CHECK(closeTo(gain, 0.0F));
    }
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.0F));
}

TEST_CASE("audio_output_device envelope equal power pair anchors both directions") {
  SUBCASE("0.0 to 1.0 (staged via instant 0.0)") {
    Float32Rig rig;
    // 执行器恒从回调真值 currentGain 起跑：先即时落 0.0，再发 0→1 EQ 轨迹。
    rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
    static_cast<void>(rig.runBlock());
    CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.0F));

    rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, kEnvDuration, GainEnvelopeCurve::EqualPowerPair, 2U));
    // 受理块从 pos 0 起：块 b 的全局 pos = b×10+j（即时块的 duration==0 不推进进度）。
    for (std::uint32_t block = 0U; block < 8U; ++block) {
      const auto gains = rig.runBlock();
      for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
        const auto position = block * kEnvBlockFrames + frame;
        CAPTURE(position);
        CHECK(closeTo(gains[frame], equalPowerTrajectoryMirror(0.0F, 1.0F, position, kEnvDuration), 1e-4F));
      }
    }
  }

  SUBCASE("1.0 to 0.0 symmetric fresh device") {
    Float32Rig rig;
    rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::EqualPowerPair, 1U));
    std::vector<float> gainAt;
    gainAt.reserve(80U);
    for (std::uint32_t block = 0U; block < 8U; ++block) {
      const auto gains = rig.runBlock();
      gainAt.insert(gainAt.end(), gains.begin(), gains.end());
    }
    CHECK(gainAt.size() == 80U);
    CHECK(closeTo(gainAt[25], std::cos(0.25F * kTestHalfPi), 1e-4F));   // cos(π/8) ≈ 0.92388
    CHECK(closeTo(gainAt[50], std::cos(0.5F * kTestHalfPi), 1e-4F));    // cos(π/4) ≈ 0.70711
    CHECK(closeTo(gainAt[75], std::cos(0.75F * kTestHalfPi), 1e-4F));   // cos(3π/8) ≈ 0.38268
  }

  SUBCASE("midpoint of 0 to 1 ramp equals sin(pi/4)") {
    Float32Rig rig;
    rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, 0U, GainEnvelopeCurve::Linear, 1U));
    static_cast<void>(rig.runBlock());
    rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, kEnvDuration, GainEnvelopeCurve::EqualPowerPair, 2U));
    // 块 0..5 覆盖 pos 0..59；pos 50 = 块 5 帧 0。
    for (std::uint32_t block = 0U; block < 5U; ++block) {
      static_cast<void>(rig.runBlock());
    }
    const auto gains = rig.runBlock();
    CHECK(closeTo(gains[0], std::sin(0.5F * kTestHalfPi), 1e-4F));      // sin(π/4) ≈ 0.70711
    CHECK(closeTo(gains[4], std::sin(0.54F * kTestHalfPi), 1e-4F));     // sin(54π/200)，单调上升
    CHECK(gains[0] < gains[4]);
  }
}

TEST_CASE("audio_output_device envelope instant applies within observing block") {
  Float32Rig rig;
  // 发布后首个块即受理：第 0 帧起增益即为 0.4（同块即时性）。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.4F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto gains = rig.runBlock();
  CHECK(gains.size() == kEnvBlockFrames);
  for (const float gain : gains) {
    CHECK(closeTo(gain, 0.4F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.4F));
}

TEST_CASE("audio_output_device envelope composite master times source per frame") {
  Float32Rig rig;
  // master 即时 0.5 × source0 线性 1→0：帧增益 = 0.5·(1 − f/100)。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.5F, 0U, GainEnvelopeCurve::Linear, 1U));
  rig.device.setSourceEnvelope(0U, envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));

  for (std::uint32_t block = 0U; block < 10U; ++block) {
    const auto gains = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = block * kEnvBlockFrames + frame;
      const auto expected = 0.5F * linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration);
      CAPTURE(position);
      CHECK(closeTo(gains[frame], expected));
    }
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.5F));
  // 含帧 99 的块写回 g(99)=0.01（块末读回粒度，同 B1/B5 语义——不是 0.0）。
  CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 0.01F));

  const auto tailGains = rig.runBlock();  // 轨迹外：0.5 × 0 = 0
  for (const float gain : tailGains) {
    CHECK(closeTo(gain, 0.0F));
  }
  CHECK(closeTo(rig.device.sourceEnvelopeGain(0U), 0.0F));
}

TEST_CASE("audio_output_device envelope mid flight publish rejected until completion then resumes from current gain") {
  Float32Rig rig;

  // v1：Linear 1.0 → 0.0，duration 100。块 0..2（帧 0..29）在途。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));
  for (std::uint32_t block = 0U; block < 3U; ++block) {
    const auto gains = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = block * kEnvBlockFrames + frame;
      CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration)));
    }
  }

  // v2 在途发布（Linear 0.0 → 1.0，版本+1，startGain 0.0 仅为账本参考）。
  rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, kEnvDuration, GainEnvelopeCurve::Linear, 2U, 0.0F));

  // 块 3..9（帧 30..99）：v2 被拒绝，v1 轨迹原样走完——不允许中途生效。
  for (std::uint32_t block = 3U; block < 10U; ++block) {
    const auto gains = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = block * kEnvBlockFrames + frame;
      CAPTURE(position);
      CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration)));
    }
  }
  // v1 完成：含帧 99 的块写回 currentGain = g(99) ≈ 0.01（不是 0.0）。
  const float v1EndGain = rig.device.masterEnvelopeGain();
  CHECK(closeTo(v1EndGain, 0.01F));

  // 块 10：v1 轨迹结束后自动受理 v2——起点 = 回调真值 v1EndGain（≈0.01），非 v2 的
  // 发布 startGain 0.0；首帧无跳变（连续）。
  const auto v2FirstBlock = rig.runBlock();
  CHECK(v2FirstBlock.size() == kEnvBlockFrames);
  CHECK(closeTo(v2FirstBlock[0], v1EndGain, 1e-5F));
  CHECK(v2FirstBlock[0] > 0.005F);  // 起点确实 ≈0.01 而非 0.0
  for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
    CHECK(closeTo(v2FirstBlock[frame], linearTrajectoryMirror(v1EndGain, 1.0F, frame, kEnvDuration), 1e-5F));
  }

  // 块 11..19：v2 从 0.01 单调升向 1.0（全局帧 110..199 → pos = 全局帧 − 100）。
  for (std::uint32_t block = 11U; block < 20U; ++block) {
    const auto gains = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = (block - 10U) * kEnvBlockFrames + frame;
      CHECK(closeTo(gains[frame], linearTrajectoryMirror(v1EndGain, 1.0F, position, kEnvDuration), 1e-5F));
    }
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(),
                linearTrajectoryMirror(v1EndGain, 1.0F, kEnvDuration - 1U, kEnvDuration), 1e-5F));

  // 块 20+：v2 轨迹外持于 1.0。
  const auto v2Tail = rig.runBlock();
  for (const float gain : v2Tail) {
    CHECK(closeTo(gain, 1.0F, 1e-6F));
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));
}

TEST_CASE("audio_output_device envelope silence blocks hold trajectory position") {
  Float32Rig rig;
  rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));

  // 2 个有声块：pos 0..19。
  static_cast<void>(rig.runBlock());
  static_cast<void>(rig.runBlock());
  CHECK(closeTo(rig.device.masterEnvelopeGain(), linearTrajectoryMirror(1.0F, 0.0F, 19U, kEnvDuration)));

  // 5 个空队列静音块：copiedFrames==0 → finalize 早退，进度与 currentGain 均不推进。
  for (int silent = 0; silent < 5; ++silent) {
    rig.runEmptyBlock();
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), linearTrajectoryMirror(1.0F, 0.0F, 19U, kEnvDuration)));

  // 恢复出声：从暂停处 pos 20 续跑（未因 50 个静音帧前移）。
  auto gains = rig.runBlock();
  for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
    const auto position = 20U + frame;
    CAPTURE(frame);
    CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration)));
  }
  gains = rig.runBlock();
  for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
    const auto position = 30U + frame;
    CAPTURE(frame);
    CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.0F, position, kEnvDuration)));
  }
}

TEST_CASE("audio_output_device envelope active path bitwise equals fast path at unity gain") {
  Float32Rig rig;
  rig.device.setVolume(0.75F);

  const std::vector<float> pattern{0.5F,   -0.25F, 0.125F, -0.0625F, 0.999F,  -0.999F, 0.75F,  -0.5F,
                                   0.25F,  0.0F,   -0.875F, 0.375F,  1.0F,    -1.0F,   0.001F, -0.001F,
                                   0.99F,  -0.33F, 0.66F,   -0.77F};

  // 阶段 0（从未发布 → 快速路径）：输出 = 逐样本 ×0.75（单值期望）。
  const auto fastOutput = rig.runPattern(pattern);
  std::vector<float> expected(pattern.size());
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    expected[index] = pattern[index] * 0.75F;
  }
  CHECK(fastOutput == expected);

  // 阶段 1：发布即时 1.0 并完成 → 活动路径与快速路径逐位一致。
  rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto instantUnityOutput = rig.runPattern(pattern);
  CHECK(instantUnityOutput == fastOutput);

  // 阶段 2（负控制）：真实在途轨迹（1.0 → 0.5）中途输出必须与快速路径不同——
  // 证明阶段 1/3 的逐位一致不是「发布被忽略」的空转。帧 0 增益恰 1.0 → 首帧仍相等。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.5F, 50U, GainEnvelopeCurve::Linear, 2U, 0.0F));
  const auto midRampOutput = rig.runPattern(pattern);
  CHECK(midRampOutput[0] == fastOutput[0]);
  bool midRampDiffers = false;
  for (std::size_t index = 1; index < pattern.size(); ++index) {
    midRampDiffers = midRampDiffers || (midRampOutput[index] != fastOutput[index]);
  }
  CHECK(midRampDiffers);

  // 排空 v2（50 帧轨迹，5 块后完成持于 0.5）。
  for (int drain = 0; drain < 4; ++drain) {
    static_cast<void>(rig.runPattern(pattern));
  }

  // 阶段 3：即时回到 1.0 → 活动路径再次与快速路径逐位一致。
  rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, 0U, GainEnvelopeCurve::Linear, 3U));
  const auto backToUnityOutput = rig.runPattern(pattern);
  CHECK(backToUnityOutput == fastOutput);
}

TEST_CASE("audio_output_device envelope int24 linear ramp widened domain reference") {
  // 端到端 Int24：加宽样本域数学（解包 → double 增益 → 打包）随轨迹逐帧正确。
  constexpr std::int32_t kValue = 1000000;  // 偶数：中点（g=0.5）精确折半
  constexpr std::uint32_t kShortDuration = 50U;
  Int24Rig rig(kValue);
  rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kShortDuration, GainEnvelopeCurve::Linear, 1U));

  // 块 0..4：全局帧 f = 块×10+j，期望 = 加宽域参考（同增益 float 下逐位相等）。
  for (std::uint32_t block = 0U; block < 5U; ++block) {
    const auto unpacked = rig.runBlock();
    for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
      const auto position = block * kEnvBlockFrames + frame;
      const auto gain = linearTrajectoryMirror(1.0F, 0.0F, position, kShortDuration);
      const auto expected = expectedS24Gain(kValue, gain);
      CAPTURE(position);
      CAPTURE(expected);
      CHECK(unpacked[frame * 2U] == expected);      // L
      CHECK(unpacked[frame * 2U + 1U] == expected);  // R
    }
  }
  // 锚点：帧 0 恒等（g=1.0）；帧 25（块 2 帧 5）g=0.5 → 精确折半。
  CHECK(closeTo(rig.device.masterEnvelopeGain(), linearTrajectoryMirror(1.0F, 0.0F, 49U, kShortDuration)));

  // 块 5：轨迹外 → 全零。
  const auto tail = rig.runBlock();
  for (const std::int32_t sample : tail) {
    CHECK(sample == 0);
  }
}

TEST_CASE("audio_output_device envelope int24 active path bitwise equals fast path") {
  Int24Rig rig(0);
  rig.device.setVolume(0.75F);

  const std::vector<std::int32_t> pattern{-8388608, 8388607, -1,       1,      0,        123456,
                                          -654321,  4194303, -4194304, 2,       -3,       3,
                                          262143,   -262144, 1000000,  -1000000};
  const auto fastOutput = rig.runPattern(pattern);
  rig.device.setMasterEnvelope(envelopeSnapshot(1.0F, 0U, GainEnvelopeCurve::Linear, 1U));
  const auto activeOutput = rig.runPattern(pattern);
  CHECK(activeOutput == fastOutput);
}

// ============================ Group C：集成 sanity ============================

TEST_CASE("audio_output_device envelope volume and mute combine with source ramp") {
  Float32Rig rig;
  rig.device.setVolume(0.5F);
  rig.device.setSourceEnvelope(0U, envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));

  // 音量 0.5 × source 1→0：帧增益 = 0.5·(1 − f/100)。
  const auto gains = rig.runBlock();
  for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
    const auto expected = 0.5F * linearTrajectoryMirror(1.0F, 0.0F, frame, kEnvDuration);
    CHECK(closeTo(gains[frame], expected));
  }

  // muted：输出全零（无视包络），但轨迹仍按 copiedFrames 推进（块末写回照常）。
  rig.device.setMuted(true);
  const auto mutedBlock = rig.runBlock();
  for (const float sample : mutedBlock) {
    CHECK(sample == 0.0F);
  }
  CHECK(closeTo(rig.device.sourceEnvelopeGain(0U),
                linearTrajectoryMirror(1.0F, 0.0F, 19U, kEnvDuration)));  // pos 10..19 照常推进
}

TEST_CASE("audio_output_device envelope mid flight stop start fresh envelope no leak") {
  Float32Rig rig;
  CHECK(rig.device.start());

  // v1 1→0 中途（pos 30）stop：reset 清空执行轨迹与读回。
  rig.device.setMasterEnvelope(envelopeSnapshot(0.0F, kEnvDuration, GainEnvelopeCurve::Linear, 1U));
  for (int block = 0; block < 3; ++block) {
    static_cast<void>(rig.runBlock());
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.71F));  // pos 0..29 → 末帧 g(29)
  CHECK(rig.device.stop());
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 1.0F));

  // start + 全新包络（1.0 → 0.5）：从干净的 currentGain=1.0 起跑，旧 v1 轨迹不泄漏
  // （若泄漏，首帧应是 pos30 的 0.7 而非 1.0）。
  CHECK(rig.device.start());
  rig.device.setMasterEnvelope(envelopeSnapshot(0.5F, kEnvDuration, GainEnvelopeCurve::Linear, 2U));
  auto gains = rig.runBlock();
  CHECK(closeTo(gains[0], 1.0F));
  for (std::uint32_t frame = 0U; frame < kEnvBlockFrames; ++frame) {
    CHECK(closeTo(gains[frame], linearTrajectoryMirror(1.0F, 0.5F, frame, kEnvDuration)));
  }
  // 排空后持于 0.5。
  for (int block = 1; block < 11; ++block) {
    static_cast<void>(rig.runBlock());
  }
  CHECK(closeTo(rig.device.masterEnvelopeGain(), 0.5F));
}

}  // namespace seriona::audio
