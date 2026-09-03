#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

std::vector<std::int16_t> makeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0)));
  }

  return samples;
}

void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples) {
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
  writeU16(output, kChannels);
  writeU32(output, kSampleRate);
  writeU32(output, kSampleRate * kChannels * (kBitsPerSample / 8U));
  writeU16(output, static_cast<std::uint16_t>(kChannels * (kBitsPerSample / 8U)));
  writeU16(output, kBitsPerSample);
  writeTag(output, "data");
  writeU32(output, dataSize);

  for (const auto sample : samples) {
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(frames));
  return path;
}

class FakeAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    queue = request.pcmQueue;
    userData = request.callbackUserData;
    format.deviceId = request.config.preferredDeviceId.empty() ? "fake-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    initialized = initializeResult;
    return initializeResult;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    started = startResult;
    return startResult;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    lastStopAt.store(std::chrono::steady_clock::now());
    started = false;
    return stopResult;
  }

  void uninitialize() noexcept override {
    ++uninitializeCalls;
    initialized = false;
    started = false;
    queue = nullptr;
    userData = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void consumeFrames(std::uint32_t frames) {
    REQUIRE(userData != nullptr);
    const auto bytesPerFrame = static_cast<std::size_t>(format.channelCount) * 4U;
    callbackBuffer.assign(static_cast<std::size_t>(frames) * bytesPerFrame, 0U);
    AudioOutputDevice::renderCallback(userData, callbackBuffer.data(), frames);
  }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 512,
                           .actualMode = AudioOutputMode::Mixed};
  PcmBufferQueue* queue{nullptr};
  AudioOutputDevice* userData{nullptr};
  std::vector<std::uint8_t> callbackBuffer{};
  // 计数器由 worker 线程（backend 方法）写、测试线程读 → atomic，防无同步轮询数据竞争。
  std::atomic<int> initializeCalls{0};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> uninitializeCalls{0};
  std::atomic<std::chrono::steady_clock::time_point> lastStopAt{};
  bool initializeResult{true};
  bool startResult{true};
  bool stopResult{true};
  bool initialized{false};
  bool started{false};
};

class BlockingStartAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    userData = request.callbackUserData;
    format.deviceId = request.config.preferredDeviceId.empty() ? "blocking-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    initialized = true;
    return true;
  }

  [[nodiscard]] bool start() override {
    std::unique_lock lock{mutex};
    ++startCalls;
    startEntered = true;
    entered.notify_all();
    release.wait(lock, [&] { return allowStartReturn; });
    started = initialized;
    return initialized;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    started = false;
    return true;
  }

  void uninitialize() noexcept override {
    initialized = false;
    started = false;
    userData = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void waitForStart() {
    std::unique_lock lock{mutex};
    REQUIRE(entered.wait_for(lock, 2s, [&] { return startEntered; }));
  }

  void unblockStart() {
    {
      std::lock_guard lock{mutex};
      allowStartReturn = true;
    }
    release.notify_all();
  }

  AudioDeviceFormat format{.deviceId = "blocking-device",
                           .deviceName = "Blocking Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 512,
                           .actualMode = AudioOutputMode::Mixed};
  AudioOutputDevice* userData{nullptr};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  bool initialized{false};
  bool started{false};

private:
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable release;
  bool startEntered{false};
  bool allowStartReturn{false};
};

std::size_t firstEventOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  const auto iterator = std::find_if(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
  REQUIRE(iterator != events.end());
  return static_cast<std::size_t>(std::distance(events.begin(), iterator));
}

std::vector<PlaybackState> statesFrom(const std::vector<BackendEvent>& events) {
  std::vector<PlaybackState> states;
  for (const auto& event : events) {
    if (event.type == BackendEventType::PlaybackStateChanged) {
      states.push_back(std::get<PlaybackStateChanged>(event.payload).state);
    }
  }
  return states;
}

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

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock{mutex_};
    return events_.size();
  }

private:
  mutable std::mutex mutex_{};
  std::vector<BackendEvent> events_{};
};

void waitForEventCount(const EventLog& events, std::size_t count) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (events.size() < count && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(events.size() >= count);
}

void waitForState(const EventLog& events, PlaybackState state) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto states = statesFrom(events.snapshot());
    if (std::find(states.begin(), states.end(), state) != states.end()) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  }
  FAIL("expected playback state was not delivered");
}

}

