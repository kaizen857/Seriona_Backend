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

}

TEST_CASE("audio_player_single_track supports fake-device command path") {
  const auto path = sineFixture("audio_player_single_track.wav", kSampleRate * 2U);
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
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
  player.play();
  fake->consumeFrames(2400U);
  const auto afterPlay = player.queryPlaybackClock();
  player.pause();
  const auto paused = player.queryPlaybackClock();
  player.resume();
  fake->consumeFrames(2400U);
  player.seek(700ms);
  const auto afterSeek = player.queryPlaybackClock();
  player.stop();

  CHECK(fake->initializeCalls == 1);
  CHECK(fake->startCalls == 3);
  CHECK(fake->stopCalls >= 3);
  CHECK(fake->uninitializeCalls == 0);
  CHECK(afterPlay.position >= 40ms);
  CHECK(paused.position >= afterPlay.position);
  CHECK(afterSeek.trackId == "single-track");
  CHECK(afterSeek.position >= 700ms);

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
}

}
