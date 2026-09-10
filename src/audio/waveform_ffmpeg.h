#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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

// 无效包被跳过时的告警出口（定义在 waveform_ffmpeg.cpp，经编译版 spdlog 输出；
// 头文件保持不依赖 spdlog，避免白盒测试 TU 以 header-only 模式实例化 spdlog 符号）。
void logWaveformInvalidPacketSkipped(std::string_view context, int ffmpegCode);

// 容错发送数据包（与播放链路 ffmpeg_audio_source 的恢复策略一致）：
// - AVERROR(EAGAIN)：先经 drainFrames 排空已解码帧，再重试一次发送；
// - AVERROR_INVALIDDATA：跳过该包并告警（返回 true，继续后续包），不中止
//   整次波形构建——畸形 MP3 的尾部垃圾包（解码器报 "Header missing"）不应
//   让整首歌失去波形；
// - 其它负值：抛出 "failed to send <context> packet: <detail>"。
// drainFrames 返回 false 表示调用方样本窗口已完成（无需更多帧）。
template <typename DrainFrames>
[[nodiscard]] bool sendWaveformPacket(AVCodecContext& decoder,
                                      const AVPacket& packet,
                                      DrainFrames&& drainFrames,
                                      std::string_view context) {
  int sendResult = avcodec_send_packet(&decoder, &packet);
  if (sendResult == AVERROR(EAGAIN)) {
    if (!drainFrames()) {
      return false;
    }
    sendResult = avcodec_send_packet(&decoder, &packet);
  }
  if (sendResult == AVERROR_INVALIDDATA) {
    logWaveformInvalidPacketSkipped(context, sendResult);
    return true;
  }
  if (sendResult < 0) {
    throw std::runtime_error{"failed to send " + std::string{context} + " packet: " + ffmpegErrorDetail(sendResult)};
  }
  return drainFrames();
}

}
