#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace seriona::audio::detail {

enum class WaveformFfmpegErrorCode {
  OpenFailed,
  UnsupportedFormat,
  AudioStreamNotFound,
  DecoderNotFound,
  AllocationFailed,
  CodecConfigurationFailed,
  PacketCloneFailed,
};

class WaveformFfmpegError final : public std::runtime_error {
public:
  WaveformFfmpegError(WaveformFfmpegErrorCode code, std::string message, std::string detail);

  [[nodiscard]] WaveformFfmpegErrorCode code() const noexcept;
  [[nodiscard]] const std::string& message() const noexcept;
  [[nodiscard]] const std::string& detail() const noexcept;

private:
  WaveformFfmpegErrorCode code_;
  std::string message_;
  std::string detail_;
};

struct WaveformFormatContextDeleter {
  void operator()(AVFormatContext* context) const noexcept;
};

struct WaveformCodecContextDeleter {
  void operator()(AVCodecContext* context) const noexcept;
};

struct WaveformPacketDeleter {
  void operator()(AVPacket* packet) const noexcept;
};

struct WaveformFrameDeleter {
  void operator()(AVFrame* frame) const noexcept;
};

struct WaveformCodecParametersDeleter {
  void operator()(AVCodecParameters* parameters) const noexcept;
};

using WaveformFormatContextPtr = std::unique_ptr<AVFormatContext, WaveformFormatContextDeleter>;
using WaveformCodecContextPtr = std::unique_ptr<AVCodecContext, WaveformCodecContextDeleter>;
using WaveformPacketPtr = std::unique_ptr<AVPacket, WaveformPacketDeleter>;
using WaveformFramePtr = std::unique_ptr<AVFrame, WaveformFrameDeleter>;
using WaveformCodecParametersPtr = std::unique_ptr<AVCodecParameters, WaveformCodecParametersDeleter>;

[[nodiscard]] std::string ffmpegErrorDetail(int value);
[[nodiscard]] WaveformFormatContextPtr openWaveformInput(const std::filesystem::path& filepath);
[[nodiscard]] AVStream& findBestAudioStream(AVFormatContext& context);
[[nodiscard]] WaveformCodecContextPtr openDecoderForStream(const AVStream& stream);
[[nodiscard]] WaveformCodecContextPtr openDecoderForStream(const AVStream& stream, int decoderThreadCount);
[[nodiscard]] std::int64_t streamDurationUs(const AVFormatContext& format, const AVStream& stream);
[[nodiscard]] int sampleRate(const AVCodecContext& context);
[[nodiscard]] int sampleRate(const AVCodecParameters& parameters);
[[nodiscard]] int channelCount(const AVCodecContext& context);
[[nodiscard]] int channelCount(const AVCodecParameters& parameters);
[[nodiscard]] AVSampleFormat sampleFormat(const AVCodecContext& context);
[[nodiscard]] AVSampleFormat sampleFormat(const AVCodecParameters& parameters);
[[nodiscard]] AVRational timeBase(const AVStream& stream);
[[nodiscard]] std::string formatName(const AVFormatContext& context);
[[nodiscard]] WaveformPacketPtr clonePacket(const AVPacket& packet);
[[nodiscard]] bool stripTrailingId3v1TagIfPresent(AVPacket& packet,
                                                  const AVFormatContext& format,
                                                  const AVStream& stream);

}
