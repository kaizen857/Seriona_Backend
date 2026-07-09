#include "seriona/audio/ffmpeg_audio_source.h"

#include "spdlog/spdlog.h"

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
#include <string_view>
#include <utility>

namespace seriona::audio {
namespace {

constexpr int kId3v1TagSize = 128;

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

bool packetHasTrailingId3v1Tag(const AVPacket& packet) {
  if (packet.data == nullptr || packet.size < kId3v1TagSize) {
    return false;
  }

  const auto* tail = packet.data + packet.size - kId3v1TagSize;
  return tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G';
}

bool isPrintableOrPaddingByte(const std::uint8_t value) {
  return value == 0U || (value >= 0x20U && value <= 0x7EU);
}

bool streamIsMp3Like(const AVFormatContext& format, const AVStream& stream) {
  if (stream.codecpar != nullptr && stream.codecpar->codec_id == AV_CODEC_ID_MP3) {
    return true;
  }
  if (format.iformat == nullptr || format.iformat->name == nullptr) {
    return false;
  }

  const auto demuxerNames = std::string_view{format.iformat->name};
  return demuxerNames.find("mp3") != std::string_view::npos;
}

bool packetEndsAtInputTail(const AVFormatContext& format, const AVPacket& packet) {
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

bool looksLikeMp3FrameHeader(const std::uint8_t* bytes, int size) {
  if (bytes == nullptr || size < 4) {
    return false;
  }

  if (bytes[0] != 0xFFU || (bytes[1] & 0xE0U) != 0xE0U) {
    return false;
  }

  const auto versionId = static_cast<std::uint8_t>((bytes[1] >> 3U) & 0x03U);
  const auto layer = static_cast<std::uint8_t>((bytes[1] >> 1U) & 0x03U);
  const auto bitrateIndex = static_cast<std::uint8_t>((bytes[2] >> 4U) & 0x0FU);
  const auto sampleRateIndex = static_cast<std::uint8_t>((bytes[2] >> 2U) & 0x03U);

  return versionId != 0x01U && layer != 0x00U && bitrateIndex != 0x00U && bitrateIndex != 0x0FU &&
         sampleRateIndex != 0x03U;
}

bool looksLikeId3v1Block(const std::uint8_t* bytes, int size) {
  if (bytes == nullptr || size < kId3v1TagSize || bytes[0] != 'T' || bytes[1] != 'A' || bytes[2] != 'G') {
    return false;
  }

  int printableOrPaddingCount = 0;
  int zeroCount = 0;
  for (int index = 3; index < kId3v1TagSize; ++index) {
    printableOrPaddingCount += isPrintableOrPaddingByte(bytes[index]) ? 1 : 0;
    zeroCount += bytes[index] == 0U ? 1 : 0;
  }

  return zeroCount >= 32 || printableOrPaddingCount >= 72;
}

std::optional<std::size_t> findNextMp3FrameHeader(const AVPacket& packet, std::size_t startOffset) {
  if (packet.data == nullptr || packet.size < 4 || startOffset >= static_cast<std::size_t>(packet.size)) {
    return std::nullopt;
  }

  for (std::size_t offset = startOffset; offset + 4U <= static_cast<std::size_t>(packet.size); ++offset) {
    if (looksLikeMp3FrameHeader(packet.data + offset,
                                packet.size - static_cast<int>(offset))) {
      return offset;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> findPreviousMp3FrameHeader(const AVPacket& packet, std::size_t endOffset) {
  if (packet.data == nullptr || packet.size < 4 || endOffset < 4U) {
    return std::nullopt;
  }

  auto offset = std::min(endOffset, static_cast<std::size_t>(packet.size));
  while (offset >= 4U) {
    --offset;
    if (looksLikeMp3FrameHeader(packet.data + offset,
                                packet.size - static_cast<int>(offset))) {
      return offset;
    }
    if (offset == 0U) {
      break;
    }
  }

  return std::nullopt;
}

bool shouldRemoveId3v1Block(const AVPacket& packet,
                            const AVFormatContext& format,
                            const AVStream& stream,
                            std::size_t tagOffset) {
  if (packet.stream_index != stream.index || !streamIsMp3Like(format, stream) ||
      tagOffset + kId3v1TagSize > static_cast<std::size_t>(packet.size) ||
      !looksLikeId3v1Block(packet.data + tagOffset, packet.size - static_cast<int>(tagOffset))) {
    return false;
  }

  const bool atPacketStart = tagOffset == 0U;
  const bool atPacketTail = tagOffset + kId3v1TagSize == static_cast<std::size_t>(packet.size);
  const auto nextHeader = findNextMp3FrameHeader(packet, tagOffset + kId3v1TagSize);
  const auto previousHeader = findPreviousMp3FrameHeader(packet, tagOffset);

  if (atPacketStart && atPacketTail) {
    return true;
  }
  if (atPacketStart && nextHeader.has_value()) {
    return true;
  }
  if (atPacketTail && (previousHeader.has_value() || packetEndsAtInputTail(format, packet) || packetHasTrailingId3v1Tag(packet))) {
    return true;
  }

  return previousHeader.has_value() || nextHeader.has_value();
}

struct SanitizedPacketResult {
  PacketPtr packet{};
  int removedBlocks{0};
};

std::optional<SanitizedPacketResult> sanitizeMp3PacketId3v1Blocks(const AVPacket& packet,
                                                                   const AVFormatContext& format,
                                                                   const AVStream& stream) {
  if (packet.stream_index != stream.index || !streamIsMp3Like(format, stream) || packet.data == nullptr ||
      packet.size < kId3v1TagSize) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> sanitizedBytes;
  sanitizedBytes.reserve(static_cast<std::size_t>(packet.size));
  std::size_t copyStart = 0U;
  int removedBlocks = 0;

  for (std::size_t offset = 0U; offset + kId3v1TagSize <= static_cast<std::size_t>(packet.size); ++offset) {
    if (packet.data[offset] != 'T' || packet.data[offset + 1U] != 'A' || packet.data[offset + 2U] != 'G') {
      continue;
    }
    if (!shouldRemoveId3v1Block(packet, format, stream, offset)) {
      continue;
    }

    sanitizedBytes.insert(sanitizedBytes.end(),
                          packet.data + copyStart,
                          packet.data + offset);
    copyStart = offset + kId3v1TagSize;
    offset = copyStart == 0U ? 0U : copyStart - 1U;
    ++removedBlocks;
  }

  if (removedBlocks == 0) {
    return std::nullopt;
  }

  sanitizedBytes.insert(sanitizedBytes.end(),
                        packet.data + copyStart,
                        packet.data + packet.size);

  auto sanitizedPacket = PacketPtr{av_packet_alloc()};
  if (!sanitizedPacket) {
    return SanitizedPacketResult{PacketPtr{}, 0};
  }
  if (!sanitizedBytes.empty()) {
    if (av_new_packet(sanitizedPacket.get(), static_cast<int>(sanitizedBytes.size())) < 0) {
      return SanitizedPacketResult{PacketPtr{}, 0};
    }
    std::memcpy(sanitizedPacket->data, sanitizedBytes.data(), sanitizedBytes.size());
  }
  if (av_packet_copy_props(sanitizedPacket.get(), &packet) < 0) {
    return SanitizedPacketResult{PacketPtr{}, 0};
  }
  sanitizedPacket->stream_index = packet.stream_index;
  sanitizedPacket->pos = packet.pos;
  sanitizedPacket->duration = packet.duration;

  return SanitizedPacketResult{std::move(sanitizedPacket), removedBlocks};
}

}

class FfmpegAudioSource::Impl {
public:
  std::optional<FfmpegAudioSourceError> open(const std::filesystem::path& path) {
    reset();
    spdlog::debug("ffmpeg source opening '{}'", path.string());

    if (!std::filesystem::exists(path)) {
      spdlog::error("ffmpeg source open failed: file not found '{}'", path.string());
      return makeError(PlaybackErrorCode::OpenFailed, "audio file does not exist", path.string());
    }

    AVFormatContext* rawFormat = nullptr;
    const auto pathString = path.string();
    int result = avformat_open_input(&rawFormat, pathString.c_str(), nullptr, nullptr);
    if (result < 0) {
      spdlog::error("ffmpeg source open failed: avformat_open_input returned {} ({})",
                    result, ffmpegErrorDetail(result));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to open audio container", result);
    }
    format_.reset(rawFormat);

    result = avformat_find_stream_info(format_.get(), nullptr);
    if (result < 0) {
      spdlog::error("ffmpeg source open failed: avformat_find_stream_info returned {} ({})",
                    result, ffmpegErrorDetail(result));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to read stream information", result);
    }

    const int bestStream = av_find_best_stream(format_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (bestStream < 0) {
      spdlog::error("ffmpeg source open failed: no audio stream found ({})",
                    ffmpegErrorDetail(bestStream));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "audio stream not found", bestStream);
    }
    audioStreamIndex_ = bestStream;

    const auto* stream = format_->streams[audioStreamIndex_];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
      spdlog::error("ffmpeg source open failed: no decoder for codec id {}",
                    static_cast<int>(stream->codecpar->codec_id));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "audio decoder not found",
                       "codec id " + std::to_string(stream->codecpar->codec_id));
    }

    codec_.reset(avcodec_alloc_context3(decoder));
    if (!codec_) {
      spdlog::error("ffmpeg source open failed: avcodec_alloc_context3 returned null");
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to allocate audio decoder",
                       "avcodec_alloc_context3 returned null");
    }

    result = avcodec_parameters_to_context(codec_.get(), stream->codecpar);
    if (result < 0) {
      spdlog::error("ffmpeg source open failed: avcodec_parameters_to_context returned {} ({})",
                    result, ffmpegErrorDetail(result));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to configure audio decoder", result);
    }

    result = avcodec_open2(codec_.get(), decoder, nullptr);
    if (result < 0) {
      spdlog::error("ffmpeg source open failed: avcodec_open2 returned {} ({})",
                    result, ffmpegErrorDetail(result));
      return makeError(PlaybackErrorCode::UnsupportedFormat, "failed to open audio decoder", result);
    }

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (!packet_ || !frame_) {
      spdlog::error("ffmpeg source open failed: failed to allocate decode buffers");
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to allocate decode buffers",
                       "av_packet_alloc or av_frame_alloc returned null");
    }

    info_.sampleRate = static_cast<std::uint32_t>(std::max(0, codec_->sample_rate));
    info_.channelCount = channelCount(*codec_);
    info_.sampleFormat = mapSampleFormat(codec_->sample_fmt);
    info_.duration = streamDuration(*format_, *stream);
    spdlog::info("ffmpeg source opened '{}': {}Hz {}ch fmt={} dur={}ms", path.string(),
                 info_.sampleRate, info_.channelCount,
                 static_cast<int>(info_.sampleFormat),
                 std::chrono::duration_cast<std::chrono::milliseconds>(info_.duration).count());
    return std::nullopt;
  }

