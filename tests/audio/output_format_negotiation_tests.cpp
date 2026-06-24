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

struct SupportedFormat {
  AudioOutputMode mode{AudioOutputMode::Mixed};
  std::uint32_t sampleRate{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::uint16_t channelCount{0};
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

std::filesystem::path sineFixture(std::string name) {
  const auto root = std::filesystem::current_path() / "generated_audio_fixtures";
  const auto path = root / std::move(name);
  writeWav(path, makeSine(kSampleRate / 10U));
  return path;
}

class MatrixAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  explicit MatrixAudioOutputDeviceBackend(std::vector<SupportedFormat> supportedFormats)
      : supportedFormats_(std::move(supportedFormats)) {}

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override { return {currentFormat_}; }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    requests.push_back(request);
    const auto supported = std::find_if(supportedFormats_.begin(),
                                        supportedFormats_.end(),
                                        [&request](const SupportedFormat& format) {
                                          return format.mode == request.config.outputMode &&
                                                 format.sampleRate == request.sampleRate &&
                                                 format.sampleFormat == request.sampleFormat &&
                                                 format.channelCount == request.channelCount;
                                        });
    if (supported == supportedFormats_.end()) {
      return false;
    }

    currentFormat_ = AudioDeviceFormat{.deviceId = request.config.preferredDeviceId.empty() ? "matrix-device" : request.config.preferredDeviceId,
                                       .deviceName = "Matrix Device",
                                       .backendName = "fake-matrix",
                                       .sampleRate = request.sampleRate,
                                       .sampleFormat = request.sampleFormat,
                                       .channelCount = request.channelCount,
                                       .bufferFrames = request.bufferFrames,
                                       .actualMode = request.config.outputMode,
                                       .fallbackApplied = false};
    initialized = true;
    return true;
  }

  [[nodiscard]] bool start() override { return initialized; }
  [[nodiscard]] bool stop() override { return true; }

  void uninitialize() noexcept override { initialized = false; }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return currentFormat_; }

  std::vector<AudioOutputDeviceOpenRequest> requests{};
  bool initialized{false};

private:
  std::vector<SupportedFormat> supportedFormats_;
  AudioDeviceFormat currentFormat_{.deviceId = "matrix-device",
                                   .deviceName = "Matrix Device",
                                   .backendName = "fake-matrix",
                                   .actualMode = AudioOutputMode::Mixed};
};

struct LoadResult {
  std::vector<BackendEvent> events{};
  std::vector<AudioOutputDeviceOpenRequest> requests{};
};

AudioOutputConfig config(AudioOutputMode mode,
                         std::uint32_t sampleRate,
                         AudioSampleFormat sampleFormat,
                         std::uint16_t channelCount) {
  AudioOutputConfig outputConfig{};
  outputConfig.outputMode = mode;
  outputConfig.targetSampleRate = sampleRate;
  outputConfig.targetSampleFormat = sampleFormat;
  outputConfig.targetChannelCount = channelCount;
  outputConfig.bufferDuration = 40ms;
  outputConfig.preferredDeviceId = "matrix-device";
  return outputConfig;
}

TrackPlaybackRequest request(const std::filesystem::path& path, std::string trackId) {
  return TrackPlaybackRequest{.trackId = std::move(trackId),
                              .filePath = path,
                              .title = "Matrix Fixture",
                              .artist = {},
                              .offset = std::nullopt,
                              .duration = std::nullopt,
                              .sampleRate = std::nullopt,
                              .bitDepth = std::nullopt,
                              .channels = std::nullopt,
                              .format = std::nullopt};
}

