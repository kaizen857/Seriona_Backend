#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct WavSpec {
  std::uint32_t sampleRate{kSampleRate};
  std::uint16_t channels{kChannels};
  std::uint16_t bitsPerSample{kBitsPerSample};
};

void writeU16(std::ofstream& stream, std::uint16_t value) {
  const auto bytes = std::array<unsigned char, 2>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream& stream, std::uint32_t value) {
  const auto bytes = std::array<unsigned char, 4>{
      static_cast<unsigned char>(value & 0xFFU),
      static_cast<unsigned char>((value >> 8U) & 0xFFU),
      static_cast<unsigned char>((value >> 16U) & 0xFFU),
      static_cast<unsigned char>((value >> 24U) & 0xFFU),
  };
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeTag(std::ofstream& stream, const char tag[4]) { stream.write(tag, 4); }

std::vector<std::int16_t> makeSine(std::uint32_t frames, double frequency, WavSpec spec = {}) {
  std::vector<std::int16_t> samples;
  samples.reserve(static_cast<std::size_t>(frames) * spec.channels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * frequency * static_cast<double>(frame)) / static_cast<double>(spec.sampleRate);
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0));
    for (std::uint16_t channel = 0; channel < spec.channels; ++channel) {
      samples.push_back(sample);
    }
  }

  return samples;
}

void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples, WavSpec spec = {}) {
  std::filesystem::create_directories(path.parent_path());
  const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeTag(output, "RIFF");
  writeU32(output, 36U + dataSize);
  writeTag(output, "WAVE");
  writeTag(output, "fmt ");
  writeU32(output, 16U);
  writeU16(output, 1U);
  writeU16(output, spec.channels);
  writeU32(output, spec.sampleRate);
  writeU32(output, spec.sampleRate * spec.channels * (spec.bitsPerSample / 8U));
  writeU16(output, static_cast<std::uint16_t>(spec.channels * (spec.bitsPerSample / 8U)));
  writeU16(output, spec.bitsPerSample);
  writeTag(output, "data");
  writeU32(output, dataSize);

  for (const auto sample : samples) {
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames, double frequency) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(frames, frequency));
  return path;
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames, double frequency, WavSpec spec) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(frames, frequency, spec), spec);
  return path;
}

std::filesystem::path silenceThenSineFixture(std::string name,
                                             std::uint32_t silentFrames,
                                             std::uint32_t audibleFrames,
                                             double frequency,
                                             WavSpec spec = {}) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  std::vector<std::int16_t> samples(static_cast<std::size_t>(silentFrames) * spec.channels, 0);
  const auto audible = makeSine(audibleFrames, frequency, spec);
  samples.insert(samples.end(), audible.begin(), audible.end());
  writeWav(path, samples, spec);
  return path;
}

class SeamlessFakeAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    format.deviceId = request.config.preferredDeviceId.empty() ? "fake-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    format.actualMode = request.config.outputMode;
    // 设备面指针/标志由 worker 线程写、测试线程读：格式字段先写、再以 release 发布
    // 指针与 initialized（读取侧 acquire 配对），防无同步轮询数据竞争。
    queue.store(request.pcmQueue, std::memory_order_release);
    userData.store(request.callbackUserData, std::memory_order_release);
    initialized.store(initializeResult, std::memory_order_release);
    return initializeResult;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    started.store(startResult, std::memory_order_release);
    return startResult;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    started.store(false, std::memory_order_release);
    return stopResult;
  }

  void uninitialize() noexcept override {
    ++uninitializeCalls;
    initialized.store(false, std::memory_order_release);
    started.store(false, std::memory_order_release);
    queue.store(nullptr, std::memory_order_release);
    userData.store(nullptr, std::memory_order_release);
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void consumeFrames(std::uint32_t frames) {
    // 慢机守卫（建模真实设备）：未 initialize / 已 uninitialize 不触发回调——饥饿下
    // worker 可能尚未执行 loadTrack 的 initialize（原 REQUIRE 会致命，BE-01 崩溃首因）。
    auto* data = userData.load(std::memory_order_acquire);
    if (data == nullptr) {
      return;
    }
    const auto bytesPerSample = format.sampleFormat == AudioSampleFormat::Float32 ? 4U : 2U;
    const auto bytesPerFrame = static_cast<std::size_t>(format.channelCount) * bytesPerSample;
    callbackBuffer.assign(static_cast<std::size_t>(frames) * bytesPerFrame, 0U);
    AudioOutputDevice::renderCallback(data, callbackBuffer.data(), frames);
  }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 512,
                           .actualMode = AudioOutputMode::Mixed};
  // 设备面指针/计数由 worker 线程写、测试线程读 → atomic 防无同步轮询数据竞争。
  std::atomic<PcmBufferQueue*> queue{nullptr};
  std::atomic<AudioOutputDevice*> userData{nullptr};
  std::vector<std::uint8_t> callbackBuffer{};
  std::atomic<int> initializeCalls{0};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> uninitializeCalls{0};
  bool initializeResult{true};
  bool startResult{true};
  bool stopResult{true};
  std::atomic<bool> initialized{false};
  std::atomic<bool> started{false};
};

