// EQ 服务转发集成单测（B2.6 服务侧补完，seriona.audio.eq_service_forward）：真实
// SingleTrackAudioPlaybackService + 真实 AudioOutputDevice（fake backend 直编，
// 仿 audio_player_small_buffer_tests 装置——被测目标直接编入 audio_playback_service
// 及其依赖闭包，service 内建 device 无法自外观察，观察点全部走服务公共面）。
//
// 被测面：audio_playback_service.cpp 的 setEqualizer/equalizerState 覆写——配置经
// enqueueCommand worker 串行域转发 device_.setEqualizerConfig（configureTransition
// 同款投递）；语义红线：仅更新均衡器配置，绝不触发输出重载/设备生命周期操作/事件。
// 观察点：
//   - service.equalizerState() = 设备生效面读回（generation = 设备单调代数；
//     config/sampleRate = 实际生效真值；曲线字段零——控制层 reducer 先行产生）；
//   - fake backend 生命周期计数器 = 无重载证明。
//
// 锁定三条（冻结语义，勿改实现只约束行为）：
//   ① 停态（设备已 initialize 未启动）setEqualizer → 立即真实应用：代数推进 +
//      config 一致 + sampleRate = 实际输出率，零设备生命周期操作；
//   ② 运行中（设备 started）setEqualizer 仅存储：生效面代数/配置冻结；下个内容
//      边界（同格式加载路径 clearDspState 幂等重放，T7 免重开）后配置生效——重建不丢；
//   ③ 从未加载（设备未 initialize）setEqualizer 仅存储（生效面空快照如实不动、
//      设备零开启），首个 loadTrack 的 initialize 幂等重放后生效。
#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"
#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr double kPi = 3.141592653589793238462643383279502884;

void writeU16(std::ofstream& stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{static_cast<unsigned char>(value & 0xFFU),
                                                   static_cast<unsigned char>((value >> 8U) & 0xFFU)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream& stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{static_cast<unsigned char>(value & 0xFFU),
                                                  static_cast<unsigned char>((value >> 8U) & 0xFFU),
                                                  static_cast<unsigned char>((value >> 16U) & 0xFFU),
                                                  static_cast<unsigned char>((value >> 24U) & 0xFFU)};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeTag(std::ofstream& stream, const char tag[4]) { stream.write(tag, 4); }

std::filesystem::path writeSineFixture(std::string name, std::uint32_t frames) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  std::filesystem::create_directories(root);

  const std::uint32_t dataSize = frames * sizeof(std::int16_t);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeTag(output, "RIFF");
  writeU32(output, 36U + dataSize);
  writeTag(output, "WAVE");
  writeTag(output, "fmt ");
  writeU32(output, 16U);
  writeU16(output, 1U);
  writeU16(output, 1U);
  writeU32(output, kSampleRate);
  writeU32(output, kSampleRate * sizeof(std::int16_t));
  writeU16(output, sizeof(std::int16_t));
  writeU16(output, 16U);
  writeTag(output, "data");
  writeU32(output, dataSize);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0));
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
  return path;
}

// fake backend：能力声明同 SmallBufferBackend（格式随 open request 记录），另计
// uninitialize 次数（生命周期计数器 = setEqualizer 无重载副作用断言）。
class EqServiceFakeBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    return true;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    started = true;
    return true;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    started = false;
    return true;
  }

  void uninitialize() noexcept override { ++uninitializeCalls; }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  AudioDeviceFormat format{.deviceId = "eq-service-forward",
                           .deviceName = "EQ Service Forward",
                           .backendName = "fake",
                           .sampleRate = kSampleRate,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 1,
                           .bufferFrames = 16,
                           .actualMode = AudioOutputMode::Mixed,
                           .supportedSampleFormats = {AudioSampleFormat::Float32},
                           .supportedSampleRates = {kSampleRate},
                           .isDefaultDevice = false};
  int initializeCalls{0};
  int startCalls{0};
  int stopCalls{0};
  int uninitializeCalls{0};
  bool started{false};
};

