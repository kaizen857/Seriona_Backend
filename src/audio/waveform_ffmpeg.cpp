#include "waveform_ffmpeg.h"

#include "path_text.h"

extern "C" {
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace seriona::audio::detail {

namespace {

constexpr int kId3v1TagSize = 128;

std::string composeErrorText(const std::string& message, const std::string& detail) {
  if (detail.empty()) {
    return message;
  }
  return message + ": " + detail;
}

[[noreturn]] void throwWaveformFfmpegError(WaveformFfmpegErrorCode code,
                                          std::string message,
                                          std::string detail) {
  throw WaveformFfmpegError{code, std::move(message), std::move(detail)};
}

[[noreturn]] void throwWaveformFfmpegError(WaveformFfmpegErrorCode code, std::string message, int ffmpegCode) {
  throwWaveformFfmpegError(code, std::move(message), ffmpegErrorDetail(ffmpegCode));
}

[[nodiscard]] bool packetHasTrailingId3v1Tag(const AVPacket& packet) {
  if (packet.data == nullptr || packet.size < kId3v1TagSize) {
    return false;
  }

  const auto* tail = packet.data + packet.size - kId3v1TagSize;
  return tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G';
}

[[nodiscard]] bool streamIsMp3Like(const AVFormatContext& format, const AVStream& stream) {
  if (stream.codecpar != nullptr && stream.codecpar->codec_id == AV_CODEC_ID_MP3) {
    return true;
  }
  if (format.iformat == nullptr || format.iformat->name == nullptr) {
    return false;
  }

  const auto demuxerNames = std::string_view{format.iformat->name};
  return demuxerNames.find("mp3") != std::string_view::npos;
}

[[nodiscard]] bool packetEndsAtInputTail(const AVFormatContext& format, const AVPacket& packet) {
  if (format.pb == nullptr || packet.pos < 0 || packet.size < 0) {
    return false;
  }

  const auto inputSize = avio_size(format.pb);
  if (inputSize <= 0) {
    return false;
  }

  const auto packetSize = static_cast<std::int64_t>(packet.size);
  if (packet.pos > std::numeric_limits<std::int64_t>::max() - packetSize) {
    return false;
  }

  return packet.pos + packetSize == inputSize;
}

}

WaveformFfmpegError::WaveformFfmpegError(WaveformFfmpegErrorCode code, std::string message, std::string detail)
    : std::runtime_error(composeErrorText(message, detail)),
      code_(code),
      message_(std::move(message)),
      detail_(std::move(detail)) {}

WaveformFfmpegErrorCode WaveformFfmpegError::code() const noexcept { return code_; }

const std::string& WaveformFfmpegError::message() const noexcept { return message_; }

const std::string& WaveformFfmpegError::detail() const noexcept { return detail_; }

void WaveformFormatContextDeleter::operator()(AVFormatContext* context) const noexcept {
  if (context == nullptr) {
    return;
  }

  avformat_close_input(&context);
}

void WaveformCodecContextDeleter::operator()(AVCodecContext* context) const noexcept {
  avcodec_free_context(&context);
}

void WaveformPacketDeleter::operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }

void WaveformFrameDeleter::operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }

void WaveformCodecParametersDeleter::operator()(AVCodecParameters* parameters) const noexcept {
  avcodec_parameters_free(&parameters);
}

std::string ffmpegErrorDetail(int value) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  const int result = av_strerror(value, buffer.data(), buffer.size());
  if (result < 0) {
    return "unknown ffmpeg error " + std::to_string(value);
  }
  return buffer.data();
}

WaveformFormatContextPtr openWaveformInput(const std::filesystem::path& filepath) {
  if (!std::filesystem::exists(filepath)) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::OpenFailed, "audio file does not exist", pathToUtf8(filepath));
  }

  AVFormatContext* rawFormat = nullptr;
  const auto pathString = pathToUtf8(filepath);
  int result = avformat_open_input(&rawFormat, pathString.c_str(), nullptr, nullptr);
  if (result < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::UnsupportedFormat, "failed to open audio container", result);
  }

  WaveformFormatContextPtr format{rawFormat};
  result = avformat_find_stream_info(format.get(), nullptr);
  if (result < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::UnsupportedFormat, "failed to read stream information", result);
  }

  return format;
}