TrackPlaybackRequest request(const std::filesystem::path& path, std::string trackId) {
  return TrackPlaybackRequest{.trackId = std::move(trackId),
                              .filePath = path,
                              .title = "Generated Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

AudioOutputConfig outputConfig(AudioOutputMode mode) {
  AudioOutputConfig config{};
  config.outputMode = mode;
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 20ms;
  return config;
}

AudioOutputConfig smallBufferOutputConfig() {
  auto config = outputConfig(AudioOutputMode::Mixed);
  config.bufferDuration = 1ms;
  return config;
}

// ================= 慢机基础设施（对齐既有 audio 测试模式） =================

// 慢机预算：极端饥饿（nice19 对 8×nice0 占位，≈600x）下单条 worker 命令的调度
// 延迟可达数秒（审计实测 1.5–4.6s）；固定 2s 屏障会在命令尚未执行时超时返回
// 陈旧快照（BE-01 崩溃链起点）。所有"等 worker 追上"的轮询统一用宽裕墙上时钟
// 预算，条件满足即返回（正常档零等待）。
constexpr auto kSlowMachineWaitBudget = std::chrono::seconds{180};

// 事件收集：worker 线程（事件 sink）写、测试线程读 → 互斥保护；断言一律走快照
//（原始裸 vector 并发读写是 BE-01 heap corruption / smallbin double free 的根因）。
// 声明纪律：各用例中 eventLog 必须声明在 service/player 之前——测试中止（REQUIRE
// 失败）时按逆序析构，service/player 先析构（join worker + 清 sink）才安全。
class EventLog {
public:
  BackendEventSink sink() {
    return [this](BackendEvent event) {
      std::lock_guard lock{mutex_};
      events_.push_back(std::move(event));
    };
  }

  [[nodiscard]] std::vector<BackendEvent> snapshot() const {
    std::lock_guard lock{mutex_};
    return events_;
  }

private:
  mutable std::mutex mutex_{};
  std::vector<BackendEvent> events_{};
};

std::vector<BackendEvent> eventsOf(const EventLog& log, BackendEventType type) {
  const auto snapshot = log.snapshot();
  std::vector<BackendEvent> filtered;
  for (const auto& event : snapshot) {
    if (event.type == type) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

// 慢机：谓词式存在性检查（内部快照，worker 并发追加安全）。
bool hasEventType(const EventLog& log, BackendEventType type) {
  const auto snapshot = log.snapshot();
  return std::any_of(snapshot.begin(), snapshot.end(), [type](const BackendEvent& event) {
    return event.type == type;
  });
}

// ================= T8 EndApproaching 服务测试辅助 =================

// CUE bounded 段请求：offset+duration+boundedSegment 三件套齐备才会让
// endPositionFor 返回定界（offset=0 也有值，位置从 0 起算）。
TrackPlaybackRequest boundedSegmentRequest(const std::filesystem::path& path,
                                           std::string trackId,
                                           std::chrono::milliseconds duration,
                                           std::chrono::milliseconds offset = std::chrono::milliseconds{0}) {
  auto segment = request(path, std::move(trackId));
  segment.offset = offset;
  segment.duration = duration;
  segment.boundedSegment = true;
  return segment;
}

// 普通整轨 + 时长元数据（非 CUE：无 offset、非 bounded）——EndApproaching 剩余
// 估算走 duration−clock 分支（非硬停语义，仅作提前量）。
TrackPlaybackRequest durationMetadataRequest(const std::filesystem::path& path,
                                             std::string trackId,
                                             std::chrono::milliseconds duration) {
  auto track = request(path, std::move(trackId));
  track.duration = duration;
  return track;
}

std::vector<BackendEvent> endApproachingEvents(const EventLog& log) {
  return eventsOf(log, BackendEventType::EndApproaching);
}

std::optional<std::chrono::milliseconds> lastEndApproachingRemaining(const EventLog& log) {
  const auto fired = endApproachingEvents(log);
  if (fired.empty()) {
    return std::nullopt;
  }
  return std::get<EndApproaching>(fired.back().payload).remainingMs;
}

// T8 过渡配置助手（只设本任务相关字段；默认即"自动档=无 + 预加载=0"的不武装配置）。
TransitionConfig endApproachConfig(AutoAdvanceFadeMode mode,
                                   std::chrono::milliseconds crossfadeMs,
                                   std::chrono::milliseconds preloadMs,
                                   bool fadeOnTransport = false) {
  TransitionConfig config{};
  config.autoAdvanceFadeMode = mode;
  config.fadeOnTransport = fadeOnTransport;
  config.gaplessPreloadMs = preloadMs;
  config.crossfadeMs = crossfadeMs;
  return config;
}

// 慢机进度屏障：queryPlaybackClock 内部 2s 兜底在饥饿下会超时返回陈旧快照，不能
// 证明"本轮消费已被 servicePlaybackProgress 处理"。判据 = 返回快照的 sampledAt
//（由 worker 上真实执行的进度 tick 写入）不早于发起时刻；未达标则重投（命令 FIFO
// 于 worker 串行域），直到达标或预算耗尽（正常档一次命中）。返回的快照即最近一次
// 真实进度 tick 的结果——保持"每次 driveAndQuery 一次进度 tick"的帧步进语义。
template <typename PlaybackHost>
PlaybackClockSnapshot queryProgressTick(PlaybackHost& host) {
  const auto issuedAt = std::chrono::steady_clock::now();
  auto snapshot = host.queryPlaybackClock();
  const auto deadline = issuedAt + kSlowMachineWaitBudget;
  while (snapshot.sampledAt < issuedAt && std::chrono::steady_clock::now() < deadline) {
    snapshot = host.queryPlaybackClock();
  }
  return snapshot;
}

// 驱动屏障：消费 frames 后投递一次真实进度 tick。48 帧 = 1ms @48k 目标率。T8 用例
// 统一走裸 AudioPlaybackService——configureTransition 与 2 参 prepareNext 在
// AudioPlayer 包装上不可见，故宿主作模板参数（AudioPlaybackService / AudioPlayer）。
template <typename PlaybackHost>
PlaybackClockSnapshot driveAndQuery(PlaybackHost& host,
                                    SeamlessFakeAudioOutputDeviceBackend* fake,
                                    std::uint32_t frames) {
  fake->consumeFrames(frames);
  return queryProgressTick(host);
}

}

TEST_CASE("audio_player_seamless_mix hands prepared next track off without reopening device or emitting playback ended") {
  const auto firstPath = sineFixture("audio_player_seamless_mix_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_seamless_mix_second.wav", 960U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(firstPath, "first"));
  player.prepareNext(request(secondPath, "second"));
  player.play();
  static_cast<void>(queryProgressTick(player));
  fake->consumeFrames(960U);
  const auto afterHandoff = queryProgressTick(player);

  CHECK(fake->initializeCalls.load() == 1);
  CHECK(fake->uninitializeCalls.load() == 0);
  CHECK(fake->startCalls.load() == 1);
  CHECK(fake->started.load());
  CHECK(afterHandoff.trackId == "second");

  const auto ended = eventsOf(eventLog, BackendEventType::PlaybackEnded);
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  CHECK(ended.empty());
  REQUIRE(tracks.size() >= 2U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "first");
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "second");
}

TEST_CASE("audio_player_redundant_play_does_not_restart_device_before_reporting_error") {
  const auto path = sineFixture("audio_player_redundant_play.wav", 960U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(path, "redundant-play"));
  static_cast<void>(queryProgressTick(player));
  player.play();
  static_cast<void>(queryProgressTick(player));

  const int startsBeforeSecondPlay = fake->startCalls.load();
  player.play();
  static_cast<void>(queryProgressTick(player));

  CHECK(fake->startCalls.load() == startsBeforeSecondPlay);
  const auto errorSnapshot = eventLog.snapshot();
  CHECK(std::any_of(errorSnapshot.begin(), errorSnapshot.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).message == "play is not legal in current state";
  }));
}

