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

enum class FailureMode {
  None,
  InitializeFormatRejected,
  InitializeDeviceUnavailable,
  StartDeviceUnavailable,
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

std::filesystem::path fixtureDir() {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  std::filesystem::create_directories(root);
  return root;
}

std::vector<std::int16_t> makeSine(std::uint32_t frames) {
  std::vector<std::int16_t> samples;
  samples.reserve(frames * kChannels);

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const double phase = (2.0 * kPi * 440.0 * static_cast<double>(frame)) / static_cast<double>(kSampleRate);
    samples.push_back(static_cast<std::int16_t>(std::lround(std::sin(phase) * 0.5 * 32767.0)));
  }

  return samples;
}

void writeWavHeader(std::ofstream& output, std::uint32_t dataSize) {
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
}

void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& samples) {
  std::filesystem::create_directories(path.parent_path());
  const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());

  writeWavHeader(output, dataSize);
  for (const auto sample : samples) {
    writeU16(output, static_cast<std::uint16_t>(sample));
  }

  REQUIRE(output.good());
}

std::filesystem::path sineFixture(std::string name, std::uint32_t frames) {
  const auto path = fixtureDir() / std::move(name);
  writeWav(path, makeSine(frames));
  return path;
}

std::filesystem::path textFixture(std::string name) {
  const auto path = fixtureDir() / std::move(name);
  std::ofstream output(path, std::ios::trunc);
  REQUIRE(output.good());
  output << "this is not an audio container";
  return path;
}

std::filesystem::path corruptPcmFixture(std::string name) {
  const auto path = fixtureDir() / std::move(name);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.good());
  writeWavHeader(output, 1U);
  const auto byte = std::array<unsigned char, 1>{0x7FU};
  output.write(reinterpret_cast<const char*>(byte.data()), static_cast<std::streamsize>(byte.size()));
  REQUIRE(output.good());
  return path;
}

AudioOutputConfig outputConfig() {
  AudioOutputConfig config{};
  config.outputMode = AudioOutputMode::Mixed;
  config.preferredDeviceId = "fake-device";
  config.targetSampleRate = kSampleRate;
  config.targetSampleFormat = AudioSampleFormat::Float32;
  config.targetChannelCount = 2;
  config.bufferDuration = 20ms;
  config.allowFallback = false;
  return config;
}

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

std::size_t bytesPerSample(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U;
  case AudioSampleFormat::Int24:
    return 3U;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U;
  case AudioSampleFormat::Unknown:
    return 0U;
  }

  return 0U;
}

class ErrorMatrixAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  explicit ErrorMatrixAudioOutputDeviceBackend(FailureMode mode = FailureMode::None) : mode_(mode) {}

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {format}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    ++initializeCalls;
    queue = request.pcmQueue;
    userData = request.callbackUserData;
    lastError_.reset();

    format.deviceId = request.config.preferredDeviceId.empty() ? "fake-device" : request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    format.actualMode = request.config.outputMode;

    if (mode_ == FailureMode::InitializeDeviceUnavailable || request.config.preferredDeviceId == "missing-device") {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "audio output device is unavailable",
                                          "preferred device " + request.config.preferredDeviceId + " is not available"};
      initialized = false;
      return false;
    }

    if (mode_ == FailureMode::InitializeFormatRejected) {
      initialized = false;
      return false;
    }

    initialized = true;
    return true;
  }

  [[nodiscard]] bool start() override {
    ++startCalls;
    lastError_.reset();
    if (mode_ == FailureMode::StartDeviceUnavailable) {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "failed to start fake audio output device",
                                          "simulated device-change start failure"};
      started = false;
      return false;
    }

    started = initialized;
    return initialized;
  }

  [[nodiscard]] bool stop() override {
    ++stopCalls;
    started = false;
    return true;
  }

  void uninitialize() noexcept override {
    ++uninitializeCalls;
    initialized = false;
    started = false;
    queue = nullptr;
    userData = nullptr;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  [[nodiscard]] std::optional<AudioOutputDeviceError> lastError() const override { return lastError_; }

  void consumeFrames(std::uint32_t frames) {
    REQUIRE(userData != nullptr);
    const auto bytesPerFrame = static_cast<std::size_t>(format.channelCount) * bytesPerSample(format.sampleFormat);
    callbackBuffer.assign(static_cast<std::size_t>(frames) * bytesPerFrame, 0U);
    AudioOutputDevice::renderCallback(userData, callbackBuffer.data(), frames);
  }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = kSampleRate,
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
  bool initialized{false};
  bool started{false};

private:
  FailureMode mode_{FailureMode::None};
  std::optional<AudioOutputDeviceError> lastError_{};
};

std::vector<BackendEvent> eventsOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  std::vector<BackendEvent> filtered;
  for (const auto& event : events) {
    if (event.type == type) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

PlaybackError requirePlaybackError(const std::vector<BackendEvent>& events, PlaybackErrorCode code) {
  for (const auto& event : events) {
    if (event.type != BackendEventType::PlaybackError) {
      continue;
    }

    const auto& error = std::get<PlaybackError>(event.payload);
    if (error.code == code) {
      return error;
    }
  }

  FAIL("expected playback error was not delivered");
  return {};
}

void checkUsefulError(const PlaybackError& error, PlaybackErrorCode code) {
  CHECK(error.code == code);
  CHECK_FALSE(error.message.empty());
  CHECK_FALSE(error.detail.empty());
  CHECK(error.clock.has_value());
}

std::vector<BackendEvent> loadEvents(const std::filesystem::path& path,
                                     std::string trackId,
                                     std::unique_ptr<AudioOutputDeviceBackend> backend,
                                     AudioOutputConfig config = outputConfig()) {
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(config);
  player.loadTrack(request(path, std::move(trackId)));
  return events;
}

}

