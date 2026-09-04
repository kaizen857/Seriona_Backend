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

namespace {

// —— T11 传送淡变与手动切歌接线测试基础设施 ——

// 与 FadingPlayer 同构，但过渡参数由调用方完整给出（fadeOnSeek/手动档位/dip 时长等），
// transition 须在 loadTrack 前配置好（configureTransition 任意态合法，但判定读取时机
// 是命令处理时——先行配置避免与播放推进竞争）。
struct ConfigurablePlayer {
  explicit ConfigurablePlayer(const TransitionConfig& transition) {
    backend = std::make_unique<FakeAudioOutputDeviceBackend>();
    fake = backend.get();
    service = makeAudioPlaybackService(std::move(backend));
    player.setPlaybackService(service);
    player.setEventSink(events.sink());
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

// 装载并播放到增益满（无 transport fade 时 stop 复位包络 1.0 → 立即满足）。
void loadAndPlayToFullGain(ConfigurablePlayer& player, const TrackPlaybackRequest& request) {
  player.player.loadTrack(request);
  waitForState(player.events, PlaybackState::Ready);
  player.player.play();
  waitForState(player.events, PlaybackState::Playing);
  driveUntilGainEstablished(*player.fake);
}

std::size_t countEvents(const std::vector<BackendEvent>& events, BackendEventType type) {
  return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [type](const BackendEvent& event) {
    return event.type == type;
  }));
}

// 归零点后驱动上行腿直到 master 增益回满（≥0.99），返回增益采样。位置事件受发布节流
// 影响不可作驱动信号——以增益为信号（轨迹由回调消费推进，确定性随块推进）。
std::vector<float> driveUntilFullGain(ConfigurablePlayer& player,
                                      std::uint32_t maxFrames = 30'000U) {
  std::vector<float> gains;
  std::uint32_t driven = 0U;
  while (driven < maxFrames) {
    player.fake->consumeFrames(240U);
    driven += 240U;
    const float gain = currentMasterGain(*player.fake);
    gains.push_back(gain);
    if (gain >= 0.99F) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  return gains;
}

// dip 归零（master ≤ ε）与"事件只出现在归零点/打断时"的联合断言驱动：驱动中一旦发现
// 被禁止提前出现的类型出现即失败。返回 [驱动帧数, 增益采样, 首次新事件前增益采样数]。
struct DipDriveResult {
  std::uint32_t drivenFrames{0};
  std::vector<float> gains;
};

DipDriveResult driveSeekDipUntilZero(ConfigurablePlayer& player,
                                     std::size_t eventsAtStart,
                                     std::uint32_t maxFrames = 30'000U) {
  DipDriveResult result;
  // 禁止提前出现：TrackChanged/Loading/PositionDiscontinuity（seek dip 期间逻辑当前曲不变、
  // 状态机事件推迟到归零点——提前出现 = 实现回退成瞬时或状态漂移）。
  while (result.drivenFrames < maxFrames) {
    player.fake->consumeFrames(240U);
    result.drivenFrames += 240U;
    const float gain = currentMasterGain(*player.fake);
    result.gains.push_back(gain);
    if (gain <= 0.02F) {
      break;
    }
    const auto snapshot = player.events.snapshot();
    if (snapshot.size() > eventsAtStart) {
      const bool premature = std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtStart),
                                         snapshot.end(),
                                         [](const BackendEvent& event) {
                                           return event.type == BackendEventType::TrackChanged ||
                                                  event.type == BackendEventType::PositionDiscontinuity ||
                                                  event.type == BackendEventType::AdvanceCompleted;
                                         });
      CHECK_FALSE(premature);  // 事件先于归零出现（TrackChanged/PD/AC 提前 = 切换提前）
      if (premature) {
        break;
      }
    }
    std::this_thread::sleep_for(1ms);
  }
  return result;
}

}