LoadResult loadWith(std::vector<SupportedFormat> supportedFormats,
                    const AudioOutputConfig& outputConfig,
                    const std::filesystem::path& path,
                    std::string trackId) {
  auto backend = std::make_unique<MatrixAudioOutputDeviceBackend>(std::move(supportedFormats));
  auto* fake = backend.get();
  AudioPlayer player{makeAudioPlaybackService(std::move(backend))};
  std::vector<BackendEvent> events;
  player.setEventSink([&events](BackendEvent event) { events.push_back(std::move(event)); });
  player.configureOutput(outputConfig);
  player.loadTrack(request(path, std::move(trackId)));
  static_cast<void>(player.queryPlaybackClock());
  return LoadResult{std::move(events), fake->requests};
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

const BackendEvent& firstEventOf(const std::vector<BackendEvent>& events, BackendEventType type) {
  const auto iterator = std::find_if(events.begin(), events.end(), [type](const BackendEvent& event) { return event.type == type; });
  REQUIRE(iterator != events.end());
  return *iterator;
}

}

TEST_CASE("output_format_negotiation covers direct, fallback, and mix downgrade") {
  const auto path = sineFixture("output_format_negotiation.wav");

  const auto directResult = loadWith({{AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2}},
                                     config(AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2),
                                     path,
                                     "direct-success");
  CHECK(directResult.requests.size() == 1U);
  CHECK(eventsOf(directResult.events, BackendEventType::OutputModeFallback).empty());
  const auto& directFormat = std::get<OutputFormatChanged>(firstEventOf(directResult.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(directFormat.actualMode == AudioOutputMode::Direct);
  CHECK(directFormat.sampleRate == 48000U);
  CHECK_FALSE(directFormat.fallbackApplied);

  const auto fallbackResult = loadWith({{AudioOutputMode::Mixed, 48000, AudioSampleFormat::Float32, 2}},
                                       config(AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2),
                                       path,
                                       "direct-fallback");
  CHECK(fallbackResult.requests.size() == 2U);
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(fallbackResult.events, BackendEventType::OutputModeFallback).payload);
  CHECK(fallback.requestedConfig.outputMode == AudioOutputMode::Direct);
  CHECK(fallback.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveFormat.actualMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Float32);
  CHECK(fallback.effectiveFormat.fallbackApplied);
  CHECK(fallback.reason.find("direct") != std::string::npos);
  const auto& fallbackFormat = std::get<OutputFormatChanged>(firstEventOf(fallbackResult.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(fallbackFormat.actualMode == AudioOutputMode::Mixed);
  CHECK(fallbackFormat.fallbackApplied);

  const auto downgradeResult = loadWith({{AudioOutputMode::Mixed, 48000, AudioSampleFormat::Int16, 1}},
                                        config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2),
                                        path,
                                        "mix-downgrade");
  CHECK(downgradeResult.requests.size() == 2U);
  const auto& downgrade = std::get<OutputModeFallback>(firstEventOf(downgradeResult.events, BackendEventType::OutputModeFallback).payload);
  CHECK(downgrade.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(downgrade.effectiveFormat.sampleRate == 48000U);
  CHECK(downgrade.effectiveFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(downgrade.effectiveFormat.channelCount == 1U);
  CHECK(downgrade.reason.find("source format") != std::string::npos);
}

TEST_CASE("output_format_negotiation_failure emits playback error only after all candidates fail") {
  const auto path = sineFixture("output_format_negotiation_failure.wav");
  const auto result = loadWith({},
                               config(AudioOutputMode::Direct, 96000, AudioSampleFormat::Float32, 2),
                               path,
                               "total-failure");

  CHECK(result.requests.size() == 3U);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  CHECK(eventsOf(result.events, BackendEventType::OutputFormatChanged).empty());
  const auto& error = std::get<PlaybackError>(firstEventOf(result.events, BackendEventType::PlaybackError).payload);
  CHECK(error.code == PlaybackErrorCode::FormatNegotiationFailed);
  CHECK(error.message == "failed to negotiate an output format");
  CHECK(error.detail.find("direct") != std::string::npos);
  CHECK(error.detail.find("mixed") != std::string::npos);
}

}