TEST_CASE("audio_player_single_track supports fake-device command path") {
  const auto path = sineFixture("audio_player_single_track.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());
  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 150ms;
  player.configureOutput(config);
  const TrackPlaybackRequest request{.trackId = "single-track",
                                     .filePath = path,
                                     .title = "Generated Fixture",
                                     .artist = {},
                                     .offset = std::nullopt,
                                     .duration = std::nullopt,
                                     .sampleRate = std::nullopt,
                                     .bitDepth = std::nullopt,
                                     .channels = std::nullopt,
                                     .format = std::nullopt};

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  player.play();
  waitForState(eventLog, PlaybackState::Playing);
  fake->consumeFrames(2400U);
  const auto afterPlay = player.queryPlaybackClock();
  player.pause();
  waitForState(eventLog, PlaybackState::Paused);
  const auto paused = player.queryPlaybackClock();
  player.resume();
  waitForState(eventLog, PlaybackState::Playing);
  fake->consumeFrames(2400U);
  player.seek(700ms);
  waitForEventCount(eventLog, 8U);
  const auto afterSeek = player.queryPlaybackClock();
  player.stop();
  waitForState(eventLog, PlaybackState::Stopped);

  CHECK(fake->initializeCalls.load() == 1);
  CHECK(fake->startCalls.load() == 3);
  CHECK(fake->stopCalls.load() >= 3);
  CHECK(fake->uninitializeCalls.load() == 0);
  CHECK(afterPlay.position >= 40ms);
  CHECK(paused.position >= afterPlay.position);
  CHECK(afterSeek.trackId == "single-track");
  CHECK(afterSeek.position >= 700ms);

  const auto events = eventLog.snapshot();
  const auto trackChanged = firstEventOf(events, BackendEventType::TrackChanged);
  const auto positionUpdated = firstEventOf(events, BackendEventType::PlaybackPositionUpdated);
  const auto discontinuity = firstEventOf(events, BackendEventType::PositionDiscontinuity);
  CHECK(trackChanged < positionUpdated);
  CHECK(discontinuity > trackChanged);
  CHECK(std::get<TrackChanged>(events[trackChanged].payload).request.trackId == "single-track");
  CHECK(std::get<PositionDiscontinuity>(events[discontinuity].payload).reason == "seek");

  const auto states = statesFrom(events);
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Loading) != states.end());
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Ready) != states.end());
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Playing) != states.end());
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Paused) != states.end());
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Stopped) != states.end());

  player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_public_commands_enqueue_without_waiting_for_device_start") {
  const auto path = sineFixture("audio_player_nonblocking_commands.wav", kSampleRate * 2U);
  auto backend = std::make_unique<BlockingStartAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());
  AudioOutputConfig config{};
  config.preferredDeviceId = "blocking-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 20ms;
  player.configureOutput(config);
  const TrackPlaybackRequest request{.trackId = "nonblocking-track",
                                     .filePath = path,
                                     .title = "Generated Fixture",
                                     .artist = {},
                                     .offset = std::nullopt,
                                     .duration = std::nullopt,
                                     .sampleRate = std::nullopt,
                                     .bitDepth = std::nullopt,
                                     .channels = std::nullopt,
                                     .format = std::nullopt};

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  player.play();
  fake->waitForStart();

  const auto beforePause = std::chrono::steady_clock::now();
  player.pause();
  const auto pauseLatency = std::chrono::steady_clock::now() - beforePause;
  const auto beforeStop = std::chrono::steady_clock::now();
  player.stop();
  const auto stopLatency = std::chrono::steady_clock::now() - beforeStop;

  CHECK(pauseLatency < 50ms);
  CHECK(stopLatency < 50ms);

  fake->unblockStart();
  waitForState(eventLog, PlaybackState::Stopped);
  player.setEventSink(BackendEventSink{});
}

#if !defined(_WIN32)
TEST_CASE("audio_player_single_track seek reuses the open source after the file is removed") {
  const auto path = sineFixture("audio_player_seek_reuses_open_source.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 150ms;
  player.configureOutput(config);

  const TrackPlaybackRequest request{.trackId = "seek-open-source",
                                     .filePath = path,
                                     .title = "Generated Fixture",
                                     .artist = {},
                                     .offset = std::nullopt,
                                     .duration = std::nullopt,
                                     .sampleRate = std::nullopt,
                                     .bitDepth = std::nullopt,
                                     .channels = std::nullopt,
                                     .format = std::nullopt};

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  REQUIRE(std::filesystem::remove(path));

  player.seek(700ms);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  auto afterSeek = player.queryPlaybackClock();
  while (afterSeek.position < 700ms && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
    afterSeek = player.queryPlaybackClock();
  }

  const auto events = eventLog.snapshot();
  CHECK(afterSeek.trackId == "seek-open-source");
  CHECK(afterSeek.position >= 700ms);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::SeekFailed;
  }));

  player.setEventSink(BackendEventSink{});
}
#endif  // Windows requires FILE_SHARE_DELETE for unlinking an open media file.