TEST_CASE("audio_player_direct_preload prepares next track but does not claim seamless handoff") {
  const auto firstPath = sineFixture("audio_player_direct_preload_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_direct_preload_second.wav", 960U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(outputConfig(AudioOutputMode::Direct));

  player.loadTrack(request(firstPath, "direct-first"));
  player.prepareNext(request(secondPath, "direct-second"));
  player.play();
  static_cast<void>(queryProgressTick(player));
  fake->consumeFrames(960U);
  const auto endedClock = queryProgressTick(player);

  CHECK(fake->initializeCalls.load() == 1);
  CHECK(fake->stopCalls.load() >= 1);
  CHECK(endedClock.trackId == "direct-first");
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 1U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "direct-first");
}

TEST_CASE("audio_player_reload_after_end_accepts_next_track_with_different_stream_parameters") {
  const auto firstPath = sineFixture("audio_player_reload_param_first.wav", 960U, 440.0,
                                     WavSpec{.sampleRate = 48'000, .channels = 1, .bitsPerSample = 16});
  const auto secondPath = sineFixture("audio_player_reload_param_second.wav", 882U, 660.0,
                                      WavSpec{.sampleRate = 44'100, .channels = 2, .bitsPerSample = 16});
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  AudioOutputConfig config{};
  config.outputMode = AudioOutputMode::Mixed;
  config.preferredDeviceId = "fake-device";
  config.bufferDuration = 20ms;
  player.configureOutput(config);

  player.loadTrack(request(firstPath, "param-first"));
  player.play();
  static_cast<void>(queryProgressTick(player));
  fake->consumeFrames(960U);
  static_cast<void>(queryProgressTick(player));
  player.loadTrack(request(secondPath, "param-second"));
  player.play();
  static_cast<void>(queryProgressTick(player));
  fake->consumeFrames(882U);
  const auto secondClock = queryProgressTick(player);

  CHECK(fake->initializeCalls.load() == 2);
  CHECK(fake->format.sampleRate == 44100U);
  CHECK(fake->format.channelCount == 2U);
  CHECK(secondClock.trackId == "param-second");
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() >= 2U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "param-first");
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "param-second");
}

TEST_CASE("audio_player_seamless_mix preserves preloaded frames larger than preload queue capacity") {
  const auto firstPath = sineFixture("audio_player_seamless_small_buffer_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_seamless_small_buffer_second.wav", kSampleRate, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(smallBufferOutputConfig());

  player.loadTrack(request(firstPath, "small-buffer-first"));
  player.prepareNext(request(secondPath, "small-buffer-second"));
  player.play();
  auto afterHandoff = queryProgressTick(player);
  for (int index = 0; index < 80 && afterHandoff.trackId != "small-buffer-second"; ++index) {
    fake->consumeFrames(48U);
    afterHandoff = queryProgressTick(player);
  }

  CHECK(afterHandoff.trackId == "small-buffer-second");
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).empty());
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
}

TEST_CASE("audio_player_preload_failure emits error without queue policy") {
  const auto firstPath = sineFixture("audio_player_preload_failure_current.wav", 960U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(firstPath, "current"));
  player.prepareNext(request(std::filesystem::current_path() / "missing-preload.wav", "missing-next"));
  player.play();
  static_cast<void>(queryProgressTick(player));
  fake->consumeFrames(960U);
  const auto endedClock = queryProgressTick(player);

  CHECK(endedClock.trackId == "current");
  CHECK(fake->initializeCalls.load() == 1);
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  const auto errors = eventsOf(eventLog, BackendEventType::PlaybackError);
  REQUIRE(errors.size() == 1U);
  CHECK(std::get<PlaybackError>(errors[0].payload).code == PlaybackErrorCode::OpenFailed);
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 1U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "current");
}