  FfmpegAudioReadResult readFrame() {
    if (!format_ || !codec_ || !packet_ || !frame_ || audioStreamIndex_ < 0) {
      return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::OpenFailed, "audio source is not open", "call open before readFrame")};
    }
    if (forcedEndOfStream_) {
      return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
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
        spdlog::debug("ffmpeg source reached EOF, draining decoder");
        draining_ = true;
        const int sendResult = avcodec_send_packet(codec_.get(), nullptr);
        if (sendResult < 0 && sendResult != AVERROR_EOF) {
          spdlog::error("ffmpeg source drain failed: avcodec_send_packet returned {} ({})",
                        sendResult, ffmpegErrorDetail(sendResult));
          return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to drain audio decoder", sendResult)};
        }
        continue;
      }

      if (readResult < 0) {
        spdlog::error("ffmpeg source read failed: av_read_frame returned {} ({})",
                      readResult, ffmpegErrorDetail(readResult));
        return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to read audio packet", readResult)};
      }

      if (packet_->stream_index != audioStreamIndex_) {
        av_packet_unref(packet_.get());
        continue;
      }

      const auto* stream = format_->streams[audioStreamIndex_];
      const AVPacket* packetToSend = packet_.get();
      PacketPtr sanitizedPacket{};
      if (auto sanitized = sanitizeMp3PacketId3v1Blocks(*packet_, *format_, *stream)) {
        if (!sanitized->packet) {
          spdlog::error("ffmpeg source failed to allocate sanitized mp3 packet");
          av_packet_unref(packet_.get());
          return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to sanitize mp3 packet", "av_packet_alloc/av_new_packet failed")};
        }
        spdlog::debug("ffmpeg source removed {} embedded ID3v1 block(s) from mp3 packet",
                      sanitized->removedBlocks);
        if (sanitized->packet->size == 0) {
          av_packet_unref(packet_.get());
          continue;
        }
        packetToSend = sanitized->packet.get();
        sanitizedPacket = std::move(sanitized->packet);
      }

