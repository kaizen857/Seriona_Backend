#pragma once

#include "seriona/audio/audio_contracts.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::audio {

struct FfmpegAudioSourceError {
  PlaybackErrorCode code{PlaybackErrorCode::OpenFailed};
  std::string message;
  std::string detail;
};

struct FfmpegAudioStreamInfo {
  std::uint32_t sampleRate{0};
  std::uint16_t channelCount{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::chrono::microseconds duration{0};
};

struct FfmpegAudioFrame {
  std::uint32_t sampleRate{0};
  std::uint16_t channelCount{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::int64_t pts{0};
  std::chrono::microseconds position{0};
  std::uint32_t frameCount{0};
  std::vector<std::uint8_t> sampleBytes;
};

struct FfmpegAudioReadResult {
  std::optional<FfmpegAudioFrame> frame;
  bool endOfStream{false};
  std::optional<FfmpegAudioSourceError> error;
};

namespace detail {

inline std::chrono::microseconds mp3TailFrameStartTolerance(std::uint32_t sampleRate) {
  if (sampleRate == 0U) {
    return std::chrono::microseconds{0};
  }

  constexpr std::uint64_t kMaxMp3FrameSamples = 1152U;
  return std::chrono::microseconds{static_cast<std::int64_t>((kMaxMp3FrameSamples * 1'000'000ULL) / sampleRate)};
}

inline bool shouldTerminateCorruptedMp3Tail(const FfmpegAudioStreamInfo& streamInfo,
                                            const FfmpegAudioFrame& frame,
                                            bool invalidPacketSkipped) {
  if (!invalidPacketSkipped || streamInfo.duration <= std::chrono::microseconds{0} || streamInfo.sampleRate == 0U ||
      frame.sampleRate == 0U || frame.channelCount == 0U || frame.frameCount == 0U) {
    return false;
  }

  const bool signatureMismatch = frame.sampleRate != streamInfo.sampleRate ||
                                 frame.channelCount != streamInfo.channelCount ||
                                 frame.sampleFormat != streamInfo.sampleFormat;
  const auto latestExpectedStart = streamInfo.duration + mp3TailFrameStartTolerance(streamInfo.sampleRate);
  const bool startsTooFarPastDuration = frame.position > latestExpectedStart;
  return signatureMismatch || startsTooFarPastDuration;
}

}

class FfmpegAudioSource {
public:
  FfmpegAudioSource();
  ~FfmpegAudioSource();

  FfmpegAudioSource(const FfmpegAudioSource&) = delete;
  FfmpegAudioSource& operator=(const FfmpegAudioSource&) = delete;
  FfmpegAudioSource(FfmpegAudioSource&&) noexcept;
  FfmpegAudioSource& operator=(FfmpegAudioSource&&) noexcept;

  [[nodiscard]] std::optional<FfmpegAudioSourceError> open(const std::filesystem::path& path);
  [[nodiscard]] FfmpegAudioReadResult readFrame();
  [[nodiscard]] std::optional<FfmpegAudioSourceError> seek(std::chrono::milliseconds position);
  [[nodiscard]] const FfmpegAudioStreamInfo& streamInfo() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}