TEST_CASE("audio_player_single_track ignores duration metadata as a hard stop when offset is absent") {
  const auto path = sineFixture("audio_player_duration_without_offset.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 150ms;
  player.configureOutput(config);

  const TrackPlaybackRequest request{.trackId = "duration-without-offset",
                                     .filePath = path,
                                     .title = "Generated Fixture",
                                     .artist = {},
                                     .offset = std::nullopt,
                                     .duration = 100ms,
                                     .sampleRate = std::nullopt,
                                     .bitDepth = std::nullopt,
                                     .channels = std::nullopt,
                                     .format = std::nullopt};

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  player.play();
  waitForState(eventLog, PlaybackState::Playing);

  for (int index = 0; index < 6; ++index) {
    fake->consumeFrames(2400U);
    static_cast<void>(player.queryPlaybackClock());
  }

  const auto clock = player.queryPlaybackClock();
  const auto events = eventLog.snapshot();
  CHECK(clock.position >= 250ms);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));

  player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_single_track honors bounded segment duration when the segment starts at offset zero") {
  const auto path = sineFixture("audio_player_bounded_segment_offset_zero.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());

  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = 48000;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 150ms;
  player.configureOutput(config);

  const TrackPlaybackRequest request{.trackId = "bounded-segment-offset-zero",
                                     .filePath = path,
                                     .title = "Generated Fixture",
                                     .artist = {},
                                     .offset = 0ms,
                                     .duration = 100ms,
                                     .sampleRate = std::nullopt,
                                     .bitDepth = std::nullopt,
                                     .channels = std::nullopt,
                                     .format = std::nullopt,
                                     .boundedSegment = true};

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  player.play();
  waitForState(eventLog, PlaybackState::Playing);

  for (int index = 0; index < 6; ++index) {
    fake->consumeFrames(2400U);
    static_cast<void>(player.queryPlaybackClock());
  }

  const auto clock = player.queryPlaybackClock();
  const auto events = eventLog.snapshot();
  CHECK(clock.position <= 150ms);
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));

  player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_single_track paused output reload stays paused without error") {
  const auto path = sineFixture("audio_player_paused_reload.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  EventLog eventLog;
  player.setEventSink(eventLog.sink());

  TrackPlaybackRequest request{};
  request.trackId = "paused-reload";
  request.filePath = path;

  player.loadTrack(request);
  waitForState(eventLog, PlaybackState::Ready);
  player.play();
  waitForState(eventLog, PlaybackState::Playing);
  player.pause();
  waitForState(eventLog, PlaybackState::Paused);

  // 模拟控制层 ConfigureOutput 立即重载序列：loadTrack → seek → pause。
  AudioOutputConfig config{};
  config.outputMode = AudioOutputMode::Mixed;
  config.bufferDuration = 100ms;
  const auto stateCountBeforeReload = statesFrom(eventLog.snapshot()).size();
  player.configureOutput(config);
  player.loadTrack(request);
  player.seek(1200ms);
  player.pause();

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto states = statesFrom(eventLog.snapshot());
    if (states.size() > stateCountBeforeReload && states.back() == PlaybackState::Paused) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  const auto reloadStates = statesFrom(eventLog.snapshot());
  REQUIRE(reloadStates.size() > stateCountBeforeReload);
  CHECK(reloadStates.back() == PlaybackState::Paused);

  for (const auto& event : eventLog.snapshot()) {
    if (event.type == BackendEventType::PlaybackError) {
      const auto& error = std::get<PlaybackError>(event.payload);
      CHECK(error.message != "pause requires active playback");
    }
  }

  const auto pausedAfterReload = player.queryPlaybackClock();
  CHECK(pausedAfterReload.trackId == "paused-reload");
  CHECK(pausedAfterReload.position >= 1150ms);
  CHECK(pausedAfterReload.position <= 1250ms);

  // 等 resume 之后的新 Playing 事件（waitForState 会命中 play#1 的历史事件，过早
  // 返回时 start 计数尚未 +1，造成假失败竞态）。
  const auto statesBeforeResume = statesFrom(eventLog.snapshot()).size();
  player.resume();
  bool resumedToPlaying = false;
  const auto resumeDeadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < resumeDeadline) {
    const auto states = statesFrom(eventLog.snapshot());
    if (states.size() > statesBeforeResume && states.back() == PlaybackState::Playing) {
      resumedToPlaying = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(resumedToPlaying);
  CHECK(fake->startCalls.load() == 2);
  player.stop();
  waitForState(eventLog, PlaybackState::Stopped);
  player.setEventSink(BackendEventSink{});
}

namespace {

// —— T6 传送淡变收尾测试基础设施 ——

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

float currentMasterGain(FakeAudioOutputDeviceBackend& fake) {
  REQUIRE(fake.userData != nullptr);
  return fake.userData->masterEnvelopeGain();
}

// 播放后推进冷启动淡入（即时 0 → ticker 观测归零 → 发布 0→1），直到增益完全建立
// （≈1.0）。否则 pause 会落在零增益窗内，淡出从 0 起算 → 瞬时完成（行为正确但测不到轨迹）。
void driveUntilGainEstablished(FakeAudioOutputDeviceBackend& fake, std::uint32_t maxFrames = 30'000U) {
  std::uint32_t driven = 0U;
  while (driven < maxFrames) {
    fake.consumeFrames(240U);
    driven += 240U;
    if (currentMasterGain(fake) >= 0.99F) {
      return;
    }
    std::this_thread::sleep_for(1ms);
  }
  FAIL("gain did not reach full after cold-start fade-in");
}

// 只统计非 BufferUnderrun 的 PlaybackError：underrun 是突发驱动/缓冲深度下的正常报告，
// 不是解码/设备故障（finishing 语义上不转 Error 态）。
bool hasRealError(const std::vector<BackendEvent>& events) {
  return std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    if (event.type != BackendEventType::PlaybackError) {
      return false;
    }
    const auto& error = std::get<PlaybackError>(event.payload);
    return error.code != PlaybackErrorCode::BufferUnderrun;
  });
}