      const int sendResult = avcodec_send_packet(codec_.get(), packetToSend);
      av_packet_unref(packet_.get());
      if (sendResult == AVERROR(EAGAIN)) {
        continue;
      }
      if (sendResult == AVERROR_INVALIDDATA) {
        if (!decodedFrameSeen_) {
          spdlog::error("ffmpeg source send failed: avcodec_send_packet returned {} ({})",
                        sendResult, ffmpegErrorDetail(sendResult));
          return FfmpegAudioReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to send audio packet", sendResult)};
        }
        invalidPacketSkipped_ = true;
        spdlog::warn("ffmpeg source skipped invalid audio packet: {}", ffmpegErrorDetail(sendResult));
        continue;
      }
      if (sendResult < 0) {
        spdlog::error("ffmpeg source send failed: avcodec_send_packet returned {} ({})",
                      sendResult, ffmpegErrorDetail(sendResult));
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
      spdlog::error("ffmpeg source seek to {}ms failed: av_seek_frame returned {} ({})",
                    position.count(), result, ffmpegErrorDetail(result));
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
    decodedFrameSeen_ = false;
    invalidPacketSkipped_ = false;
    forcedEndOfStream_ = false;
    return std::nullopt;
  }

  const FfmpegAudioStreamInfo& streamInfo() const { return info_; }

private:
  bool sourceIsMp3Like() const {
    return format_ && audioStreamIndex_ >= 0 && format_->streams[audioStreamIndex_] != nullptr &&
           streamIsMp3Like(*format_, *format_->streams[audioStreamIndex_]);
  }

