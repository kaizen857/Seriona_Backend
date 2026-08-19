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

  const auto directResult = loadWith({{AudioOutputMode::Direct, kSampleRate, AudioSampleFormat::Int16, kChannels}},
                                     config(AudioOutputMode::Direct, 48000, AudioSampleFormat::Float32, 2),
                                     path,
                                     "direct-success");
  CHECK(directResult.requests.size() == 1U);
  CHECK(eventsOf(directResult.events, BackendEventType::OutputModeFallback).empty());
  const auto& directFormat = std::get<OutputFormatChanged>(firstEventOf(directResult.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(directFormat.actualMode == AudioOutputMode::Direct);
  CHECK(directFormat.sampleRate == kSampleRate);
  CHECK(directFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(directFormat.channelCount == kChannels);
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
  CHECK(downgradeResult.requests.size() == 3U);
  const auto& downgrade = std::get<OutputModeFallback>(firstEventOf(downgradeResult.events, BackendEventType::OutputModeFallback).payload);
  CHECK(downgrade.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(downgrade.effectiveFormat.sampleRate == 48000U);
  CHECK(downgrade.effectiveFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(downgrade.effectiveFormat.channelCount == 1U);
  CHECK(downgrade.reason.find("source format") != std::string::npos);
}

TEST_CASE("output_format_negotiation direct mode initializes device with track parameters") {
  const auto path = sineFixture("output_format_negotiation_direct_track_params.wav");

  const auto result = loadWith({{AudioOutputMode::Direct, kSampleRate, AudioSampleFormat::Int16, kChannels},
                                {AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               config(AudioOutputMode::Direct, 96000, AudioSampleFormat::Float32, 2),
                               path,
                               "direct-track-params");
  CHECK(result.requests.size() == 1U);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  const auto& format = std::get<OutputFormatChanged>(firstEventOf(result.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(format.actualMode == AudioOutputMode::Direct);
  CHECK(format.sampleRate == kSampleRate);
  CHECK(format.sampleFormat == AudioSampleFormat::Int16);
  CHECK(format.channelCount == kChannels);
  CHECK_FALSE(format.fallbackApplied);
}

TEST_CASE("output_format_negotiation_failure emits playback error only after all candidates fail") {
  const auto path = sineFixture("output_format_negotiation_failure.wav");
  const auto result = loadWith({},
                               config(AudioOutputMode::Direct, 96000, AudioSampleFormat::Float32, 2),
                               path,
                               "total-failure");

  CHECK(result.requests.size() == 4U);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  CHECK(eventsOf(result.events, BackendEventType::OutputFormatChanged).empty());
  const auto& error = std::get<PlaybackError>(firstEventOf(result.events, BackendEventType::PlaybackError).payload);
  CHECK(error.code == PlaybackErrorCode::FormatNegotiationFailed);
  CHECK(error.message == "failed to negotiate an output format");
  CHECK(error.detail.find("direct") != std::string::npos);
  CHECK(error.detail.find("mixed") != std::string::npos);
}

TEST_CASE("format_fallback downgrades bit depth when device rejects requested format") {
  // 计划 happy 场景：指定 Int24，设备仅支持 Float32（同采样率/声道）→
  // 候选链 Int24 → Int16 → Float32 自动降级并通知，不报错。
  const auto path = sineFixture("format_fallback_int24_to_float32.wav");

  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int24, 2),
                               path,
                               "int24-to-float32");
  CHECK(result.requests.size() == 3U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int24);
  CHECK(result.requests[1].sampleFormat == AudioSampleFormat::Int16);
  CHECK(result.requests[2].sampleFormat == AudioSampleFormat::Float32);

  CHECK(eventsOf(result.events, BackendEventType::PlaybackError).empty());
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(result.events, BackendEventType::OutputModeFallback).payload);
  CHECK(fallback.requestedConfig.outputMode == AudioOutputMode::Mixed);
  CHECK(fallback.effectiveConfig.outputMode == AudioOutputMode::Mixed);
  CHECK_FALSE(fallback.reason.empty());
  CHECK(fallback.reason.find("24 位") != std::string::npos);
  CHECK(fallback.reason.find("32 位浮点") != std::string::npos);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Float32);
  CHECK(fallback.effectiveFormat.sampleRate == 96000U);
  CHECK(fallback.effectiveFormat.fallbackApplied);

  const auto& format = std::get<OutputFormatChanged>(firstEventOf(result.events, BackendEventType::OutputFormatChanged).payload).deviceFormat;
  CHECK(format.sampleFormat == AudioSampleFormat::Float32);
  CHECK(format.fallbackApplied);
}

TEST_CASE("format_fallback rejects unsupported sample format enum and moves to next candidate") {
  // 人为构造不支持枚举值（Unknown）：候选被拒后降级到 Int16 成功播放 + 通知。
  const auto path = sineFixture("format_fallback_unknown_enum.wav");

  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int16, 2}},
                               config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Unknown, 2),
                               path,
                               "unknown-enum");
  CHECK(result.requests.size() == 1U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int16);

  CHECK(eventsOf(result.events, BackendEventType::PlaybackError).empty());
  const auto& fallback = std::get<OutputModeFallback>(firstEventOf(result.events, BackendEventType::OutputModeFallback).payload);
  CHECK_FALSE(fallback.reason.empty());
  CHECK(fallback.reason.find("16 位整数") != std::string::npos);
  CHECK(fallback.effectiveFormat.sampleFormat == AudioSampleFormat::Int16);
  CHECK(fallback.effectiveFormat.fallbackApplied);
}

TEST_CASE("format_fallback respects allowFallback disabled") {
  // allowFallback=false：候选循环退化为单候选（用户指定格式），设备不支持即失败。
  const auto path = sineFixture("format_fallback_allow_fallback_off.wav");

  auto outputConfig = config(AudioOutputMode::Mixed, 96000, AudioSampleFormat::Int24, 2);
  outputConfig.allowFallback = false;
  const auto result = loadWith({{AudioOutputMode::Mixed, 96000, AudioSampleFormat::Float32, 2}},
                               outputConfig,
                               path,
                               "allow-fallback-off");
  CHECK(result.requests.size() == 1U);
  CHECK(result.requests[0].sampleFormat == AudioSampleFormat::Int24);
  CHECK(eventsOf(result.events, BackendEventType::OutputModeFallback).empty());
  CHECK(eventsOf(result.events, BackendEventType::OutputFormatChanged).empty());
  const auto& error = std::get<PlaybackError>(firstEventOf(result.events, BackendEventType::PlaybackError).payload);
  CHECK(error.code == PlaybackErrorCode::FormatNegotiationFailed);
}

}