TEST_CASE("audio_player_t11 seek dip: playing seek with fade dips master to zero then restores full gain at target without device restart") {
  const auto path = sineFixture("audio_player_t11_seekdip.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.fadeOnSeek = true;
  transition.seekFadeMs = 200ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-seekdip"));
  const auto before = player.player.queryPlaybackClock();
  REQUIRE(before.continuous);

  const auto eventsAtSeek = player.events.size();
  const auto startCallsBefore = player.fake->startCalls.load();
  player.player.seek(1200ms);

  // 下行腿（100ms）期间：状态机保持 Playing、无 TrackChanged/Loading/PD；增益单调落零。
  auto drive = driveSeekDipUntilZero(player, eventsAtSeek);
  REQUIRE(drive.gains.size() > 1U);
  CHECK(drive.gains.front() >= 0.85F);
  CHECK(drive.drivenFrames >= 2'400U);  // 至少 ~50ms 下行（200ms dip 的 ½ 腿）
  // 归零读回（块末粒度）→ ε 吸收，允许 0.02~0.05 松弛（本测试 240 帧/块 = 5ms 粒度）。
  CHECK(drive.gains.back() <= 0.05F);

  // 归零点：事件齐发（Loading→PD→Playing），增益开始回升。归零读回（回调块内）先于
  // worker 派发事件（progress ticker 异步）——有界等待 Playing 出现（归零序列的终态：
  // completeSeek 内 PD 先于 Playing 连续入列，见 Playing ⇒ PD 必已入列，快照完整；勿等
  // 任意事件：underrun 等杂音会提前满足快照，错过归零事件）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::PlaybackStateChanged &&
                                std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                       });
  }));
  const auto postZero = player.events.snapshot();
  bool sawLoading = false;
  bool sawPlaying = false;
  bool sawDiscontinuity = false;
  std::size_t loadingIndex = 0U;
  std::size_t discontinuityIndex = 0U;
  std::size_t playingIndex = 0U;
  for (std::size_t i = eventsAtSeek; i < postZero.size(); ++i) {
    if (postZero[i].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(postZero[i].payload);
      if (change.state == PlaybackState::Loading && !sawLoading) {
        sawLoading = true;
        loadingIndex = i;
      }
      if (change.state == PlaybackState::Playing && !sawPlaying) {
        sawPlaying = true;
        playingIndex = i;
      }
    }
    if (postZero[i].type == BackendEventType::PositionDiscontinuity && !sawDiscontinuity) {
      sawDiscontinuity = true;
      discontinuityIndex = i;
    }
  }
  CHECK(sawLoading);
  CHECK(sawDiscontinuity);
  CHECK(sawPlaying);
  CHECK(loadingIndex < discontinuityIndex);
  CHECK(discontinuityIndex < playingIndex);
  const auto& discontinuity = std::get<PositionDiscontinuity>(postZero[discontinuityIndex].payload);
  CHECK(discontinuity.after.position >= 1190ms);
  CHECK(discontinuity.after.position <= 1210ms);
  // 无 TrackChanged（同一曲内 seek，非切歌）、无 AdvanceCompleted（非重叠/直切交接）。
  // 窗口自 seek 锚点起：loadTrack 基线各发一次 TrackChanged，全日志计数会误报。
  const auto zeroWindow = postZero.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek);
  CHECK(std::none_of(zeroWindow, postZero.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::TrackChanged;
  }));
  CHECK(std::none_of(zeroWindow, postZero.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::AdvanceCompleted;
  }));

  // 上行腿：增益回升到满；时钟从目标继续（无回退、连续）。
  const auto riseGains = driveUntilFullGain(player);
  REQUIRE_FALSE(riseGains.empty());
  CHECK(riseGains.back() >= 0.99F);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.continuous);
  CHECK(after.position >= 1200ms);
  CHECK(after.position <= 1400ms);  // 上行 100ms + 余量
  // 设备全程未停未重启（dip 零点是 ring 换面而非 stop/start）。
  CHECK(player.fake->startCalls.load() == startCallsBefore);
  CHECK(player.fake->stopCalls.load() == 0);
  CHECK(player.fake->userData != nullptr);

  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 seek dip: second seek during dip only updates the pending target (single zero point)") {
  const auto path = sineFixture("audio_player_t11_seekdip_retarget.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.fadeOnSeek = true;
  transition.seekFadeMs = 300ms;  // 下行 150ms：给二次 seek 留窗口
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-retarget"));
  const auto eventsAtSeek = player.events.size();

  player.player.seek(600ms);
  // 下行腿中途二次 seek：不打断在途下行、不产生第二次 dip——仅覆盖 pending 目标。
  player.player.seek(1500ms);

  const auto drive = driveSeekDipUntilZero(player, eventsAtSeek);
  REQUIRE(drive.gains.size() > 1U);
  // 归零读回（回调块内）先于 worker 派发事件（progress ticker 异步）——有界等待 Playing
  // 出现（归零序列终态：completeSeek 内 PD 先于 Playing 连续入列，见 Playing ⇒ PD 必已
  // 入列，快照完整；勿等任意事件：杂音事件会提前满足快照，遗漏归零事件）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::PlaybackStateChanged &&
                                std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                       });
  }));
  const auto postZero = player.events.snapshot();

  // 单次归零点：只应有一对 Loading→Playing 与一个 PositionDiscontinuity。
  std::size_t loadingCount = 0U;
  std::size_t playingCount = 0U;
  std::size_t discontinuityCount = 0U;
  std::optional<std::chrono::milliseconds> discontinuityTarget;
  for (std::size_t i = eventsAtSeek; i < postZero.size(); ++i) {
    if (postZero[i].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(postZero[i].payload);
      if (change.state == PlaybackState::Loading) {
        ++loadingCount;
      }
      if (change.state == PlaybackState::Playing) {
        ++playingCount;
      }
    }
    if (postZero[i].type == BackendEventType::PositionDiscontinuity) {
      ++discontinuityCount;
      discontinuityTarget = std::get<PositionDiscontinuity>(postZero[i].payload).after.position;
    }
  }
  CHECK(loadingCount == 1U);
  CHECK(playingCount == 1U);
  CHECK(discontinuityCount == 1U);
  REQUIRE(discontinuityTarget.has_value());
  // 归零点采纳的是最新目标（1500ms），非首发目标（600ms）。
  CHECK(*discontinuityTarget >= 1490ms);
  CHECK(*discontinuityTarget <= 1510ms);

  const auto riseGains = driveUntilFullGain(player);
  REQUIRE_FALSE(riseGains.empty());
  CHECK(riseGains.back() >= 0.99F);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.continuous);
  CHECK(after.position >= 1500ms);
  CHECK(after.position <= 1800ms);  // 上行 150ms + 余量
  CHECK(player.fake->startCalls.load() == 1);
  CHECK(player.fake->stopCalls.load() == 0);

  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 seek dip: pause during dip converts to pause-freeze (seek dropped, device stops once, no discontinuity)") {
  const auto path = sineFixture("audio_player_t11_seekdip_pause.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.fadeOnSeek = true;
  transition.seekFadeMs = 400ms;  // 下行 200ms：宽窗口保证 pause 落在 dip 下行内
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-seekdip-pause"));
  const auto before = player.player.queryPlaybackClock();

  player.player.seek(1800ms);

  // 下行腿（200ms）发布后立即在未归零时发 pause——驱动直到增益跌破 0.95（确认 worker
  // 已处理 seek 并发布下行），但保持 > 0.1（远未归零，转换窗口有效）。
  std::uint32_t guard = 0U;
  float dipGain = 1.0F;
  while (guard < 200U && dipGain >= 0.95F) {
    player.fake->consumeFrames(240U);
    ++guard;
    dipGain = currentMasterGain(*player.fake);
    if (dipGain >= 0.95F) {
      std::this_thread::sleep_for(1ms);
    }
  }
  REQUIRE(dipGain < 0.95F);  // dip 下行已发布并开始执行
  REQUIRE(dipGain > 0.1F);   // 未归零：pause 命中 SeekDipDown 在途窗口
  const auto eventsBeforePause = player.events.size();
  player.player.pause();
  waitForState(player.events, PlaybackState::Paused);
  REQUIRE(player.events.size() > eventsBeforePause);  // Paused 立即发（逻辑翻转先于物理）
  REQUIRE(player.fake->stopCalls.load() == 0);        // 转换不清设备：在途下行继续走完

  // 在途下行腿走完 → 归零点按 PauseFreeze 收尾：停设备一次、时钟定格、无 seek 应用。
  guard = 0U;
  while (guard < 400U && player.fake->stopCalls.load() == 0) {
    player.fake->consumeFrames(240U);
    ++guard;
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(player.fake->stopCalls.load() == 1);
  const auto postPause = player.events.snapshot();
  // 无 PositionDiscontinuity（seek 被 pause 接管作废）、无 Loading（beginSeek 从未执行）。
  CHECK(countEvents(postPause, BackendEventType::PositionDiscontinuity) == 0U);
  CHECK(std::none_of(postPause.begin() + static_cast<std::ptrdiff_t>(eventsBeforePause), postPause.end(),
                     [](const BackendEvent& event) {
                       return event.type == BackendEventType::PlaybackStateChanged &&
                              std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Loading;
                     }));
  // 无 TrackChanged（同曲 pause，未切歌）——窗口自 pause 锚点起（loadTrack 基线算一次）。
  CHECK(std::none_of(postPause.begin() + static_cast<std::ptrdiff_t>(eventsBeforePause), postPause.end(),
                     [](const BackendEvent& event) { return event.type == BackendEventType::TrackChanged; }));
  // 时钟定格：连续播放结束；位置 < seek 目标（seek 未应用），从 dip 打断点位置定格。
  const auto frozen = player.player.queryPlaybackClock();
  const auto frozenAgain = player.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position == frozenAgain.position);
  CHECK(frozen.position < 1800ms);
  CHECK(frozen.position >= before.position);  // dip 期间旧曲内容继续消费（淡出走向零）
  CHECK(player.fake->startCalls.load() == 1);
  CHECK(player.fake->stopCalls.load() == 1);

  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 seek: seek while paused stays instant even with fadeOnSeek (no dip, device not reopened)") {
  const auto path = sineFixture("audio_player_t11_paused_seek.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.fadeOnSeek = true;
  transition.seekFadeMs = 300ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-paused-seek"));
  player.player.pause();
  waitForState(player.events, PlaybackState::Paused);
  REQUIRE(waitUntil([&] { return player.fake->stopCalls.load() == 1; }));  // 瞬时 pause（fade 关）

  const auto startCallsBefore = player.fake->startCalls.load();
  const auto eventsAtSeek = player.events.size();
  player.player.seek(900ms);

  // 事件立即发出（无需驱动：暂停 seek 瞬时路径不依赖包络推进）。但 Loading→PD→Ready
  // 由 worker 分次 push（互斥日志），Loading 与 PD 之间隔着 fillQueue 解码（耗时窗口）——
  // 只等日志增长会停在 Loading 后、PD/Ready 未入列的半途快照（间歇 sawReady=false /
  // PD==0）。等 Ready（序列终态：completeSeek 内 PD 先于 Ready 连续入列，见 Ready ⇒
  // PD 必已入列，快照完整）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::PlaybackStateChanged &&
                                std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Ready;
                       });
  }));
  const auto postSeek = player.events.snapshot();
  REQUIRE(postSeek.size() > eventsAtSeek);
  // Loading→（PD）→ Ready（暂停 seek 非连续 → 完成后 Ready；后续 play 再回 Playing）。
  bool sawLoading = false;
  bool sawReady = false;
  for (std::size_t i = eventsAtSeek; i < postSeek.size(); ++i) {
    if (postSeek[i].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(postSeek[i].payload);
      if (change.state == PlaybackState::Loading) {
        sawLoading = true;
      }
      if (change.state == PlaybackState::Ready) {
        sawReady = true;
      }
    }
  }
  CHECK(sawLoading);
  CHECK(sawReady);
  const auto postSeekWindow = postSeek.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek);
  CHECK(std::count_if(postSeekWindow, postSeek.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::PositionDiscontinuity;
        }) == 1U);
  CHECK(std::none_of(postSeekWindow, postSeek.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::TrackChanged;
  }));

  // 无 dip：设备未重启（start 计数不变）；增益无轨迹（设备停，包络保持复位值 1.0）。
  CHECK(player.fake->startCalls.load() == startCallsBefore);
  const auto frozen = player.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position >= 895ms);
  CHECK(frozen.position <= 905ms);

  // 可继续 play（Ready → Playing），位置从 seek 目标续走。
  player.player.play();
  REQUIRE(waitUntil([&] {
    const auto states = statesFrom(player.events.snapshot());
    return !states.empty() && states.back() == PlaybackState::Playing;
  }));
  const auto resumed = player.player.queryPlaybackClock();
  CHECK(resumed.continuous);
  CHECK(resumed.position >= 900ms);
  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 seek: seek during finishing pause-fade is deferred and applied at the zero point (frozen position)") {
  const auto path = sineFixture("audio_player_t11_frozen_seek.wav", kSampleRate * 2U);
  FadingPlayer fading{100ms};  // transport fade 100ms → PauseFreeze 收尾
  auto* fake = fading.fake;
  fading.configureOutput(60ms);
  fading.player.loadTrack(makeSineRequest(path, "t11-frozen-seek"));
  waitForState(fading.events, PlaybackState::Ready);
  fading.player.play();
  waitForState(fading.events, PlaybackState::Playing);
  driveUntilGainEstablished(*fake);

  fading.player.pause();
  waitForState(fading.events, PlaybackState::Paused);
  REQUIRE(fake->stopCalls.load() == 0);  // 淡出中设备仍运行

  // 淡出在途 seek：不得打断淡出/启动新 dip/触碰状态机——只更新定格位置。
  fake->consumeFrames(1'200U);  // 淡出进行中（25ms / 100ms）
  REQUIRE(currentMasterGain(*fake) > 0.4F);
  const auto eventsAtSeek = fading.events.size();
  fading.player.seek(1500ms);
  // seek 命令本身零事件（仅冻结目标）。
  const auto deadline = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < deadline && fading.events.size() == eventsAtSeek) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(fading.events.size() == eventsAtSeek);  // 淡出期 seek 不产生任何事件
  REQUIRE(fake->stopCalls.load() == 0);

  // 驱动到收尾完成：归零点 stopDevice + 定格位置补做物理 seek（1500ms）。
  const auto eventsAtFinish = fading.events.size();
  std::uint32_t driven = 0U;
  while (driven < 20'000U && fading.events.size() == eventsAtFinish) {
    fake->consumeFrames(240U);
    driven += 240U;
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(fading.events.size() > eventsAtFinish);
  REQUIRE(fake->stopCalls.load() == 1);

  const auto frozen = fading.player.queryPlaybackClock();
  const auto frozenAgain = fading.player.queryPlaybackClock();
  CHECK_FALSE(frozen.continuous);
  CHECK(frozen.position == frozenAgain.position);
  // 定格位置 = seek 目标（1500ms）：收尾补做物理 seek + clock.seek(target)。
  CHECK(frozen.position >= 1495ms);
  CHECK(frozen.position <= 1505ms);
  // 无 TrackChanged / 无 PositionDiscontinuity（非状态机 seek，纯物理补做）——
  // 窗口自 seek 锚点起（loadTrack 基线各发一次 TrackChanged）。
  const auto postSeek = fading.events.snapshot();
  const auto frozenWindow = postSeek.begin() + static_cast<std::ptrdiff_t>(eventsAtSeek);
  CHECK(std::none_of(frozenWindow, postSeek.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::TrackChanged;
  }));
  CHECK(std::none_of(frozenWindow, postSeek.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PositionDiscontinuity;
  }));
  // 无 Loading 悬挂：状态机停留在 Paused。
  const auto states = statesFrom(postSeek);
  CHECK(states.back() == PlaybackState::Paused);

  // 冷恢复：设备重启（第二次 start）、淡入、从定格位置续播。
  fading.player.resume();
  REQUIRE(waitUntil([&] {
    const auto latest = statesFrom(fading.events.snapshot());
    return !latest.empty() && latest.back() == PlaybackState::Playing;
  }));
  CHECK(fake->startCalls.load() == 2);
  const auto afterResume = fading.player.queryPlaybackClock();
  CHECK(afterResume.continuous);
  CHECK(afterResume.position >= 1500ms);
  CHECK(afterResume.position <= 1560ms);
  fading.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 manual switch: Off stays an instant hard cut (TrackChanged immediate, no dip window)") {
  const auto path = sineFixture("audio_player_t11_manual_off.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::Off;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-manual-off-a"));

  const auto eventsAtSwitch = player.events.size();
  player.player.loadTrack(makeSineRequest(path, "t11-manual-off-b"));
  // Off = 瞬时直切：TrackChanged(B) 立即出现，无需驱动下行腿（对照 dip 档需驱动至归零）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::TrackChanged &&
                                std::get<TrackChanged>(event.payload).request.trackId == "t11-manual-off-b";
                       });
  }));
  const auto postSwitch = player.events.snapshot();
  CHECK(countEvents(postSwitch, BackendEventType::PositionDiscontinuity) == 0U);
  CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
  const auto clock = player.player.queryPlaybackClock();
  CHECK(clock.trackId == "t11-manual-off-b");
  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 manual switch: ShortDip dips master, adopts staged track at zero point (states+TrackChanged together, no device restart)") {
  const auto path = sineFixture("audio_player_t11_manual_dip.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::ShortDip;
  transition.manualShortCrossfadeMs = 200ms;  // 下行 100ms + 上行 100ms
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-manual-dip-a"));

  const auto eventsAtSwitch = player.events.size();
  const auto startCallsBefore = player.fake->startCalls.load();
  player.player.loadTrack(makeSineRequest(path, "t11-manual-dip-b"));

  // 下行腿期间：无 TrackChanged/PD/AC 提前（逻辑当前曲仍是 A；换曲事件只在归零点）。
  auto drive = driveSeekDipUntilZero(player, eventsAtSwitch);
  REQUIRE(drive.gains.size() > 1U);
  CHECK(drive.drivenFrames >= 2'400U);  // ~50ms 下行（200ms dip 的 ½ 腿）
  CHECK(drive.gains.back() <= 0.05F);

  // 归零点：adoptStagedSlotAsMain 背靠背 Loading→TrackChanged→Ready→Playing；无 PD（非
  // seek）、无 AC（手动切换无 pendingAdvance 账本）。等待 Playing（adopt 序列终态：
  // 前面事件按序先入列，见 Playing ⇒ 快照完整）再断言——等 TrackChanged 会停在半途
  // （其后 Ready/Playing 尚未入列时快照）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::PlaybackStateChanged &&
                                std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                       });
  }));
  const auto postZero = player.events.snapshot();
  bool sawLoading = false;
  bool sawReady = false;
  bool sawPlaying = false;
  for (std::size_t i = eventsAtSwitch; i < postZero.size(); ++i) {
    if (postZero[i].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(postZero[i].payload);
      if (change.state == PlaybackState::Loading) {
        sawLoading = true;
      }
      if (change.state == PlaybackState::Ready) {
        sawReady = true;
      }
      if (change.state == PlaybackState::Playing) {
        sawPlaying = true;
      }
    }
  }
  CHECK(sawLoading);
  CHECK(sawReady);
  CHECK(sawPlaying);
  CHECK(std::count_if(postZero.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), postZero.end(),
                      [](const BackendEvent& event) {
                        return event.type == BackendEventType::TrackChanged &&
                               std::get<TrackChanged>(event.payload).request.trackId == "t11-manual-dip-b";
                      }) == 1U);
  CHECK(countEvents(postZero, BackendEventType::PositionDiscontinuity) == 0U);
  CHECK(countEvents(postZero, BackendEventType::AdvanceCompleted) == 0U);

  // 上行腿回满；时钟从 B 的 offset(0) 起连续（adopt 后 resume），位置 < 上行腿长 + 余量。
  const auto riseGains = driveUntilFullGain(player);
  REQUIRE_FALSE(riseGains.empty());
  CHECK(riseGains.back() >= 0.99F);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t11-manual-dip-b");
  CHECK(after.continuous);
  CHECK(after.position >= 0ms);
  CHECK(after.position <= 400ms);  // 上行 100ms + 解码续喂余量
  // dip 采纳 = rebind（单面换 ring），非设备重启。
  CHECK(player.fake->startCalls.load() == startCallsBefore);
  CHECK(player.fake->stopCalls.load() == 0);
  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 manual switch: FullCrossfade with matching preloaded slot runs true dual-source overlap (master flat, no AC)") {
  const auto path = sineFixture("audio_player_t11_manual_overlap.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
  transition.crossfadeMs = 200ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(path, "t11-manual-overlap-a"));

  // 档 3 复用槽：先 prepareNext(B)（worker FIFO——loadTrack(B) 处理时槽必已就绪）。
  const auto eventsAtSwitch = player.events.size();
  const auto masterSamples = std::make_shared<std::vector<float>>();
  player.player.prepareNext(makeSineRequest(path, "t11-manual-overlap-b"));
  player.player.loadTrack(makeSineRequest(path, "t11-manual-overlap-b"));

  // 真重叠：master 包络全程持平（不动 1.0——交叉发生在源层 EQ 对）；无 AC/PD；
  // TrackChanged(B) 在 promote（source0≤ε 且 source1≥1−ε）后发出。驱动中持续采样 master。
  REQUIRE(waitUntil([&] {
    player.fake->consumeFrames(240U);
    masterSamples->push_back(currentMasterGain(*player.fake));
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::TrackChanged &&
                                std::get<TrackChanged>(event.payload).request.trackId == "t11-manual-overlap-b";
                       });
  }));
  REQUIRE_FALSE(masterSamples->empty());
  const float minMaster = *std::min_element(masterSamples->begin(), masterSamples->end());
  CHECK(minMaster >= 0.99F);  // 源层交叉不动 master → 全程 ≥0.99（对照 dip 的 0 谷底）
  const auto postZero = player.events.snapshot();
  CHECK(countEvents(postZero, BackendEventType::AdvanceCompleted) == 0U);
  CHECK(countEvents(postZero, BackendEventType::PositionDiscontinuity) == 0U);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t11-manual-overlap-b");
  CHECK(after.continuous);
  player.player.setEventSink(BackendEventSink{});
}