  std::optional<FfmpegAudioReadResult> receiveFrame() {
    const int result = avcodec_receive_frame(codec_.get(), frame_.get());
    if (result == AVERROR(EAGAIN)) {
      return std::nullopt;
    }
    if (result == AVERROR_EOF) {
      return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
    }
    if (result == AVERROR_INVALIDDATA && decodedFrameSeen_) {
      av_frame_unref(frame_.get());
      if (draining_) {
        spdlog::warn("ffmpeg source ignored trailing invalid decoder data while draining: {}",
                     ffmpegErrorDetail(result));
        return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
      }

      spdlog::warn("ffmpeg source skipped invalid decoded audio frame: {}",
                   ffmpegErrorDetail(result));
      return std::nullopt;
    }
    if (result < 0) {
      spdlog::error("ffmpeg source decode failed: avcodec_receive_frame returned {} ({})",
                    result, ffmpegErrorDetail(result));
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

    if (sourceIsMp3Like() && seriona::audio::detail::shouldTerminateCorruptedMp3Tail(info_, decoded, invalidPacketSkipped_)) {
      spdlog::warn("ffmpeg source truncated corrupted mp3 tail after invalid packet: pos={}ms duration={}ms rate={}Hz expected={}Hz ch={} expected={} fmt={} expected={}",
                   std::chrono::duration_cast<std::chrono::milliseconds>(decoded.position).count(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(info_.duration).count(),
                   decoded.sampleRate,
                   info_.sampleRate,
                   decoded.channelCount,
                   info_.channelCount,
                   static_cast<int>(decoded.sampleFormat),
                   static_cast<int>(info_.sampleFormat));
      av_frame_unref(frame_.get());
      forcedEndOfStream_ = true;
      return FfmpegAudioReadResult{std::nullopt, true, std::nullopt};
    }

    av_frame_unref(frame_.get());
    decodedFrameSeen_ = true;
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
    decodedFrameSeen_ = false;
    invalidPacketSkipped_ = false;
    forcedEndOfStream_ = false;
  }

  FormatContextPtr format_{};
  CodecContextPtr codec_{};
  PacketPtr packet_{};
  FramePtr frame_{};
  FfmpegAudioStreamInfo info_{};
  int audioStreamIndex_{-1};
  bool draining_{false};
  bool decodedFrameSeen_{false};
  bool invalidPacketSkipped_{false};
  bool forcedEndOfStream_{false};
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