TEST_CASE("audio_player_seamless_mix honors preloaded next-track offset and duration boundaries") {
  const auto firstPath = sineFixture("audio_player_seamless_offset_first.wav", 960U, 440.0);
  const auto secondPath = silenceThenSineFixture("audio_player_seamless_offset_second.wav",
                                                 4'800U,
                                                 2'400U,
                                                 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  player.setEventSink(eventLog.sink());
  player.configureOutput(smallBufferOutputConfig());

  auto nextRequest = request(secondPath, "second-with-offset");
  nextRequest.offset = 100ms;
  nextRequest.duration = 50ms;
  nextRequest.boundedSegment = true;

  player.loadTrack(request(firstPath, "first-with-offset"));
  player.prepareNext(nextRequest);
  player.play();

  // 慢机：就绪/进度屏障 + 每步消费后保证一次真实进度 tick（裸 queryPlaybackClock 的
  // 2s 兜底在饥饿下返回陈旧快照，位置断言与零发射窗口都会失准）。
  auto clock = queryProgressTick(player);
  for (int index = 0; index < 80 && clock.trackId != "second-with-offset"; ++index) {
    clock = driveAndQuery(player, fake, 48U);
  }
  REQUIRE(clock.trackId == "second-with-offset");

  // 交接后 B 段内容已在主 ring：下一块必须非静音（原断言不变）。
  clock = driveAndQuery(player, fake, 48U);
  CHECK(std::any_of(fake->callbackBuffer.begin(), fake->callbackBuffer.end(), [](std::uint8_t value) {
    return value != 0U;
  }));

  for (int index = 0; index < 240 && !hasEventType(eventLog, BackendEventType::PlaybackEnded); ++index) {
    clock = driveAndQuery(player, fake, 48U);
  }

  CHECK(hasEventType(eventLog, BackendEventType::PlaybackEnded));
  CHECK(clock.position >= 149ms);
  CHECK(clock.position < 170ms);
}

// ================= T8 EndApproaching 服务测试 =================
// 场景约定：fixture 均 6s（288000 帧，mono 16-bit 48k）；Mixed 目标 48k 浮点立体声。
// 48 帧=1ms；EndApproaching 在 servicePlaybackProgress 内发射，driveAndQuery 经
// queryProgressTick 取"真实执行过"的快照作屏障（每次 driveAndQuery 保证一次消费后的
// 进度 tick；2s 超时兜底不算屏障）。剩余估算含解码提前量（≤缓冲 20ms），故"阈值前
// 零发射"检查点一律留 ≥50ms 余量。

TEST_CASE("audio_player_end_approach_bounded_fires_once_within_threshold") {
  const auto path = sineFixture("audio_player_end_approach_bounded.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(path, "end-approach-bounded", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  // 阈值=3000ms：位置 <1000ms 时剩余 >3000ms，不得提前发射。
  for (int index = 0; index < 200 && clock.position < 900ms; ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(endApproachingEvents(eventLog).empty());
  // 进入阈值窗口后（位置 ≥1000ms，剩余 ≤3000ms）应恰好发射一次。
  for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  const auto remaining = lastEndApproachingRemaining(eventLog);
  REQUIRE(remaining.has_value());
  CHECK(*remaining > 0ms);
  CHECK(*remaining <= 3000ms);
  // 一次发射后推进到自然终点：不再重复发射，且无额外 TrackChanged（无预加载交接）。
  for (int index = 0; index < 1200 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() == 1U);
  CHECK(eventsOf(eventLog, BackendEventType::TrackChanged).size() == 1U);
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
}

TEST_CASE("audio_player_end_approach_gapless_preload_max_threshold") {
  const auto path = sineFixture("audio_player_end_approach_max_threshold.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  // 阈值 = max(crossfadeMs=1500, gaplessPreloadMs=3000) = 3000ms（裁定）。
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 1500ms, 3000ms));

  service->loadTrack(boundedSegmentRequest(path, "end-approach-max-threshold", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  for (int index = 0; index < 200 && clock.position < 900ms; ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(endApproachingEvents(eventLog).empty());
  // 若阈值被误取 min=1500，此处（剩余∈(1500,3000]）不会发射——本检查点即判定。
  for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  const auto remaining = lastEndApproachingRemaining(eventLog);
  REQUIRE(remaining.has_value());
  CHECK(*remaining > 1500ms);
  CHECK(*remaining <= 3000ms);
}

TEST_CASE("audio_player_end_approach_off_mode_requires_preload") {
  SUBCASE("off mode with preload arms and fires within the preload threshold") {
    const auto path = sineFixture("audio_player_end_approach_off_preload.wav", 288'000U, 440.0);
    EventLog eventLog;
    auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
    auto* fake = backend.get();
    auto service = makeAudioPlaybackService(std::move(backend));
    service->setEventSink(eventLog.sink());
    service->configureOutput(outputConfig(AudioOutputMode::Mixed));
    // Off + 预加载 2000ms：武装（自动档=无但有预加载），阈值=2000ms（基线③）。
    service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::Off, 0ms, 2000ms));

    service->loadTrack(boundedSegmentRequest(path, "end-approach-off-preload", 3000ms));
    service->play();
    auto clock = queryProgressTick(*service);
    for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    CHECK(endApproachingEvents(eventLog).empty());
    for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
      clock = driveAndQuery(*service, fake, 48U);
    }
    REQUIRE(endApproachingEvents(eventLog).size() == 1U);
    const auto remaining = lastEndApproachingRemaining(eventLog);
    REQUIRE(remaining.has_value());
    CHECK(*remaining > 0ms);
    CHECK(*remaining <= 2000ms);
    for (int index = 0; index < 1000 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
    CHECK(endApproachingEvents(eventLog).size() == 1U);
  }

  SUBCASE("default config never arms: zero EndApproaching through natural end") {
    const auto path = sineFixture("audio_player_end_approach_off_default.wav", 288'000U, 440.0);
    EventLog eventLog;
    auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
    auto* fake = backend.get();
    auto service = makeAudioPlaybackService(std::move(backend));
    service->setEventSink(eventLog.sink());
    service->configureOutput(outputConfig(AudioOutputMode::Mixed));
    // 默认配置（自动档=无、预加载=0）：必须零新事件（默认值等价回归）。
    // configureTransition 故意不调用。

    service->loadTrack(boundedSegmentRequest(path, "end-approach-off-default", 3000ms));
    service->play();
    auto clock = queryProgressTick(*service);
    for (int index = 0; index < 1200 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
    CHECK(endApproachingEvents(eventLog).empty());
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  }
}

TEST_CASE("audio_player_end_approach_direct_mode_never_fires") {
  const auto path = sineFixture("audio_player_end_approach_direct.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Direct));
  // Direct 非 Mixed：即使档位/长度齐备也永不武装（仅 Mixed 生效）。
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(path, "end-approach-direct", 3000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  for (int index = 0; index < 1200 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(endApproachingEvents(eventLog).empty());
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
}

TEST_CASE("audio_player_end_approach_regular_track_duration_estimate") {
  const auto path = sineFixture("audio_player_end_approach_regular.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  // 非 CUE：整轨 6s + 时长元数据 5000ms → 估算剩余=duration−clock，阈值前零发射，
  // 位置 ≥2000ms（剩余 ≤3000ms）时发射一次；自然终点仍按真实文件尾（~6s）。
  service->loadTrack(durationMetadataRequest(path, "end-approach-regular", 5000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  for (int index = 0; index < 400 && clock.position < 1900ms; ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(endApproachingEvents(eventLog).empty());
  for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  const auto remaining = lastEndApproachingRemaining(eventLog);
  REQUIRE(remaining.has_value());
  CHECK(*remaining > 0ms);
  CHECK(*remaining <= 3000ms);
  for (int index = 0; index < 1400 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() == 1U);
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
}

TEST_CASE("audio_player_end_approach_resume_does_not_refire") {
  const auto path = sineFixture("audio_player_end_approach_resume.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(path, "end-approach-resume", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  // 先粗驱到阈值前（位置 <1000ms，剩余 >3000ms 零发射），再 1ms 步进进入阈值窗口。
  for (int index = 0; index < 200 && clock.position < 900ms; ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  // 瞬时暂停（无传送淡变）→ 暂停中不重复发射。
  service->pause();
  clock = queryProgressTick(*service);
  CHECK(endApproachingEvents(eventLog).size() == 1U);
  // 恢复后仍在阈值窗口内（剩余 ≤3000ms），但不得二次发射（本臂一次性）。
  service->resume();
  for (int index = 0; index < 100; ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  CHECK(endApproachingEvents(eventLog).size() == 1U);
  // 推进到自然终点：仍只有一次。
  for (int index = 0; index < 1200 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() == 1U);
}

TEST_CASE("audio_player_end_approach_pause_finishing_suppresses_emission") {
  const auto path = sineFixture("audio_player_end_approach_finishing.wav", 288'000U, 440.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  // fadeOnTransport：暂停走物理收尾（淡出期 2ms ticker 继续跑，但 Playing 门控前
  // 早退）——淡出期间即使剩余跨过阈值也零发射。
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms, true));

  service->loadTrack(boundedSegmentRequest(path, "end-approach-finishing", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  // 推进到位置 ~800ms（剩余 3200ms > 阈值），尚未发射。
  for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(endApproachingEvents(eventLog).empty());
  // 暂停进入收尾：继续消费以驱动淡出（300ms），直到设备停止（归零点）。
  service->pause();
  const auto stopsBeforeFinishing = fake->stopCalls.load();
  for (int index = 0; index < 800 && fake->stopCalls.load() == stopsBeforeFinishing; ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  CHECK(fake->stopCalls.load() > stopsBeforeFinishing);
  // 收尾淡出期与暂停定格期：剩余已 ≤3000ms，但零发射。
  CHECK(endApproachingEvents(eventLog).empty());
  // 恢复（冷恢复淡入）：重新 Playing 后才发射一次；推进到自然终点仍只有一次。
  service->resume();
  for (int index = 0; index < 600 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  const auto remaining = lastEndApproachingRemaining(eventLog);
  REQUIRE(remaining.has_value());
  CHECK(*remaining > 0ms);
  CHECK(*remaining <= 3000ms);
  for (int index = 0; index < 1200 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = driveAndQuery(*service, fake, 240U);
  }
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() == 1U);
}

TEST_CASE("audio_player_end_approach_handoff_rearms_next_track") {
  SUBCASE("seamless direct handoff re-arms EndApproaching for the next track") {
    const auto firstPath = sineFixture("audio_player_end_approach_handoff_first.wav", 288'000U, 440.0);
    EventLog eventLog;
    auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
    auto* fake = backend.get();
    auto service = makeAudioPlaybackService(std::move(backend));
    service->setEventSink(eventLog.sink());
    service->configureOutput(outputConfig(AudioOutputMode::Mixed));
    service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

    service->loadTrack(boundedSegmentRequest(firstPath, "end-approach-handoff-first", 4000ms));
    service->play();
    auto clock = queryProgressTick(*service);
    // 第一曲发射一次（位置 ≥1000ms）。
    for (int index = 0; index < 200 && clock.position < 900ms; ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
      clock = driveAndQuery(*service, fake, 48U);
    }
    REQUIRE(endApproachingEvents(eventLog).size() == 1U);
    // 控制器流：EndApproaching → prepareNext（SeamlessDirect 直切语义）。
    service->prepareNext(boundedSegmentRequest(firstPath, "end-approach-handoff-second", 2000ms));
    // 第一曲自然终点（4000ms，约 3000ms 路程）→ 无缝交接（无 PlaybackEnded）。
    for (int index = 0; index < 1600 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty() &&
                          eventsOf(eventLog, BackendEventType::TrackChanged).size() < 2U;
         ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
    REQUIRE(tracks.size() == 2U);
    CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "end-approach-handoff-second");
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).empty());
    // 交接 = 新曲接管：第二曲（2000ms bounded，剩余 2000 ≤3000）重新武装并再发射一次。
    for (int index = 0; index < 100 && endApproachingEvents(eventLog).size() < 2U; ++index) {
      clock = driveAndQuery(*service, fake, 48U);
    }
    REQUIRE(endApproachingEvents(eventLog).size() == 2U);
    const auto remaining = lastEndApproachingRemaining(eventLog);
    REQUIRE(remaining.has_value());
    CHECK(*remaining > 0ms);
    CHECK(*remaining <= 3000ms);
    CHECK(fake->initializeCalls.load() == 1);
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  }

  SUBCASE("two-argument prepareNext with crossfade meta keeps handoff working") {
    const auto firstPath = sineFixture("audio_player_end_approach_handoff_meta.wav", 288'000U, 440.0);
    EventLog eventLog;
    auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
    auto* fake = backend.get();
    // 2 参重载在 AudioPlayer 包装上不可见：直接走服务接口（同 makeAudioPlaybackService 产物）。
    auto service = makeAudioPlaybackService(std::move(backend));
    service->setEventSink(eventLog.sink());
    service->configureOutput(outputConfig(AudioOutputMode::Mixed));
    service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

    service->loadTrack(boundedSegmentRequest(firstPath, "end-approach-meta-first", 4000ms));
    service->play();
    auto clock = queryProgressTick(*service);
    for (int index = 0; index < 200 && clock.position < 900ms; ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    for (int index = 0; index < 300 && endApproachingEvents(eventLog).empty(); ++index) {
      clock = driveAndQuery(*service, fake, 48U);
    }
    REQUIRE(endApproachingEvents(eventLog).size() == 1U);
    // 控制器携带元数据（Crossfade 行）——T8 期间槽就绪自然终点仍按直切兜底。
    service->prepareNext(boundedSegmentRequest(firstPath, "end-approach-meta-second", 2000ms),
                         PrepareNextMeta{.kind = PrepareNextKind::Crossfade, .isGaplessGroup = true});
    // 第一曲自然终点（4000ms）→ 无缝交接：5ms 步进驱动（上限 8000ms）。
    for (int index = 0; index < 1600 && eventsOf(eventLog, BackendEventType::TrackChanged).size() < 2U; ++index) {
      clock = driveAndQuery(*service, fake, 240U);
    }
    const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
    REQUIRE(tracks.size() == 2U);
    CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "end-approach-meta-second");
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).empty());
    CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
    CHECK(fake->initializeCalls.load() == 1);
  }
}

// ================= T10 自动交叉重叠（AdvanceCompleted 提交 + 重叠窗口） =================

TEST_CASE("audio_player_crossfade_overlap_promotes_with_advance_completed_before_track_changed") {
  // A=440Hz（4s 文件、bounded 4000ms）、B=660Hz（5s 文件、bounded 5000ms）：不同频率
  // 正交叠加 → 重叠期总功率恒定（RMS 不塌零），可区分"双源都出声"与"静音缺口"。
  const auto firstPath = sineFixture("audio_player_overlap_first.wav", 192'000U, 440.0);
  const auto secondPath = sineFixture("audio_player_overlap_second.wav", 240'000U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  // EA 阈值 = max(crossfade, gaplessPreload) = 3000ms：与重叠窗口（≤3000ms）同刻竞争，
  // 覆盖"EA 与窗口同 tick 时槽在途"的场景（就绪即启，裁定⑧）。
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(firstPath, "overlap-first", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  double minRms = 1e9;
  const auto step = [&](std::uint32_t frames) {
    fake->consumeFrames(frames);
    const auto& buffer = fake->callbackBuffer;
    const auto* samples = reinterpret_cast<const float*>(buffer.data());
    const auto count = buffer.size() / sizeof(float);
    double sum = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
      sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
    }
    minRms = std::min(minRms, count == 0U ? 1e9 : std::sqrt(sum / static_cast<double>(count)));
    return queryProgressTick(*service);
  };
  // 推进到 EA 阈值前（位置 ≥ 800ms），再 1ms 步进触发 EndApproaching。
  for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
    clock = step(240U);
  }
  for (int index = 0; index < 400 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = step(48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);

  // 控制器流：EA → PrepareNext（Crossfade 行）→ 槽就绪后窗口内启动重叠。
  service->prepareNext(boundedSegmentRequest(secondPath, "overlap-second", 5000ms),
                       PrepareNextMeta{.kind = PrepareNextKind::Crossfade, .isGaplessGroup = false});
  // 驱动到主源排空提升（TrackChanged 第二次），再等 B 臂 EndApproaching 重武装。
  const auto hasSecondTrack = [&] { return eventsOf(eventLog, BackendEventType::TrackChanged).size() >= 2U; };
  for (int index = 0; index < 2000 && !hasSecondTrack(); ++index) {
    clock = step(240U);
  }
  REQUIRE(hasSecondTrack());
  for (int index = 0; index < 100 && endApproachingEvents(eventLog).size() < 2U; ++index) {
    clock = step(48U);
  }

  // 事件序：AdvanceCompleted 先于新曲 TrackChanged（控制器先提交后同步状态）。
  const auto advances = eventsOf(eventLog, BackendEventType::AdvanceCompleted);
  REQUIRE(advances.size() == 1U);
  CHECK(std::get<AdvanceCompleted>(advances[0].payload).trackId == "overlap-second");
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 2U);
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "overlap-second");
  const auto orderSnapshot = eventLog.snapshot();
  const auto advanceIndex = static_cast<std::size_t>(
      std::distance(orderSnapshot.begin(),
                    std::find_if(orderSnapshot.begin(), orderSnapshot.end(), [](const BackendEvent& event) {
                      return event.type == BackendEventType::AdvanceCompleted;
                    })));
  const auto secondTrackIndex = static_cast<std::size_t>(
      std::distance(orderSnapshot.begin(),
                    std::find_if(orderSnapshot.begin(), orderSnapshot.end(), [](const BackendEvent& event) {
                      return event.type == BackendEventType::TrackChanged &&
                             std::get<TrackChanged>(event.payload).request.trackId == "overlap-second";
                    })));
  CHECK(advanceIndex < secondTrackIndex);
  // 无缝语义：无 PlaybackEnded / 无 Error / 设备零重开零停止。
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).empty());
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  CHECK(fake->initializeCalls.load() == 1);
  CHECK(fake->stopCalls.load() == 0);
  // 重叠窗口期第二源已被消费：提升后 B 时钟从已消费位置续走（远大于 0），证明
  // 交叉真实发生（直切兜底则从头起播、位置≈0）——区分两路径的关键断言。
  CHECK(clock.trackId == "overlap-second");
  CHECK(clock.position >= 500ms);
  // 整个生命周期输出无静音缺口（双源等功率互补；<0.1 = 至少一路静音/欠载）。
  CHECK(minRms > 0.1);
}

TEST_CASE("audio_player_seamless_direct_meta_declines_overlap_and_handoffs_from_zero") {
  // kind 门控（裁定⑧ 就绪即启只对 Crossfade 槽）：SeamlessDirect 交接方式下窗口到点
  // 裁决放弃交叉（declined），排空时直切兜底——B 从头起播（无重叠消费，位置≈0）。
  const auto firstPath = sineFixture("audio_player_overlap_decline_first.wav", 192'000U, 440.0);
  const auto secondPath = sineFixture("audio_player_overlap_decline_second.wav", 240'000U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(firstPath, "overlap-decline-first", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  const auto step = [&](std::uint32_t frames) {
    fake->consumeFrames(frames);
    return queryProgressTick(*service);
  };
  for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
    clock = step(240U);
  }
  for (int index = 0; index < 400 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = step(48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  service->prepareNext(boundedSegmentRequest(secondPath, "overlap-decline-second", 5000ms),
                       PrepareNextMeta{.kind = PrepareNextKind::SeamlessDirect, .isGaplessGroup = false});
  const auto hasSecondTrack = [&] { return eventsOf(eventLog, BackendEventType::TrackChanged).size() >= 2U; };
  for (int index = 0; index < 2000 && !hasSecondTrack(); ++index) {
    clock = step(240U);
  }
  REQUIRE(hasSecondTrack());

  const auto advances = eventsOf(eventLog, BackendEventType::AdvanceCompleted);
  REQUIRE(advances.size() == 1U);
  CHECK(std::get<AdvanceCompleted>(advances[0].payload).trackId == "overlap-decline-second");
  const auto tracks = eventsOf(eventLog, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 2U);
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "overlap-decline-second");
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackEnded).empty());
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  CHECK(fake->initializeCalls.load() == 1);
  // 直切兜底：槽 PCM 搬运到主 ring，B 从头起播（无重叠消费）。
  CHECK(clock.trackId == "overlap-decline-second");
  CHECK(clock.position < 500ms);
}

TEST_CASE("audio_player_abort_transition_drops_preload_and_rearms_end_approach") {
  const auto firstPath = sineFixture("audio_player_overlap_abort_first.wav", 192'000U, 440.0);
  const auto secondPath = sineFixture("audio_player_overlap_abort_second.wav", 240'000U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(firstPath, "overlap-abort-first", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  const auto step = [&](std::uint32_t frames) {
    fake->consumeFrames(frames);
    return queryProgressTick(*service);
  };
  for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
    clock = step(240U);
  }
  for (int index = 0; index < 400 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = step(48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  // 槽就绪、窗口内（可能已启动重叠）→ 中止在途过渡。
  service->prepareNext(boundedSegmentRequest(secondPath, "overlap-abort-second", 5000ms),
                       PrepareNextMeta{.kind = PrepareNextKind::Crossfade, .isGaplessGroup = false});
  for (int index = 0; index < 50; ++index) {
    clock = step(48U);
  }
  service->abortTransition();
  // abort 后：弃槽 → 自然终点无交接 → PlaybackEnded 照常（无 AdvanceCompleted）；
  // 预告重新武装 → EndApproaching 再次发出。
  for (int index = 0; index < 2000 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = step(240U);
  }
  REQUIRE(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(eventsOf(eventLog, BackendEventType::AdvanceCompleted).empty());
  CHECK(eventsOf(eventLog, BackendEventType::TrackChanged).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() >= 2U);
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  CHECK(fake->initializeCalls.load() == 1);
  CHECK(fake->stopCalls.load() >= 1);
}

// T10 N7 加固回归：窗口内 configureTransition（服务侧直接重配边缘）必须同 abortTransition
// 弃 preloadSlot_——旧槽作废后自然终点无交接（PlaybackEnded 照常、无 AdvanceCompleted），
// 预告重新武装（EndApproaching 再次发出）。若 configureTransition 保留旧槽，本用例会在
// 自然终点直切旧槽（无 PlaybackEnded / TrackChanged==2）而失败。
TEST_CASE("audio_player_configure_transition_in_window_drops_preload_and_rearms_end_approach") {
  const auto firstPath = sineFixture("audio_player_overlap_config_first.wav", 192'000U, 440.0);
  const auto secondPath = sineFixture("audio_player_overlap_config_second.wav", 240'000U, 660.0);
  EventLog eventLog;
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  auto service = makeAudioPlaybackService(std::move(backend));
  service->setEventSink(eventLog.sink());
  service->configureOutput(outputConfig(AudioOutputMode::Mixed));
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));

  service->loadTrack(boundedSegmentRequest(firstPath, "overlap-config-first", 4000ms));
  service->play();
  auto clock = queryProgressTick(*service);
  const auto step = [&](std::uint32_t frames) {
    fake->consumeFrames(frames);
    return queryProgressTick(*service);
  };
  for (int index = 0; index < 200 && clock.position < 800ms; ++index) {
    clock = step(240U);
  }
  for (int index = 0; index < 400 && endApproachingEvents(eventLog).empty(); ++index) {
    clock = step(48U);
  }
  REQUIRE(endApproachingEvents(eventLog).size() == 1U);
  // 槽就绪、窗口内 → 服务侧直接重配（新配置 = 新臂，旧槽作废）。
  service->prepareNext(boundedSegmentRequest(secondPath, "overlap-config-second", 5000ms),
                       PrepareNextMeta{.kind = PrepareNextKind::Crossfade, .isGaplessGroup = false});
  for (int index = 0; index < 50; ++index) {
    clock = step(48U);
  }
  service->configureTransition(endApproachConfig(AutoAdvanceFadeMode::All, 3000ms, 0ms));
  // 弃槽后自然终点无交接 → PlaybackEnded 照常（无 AdvanceCompleted / 无第二 TrackChanged）；
  // 预告重新武装 → EndApproaching 再次发出。
  for (int index = 0; index < 2000 && eventsOf(eventLog, BackendEventType::PlaybackEnded).empty(); ++index) {
    clock = step(240U);
  }
  REQUIRE(eventsOf(eventLog, BackendEventType::PlaybackEnded).size() == 1U);
  CHECK(eventsOf(eventLog, BackendEventType::AdvanceCompleted).empty());
  CHECK(eventsOf(eventLog, BackendEventType::TrackChanged).size() == 1U);
  CHECK(endApproachingEvents(eventLog).size() >= 2U);
  CHECK(eventsOf(eventLog, BackendEventType::PlaybackError).empty());
  CHECK(fake->initializeCalls.load() == 1);
}

}
