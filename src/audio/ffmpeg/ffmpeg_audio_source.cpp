#include "seriona/audio/ffmpeg_audio_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

namespace seriona::audio {
namespace {

struct FormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    if (context == nullptr) {
      return;
    }

    avformat_close_input(&context);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const {
    av_packet_free(&packet);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const {
    av_frame_free(&frame);
  }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

std::string ffmpegErrorDetail(int value) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  const int result = av_strerror(value, buffer.data(), buffer.size());
  if (result < 0) {
    return "unknown ffmpeg error " + std::to_string(value);
  }
  return buffer.data();
}

FfmpegAudioSourceError makeError(PlaybackErrorCode code, std::string message, int ffmpegCode) {
  return FfmpegAudioSourceError{code, std::move(message), ffmpegErrorDetail(ffmpegCode)};
}

FfmpegAudioSourceError makeError(PlaybackErrorCode code, std::string message, std::string detail) {
  return FfmpegAudioSourceError{code, std::move(message), std::move(detail)};
}

AudioSampleFormat mapSampleFormat(AVSampleFormat format) {
  switch (format) {
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    return AudioSampleFormat::Unknown;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    return AudioSampleFormat::Int16;
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
    return AudioSampleFormat::Int32;
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    return AudioSampleFormat::Float32;
  default:
    return AudioSampleFormat::Unknown;
  }
}

std::uint16_t channelCount(const AVCodecContext& context) {
#if LIBAVCODEC_VERSION_MAJOR >= 59
  return static_cast<std::uint16_t>(std::clamp(context.ch_layout.nb_channels, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
#else
  return static_cast<std::uint16_t>(std::clamp(context.channels, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
#endif
}

std::chrono::microseconds streamDuration(const AVFormatContext& format, const AVStream& stream) {
  if (stream.duration > 0) {
    return std::chrono::microseconds{av_rescale_q(stream.duration, stream.time_base, AVRational{1, 1'000'000})};
  }

  if (format.duration > 0) {
    return std::chrono::microseconds{format.duration};
  }

  return std::chrono::microseconds{0};
}

std::chrono::microseconds framePosition(const AVFrame& frame, const AVStream& stream) {
  const auto timestamp = frame.best_effort_timestamp == AV_NOPTS_VALUE ? frame.pts : frame.best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) {
    return std::chrono::microseconds{0};
  }

  return std::chrono::microseconds{av_rescale_q(timestamp, stream.time_base, AVRational{1, 1'000'000})};
}

int frameChannelCount(const AVFrame& frame) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
  return std::max(0, frame.ch_layout.nb_channels);
#else
  return std::max(0, frame.channels);
#endif
}

std::vector<std::uint8_t> copyFrameBytes(const AVFrame& frame) {
  const auto sampleCount = std::max(0, frame.nb_samples);
  const auto format = static_cast<AVSampleFormat>(frame.format);
  const auto channels = frameChannelCount(frame);
  const auto bytesPerSample = std::max(0, av_get_bytes_per_sample(format));
  const bool planar = av_sample_fmt_is_planar(format) != 0;
  const auto expectedBytes = sampleCount * channels * bytesPerSample;

  std::vector<std::uint8_t> bytes;
  if (expectedBytes <= 0) {
    return bytes;
  }

  bytes.reserve(static_cast<std::size_t>(expectedBytes));

  if (!planar) {
    if (frame.extended_data[0] == nullptr) {
      return bytes;
    }
    const auto* begin = frame.extended_data[0];
    bytes.insert(bytes.end(), begin, begin + expectedBytes);
    return bytes;
  }

  for (int sample = 0; sample < sampleCount; ++sample) {
    for (int channel = 0; channel < channels; ++channel) {
      if (frame.extended_data[channel] == nullptr) {
        return {};
      }
      const auto* begin = frame.extended_data[channel] + (sample * bytesPerSample);
      bytes.insert(bytes.end(), begin, begin + bytesPerSample);
    }
  }

  return bytes;
}

}

class FfmpegAudioSource::Impl {
public:
  std::optional<FfmpegAudioSourceError> open(const std::filesystem::path& path) {
    reset();

    if (!std::filesystem::exists(path)) {
      return makeError(PlaybackErrorCode::OpenFailed, "audio file does not exist", path.string());
    }

    AVFormatContext* rawFormat = nullptr;
    const auto pathString = path.string();
    int result = avformat_open_input(&rawFormat, pathString.c_str(), nullptr, nullptr);
    if (result < 0) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to open audio container", result);
    }
    format_.reset(rawFormat);

    result = avformat_find_stream_info(format_.get(), nullptr);
    if (result < 0) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to read stream information", result);
    }

    const int bestStream = av_find_best_stream(format_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (bestStream < 0) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "audio stream not found", bestStream);
    }
    audioStreamIndex_ = bestStream;

