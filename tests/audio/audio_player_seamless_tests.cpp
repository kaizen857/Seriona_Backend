#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
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
    queue = request.pcmQueue;
    userData = request.callbackUserData;
    format.deviceId = request.config.preferredDeviceId.empty() ? "fake-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    format.actualMode = request.config.outputMode;
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
    const auto bytesPerSample = format.sampleFormat == AudioSampleFormat::Float32 ? 4U : 2U;
    const auto bytesPerFrame = static_cast<std::size_t>(format.channelCount) * bytesPerSample;
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

std::vector<BackendEvent> eventsOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  std::vector<BackendEvent> filtered;
  for (const auto& event : events) {
    if (event.type == type) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

}

TEST_CASE("audio_player_seamless_mix hands prepared next track off without reopening device or emitting playback ended") {
  const auto firstPath = sineFixture("audio_player_seamless_mix_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_seamless_mix_second.wav", 960U, 660.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(firstPath, "first"));
  player.prepareNext(request(secondPath, "second"));
  player.play();
  static_cast<void>(player.queryPlaybackClock());
  fake->consumeFrames(960U);
  const auto afterHandoff = player.queryPlaybackClock();

  CHECK(fake->initializeCalls == 1);
  CHECK(fake->uninitializeCalls == 0);
  CHECK(fake->startCalls == 1);
  CHECK(fake->started);
  CHECK(afterHandoff.trackId == "second");

  const auto ended = eventsOf(events, BackendEventType::PlaybackEnded);
  const auto tracks = eventsOf(events, BackendEventType::TrackChanged);
  CHECK(ended.empty());
  REQUIRE(tracks.size() >= 2U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "first");
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "second");
}

TEST_CASE("audio_player_redundant_play_does_not_restart_device_before_reporting_error") {
  const auto path = sineFixture("audio_player_redundant_play.wav", 960U, 440.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(path, "redundant-play"));
  static_cast<void>(player.queryPlaybackClock());
  player.play();
  static_cast<void>(player.queryPlaybackClock());

  const int startsBeforeSecondPlay = fake->startCalls;
  player.play();
  static_cast<void>(player.queryPlaybackClock());

  CHECK(fake->startCalls == startsBeforeSecondPlay);
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).message == "play is not legal in current state";
  }));
}

TEST_CASE("audio_player_direct_preload prepares next track but does not claim seamless handoff") {
  const auto firstPath = sineFixture("audio_player_direct_preload_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_direct_preload_second.wav", 960U, 660.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig(AudioOutputMode::Direct));

  player.loadTrack(request(firstPath, "direct-first"));
  player.prepareNext(request(secondPath, "direct-second"));
  player.play();
  static_cast<void>(player.queryPlaybackClock());
  fake->consumeFrames(960U);
  const auto endedClock = player.queryPlaybackClock();

  CHECK(fake->initializeCalls == 1);
  CHECK(fake->stopCalls >= 1);
  CHECK(endedClock.trackId == "direct-first");
  CHECK(eventsOf(events, BackendEventType::PlaybackEnded).size() == 1U);
  const auto tracks = eventsOf(events, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 1U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "direct-first");
}

