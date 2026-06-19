#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/ffmpeg_audio_source.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace seriona::audio {

struct FfmpegFilterTargetFormat {
  std::uint32_t sampleRate{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
  std::uint16_t channelCount{0};
};

struct FfmpegFilterPipelineError {
  PlaybackErrorCode code{PlaybackErrorCode::FormatNegotiationFailed};
  std::string message;
  std::string detail;
};

struct FfmpegFilterReadResult {
  std::optional<FfmpegAudioFrame> frame;
  bool endOfStream{false};
  std::optional<FfmpegFilterPipelineError> error;
};

class FfmpegFilterPipeline {
public:
  FfmpegFilterPipeline();
  ~FfmpegFilterPipeline();

  FfmpegFilterPipeline(const FfmpegFilterPipeline&) = delete;
  FfmpegFilterPipeline& operator=(const FfmpegFilterPipeline&) = delete;
  FfmpegFilterPipeline(FfmpegFilterPipeline&&) noexcept;
  FfmpegFilterPipeline& operator=(FfmpegFilterPipeline&&) noexcept;

  [[nodiscard]] std::optional<FfmpegFilterPipelineError> configure(FfmpegFilterTargetFormat target);
  [[nodiscard]] std::optional<FfmpegFilterPipelineError> pushFrame(const FfmpegAudioFrame& frame);
  [[nodiscard]] std::optional<FfmpegFilterPipelineError> signalEndOfInput();
  [[nodiscard]] FfmpegFilterReadResult readFrame();
  void reset();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}