AVStream& findBestAudioStream(AVFormatContext& context) {
  const int bestStream = av_find_best_stream(&context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (bestStream < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::AudioStreamNotFound, "audio stream not found", bestStream);
  }

  const auto streamIndex = static_cast<unsigned int>(bestStream);
  if (streamIndex >= context.nb_streams || context.streams[streamIndex] == nullptr) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::AudioStreamNotFound,
                             "audio stream index is invalid",
                             "stream index " + std::to_string(bestStream));
  }

  AVStream* stream = context.streams[streamIndex];
  if (stream->codecpar == nullptr) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::AudioStreamNotFound,
                             "audio stream is missing codec parameters",
                             "stream index " + std::to_string(bestStream));
  }

  return *stream;
}

WaveformCodecContextPtr openDecoderForStream(const AVStream& stream) { return openDecoderForStream(stream, 0); }

WaveformCodecContextPtr openDecoderForStream(const AVStream& stream, int decoderThreadCount) {
  if (stream.codecpar == nullptr) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::DecoderNotFound,
                             "audio stream is missing codec parameters",
                             "stream index " + std::to_string(stream.index));
  }

  const AVCodec* decoder = avcodec_find_decoder(stream.codecpar->codec_id);
  if (decoder == nullptr) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::DecoderNotFound,
                             "audio decoder not found",
                             "codec id " + std::to_string(static_cast<int>(stream.codecpar->codec_id)));
  }

  WaveformCodecContextPtr codec{avcodec_alloc_context3(decoder)};
  if (!codec) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::AllocationFailed,
                             "failed to allocate audio decoder",
                             "avcodec_alloc_context3 returned null");
  }

  int result = avcodec_parameters_to_context(codec.get(), stream.codecpar);
  if (result < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::CodecConfigurationFailed,
                             "failed to configure audio decoder",
                             result);
  }

  codec->pkt_timebase = stream.time_base;
  if (decoderThreadCount > 0) {
    codec->thread_count = decoderThreadCount;
  }
  result = avcodec_open2(codec.get(), decoder, nullptr);
  if (result < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::CodecConfigurationFailed,
                             "failed to open audio decoder",
                             result);
  }

  return codec;
}

std::int64_t streamDurationUs(const AVFormatContext& format, const AVStream& stream) {
  if (stream.duration > 0) {
    return av_rescale_q(stream.duration, stream.time_base, AVRational{1, 1'000'000});
  }

  if (format.duration > 0) {
    return format.duration;
  }

  return 0;
}

int sampleRate(const AVCodecContext& context) { return std::max(0, context.sample_rate); }

int sampleRate(const AVCodecParameters& parameters) { return std::max(0, parameters.sample_rate); }

int channelCount(const AVCodecContext& context) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
  return std::max(0, context.ch_layout.nb_channels);
#else
  return std::max(0, context.channels);
#endif
}

int channelCount(const AVCodecParameters& parameters) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
  return std::max(0, parameters.ch_layout.nb_channels);
#else
  return std::max(0, parameters.channels);
#endif
}

AVSampleFormat sampleFormat(const AVCodecContext& context) { return context.sample_fmt; }

AVSampleFormat sampleFormat(const AVCodecParameters& parameters) {
  return static_cast<AVSampleFormat>(parameters.format);
}

AVRational timeBase(const AVStream& stream) {
  if (stream.time_base.den == 0) {
    return AVRational{0, 1};
  }
  return stream.time_base;
}

std::string formatName(const AVFormatContext& context) {
  if (context.iformat == nullptr || context.iformat->name == nullptr) {
    return {};
  }
  return context.iformat->name;
}

WaveformPacketPtr clonePacket(const AVPacket& packet) {
  WaveformPacketPtr cloned{av_packet_alloc()};
  if (!cloned) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::AllocationFailed,
                             "failed to allocate audio packet",
                             "av_packet_alloc returned null");
  }

  const int result = av_packet_ref(cloned.get(), &packet);
  if (result < 0) {
    throwWaveformFfmpegError(WaveformFfmpegErrorCode::PacketCloneFailed, "failed to clone audio packet", result);
  }

  return cloned;
}

bool stripTrailingId3v1TagIfPresent(AVPacket& packet, const AVFormatContext& format, const AVStream& stream) {
  if (packet.stream_index != stream.index) {
    return false;
  }
  if (!packetHasTrailingId3v1Tag(packet)) {
    return false;
  }
  if (!streamIsMp3Like(format, stream)) {
    return false;
  }
  if (!packetEndsAtInputTail(format, packet)) {
    return false;
  }

  packet.size -= kId3v1TagSize;
  return true;
}

}