TEST_CASE("audio_player_reload_after_end_accepts_next_track_with_different_stream_parameters") {
  const auto firstPath = sineFixture("audio_player_reload_param_first.wav", 960U, 440.0,
                                     WavSpec{.sampleRate = 48'000, .channels = 1, .bitsPerSample = 16});
  const auto secondPath = sineFixture("audio_player_reload_param_second.wav", 882U, 660.0,
                                      WavSpec{.sampleRate = 44'100, .channels = 2, .bitsPerSample = 16});
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  AudioOutputConfig config{};
  config.outputMode = AudioOutputMode::Mixed;
  config.preferredDeviceId = "fake-device";
  config.bufferDuration = 20ms;
  player.configureOutput(config);

  player.loadTrack(request(firstPath, "param-first"));
  player.play();
  static_cast<void>(player.queryPlaybackClock());
  fake->consumeFrames(960U);
  static_cast<void>(player.queryPlaybackClock());
  player.loadTrack(request(secondPath, "param-second"));
  player.play();
  static_cast<void>(player.queryPlaybackClock());
  fake->consumeFrames(882U);
  const auto secondClock = player.queryPlaybackClock();

  CHECK(fake->initializeCalls == 2);
  CHECK(fake->format.sampleRate == 44100U);
  CHECK(fake->format.channelCount == 2U);
  CHECK(secondClock.trackId == "param-second");
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());
  const auto tracks = eventsOf(events, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() >= 2U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "param-first");
  CHECK(std::get<TrackChanged>(tracks[1].payload).request.trackId == "param-second");
}

TEST_CASE("audio_player_seamless_mix preserves preloaded frames larger than preload queue capacity") {
  const auto firstPath = sineFixture("audio_player_seamless_small_buffer_first.wav", 960U, 440.0);
  const auto secondPath = sineFixture("audio_player_seamless_small_buffer_second.wav", kSampleRate, 660.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(smallBufferOutputConfig());

  player.loadTrack(request(firstPath, "small-buffer-first"));
  player.prepareNext(request(secondPath, "small-buffer-second"));
  player.play();
  auto afterHandoff = player.queryPlaybackClock();
  for (int index = 0; index < 80 && afterHandoff.trackId != "small-buffer-second"; ++index) {
    fake->consumeFrames(48U);
    afterHandoff = player.queryPlaybackClock();
  }

  CHECK(afterHandoff.trackId == "small-buffer-second");
  CHECK(eventsOf(events, BackendEventType::PlaybackEnded).empty());
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());
}

TEST_CASE("audio_player_preload_failure emits error without queue policy") {
  const auto firstPath = sineFixture("audio_player_preload_failure_current.wav", 960U, 440.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig(AudioOutputMode::Mixed));

  player.loadTrack(request(firstPath, "current"));
  player.prepareNext(request(std::filesystem::current_path() / "missing-preload.wav", "missing-next"));
  player.play();
  static_cast<void>(player.queryPlaybackClock());
  fake->consumeFrames(960U);
  const auto endedClock = player.queryPlaybackClock();

  CHECK(endedClock.trackId == "current");
  CHECK(fake->initializeCalls == 1);
  CHECK(eventsOf(events, BackendEventType::PlaybackEnded).size() == 1U);
  const auto errors = eventsOf(events, BackendEventType::PlaybackError);
  REQUIRE(errors.size() == 1U);
  CHECK(std::get<PlaybackError>(errors[0].payload).code == PlaybackErrorCode::OpenFailed);
  const auto tracks = eventsOf(events, BackendEventType::TrackChanged);
  REQUIRE(tracks.size() == 1U);
  CHECK(std::get<TrackChanged>(tracks[0].payload).request.trackId == "current");
}

TEST_CASE("audio_player_seamless_mix honors preloaded next-track offset and duration boundaries") {
  const auto firstPath = sineFixture("audio_player_seamless_offset_first.wav", 960U, 440.0);
  const auto secondPath = silenceThenSineFixture("audio_player_seamless_offset_second.wav",
                                                 4'800U,
                                                 2'400U,
                                                 660.0);
  auto backend = std::make_unique<SeamlessFakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(smallBufferOutputConfig());

  auto nextRequest = request(secondPath, "second-with-offset");
  nextRequest.offset = 100ms;
  nextRequest.duration = 50ms;
  nextRequest.boundedSegment = true;

  player.loadTrack(request(firstPath, "first-with-offset"));
  player.prepareNext(nextRequest);
  player.play();

  auto clock = player.queryPlaybackClock();
  for (int index = 0; index < 80 && clock.trackId != "second-with-offset"; ++index) {
    fake->consumeFrames(48U);
    clock = player.queryPlaybackClock();
  }
  REQUIRE(clock.trackId == "second-with-offset");

  fake->consumeFrames(48U);
  CHECK(std::any_of(fake->callbackBuffer.begin(), fake->callbackBuffer.end(), [](std::uint8_t value) {
    return value != 0U;
  }));

  for (int index = 0; index < 240 && std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
         return event.type == BackendEventType::PlaybackEnded;
       });
       ++index) {
    fake->consumeFrames(48U);
    clock = player.queryPlaybackClock();
  }

  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
  CHECK(clock.position >= 149ms);
  CHECK(clock.position < 170ms);
}

}