TrackPlaybackRequest requestFor(const std::filesystem::path& path) {
  return TrackPlaybackRequest{.trackId = "eq-service-forward-track",
                              .filePath = path,
                              .title = "EQ Service Forward Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

// 输出目标固定（48k Float32 单声道）——使加载协商确定化，device 实际输出率
// = kSampleRate，equalizerState().sampleRate 断言可精确。
AudioOutputConfig outputConfigForTest() {
  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  return config;
}

// 与出厂默认不同的特征配置（enabled + Band31 + preGain + 数个 band + limiter）。
EqualizerConfig eqFeatureConfig() {
  EqualizerConfig config{};
  config.enabled = true;
  config.mode = EqualizerBandMode::Band31;
  config.preGainDb = -3.5F;
  config.bandGainsDb[5] = 2.5F;
  config.bandGainsDb[10] = -4.0F;
  config.bandGainsDb[20] = 1.5F;
  config.limiterEnabled = true;
  return config;
}

bool sameEqConfig(const EqualizerConfig& actual, const EqualizerConfig& expected) {
  return actual.enabled == expected.enabled && actual.mode == expected.mode &&
         actual.preGainDb == expected.preGainDb && actual.limiterEnabled == expected.limiterEnabled &&
         actual.bandGainsDb == expected.bandGainsDb;
}

bool isFactoryDefaultConfig(const EqualizerConfig& config) {
  return sameEqConfig(config, EqualizerConfig{});
}

// 命令队列屏障：queryPlaybackClock 经 promise/future 投递到 worker 串行域，
// 返回时此前全部入队命令已执行完（既有 audio_player 测试同款同步手段；调用
// 方保持仅在命令线程调用）。
void barrier(std::shared_ptr<AudioPlaybackService>& service) {
  static_cast<void>(service->queryPlaybackClock());
}

}  // namespace

TEST_CASE("eq_service stopped_window set_equalizer applies without device reload") {
  const auto path = writeSineFixture("eq_service_forward_stopped.wav", kSampleRate);
  auto backend = std::make_unique<EqServiceFakeBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  REQUIRE(service);

  service->configureOutput(outputConfigForTest());
  barrier(service);
  service->loadTrack(requestFor(path));
  barrier(service);

  // 加载完成：设备已 initialize、未启动（停态窗口）。加载成功点经 clearDspState
  // 幂等重放发布过出厂默认配置的生效快照（代数 ≥1、实际输出率回填）。
  REQUIRE(fake->initializeCalls == 1);
  REQUIRE(fake->startCalls == 0);
  const auto baseline = service->equalizerState();
  REQUIRE(baseline.generation >= 1U);
  REQUIRE(isFactoryDefaultConfig(baseline.config));
  REQUIRE(baseline.sampleRate == kSampleRate);

  const int initBefore = fake->initializeCalls;
  const int startBefore = fake->startCalls;
  const int stopBefore = fake->stopCalls;
  const int uninitBefore = fake->uninitializeCalls;

  service->setEqualizer(eqFeatureConfig());
  barrier(service);

  // ① 停态窗口真实应用：代数推进 + 配置一致 + 实际输出率；零生命周期操作 = 无重载。
  const auto applied = service->equalizerState();
  CHECK(applied.generation > baseline.generation);
  CHECK(sameEqConfig(applied.config, eqFeatureConfig()));
  CHECK(applied.sampleRate == kSampleRate);
  CHECK(fake->initializeCalls == initBefore);
  CHECK(fake->startCalls == startBefore);
  CHECK(fake->stopCalls == stopBefore);
  CHECK(fake->uninitializeCalls == uninitBefore);
  CHECK(!fake->started);
}

TEST_CASE("eq_service running set_equalizer stores until next load rebuild replays") {
  const auto path = writeSineFixture("eq_service_forward_running.wav", kSampleRate);
  auto backend = std::make_unique<EqServiceFakeBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  REQUIRE(service);

  service->configureOutput(outputConfigForTest());
  barrier(service);
  service->loadTrack(requestFor(path));
  barrier(service);
  service->play();
  barrier(service);
  REQUIRE(fake->startCalls == 1);
  REQUIRE(fake->started);

  const auto before = service->equalizerState();
  REQUIRE(before.generation >= 1U);
  REQUIRE(isFactoryDefaultConfig(before.config));

  service->setEqualizer(eqFeatureConfig());
  barrier(service);

  // ②a 运行中到达仅存储：生效面代数/配置冻结（设备 started_ 防御分支，未真实应用）。
  const auto during = service->equalizerState();
  CHECK(during.generation == before.generation);
  CHECK(sameEqConfig(during.config, before.config));
  CHECK(fake->startCalls == 1);
  CHECK(fake->initializeCalls == 1);

  // ②b 下个内容边界（同格式加载路径：stop → T7 免重开 rebind + clearDspState 幂等
  // 重放存储配置）后生效——重建不丢，且无设备重开/重启。
  service->loadTrack(requestFor(path));
  barrier(service);

  const auto after = service->equalizerState();
  CHECK(after.generation > before.generation);
  CHECK(sameEqConfig(after.config, eqFeatureConfig()));
  CHECK(after.sampleRate == kSampleRate);
  CHECK(fake->initializeCalls == 1);
  CHECK(fake->startCalls == 1);
  CHECK(fake->stopCalls == 1);
}

TEST_CASE("eq_service fresh set_equalizer stores until first initialize replays") {
  const auto path = writeSineFixture("eq_service_forward_fresh.wav", kSampleRate);
  auto backend = std::make_unique<EqServiceFakeBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  REQUIRE(service);

  service->configureOutput(outputConfigForTest());
  barrier(service);
  REQUIRE(fake->initializeCalls == 0);
  REQUIRE(fake->startCalls == 0);

  service->setEqualizer(eqFeatureConfig());
  barrier(service);

  // ③a 设备从未 initialize：仅存储——生效面如实保持空快照（gen 0/率 0/出厂配置），
  // setEqualizer 自身零设备开启/零生命周期操作。
  const auto pending = service->equalizerState();
  CHECK(pending.generation == 0U);
  CHECK(pending.sampleRate == 0U);
  CHECK(isFactoryDefaultConfig(pending.config));
  CHECK(fake->initializeCalls == 0);
  CHECK(fake->startCalls == 0);
  CHECK(fake->stopCalls == 0);
  CHECK(fake->uninitializeCalls == 0);

  // ③b 首个 loadTrack 的 initialize 幂等重放存储配置 → 生效（代数 ≥1、实际输出率）。
  service->loadTrack(requestFor(path));
  barrier(service);

  const auto applied = service->equalizerState();
  CHECK(applied.generation >= 1U);
  CHECK(sameEqConfig(applied.config, eqFeatureConfig()));
  CHECK(applied.sampleRate == kSampleRate);
  CHECK(fake->initializeCalls == 1);
  CHECK(!fake->started);
}

}  // namespace seriona::audio