    const auto* stream = format_->streams[audioStreamIndex_];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "audio decoder not found", "codec id " + std::to_string(stream->codecpar->codec_id));
    }

    codec_.reset(avcodec_alloc_context3(decoder));
    if (!codec_) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to allocate audio decoder", "avcodec_alloc_context3 returned null");
    }

    result = avcodec_parameters_to_context(codec_.get(), stream->codecpar);
    if (result < 0) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to configure audio decoder", result);
    }

    result = avcodec_open2(codec_.get(), decoder, nullptr);
    if (result < 0) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to open audio decoder", result);
    }

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (!packet_ || !frame_) {
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to allocate decode buffers", "av_packet_alloc or av_frame_alloc returned null");
    }

    info_.sampleRate = static_cast<std::uint32_t>(std::max(0, codec_->sample_rate));
    info_.channelCount = channelCount(*codec_);
    info_.sampleFormat = mapSampleFormat(codec_->sample_fmt);
    info_.duration = streamDuration(*format_, *stream);
    return std::nullopt;
  }

  FfmpegAudioReadResult readFrame() {
    if (!format_ || !codec_ || !packet_ || !frame_ || audioStreamIndex_ < 0) {
      return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::OpenFailed, "audio source is not open", "call open before readFrame")};
    }

    while (true) {
      if (auto result = receiveFrame()) {
        return *result;
      }

      if (draining_) {
        return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
      }

      const int readResult = av_read_frame(format_.get(), packet_.get());
      if (readResult == AVERROR_EOF) {
        draining_ = true;
        const int sendResult = avcodec_send_packet(codec_.get(), nullptr);
        if (sendResult < 0 && sendResult != AVERROR_EOF) {
          return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to drain audio decoder", sendResult)};
        }
        continue;
      }

      if (readResult < 0) {
        return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to read audio packet", readResult)};
      }

      if (packet_->stream_index != audioStreamIndex_) {
        av_packet_unref(packet_.get());
        continue;
      }

      const int sendResult = avcodec_send_packet(codec_.get(), packet_.get());
      av_packet_unref(packet_.get());
      if (sendResult == AVERROR(EAGAIN)) {
        continue;
      }
      if (sendResult < 0) {
        return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to send audio packet", sendResult)};
      }
    }
  }

  std::optional<FfmpegAudioSourceError> seek(std::chrono::milliseconds position) {
    if (!format_ || !codec_ || audioStreamIndex_ < 0) {
      return makeError(PlaybackErrorCode::OpenFailed, "audio source is not open", "call open before seek");
    }

    const auto* stream = format_->streams[audioStreamIndex_];
    const auto timestamp = av_rescale_q(position.count(), AVRational{1, 1'000}, stream->time_base);
    const int result = av_seek_frame(format_.get(), audioStreamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
      return makeError(PlaybackErrorCode::SeekFailed, "failed to seek audio stream", result);
    }

    if (packet_) {
      av_packet_unref(packet_.get());
    }
    if (frame_) {
      av_frame_unref(frame_.get());
    }
    avcodec_flush_buffers(codec_.get());
    draining_ = false;
    return std::nullopt;
  }

  const FfmpegAudioStreamInfo& streamInfo() const { return info_; }

private:
  std::optional<FfmpegAudioReadResult> receiveFrame() {
    const int result = avcodec_receive_frame(codec_.get(), frame_.get());
    if (result == AVERROR(EAGAIN)) {
      return std::nullopt;
    }
    if (result == AVERROR_EOF) {
      return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
    }
    if (result < 0) {
      return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to receive audio frame", result)};
    }

    const auto* stream = format_->streams[audioStreamIndex_];
    FfmpegAudioFrame decoded{};
    decoded.sampleRate = static_cast<std::uint32_t>(std::max(0, frame_->sample_rate));
    decoded.channelCount = static_cast<std::uint16_t>(std::clamp(frameChannelCount(*frame_), 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
    decoded.sampleFormat = mapSampleFormat(static_cast<AVSampleFormat>(frame_->format));
    decoded.pts = frame_->best_effort_timestamp == AV_NOPTS_VALUE ? frame_->pts : frame_->best_effort_timestamp;
    decoded.position = framePosition(*frame_, *stream);
    decoded.frameCount = static_cast<std::uint32_t>(std::max(0, frame_->nb_samples));
    decoded.sampleBytes = copyFrameBytes(*frame_);
    av_frame_unref(frame_.get());
    return FfmpegAudioReadResult{std::move(decoded), false, std::nullopt};
  }

  void reset() {
    packet_.reset();
    frame_.reset();
    codec_.reset();
    format_.reset();
    info_ = {};
    audioStreamIndex_ = -1;
    draining_ = false;
  }

  FormatContextPtr format_{};
  CodecContextPtr codec_{};
  PacketPtr packet_{};
  FramePtr frame_{};
  FfmpegAudioStreamInfo info_{};
  int audioStreamIndex_{-1};
  bool draining_{false};
};

FfmpegAudioSource::FfmpegAudioSource() : impl_(std::make_unique<Impl>()) {}

FfmpegAudioSource::~FfmpegAudioSource() = default;

FfmpegAudioSource::FfmpegAudioSource(FfmpegAudioSource&&) noexcept = default;

FfmpegAudioSource& FfmpegAudioSource::operator=(FfmpegAudioSource&&) noexcept = default;

std::optional<FfmpegAudioSourceError> FfmpegAudioSource::open(const std::filesystem::path& path) {
  return impl_->open(path);
}

FfmpegAudioReadResult FfmpegAudioSource::readFrame() { return impl_->readFrame(); }

std::optional<FfmpegAudioSourceError> FfmpegAudioSource::seek(std::chrono::milliseconds position) {
  return impl_->seek(position);
}

const FfmpegAudioStreamInfo& FfmpegAudioSource::streamInfo() const { return impl_->streamInfo(); }

}
