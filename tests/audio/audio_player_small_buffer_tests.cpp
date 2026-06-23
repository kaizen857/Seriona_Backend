#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/audio_playback_service.h"

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

class SmallBufferBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    userData = request.callbackUserData;
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

  void uninitialize() noexcept override {
    userData = nullptr;
    started = false;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  void consume(std::uint32_t frames) {
    REQUIRE(userData != nullptr);
    callbackBuffer.assign(static_cast<std::size_t>(frames) * format.channelCount * 4U, 0U);
    AudioOutputDevice::renderCallback(userData, callbackBuffer.data(), frames);
    if (std::any_of(callbackBuffer.begin(), callbackBuffer.end(), [](std::uint8_t value) { return value != 0U; })) {
      ++nonSilentCallbacks;
    } else {
      ++silentCallbacks;
    }
  }

  AudioDeviceFormat format{.deviceId = "small-buffer",
                           .deviceName = "Small Buffer",
                           .backendName = "fake",
                           .sampleRate = kSampleRate,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 1,
                           .bufferFrames = 16,
                           .actualMode = AudioOutputMode::Mixed};
  AudioOutputDevice* userData{nullptr};
  std::vector<std::uint8_t> callbackBuffer{};
  int initializeCalls{0};
  int startCalls{0};
  int stopCalls{0};
  int nonSilentCallbacks{0};
  int silentCallbacks{0};
  bool started{false};
};

TrackPlaybackRequest requestFor(const std::filesystem::path& path) {
  return TrackPlaybackRequest{.trackId = "small-buffer-track",
                              .filePath = path,
                              .title = "Small Buffer Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

}

TEST_CASE("audio_player_small_buffer keeps playback running when decoded frames exceed queue capacity") {
  const auto path = writeSineFixture("audio_player_small_buffer.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  for (int index = 0; index < 40; ++index) {
    fake->consume(16U);
    static_cast<void>(player.queryPlaybackClock());
  }
  const auto clock = player.queryPlaybackClock();

  CHECK(clock.position > 0ms);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
}

TEST_CASE("audio_player_small_buffer refills playback without clock polling") {
  const auto path = writeSineFixture("audio_player_small_buffer_no_poll.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  for (int index = 0; index < 80; ++index) {
    fake->consume(16U);
    std::this_thread::sleep_for(2ms);
  }

  const auto clock = player.queryPlaybackClock();
  CHECK(clock.position > 0ms);
  CHECK(fake->nonSilentCallbacks > 10);
  CHECK(fake->started);
}

TEST_CASE("audio_player_small_buffer publishes progress while playing without clock polling") {
  const auto path = writeSineFixture("audio_player_small_buffer_progress_events.wav", kSampleRate * 2U);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();
  for (int index = 0; index < 120; ++index) {
    fake->consume(16U);
    std::this_thread::sleep_for(2ms);
  }

  std::vector<std::chrono::milliseconds> positions;
  for (const auto& event : events) {
    if (event.type == BackendEventType::PlaybackPositionUpdated) {
      positions.push_back(std::get<PlaybackPositionUpdated>(event.payload).clock.position);
    }
  }
  REQUIRE(positions.size() >= 3U);
  CHECK(positions.back() > positions.front());
}

TEST_CASE("audio_player_small_buffer drains pending tail before playback ended") {
  const auto path = writeSineFixture("audio_player_small_buffer_tail.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.play();

  auto clock = player.queryPlaybackClock();
  for (int index = 0; index < 4000 && std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
         return event.type == BackendEventType::PlaybackEnded;
       });
       ++index) {
    fake->consume(16U);
    clock = player.queryPlaybackClock();
  }

  CHECK(clock.position >= 990ms);
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackEnded;
  }));
}

TEST_CASE("audio_player_resume_from_stopped_does_not_start_device") {
  const auto path = writeSineFixture("audio_player_resume_from_stopped.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.stop();
  const int startsBeforeResume = fake->startCalls;
  player.resume();

  CHECK(fake->startCalls == startsBeforeResume);
  CHECK_FALSE(fake->started);
  CHECK(std::any_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).message == "resume requires Paused state";
  }));
}

TEST_CASE("audio_player_seek_from_stopped_reports_one_error_without_clock_mutation") {
  const auto path = writeSineFixture("audio_player_seek_from_stopped.wav", kSampleRate);
  auto backend = std::make_unique<SmallBufferBackend>();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });

  AudioOutputConfig config{};
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 1;
  config.bufferDuration = 1ms;
  player.configureOutput(config);

  player.loadTrack(requestFor(path));
  player.stop();
  const auto beforeSeek = player.queryPlaybackClock();
  player.seek(500ms);
  const auto afterSeek = player.queryPlaybackClock();
  const auto errorCount = std::count_if(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PlaybackError &&
           std::get<PlaybackError>(event.payload).code == PlaybackErrorCode::SeekFailed;
  });

  CHECK(errorCount == 1);
  CHECK(afterSeek.position == beforeSeek.position);
  CHECK(std::none_of(events.begin(), events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PositionDiscontinuity;
  }));
}

}