TEST_CASE("audio_player_t11 manual switch review F2: near-end FullCrossfade drain-promotes promptly (no frozen legs, no permanent silence, no pop)") {
  const auto pathA = sineFixture("audio_player_t11_review_f2_a.wav", kSampleRate * 2U);
  const auto pathB = sineFixture("audio_player_t11_review_f2_b.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
  transition.crossfadeMs = 1000ms;  // 交叉长 ≫ 主源剩余 → legMs 钳制到剩余（排空先于腿完）
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  auto boundedA = makeSineRequest(pathA, "t11-review-f2-a");
  boundedA.offset = 0ms;
  boundedA.duration = 400ms;  // 定界段：剩余估算精确（endPosition − position）
  boundedA.boundedSegment = true;
  loadAndPlayToFullGain(player, boundedA);

  // 播放到 ≈200ms（剩余 ≈200ms=40 块）：排空先于 source1 上行腿完成 2-3 块
  // （source1 腿需先经即时 0 观测+受理）→ 修复前双腿冻结于 g1≈0.93 <0.98，promote 永不达。
  for (int index = 0; index < 40; ++index) {
    player.fake->consumeFrames(240U);
    std::this_thread::sleep_for(1ms);
  }
  const auto eventsAtSwitch = player.events.size();
  player.player.prepareNext(makeSineRequest(pathB, "t11-review-f2-b"));
  player.player.loadTrack(makeSineRequest(pathB, "t11-review-f2-b"));

  // 主源排空（loadedToEnd && queue 空）即应采纳：等终态 Playing（采纳序列
  // Loading→TrackChanged(B)→Ready→Playing 背靠背入列，见 Playing ⇒ 快照完整）；全程样本
  // 级检查：双腿推进只随主源 copiedFrames，冻结恢复腿跳变 ≤ 残余量（无 pop）；master 持平。
  float minMaster = 1.0F;
  float maxSampleStep = 0.0F;
  float prevChannel0 = 0.0F;
  float prevChannel1 = 0.0F;
  bool havePreviousFrame = false;
  REQUIRE(waitUntil([&] {
    player.fake->consumeFrames(240U);
    // 实时节奏：240 帧(5ms@48k) + 4ms 内睡 + waitUntil 的 1ms ≈ 48k/s——避免超实时消费
    // 在 worker 处理 prepareNext(B)/切歌命令期间饿死主源 ring（60ms 容量），否则测试自身
    // 制造人工欠载静音块（样本步进断言误报，非服务爆音）。
    std::this_thread::sleep_for(4ms);
    minMaster = std::min(minMaster, currentMasterGain(*player.fake));
    const auto* samples = reinterpret_cast<const float*>(player.fake->callbackBuffer.data());
    const auto frameCount = player.fake->callbackBuffer.size() / (2U * sizeof(float));
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
      const float left = samples[frame * 2U];
      const float right = samples[frame * 2U + 1U];
      if (havePreviousFrame) {
        maxSampleStep = std::max(maxSampleStep, std::abs(left - prevChannel0));
        maxSampleStep = std::max(maxSampleStep, std::abs(right - prevChannel1));
      }
      prevChannel0 = left;
      prevChannel1 = right;
      havePreviousFrame = true;
    }
    const auto snapshot = player.events.snapshot();
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::PlaybackStateChanged &&
                                std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                       });
  }));
  CHECK(minMaster >= 0.99F);  // 手动交叉全程 master 持平（采纳只动源层）
  CHECK(maxSampleStep <= 0.15F);  // 冻结增益步进到 1.0 无爆音（正弦 440Hz 邻帧差 ≈0.03）
  const auto postSwitch = player.events.snapshot();
  CHECK(std::count_if(postSwitch.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), postSwitch.end(),
                      [](const BackendEvent& event) {
                        return event.type == BackendEventType::TrackChanged &&
                               std::get<TrackChanged>(event.payload).request.trackId == "t11-review-f2-b";
                      }) == 1U);
  CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
  CHECK(countEvents(postSwitch, BackendEventType::PositionDiscontinuity) == 0U);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t11-review-f2-b");
  CHECK(after.continuous);
  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 manual switch review F1: seek dip in flight superseded by FullCrossfade switch (no stale seek applied, overlap unblocked)") {
  const auto pathA = sineFixture("audio_player_t11_review_f1_a.wav", kSampleRate * 2U);
  const auto pathB = sineFixture("audio_player_t11_review_f1_b.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.fadeOnSeek = true;
  transition.seekFadeMs = 300ms;  // 下行 150ms：给 loadTrack(B) 落进在途窗
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
  transition.crossfadeMs = 400ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(pathA, "t11-review-f1-a"));

  // 槽先就绪（FIFO：prepareNext 处理完 loadTrack 才执行）；seek dip 下行在途即切 B。
  const auto eventsAtSwitch = player.events.size();
  player.player.prepareNext(makeSineRequest(pathB, "t11-review-f1-b"));
  player.player.seek(1200ms);
  player.player.loadTrack(makeSineRequest(pathB, "t11-review-f1-b"));

  // 就绪槽档 3 重叠应接管在途 seek dip：陈旧 seek（1200ms）绝不应用——窗口内无 PD、
  // Loading 仅来自采纳的 loadTrack（陈旧 seek 应用会多一次）；残留下行腿走完后 master
  // 恢复 1.0（不被冻结于 0）；重叠推进不被 dip 屏蔽（source1 腿发布 → promote）。
  bool sawPlaying = false;
  REQUIRE(waitUntil([&] {
    player.fake->consumeFrames(240U);
    std::this_thread::sleep_for(4ms);
    const auto snapshot = player.events.snapshot();
    if (std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                    [](const BackendEvent& event) {
                      return event.type == BackendEventType::PlaybackStateChanged &&
                             std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                    })) {
      sawPlaying = true;
    }
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::TrackChanged &&
                                std::get<TrackChanged>(event.payload).request.trackId == "t11-review-f1-b";
                       });
   }));
   CHECK(sawPlaying);
    const auto postSwitch = player.events.snapshot();
    const auto switchWindow = postSwitch.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch);
    CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
            return event.type == BackendEventType::PositionDiscontinuity;
          }) == 0U);
  CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::PlaybackStateChanged &&
                 std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Loading;
        }) == 1U);
  CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::TrackChanged &&
                 std::get<TrackChanged>(event.payload).request.trackId == "t11-review-f1-b";
        }) == 1U);
  CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t11-review-f1-b");
  CHECK(after.continuous);
  player.player.setEventSink(BackendEventSink{});
}

