#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
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
  int initializeCalls{0};
  int startCalls{0};
  int stopCalls{0};
  int uninitializeCalls{0};
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
  int startCalls{0};
  int stopCalls{0};
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

  CHECK(fake->initializeCalls == 1);
  CHECK(fake->startCalls == 3);
  CHECK(fake->stopCalls >= 3);
  CHECK(fake->uninitializeCalls == 0);
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

}