// 从暂停观测时刻起驱动 240 帧（5ms）块，直到事件日志出现新事件（收尾完成发布——
// 事件在 stopDevice 之后推送，互斥日志同步保证 stop 计数可见）。返回 [驱动总帧数, 增益采样]。
std::pair<std::uint32_t, std::vector<float>> driveUntilFinishingDone(const EventLog& events,
                                                                     std::size_t eventsAtStart,
                                                                     FakeAudioOutputDeviceBackend& fake,
                                                                     std::uint32_t maxFrames = 30'000U) {
  std::vector<float> gains;
  std::uint32_t driven = 0U;
  while (driven < maxFrames) {
    fake.consumeFrames(240U);
    driven += 240U;
    gains.push_back(currentMasterGain(fake));
    if (events.size() > eventsAtStart) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  return {driven, std::move(gains)};
}

std::size_t pausedEventIndex(const std::vector<BackendEvent>& events) {
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (events[index].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(events[index].payload);
      if (change.state == PlaybackState::Paused) {
        return index;
      }
    }
  }
  FAIL("no paused state event");
  return 0U;
}

TrackPlaybackRequest makeSineRequest(std::filesystem::path path, std::string trackId) {
  return TrackPlaybackRequest{.trackId = std::move(trackId),
                              .filePath = std::move(path),
                              .title = "Generated Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

// 配置 fade 使能的播放链路（transition 需在 loadTrack 前设好）。
struct FadingPlayer {
  explicit FadingPlayer(std::chrono::milliseconds fadeMs) {
    backend = std::make_unique<FakeAudioOutputDeviceBackend>();
    fake = backend.get();
    service = makeAudioPlaybackService(std::move(backend));
    player.setPlaybackService(service);
    player.setEventSink(events.sink());
    TransitionConfig transition{};
    transition.fadeOnTransport = true;
    transition.transportFadeMs = fadeMs;
    service->configureTransition(transition);
  }

  void configureOutput(std::chrono::milliseconds bufferDuration) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    config.targetSampleRate = 48000;
    config.targetSampleFormat = AudioSampleFormat::Float32;
    config.targetChannelCount = 2;
    config.bufferDuration = bufferDuration;
    player.configureOutput(config);
  }

  std::unique_ptr<FakeAudioOutputDeviceBackend> backend;
  FakeAudioOutputDeviceBackend* fake{nullptr};
  std::shared_ptr<AudioPlaybackService> service;
  AudioPlayer player;
  EventLog events;
};

}