TEST_CASE("audio_player_t11 manual switch review F3: FullCrossfade ready-slot switch swallows reducer batch-tail play (zero PlaybackError)") {
  const auto pathA = sineFixture("audio_player_t11_review_f3_a.wav", kSampleRate * 2U);
  const auto pathB = sineFixture("audio_player_t11_review_f3_b.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
  transition.crossfadeMs = 400ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(pathA, "t11-review-f3-a"));

  // 生产形状：reducer selectTrack(startPlayback=true) 恒发 [LoadTrack, …, Play] 批尾。
  // 档 3 就绪槽重叠入口不动状态机（机器保持 Playing，adopt 在 promote 自行 play）——
  // 批尾 play() 紧随 loadTrack(B) 入队，worker FIFO 处理时重叠已激活；扩展后的吞掉
  // 条件（|| manualOverlapActive_）必须在状态机调用前拦截，否则 play() 自 Playing 走
  // 非法迁移 → PlaybackError（伪错误 UI 全程交叉窗）。F1 接管把在途 dip 清成 None 后，
  // 旧 SeekDipDown/ManualDipDown 条件对 F1 场景与普通档 3 双双失守——本用例覆盖此域。
  const auto eventsAtSwitch = player.events.size();
  player.player.prepareNext(makeSineRequest(pathB, "t11-review-f3-b"));
  player.player.loadTrack(makeSineRequest(pathB, "t11-review-f3-b"));
  player.player.play();

  bool sawPlaying = false;
  REQUIRE(waitUntil([&] {
    player.fake->consumeFrames(240U);
    std::this_thread::sleep_for(4ms);
    const auto snapshot = player.events.snapshot();
    if (std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                    [](const BackendEvent& event) {
                      return event.type == BackendEventType::PlaybackStateChanged &&
                             std::get<PlaybackStateChanged>(event.payload).state == PlaybackState::Playing;
                    })) {
      sawPlaying = true;
    }
    return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                       [](const BackendEvent& event) {
                         return event.type == BackendEventType::TrackChanged &&
                                std::get<TrackChanged>(event.payload).request.trackId == "t11-review-f3-b";
                       });
  }));
  const auto postSwitch = player.events.snapshot();
  const auto switchWindow = postSwitch.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch);
  CHECK(sawPlaying);
  CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::PlaybackError;
        }) == 0U);
  CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::TrackChanged &&
                 std::get<TrackChanged>(event.payload).request.trackId == "t11-review-f3-b";
        }) == 1U);
  CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
  CHECK(countEvents(postSwitch, BackendEventType::PositionDiscontinuity) == 0U);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t11-review-f3-b");
  CHECK(after.continuous);
  player.player.setEventSink(BackendEventSink{});
}