TEST_CASE("audio_error_matrix maps open and format failures to typed playback errors") {
  const auto missingError = requirePlaybackError(loadEvents(fixtureDir() / "missing-error-matrix.wav",
                                                           "missing",
                                                           std::make_unique<ErrorMatrixAudioOutputDeviceBackend>()),
                                                 PlaybackErrorCode::OpenFailed);
  checkUsefulError(missingError, PlaybackErrorCode::OpenFailed);
  CHECK(missingError.detail.find("missing-error-matrix.wav") != std::string::npos);

  const auto unsupportedError = requirePlaybackError(loadEvents(textFixture("audio_error_matrix_not_audio.txt"),
                                                               "unsupported",
                                                               std::make_unique<ErrorMatrixAudioOutputDeviceBackend>()),
                                                     PlaybackErrorCode::UnsupportedFormat);
  checkUsefulError(unsupportedError, PlaybackErrorCode::UnsupportedFormat);
  const bool unsupportedHasContext = unsupportedError.message.find("audio") != std::string::npos ||
                                     unsupportedError.detail.find("Invalid") != std::string::npos;
  CHECK(unsupportedHasContext);

  const auto negotiationError = requirePlaybackError(loadEvents(sineFixture("audio_error_matrix_negotiation.wav", kSampleRate / 10U),
                                                               "negotiation",
                                                               std::make_unique<ErrorMatrixAudioOutputDeviceBackend>(FailureMode::InitializeFormatRejected)),
                                                     PlaybackErrorCode::FormatNegotiationFailed);
  checkUsefulError(negotiationError, PlaybackErrorCode::FormatNegotiationFailed);
  CHECK(negotiationError.detail.find("rejected by audio output device") != std::string::npos);
}

TEST_CASE("audio_error_matrix maps device unavailable init and start failures") {
  auto deviceChangeConfig = outputConfig();
  deviceChangeConfig.preferredDeviceId = "missing-device";
  const auto initError = requirePlaybackError(loadEvents(sineFixture("audio_error_matrix_device_init.wav", kSampleRate / 10U),
                                                        "device-init",
                                                        std::make_unique<ErrorMatrixAudioOutputDeviceBackend>(),
                                                        deviceChangeConfig),
                                              PlaybackErrorCode::DeviceUnavailable);
  checkUsefulError(initError, PlaybackErrorCode::DeviceUnavailable);
  CHECK(initError.detail.find("missing-device") != std::string::npos);

  const auto path = sineFixture("audio_error_matrix_device_start.wav", kSampleRate / 10U);
  auto backend = std::make_unique<ErrorMatrixAudioOutputDeviceBackend>(FailureMode::StartDeviceUnavailable);
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig());
  player.loadTrack(request(path, "device-start"));
  CHECK(eventsOf(events, BackendEventType::PlaybackError).empty());

  player.play();

  CHECK(fake->startCalls == 1);
  const auto startError = requirePlaybackError(events, PlaybackErrorCode::DeviceUnavailable);
  checkUsefulError(startError, PlaybackErrorCode::DeviceUnavailable);
  CHECK(startError.detail.find("start failure") != std::string::npos);
}

TEST_CASE("audio_error_matrix maps decode, underrun, and seek failures") {
  const auto decodeError = requirePlaybackError(loadEvents(corruptPcmFixture("audio_error_matrix_corrupt.wav"),
                                                          "decode",
                                                          std::make_unique<ErrorMatrixAudioOutputDeviceBackend>()),
                                                PlaybackErrorCode::DecodeFailed);
  checkUsefulError(decodeError, PlaybackErrorCode::DecodeFailed);

  AudioPlayer seekPlayer{makeAudioPlaybackService(std::make_unique<ErrorMatrixAudioOutputDeviceBackend>())};
  std::vector<BackendEvent> seekEvents;
  seekPlayer.setEventSink([&seekEvents](BackendEvent event) { seekEvents.push_back(std::move(event)); });
  seekPlayer.seek(500ms);
  const auto seekError = requirePlaybackError(seekEvents, PlaybackErrorCode::SeekFailed);
  checkUsefulError(seekError, PlaybackErrorCode::SeekFailed);
  CHECK(seekError.detail.find("missing playback pipeline") != std::string::npos);

  const auto underrunPath = sineFixture("audio_error_matrix_underrun.wav", 480U);
  auto backend = std::make_unique<ErrorMatrixAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig());
  player.loadTrack(request(underrunPath, "underrun"));
  player.play();
  fake->consumeFrames(960U);
  const auto clock = player.queryPlaybackClock();

  CHECK(clock.trackId == "underrun");
  const auto underrunError = requirePlaybackError(events, PlaybackErrorCode::BufferUnderrun);
  checkUsefulError(underrunError, PlaybackErrorCode::BufferUnderrun);
  REQUIRE(underrunError.clock.has_value());
  CHECK(underrunError.clock->trackId == "underrun");
  CHECK(underrunError.detail.find("silence frames") != std::string::npos);
}

}