TEST_CASE("audio_player_finishing pause fade-out runs to zero then stops device once") {
  const auto path = sineFixture("audio_player_finishing_pause.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-pause"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);  // 等冷启动淡入跑完（gain≈1.0），避开零增益窗

  const auto prePauseClock = fading.player.queryPlaybackClock();
  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  const auto eventsAtPause = fading.events.size();
  const auto pausedIndex = pausedEventIndex(fading.events.snapshot());
  REQUIRE(fake->stopCalls.load() == 0);  // 逻辑 Paused 已发，设备仍在运行（淡出中）

  // 驱动渲染块直到收尾完成（归零后 stopDevice + 定格发布）。
  const auto [driven, gains] = driveUntilFinishingDone(fading.events, eventsAtPause, *fake);
  CHECK(driven >= 4'320U);  // 淡出至少吃掉 ~90ms（100ms 目标）
  CHECK(driven <= 10'000U);
  REQUIRE(fake->stopCalls.load() == 1);
  REQUIRE_FALSE(gains.empty());

  // T1：逻辑 Paused 事件先于设备 stop。
  const auto snapshotAfter = fading.events.snapshot();
  CHECK(snapshotAfter[pausedIndex].timestamp < fake->lastStopAt.load());

  // 淡出轨迹单调降至零：首采样仍接近 1.0，全程最小值归零（stop 复位包络到 1.0，
  // 末尾采样可能是复位值，故用最小值而非末值断言）。
  CHECK(gains.front() >= 0.85F);
  const auto minimum = std::min_element(gains.begin(), gains.end());
  REQUIRE(minimum != gains.end());
  CHECK(*minimum <= 0.02F);
  bool sawMidFade = false;
  bool monotoneUntilZero = true;
  for (std::size_t i = 1; i < gains.size(); ++i) {
    if (gains[i] <= 0.02F) {
      break;  // 归零后不再检查（后续可能是 stop 复位采样）
    }
    if (gains[i] > gains[i - 1] + 0.01F) {
      monotoneUntilZero = false;
    }
    if (gains[i] > 0.15F && gains[i] < 0.85F) {
      sawMidFade = true;
    }
  }
  CHECK(sawMidFade);
  CHECK(monotoneUntilZero);
  CHECK(fake->startCalls.load() == 1);  // 未重启设备

  // R3：归零点定格——时钟冻结，且定格在淡出末尾（≈暂停点+淡出长），而非淡出前位置。
  const auto frozen = fading.player.queryPlaybackClock();
  const auto frozenAgain = fading.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position == frozenAgain.position);
  const auto advance = frozen.position - prePauseClock.position;
  CHECK(advance >= 90ms);
  CHECK(advance <= 150ms);

  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing resume during fade-out keeps device running and rises back to full gain") {
  const auto path = sineFixture("audio_player_finishing_resume.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-resume"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);  // 等冷启动淡入跑完（gain≈1.0），避开零增益窗

  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  REQUIRE(fake->stopCalls.load() == 0);

  fake->consumeFrames(1'200U);  // 淡出进行中（25ms / 100ms）
  const float gainMidFade = currentMasterGain(*fake);
  REQUIRE(gainMidFade > 0.4F);
  REQUIRE(gainMidFade < 0.95F);

  // 淡出在途时 resume："暂停中再播放"——不得停设备/重启后端。
  const auto stateCountBeforeResume = statesFrom(fading.events.snapshot()).size();
  fading.player.resume();
  REQUIRE(waitUntil([&] {
    const auto states = statesFrom(fading.events.snapshot());
    return states.size() > stateCountBeforeResume && states.back() == PlaybackState::Playing;
  }));
  CHECK(fake->startCalls.load() == 1);
  CHECK(fake->stopCalls.load() == 0);

  const auto resumedClock = fading.player.queryPlaybackClock();
  CHECK(resumedClock.continuous);
  // 位置从暂停+淡出消费处继续（≈25ms 后），无回退。
  CHECK(resumedClock.position >= 40ms);

  // 在途淡出走完后执行器自动受理 0→1：增益先到零、随后连续回升到 1.0。
  std::vector<float> riseGains;
  float previous = gainMidFade;
  float minimum = 1.0F;
  bool full = false;
  std::uint32_t guard = 0U;
  while (guard < 2'000U) {
    fake->consumeFrames(240U);
    ++guard;
    const float gain = currentMasterGain(*fake);
    riseGains.push_back(gain);
    minimum = std::min(minimum, gain);
    // 块末粒度回读：相邻采样跳变应 ≤ ~0.1（无瞬时重置/突跳）。
    CHECK(gain <= previous + 0.15F);
    previous = gain;
    if (gain >= 0.95F) {
      full = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  CHECK(full);
  CHECK(minimum <= 0.1F);  // 确实先归零再回升（执行器在途延迟受理淡入）
  CHECK(fake->stopCalls.load() == 0);  // 收尾监督已中止：全程无设备 stop
  CHECK(fake->startCalls.load() == 1);

  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing resume after fade-out completes restarts device from frozen position with fade-in") {
  const auto path = sineFixture("audio_player_finishing_cold_resume.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-cold-resume"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);  // 等冷启动淡入跑完（gain≈1.0），避开零增益窗

  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  const auto eventsAtPause = fading.events.size();

  // 完整走完淡出（设备停、时钟定格）。
  const auto [driven, fadeGains] = driveUntilFinishingDone(fading.events, eventsAtPause, *fake);
  REQUIRE(fake->stopCalls.load() == 1);
  REQUIRE(driven > 0U);
  // stop 会复位包络到 1.0，末尾采样可能读回复位值——归零断言用全程最小值。
  const auto fadeMinimum = std::min_element(fadeGains.begin(), fadeGains.end());
  REQUIRE(fadeMinimum != fadeGains.end());
  CHECK(*fadeMinimum <= 0.02F);
  const auto frozenBeforeResume = fading.player.queryPlaybackClock();
  CHECK_FALSE(frozenBeforeResume.continuous);

  // 冷恢复：设备重新启动（第二次 backend start），位置从归零点继续（精确相等）。
  const auto stateCountBeforeResume = statesFrom(fading.events.snapshot()).size();
  fading.player.resume();
  REQUIRE(waitUntil([&] {
    const auto states = statesFrom(fading.events.snapshot());
    return states.size() > stateCountBeforeResume && states.back() == PlaybackState::Playing;
  }));
  CHECK(fake->startCalls.load() == 2);
  const auto afterResumeClock = fading.player.queryPlaybackClock();
  CHECK(afterResumeClock.continuous);
  CHECK(afterResumeClock.position >= frozenBeforeResume.position);
  CHECK(afterResumeClock.position <= frozenBeforeResume.position + 5ms);

  // 淡入握手：先即时落零（首采样 ≈0），随后 0→1 淡入回升到 1.0。
  float firstGain = -1.0F;
  float previous = 0.0F;
  bool sawZeroThenFull = false;
  bool sawDropBelowQuarter = false;
  std::uint32_t guard = 0U;
  while (guard < 2'000U) {
    fake->consumeFrames(240U);
    ++guard;
    const float gain = currentMasterGain(*fake);
    if (firstGain < 0.0F) {
      firstGain = gain;
      REQUIRE(firstGain <= 0.05F);  // 冷恢复先落零（无爆音）
    }
    if (gain <= 0.25F) {
      sawDropBelowQuarter = true;
    }
    CHECK(gain <= previous + 0.15F);  // 单调回升，无跳变
    previous = gain;
    if (gain >= 0.95F) {
      sawZeroThenFull = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  CHECK(sawDropBelowQuarter);
  CHECK(sawZeroThenFull);
  CHECK(fake->stopCalls.load() == 1);

  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing pause without fade enabled stays instant (regression)") {
  // fade 关（默认）与 transportFadeMs==0 两种配置都走瞬时暂停路径。
  for (const bool zeroMilliseconds : {false, true}) {
    CAPTURE(zeroMilliseconds);
    const auto path = sineFixture(
        zeroMilliseconds ? "audio_player_finishing_instant_zero.wav" : "audio_player_finishing_instant_off.wav",
        kSampleRate * 2U);
    auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
    auto* fake = backend.get();
    auto service = makeAudioPlaybackService(std::move(backend));
    AudioPlayer player{service};
    EventLog eventLog;
    player.setEventSink(eventLog.sink());
    if (zeroMilliseconds) {
      TransitionConfig transition{};
      transition.fadeOnTransport = true;
      transition.transportFadeMs = 0ms;
      service->configureTransition(transition);
    }
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    config.targetSampleRate = 48000;
    config.targetSampleFormat = AudioSampleFormat::Float32;
    config.targetChannelCount = 2;
    config.bufferDuration = 60ms;
    player.configureOutput(config);
    player.loadTrack(makeSineRequest(path, zeroMilliseconds ? "instant-zero" : "instant-off"));
    waitForState(eventLog, PlaybackState::Ready);
    player.play();
    waitForState(eventLog, PlaybackState::Playing);

    fake->consumeFrames(960U);
    player.pause();
    waitForState(eventLog, PlaybackState::Paused);
    REQUIRE(waitUntil([&] { return fake->stopCalls.load() == 1; }));
    CHECK(fake->stopCalls.load() == 1);

    // 无淡出：时钟立即定格在暂停位置，无额外消费推进。
    const auto frozen = player.queryPlaybackClock();
    CHECK_FALSE(frozen.continuous);
    player.setEventSink(BackendEventSink{});
  }
}

TEST_CASE("audio_player_finishing bounded track ending during fade-out stops at track end without residual window") {
  // 短曲目（100ms）淡出期耗尽：帧耗尽兜底应即时收尾——不等淡出(200ms)走完。
  const auto path = sineFixture("audio_player_finishing_bounded.wav", kSampleRate / 10U);
  FadingPlayer fading{200ms};
  auto* fake = fading.fake;
  fading.configureOutput(20ms);
  const auto request = TrackPlaybackRequest{.trackId = "finishing-bounded",
                                             .filePath = path,
                                             .title = "Generated Fixture",
                                             .artist = {},
                                             .offset = 0ms,
                                             .duration = 100ms,
                                             .sampleRate = std::nullopt,
                                             .bitDepth = std::nullopt,
                                             .channels = std::nullopt,
                                             .format = std::nullopt,
                                             .boundedSegment = true};
  fading.player.loadTrack(request);
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);

  // 推进到 ~55ms 后暂停（剩余 ~40ms < 淡出 200ms）。小块驱动防超过 20ms 缓冲容量的
  // 突发 → BufferUnderrun 误报。
  bool reached = false;
  std::uint32_t guard = 0U;
  while (guard++ < 2'000U && !reached) {
    fake->consumeFrames(240U);
    if (fading.player.queryPlaybackClock().position >= 55ms) {
      reached = true;
    }
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(reached);
  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  REQUIRE(fake->stopCalls.load() == 0);
  const auto eventsAtPause = fading.events.size();

  // 驱动直到收尾：剩余音频耗尽即停（驱动 << 淡出全长 200ms=9600 帧）。
  std::uint32_t driven = 0U;
  while (driven < 20'000U) {
    fake->consumeFrames(240U);
    driven += 240U;
    if (fading.events.size() > eventsAtPause) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(fake->stopCalls.load() == 1);
  CHECK(driven <= 4'800U);  // ≤100ms：无残余淡出窗口
  const auto frozen = fading.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position <= 130ms);

  // 无真实错误（BufferUnderrun 属驱动伪影，非 finishing 语义错误）；无自然播完事件；
  // 尾部状态仍是 Paused（归零收尾不是 naturalEnd）。
  const auto snapshot = fading.events.snapshot();
  CHECK_FALSE(hasRealError(snapshot));
  bool sawEnded = false;
  for (const auto& event : snapshot) {
    if (event.type == BackendEventType::PlaybackEnded) {
      sawEnded = true;
    }
  }
  CHECK_FALSE(sawEnded);
  const auto states = statesFrom(snapshot);
  CHECK(states.back() == PlaybackState::Paused);

  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing natural end with fade enabled stays instant (regression)") {
  const auto path = sineFixture("audio_player_finishing_natural_end.wav", kSampleRate / 10U);
  FadingPlayer fading{200ms};
  auto* fake = fading.fake;
  fading.configureOutput(20ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-natural-end"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);

  bool ended = false;
  for (int index = 0; index < 12 && !ended; ++index) {
    fake->consumeFrames(1'200U);
    const auto snapshot = fading.events.snapshot();
    ended = std::any_of(snapshot.begin(), snapshot.end(), [](const BackendEvent& event) {
      return event.type == BackendEventType::PlaybackEnded;
    });
    if (!ended) {
      std::this_thread::sleep_for(1ms);
    }
  }
  REQUIRE(ended);
  CHECK(fake->stopCalls.load() == 1);
  const auto clock = fading.player.queryPlaybackClock();
  CHECK_FALSE(clock.continuous);
  CHECK(clock.position <= 130ms);

  // fade 开不影响自然播完：无 Paused 介入。
  const auto states = statesFrom(fading.events.snapshot());
  CHECK(std::find(states.begin(), states.end(), PlaybackState::Paused) == states.end());
  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing fade disabled during fade-out resume returns to full gain (F1 regression)") {
  const auto path = sineFixture("audio_player_finishing_f1_fadeoff.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-f1-fadeoff"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);

  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  REQUIRE(fake->stopCalls.load() == 0);

  // 淡出在途（消耗 ~30%）时禁用 fade（configureTransition 任意态合法）。
  float gainMidFade = 1.0F;
  std::uint32_t guard = 0U;
  while (guard++ < 2'000U && gainMidFade > 0.7F) {
    fake->consumeFrames(240U);
    gainMidFade = currentMasterGain(*fake);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(gainMidFade > 0.4F);
  REQUIRE(gainMidFade < 0.85F);
  CHECK(fake->stopCalls.load() == 0);

  TransitionConfig disabled{};
  fading.service->configureTransition(disabled);

  // fade-off 后 resume：收尾监督中止但设备保持运行；不得出现"逻辑 Playing、增益永久
  // 停在 0"的静音缺陷。在途 1→0 轨迹继续走完（dip），随后即时直落回 1.0——dip-then-
  // jump 是 fade-off 的正确语义，不断言平滑回升。
  const auto stateCountBeforeResume = statesFrom(fading.events.snapshot()).size();
  fading.player.resume();
  REQUIRE(waitUntil([&] {
    const auto states = statesFrom(fading.events.snapshot());
    return states.size() > stateCountBeforeResume && states.back() == PlaybackState::Playing;
  }));
  CHECK(fake->startCalls.load() == 1);
  CHECK(fake->stopCalls.load() == 0);

  bool reachedFull = false;
  float minimum = 1.0F;
  guard = 0U;
  // 预算 1s（曲目剩余 >1.8s）：缺陷代码下 gain 停在 0 会跑满预算（且无 stop 事件把
  // 包络复位成 1.0 造成假阳性），修复代码下 ~30 次迭代内即回升。
  while (guard++ < 200U && fake->stopCalls.load() == 0) {
    fake->consumeFrames(240U);
    const float gain = currentMasterGain(*fake);
    minimum = std::min(minimum, gain);
    if (gain >= 0.95F) {
      reachedFull = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  CHECK(reachedFull);
  CHECK(minimum <= 0.2F);  // 在途淡出走完的 dip（dip-then-jump 语义）
  CHECK(fake->stopCalls.load() == 0);

  // 满增益保持（非瞬时巧合）：位置连续推进、增益不回零。
  const auto clockAfterFull = fading.player.queryPlaybackClock();
  CHECK(clockAfterFull.continuous);
  for (int index = 0; index < 6; ++index) {
    fake->consumeFrames(240U);
    CHECK(currentMasterGain(*fake) >= 0.95F);
  }
  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_finishing illegal seek during stop fade-out keeps finishing and stops device once (F2 regression)") {
  const auto path = sineFixture("audio_player_finishing_f2_stop_seek.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "finishing-f2-stop-seek"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);

  // stop（fade on → StopCleanup 收尾）：逻辑 Stopped 已发，设备仍在淡出运行。
  fading.player.stop();
  waitForState(fading.events, PlaybackState::Stopped);
  REQUIRE(fake->stopCalls.load() == 0);

  // 淡出在途（消耗 ~20%）时发非法 seek（逻辑 Stopped 态 seek 非法）。
  float gainBeforeSeek = 1.0F;
  std::uint32_t guard = 0U;
  while (guard++ < 2'000U && gainBeforeSeek > 0.8F) {
    fake->consumeFrames(240U);
    gainBeforeSeek = currentMasterGain(*fake);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(gainBeforeSeek < 0.95F);
  CHECK(fake->stopCalls.load() == 0);

  const auto eventsBeforeSeek = fading.events.size();
  fading.player.seek(0ms);
  // 非法态 seek：状态机只发 SeekFailed 错误事件；在途收尾监督必须保留（修复前会清
  // finishing + 停 ticker → 淡出走完无人停设备的僵尸）。
  REQUIRE(waitUntil([&] {
    for (const auto& event : fading.events.snapshot()) {
      if (event.type == BackendEventType::PlaybackError) {
        const auto& error = std::get<PlaybackError>(event.payload);
        if (error.code == PlaybackErrorCode::SeekFailed) {
          return true;
        }
      }
    }
    return false;
  }));
  CHECK(fake->startCalls.load() == 1);
  CHECK(fake->stopCalls.load() == 0);

  // 驱动直到收尾完成事件（stopDevice 后 publishPosition）→ 恰好停一次，无僵尸设备。
  const auto eventsAtSeekError = fading.events.size();
  std::uint32_t drivenAfterSeek = 0U;
  while (drivenAfterSeek < 12'000U && fading.events.size() == eventsAtSeekError) {
    fake->consumeFrames(240U);
    drivenAfterSeek += 240U;
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(fading.events.size() > eventsAtSeekError);
  CHECK(fake->stopCalls.load() == 1);
  CHECK(fake->startCalls.load() == 1);

  // StopCleanup 完成：时钟定格、逻辑态仍 Stopped、位置无回退。
  const auto frozen = fading.player.queryPlaybackClock();
  const auto frozenAgain = fading.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position == frozenAgain.position);
  const auto states = statesFrom(fading.events.snapshot());
  CHECK(states.back() == PlaybackState::Stopped);
  fading.player.setEventSink(BackendEventSink{});
}

}