// —— 任务 13 收编：裁定矩阵缺口的补用例（事件顺序/硬切回归，服务级视图）——

// 事件窗口内出现新 Playing 且其为窗口内最后一个状态事件（等待切换/恢复的完成点；
// 全日志 back()==Playing 会命中切换前的旧 Playing，需按事件序号限定窗口）。
bool playingSince(std::size_t fromIndex, const std::vector<BackendEvent>& events) {
  bool playingSeen = false;
  PlaybackState lastState{PlaybackState::Idle};
  for (std::size_t index = fromIndex; index < events.size(); ++index) {
    if (events[index].type == BackendEventType::PlaybackStateChanged) {
      const auto& change = std::get<PlaybackStateChanged>(events[index].payload);
      lastState = change.state;
      if (change.state == PlaybackState::Playing) {
        playingSeen = true;
      }
    }
  }
  return playingSeen && lastState == PlaybackState::Playing;
}

TEST_CASE("audio_player_t13 manual switch: Direct mode ignores dip and crossfade gears (gears 8/9 invalid, instant hard cut + reopen)") {
  const auto path = sineFixture("audio_player_t13_direct_gears.wav", kSampleRate * 2U);
  SUBCASE("ShortDip gear stays an instant hard cut in Direct mode") {
    TransitionConfig transition{};
    transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::ShortDip;
    transition.manualShortCrossfadeMs = 300ms;
    ConfigurablePlayer player{transition};
    AudioOutputConfig config{};
    config.outputMode = AudioOutputMode::Direct;
    config.preferredDeviceId = "fake-device";
    config.targetSampleRate = 48000;
    config.targetSampleFormat = AudioSampleFormat::Float32;
    config.targetChannelCount = 2;
    config.bufferDuration = 60ms;
    player.player.configureOutput(config);
    loadAndPlayToFullGain(player, makeSineRequest(path, "t13-direct-gear-a"));

    const auto eventsAtSwitch = player.events.size();
    const auto initBefore = player.fake->initializeCalls.load();
    const auto startBefore = player.fake->startCalls.load();
    const auto stopBefore = player.fake->stopCalls.load();
    player.player.loadTrack(makeSineRequest(path, "t13-direct-gear-b"));
    player.player.play();

    // 档 2/3 判定要求 Mixed（manualSwitchTransitionEligible 第一闸）；Direct 恒走瞬时体
    // → TrackChanged(B) 不经任何消费驱动即可达（dip/交叉监督的换曲事件必须由回调消费
    // 推进到归零点/promote 才发——若 Direct 误入监督，本 waitUntil 不消费必然超时）。
    REQUIRE(waitUntil([&] {
      const auto snapshot = player.events.snapshot();
      return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                         [](const BackendEvent& event) {
                           return event.type == BackendEventType::TrackChanged &&
                                  std::get<TrackChanged>(event.payload).request.trackId == "t13-direct-gear-b";
                         });
    }));
    const auto postSwitch = player.events.snapshot();
    const auto switchWindow = postSwitch.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch);
    CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
            return event.type == BackendEventType::PositionDiscontinuity;
          }) == 0U);
    CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
            return event.type == BackendEventType::TrackChanged &&
                   std::get<TrackChanged>(event.payload).request.trackId == "t13-direct-gear-b";
          }) == 1U);
    CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
    CHECK(countEvents(postSwitch, BackendEventType::PlaybackEnded) == 0U);

    // Direct 恒重开（contrast Mixed 档 2/3 rebind 不重启）：A 切走停一次、B 全新开+启动。
    // TrackChanged(B) 在 switch 管线早期即广播（load 派发即发事件），设备重开随后才在
    // worker 完成；直接读计数会与 worker 竞态，先等待计数到位再断言。
    REQUIRE(waitUntil([&] { return player.fake->startCalls.load() == startBefore + 1; }));
    CHECK(player.fake->initializeCalls.load() == initBefore + 1);
    CHECK(player.fake->stopCalls.load() == stopBefore + 1);
    CHECK(player.fake->startCalls.load() == startBefore + 1);

    // 切后消费驱动：master 全程持平 1.0（无任何下行腿残余），B 内容出声。
    REQUIRE(waitUntil([&] {
      const auto snapshot = player.events.snapshot();
      return playingSince(eventsAtSwitch, snapshot);
    }));
    float minMaster = 1.0F;
    for (int index = 0; index < 10; ++index) {
      player.fake->consumeFrames(240U);
      std::this_thread::sleep_for(4ms);
      minMaster = std::min(minMaster, currentMasterGain(*player.fake));
    }
    CHECK(minMaster >= 0.99F);
    const auto after = player.player.queryPlaybackClock();
    CHECK(after.trackId == "t13-direct-gear-b");
    CHECK(after.continuous);
    player.player.setEventSink(BackendEventSink{});
  }
  SUBCASE("FullCrossfade gear stays an instant hard cut in Direct mode") {
    TransitionConfig transition{};
    transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
    transition.crossfadeMs = 400ms;
    ConfigurablePlayer player{transition};
    AudioOutputConfig config{};
    config.outputMode = AudioOutputMode::Direct;
    config.preferredDeviceId = "fake-device";
    config.targetSampleRate = 48000;
    config.targetSampleFormat = AudioSampleFormat::Float32;
    config.targetChannelCount = 2;
    config.bufferDuration = 60ms;
    player.player.configureOutput(config);
    loadAndPlayToFullGain(player, makeSineRequest(path, "t13-direct-gear-a"));

    const auto eventsAtSwitch = player.events.size();
    const auto initBefore = player.fake->initializeCalls.load();
    const auto startBefore = player.fake->startCalls.load();
    player.player.loadTrack(makeSineRequest(path, "t13-direct-gear-b"));
    player.player.play();

    REQUIRE(waitUntil([&] {
      const auto snapshot = player.events.snapshot();
      return std::any_of(snapshot.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), snapshot.end(),
                         [](const BackendEvent& event) {
                           return event.type == BackendEventType::TrackChanged &&
                                  std::get<TrackChanged>(event.payload).request.trackId == "t13-direct-gear-b";
                         });
    }));
    const auto postSwitch = player.events.snapshot();
    const auto switchWindow = postSwitch.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch);
    CHECK(std::count_if(switchWindow, postSwitch.end(), [](const BackendEvent& event) {
            return event.type == BackendEventType::PositionDiscontinuity;
          }) == 0U);
    CHECK(countEvents(postSwitch, BackendEventType::AdvanceCompleted) == 0U);
    REQUIRE(waitUntil([&] { return player.fake->startCalls.load() == startBefore + 1; }));
    CHECK(player.fake->initializeCalls.load() == initBefore + 1);
    CHECK(player.fake->startCalls.load() == startBefore + 1);
    const auto after = player.player.queryPlaybackClock();
    CHECK(after.trackId == "t13-direct-gear-b");
    player.player.setEventSink(BackendEventSink{});
  }
}

TEST_CASE("audio_player_t13 manual switch: pause during FullCrossfade overlap aborts crossfade and stays on current track (no adoption, no event drift)") {
  const auto pathA = sineFixture("audio_player_t13_overlap_pause_a.wav", kSampleRate * 2U);
  const auto pathB = sineFixture("audio_player_t13_overlap_pause_b.wav", kSampleRate * 2U);
  TransitionConfig transition{};
  transition.manualAdvanceFadeMode = ManualAdvanceFadeMode::FullCrossfade;
  transition.crossfadeMs = 400ms;
  ConfigurablePlayer player{transition};
  player.configureOutput(60ms);
  loadAndPlayToFullGain(player, makeSineRequest(pathA, "t13-overlap-pause-a"));

  const auto eventsAtSwitch = player.events.size();
  const auto startBefore = player.fake->startCalls.load();
  const auto stopBefore = player.fake->stopCalls.load();
  // FIFO 批：[prepareNext(B), loadTrack(B), pause]——worker 依序处理：档 3 就绪槽启动
  // 真重叠（不动状态机），pause 紧随其后到达时 secondSourceActive 必为真 → 立即撤重叠
  // （打断即瞬时）+ 主源瞬时暂停。promote 需回调消费推进源层腿，本序列零消费 → 途中
  // 任何 tick 都不可能提前 promote（无竞态）。
  player.player.prepareNext(makeSineRequest(pathB, "t13-overlap-pause-b"));
  player.player.loadTrack(makeSineRequest(pathB, "t13-overlap-pause-b"));
  player.player.pause();

  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    const auto states = statesFrom(snapshot);
    return std::find(states.begin(), states.end(), PlaybackState::Paused) != states.end();
  }));
  const auto pausedEvents = player.events.snapshot();
  const auto window = pausedEvents.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch);
  // 重叠被中止：B 从未被采纳——无 TrackChanged(B)/AC/PD；只多出 Paused 状态。
  CHECK(std::count_if(window, pausedEvents.end(), [](const BackendEvent& event) {
          return event.type == BackendEventType::TrackChanged;
        }) == 0U);
  CHECK(countEvents(pausedEvents, BackendEventType::AdvanceCompleted) == 0U);
  CHECK(countEvents(pausedEvents, BackendEventType::PositionDiscontinuity) == 0U);
  // 暂停只停一次设备（重叠撤面不触设备停止）；恢复前无新启动。
  CHECK(player.fake->stopCalls.load() == stopBefore + 1);
  CHECK(player.fake->startCalls.load() == startBefore);

  player.player.resume();
  // 冷恢复续播 A（服务停在 A——重叠从未采纳 B；N4 语义锁定：pause 中断手动切换 = 停在 A）。
  REQUIRE(waitUntil([&] {
    const auto snapshot = player.events.snapshot();
    return playingSince(eventsAtSwitch, snapshot);
  }));
  const auto resumedEvents = player.events.snapshot();
  CHECK(std::count_if(resumedEvents.begin() + static_cast<std::ptrdiff_t>(eventsAtSwitch), resumedEvents.end(),
                      [](const BackendEvent& event) {
                        return event.type == BackendEventType::TrackChanged;
                      }) == 0U);
  CHECK(player.fake->startCalls.load() == startBefore + 1);
  // 定长消费驱动（50ms ≈ 10×240 帧，4ms 内睡 ≈ 实时）：A 内容持续出声、master 持平 1.0。
  float minMaster = 1.0F;
  for (int index = 0; index < 10; ++index) {
    player.fake->consumeFrames(240U);
    std::this_thread::sleep_for(4ms);
    minMaster = std::min(minMaster, currentMasterGain(*player.fake));
  }
  CHECK(minMaster >= 0.99F);
  const auto after = player.player.queryPlaybackClock();
  CHECK(after.trackId == "t13-overlap-pause-a");
  CHECK(after.continuous);
  const auto* samples = reinterpret_cast<const float*>(player.fake->callbackBuffer.data());
  const auto sampleCount = player.fake->callbackBuffer.size() / sizeof(float);
  const bool audible =
      std::any_of(samples, samples + sampleCount, [](float value) { return std::abs(value) > 0.01F; });
  CHECK(audible);
  player.player.setEventSink(BackendEventSink{});
}

}  // namespace seriona::audio

