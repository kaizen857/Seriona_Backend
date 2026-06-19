#include "seriona/audio/ffmpeg_filter_pipeline.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace {

struct FilterGraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct ChannelLayoutHolder {
  AVChannelLayout layout{};

  explicit ChannelLayoutHolder(std::uint16_t channelCount) { av_channel_layout_default(&layout, channelCount); }
  ~ChannelLayoutHolder() { av_channel_layout_uninit(&layout); }

  ChannelLayoutHolder(const ChannelLayoutHolder&) = delete;
  ChannelLayoutHolder& operator=(const ChannelLayoutHolder&) = delete;
};

using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct FrameSignature {
  std::uint32_t sampleRate{0};
  std::uint16_t channelCount{0};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Unknown};
};

bool operator==(const FrameSignature& left, const FrameSignature& right) {
  return left.sampleRate == right.sampleRate && left.channelCount == right.channelCount && left.sampleFormat == right.sampleFormat;
}

std::string ffmpegErrorDetail(int value) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  const int result = av_strerror(value, buffer.data(), buffer.size());
  if (result < 0) {
    return "unknown ffmpeg error " + std::to_string(value);
  }
  return buffer.data();
}

FfmpegFilterPipelineError makeError(PlaybackErrorCode code, std::string message, int ffmpegCode) {
  return FfmpegFilterPipelineError{code, std::move(message), ffmpegErrorDetail(ffmpegCode)};
}

FfmpegFilterPipelineError makeError(PlaybackErrorCode code, std::string message, std::string detail) {
  return FfmpegFilterPipelineError{code, std::move(message), std::move(detail)};
}

AVSampleFormat toPackedAvSampleFormat(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return AV_SAMPLE_FMT_S16;
  case AudioSampleFormat::Int32:
    return AV_SAMPLE_FMT_S32;
  case AudioSampleFormat::Float32:
    return AV_SAMPLE_FMT_FLT;
  case AudioSampleFormat::Int24:
  case AudioSampleFormat::Unknown:
    return AV_SAMPLE_FMT_NONE;
  }

  return AV_SAMPLE_FMT_NONE;
}

AudioSampleFormat fromAvSampleFormat(AVSampleFormat format) {
  switch (format) {
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

int bytesPerSample(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2;
  case AudioSampleFormat::Int24:
    return 3;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4;
  case AudioSampleFormat::Unknown:
    return 0;
  }

  return 0;
}

FrameSignature signatureOf(const FfmpegAudioFrame& frame) {
  return FrameSignature{frame.sampleRate, frame.channelCount, frame.sampleFormat};
}

std::optional<FfmpegFilterPipelineError> validateTarget(const FfmpegFilterTargetFormat& target) {
  if (target.sampleRate == 0) {
    return makeError(PlaybackErrorCode::FormatNegotiationFailed, "target sample rate is invalid", "sample rate must be greater than zero");
  }
  if (target.channelCount == 0) {
    return makeError(PlaybackErrorCode::FormatNegotiationFailed, "target channel count is invalid", "channel count must be greater than zero");
  }
  if (toPackedAvSampleFormat(target.sampleFormat) == AV_SAMPLE_FMT_NONE) {
    return makeError(PlaybackErrorCode::UnsupportedFormat, "target sample format is unsupported", "only Int16, Int32, and Float32 PCM targets are supported");
  }

  return std::nullopt;
}

std::optional<FfmpegFilterPipelineError> validateInputFrame(const FfmpegAudioFrame& frame) {
  if (frame.sampleRate == 0 || frame.channelCount == 0 || frame.frameCount == 0) {
    return makeError(PlaybackErrorCode::UnsupportedFormat, "decoded frame has invalid audio shape", "sample rate, channel count, and frame count must be nonzero");
  }
  if (toPackedAvSampleFormat(frame.sampleFormat) == AV_SAMPLE_FMT_NONE) {
    return makeError(PlaybackErrorCode::UnsupportedFormat, "decoded frame sample format is unsupported", "only Int16, Int32, and Float32 decoded frames are supported");
  }
  const auto expectedBytes = static_cast<std::uint64_t>(frame.frameCount) * frame.channelCount * bytesPerSample(frame.sampleFormat);
  if (expectedBytes == 0 || expectedBytes > std::numeric_limits<int>::max() || frame.sampleBytes.size() != expectedBytes) {
    return makeError(PlaybackErrorCode::UnsupportedFormat, "decoded frame sample data size is invalid", "sampleBytes size does not match frame format");
  }

  return std::nullopt;
}

std::vector<std::uint8_t> copyFrameBytes(const AVFrame& frame) {
  const auto format = static_cast<AVSampleFormat>(frame.format);
  const int sampleCount = std::max(0, frame.nb_samples);
  const int channels = std::max(0, frame.ch_layout.nb_channels);
  const int sampleBytes = std::max(0, av_get_bytes_per_sample(format));
  const bool planar = av_sample_fmt_is_planar(format) != 0;
  const int planes = planar ? channels : 1;
  const int samplesPerPlane = planar ? sampleCount : sampleCount * channels;
  const int bytesPerPlane = samplesPerPlane * sampleBytes;

  std::vector<std::uint8_t> bytes;
  bytes.reserve(static_cast<std::size_t>(std::max(0, planes) * bytesPerPlane));

  for (int plane = 0; plane < planes; ++plane) {
    if (frame.extended_data[plane] == nullptr || bytesPerPlane <= 0) {
      continue;
    }
    const auto* begin = frame.extended_data[plane];
    bytes.insert(bytes.end(), begin, begin + bytesPerPlane);
  }

  return bytes;
}

}

class FfmpegFilterPipeline::Impl {
public:
  std::optional<FfmpegFilterPipelineError> configure(FfmpegFilterTargetFormat target) {
    if (const auto error = validateTarget(target)) {
      clearGraph();
      target_ = {};
      configured_ = false;
      return error;
    }

    clearGraph();
    target_ = target;
    configured_ = true;
    return std::nullopt;
  }

  std::optional<FfmpegFilterPipelineError> pushFrame(const FfmpegAudioFrame& frame) {
    if (!configured_) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "filter pipeline is not configured", "call configure before pushFrame");
    }
    if (inputClosed_) {
      return makeError(PlaybackErrorCode::DecodeFailed, "filter pipeline input is already drained", "cannot push after signalEndOfInput");
    }
    if (const auto error = validateInputFrame(frame)) {
      return error;
    }

    const auto input = signatureOf(frame);
    if (!graph_ || !(input == input_)) {
      if (graph_ && !sinkDrained_) {
        return makeError(PlaybackErrorCode::FormatNegotiationFailed, "input format changed while filtered frames are pending", "drain or reset before changing input format");
      }
      if (const auto error = buildGraph(input)) {
        return error;
      }
    }

    FramePtr avFrame(av_frame_alloc());
    if (!avFrame) {
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to allocate filter input frame", "av_frame_alloc returned null");
    }

    if (const auto error = fillInputFrame(*avFrame, frame)) {
      return error;
    }

    const int result = av_buffersrc_add_frame_flags(source_, avFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
    if (result < 0) {
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to push frame into filter graph", result);
    }
    sinkDrained_ = false;
    return std::nullopt;
  }

  std::optional<FfmpegFilterPipelineError> signalEndOfInput() {
    if (!configured_) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "filter pipeline is not configured", "call configure before signalEndOfInput");
    }
    if (inputClosed_) {
      return std::nullopt;
    }
    if (!graph_) {
      inputClosed_ = true;
      sinkDrained_ = true;
      return std::nullopt;
    }

    const int result = av_buffersrc_add_frame_flags(source_, nullptr, 0);
    if (result < 0 && result != AVERROR_EOF) {
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to drain filter graph", result);
    }
    inputClosed_ = true;
    return std::nullopt;
  }

  FfmpegFilterReadResult readFrame() {
    if (!configured_) {
      return FfmpegFilterReadResult{std::nullopt, false, makeError(PlaybackErrorCode::FormatNegotiationFailed, "filter pipeline is not configured", "call configure before readFrame")};
    }
    if (!graph_ || sinkDrained_) {
      return FfmpegFilterReadResult{std::nullopt, inputClosed_ || sinkDrained_, std::nullopt};
    }

    FramePtr output(av_frame_alloc());
    if (!output) {
      return FfmpegFilterReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to allocate filter output frame", "av_frame_alloc returned null")};
    }

    const int result = av_buffersink_get_frame(sink_, output.get());
    if (result == AVERROR(EAGAIN)) {
      return FfmpegFilterReadResult{std::nullopt, false, std::nullopt};
    }
    if (result == AVERROR_EOF) {
      sinkDrained_ = true;
      return FfmpegFilterReadResult{std::nullopt, true, std::nullopt};
    }
    if (result < 0) {
      return FfmpegFilterReadResult{std::nullopt, false, makeError(PlaybackErrorCode::DecodeFailed, "failed to read filtered frame", result)};
    }

    FfmpegAudioFrame filtered{};
    filtered.sampleRate = static_cast<std::uint32_t>(std::max(0, output->sample_rate));
    filtered.channelCount = static_cast<std::uint16_t>(std::clamp(output->ch_layout.nb_channels, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
    filtered.sampleFormat = fromAvSampleFormat(static_cast<AVSampleFormat>(output->format));
    filtered.pts = output->pts;
    filtered.position = output->pts == AV_NOPTS_VALUE ? std::chrono::microseconds{0} : std::chrono::microseconds{av_rescale_q(output->pts, AVRational{1, static_cast<int>(filtered.sampleRate)}, AVRational{1, 1'000'000})};
    filtered.frameCount = static_cast<std::uint32_t>(std::max(0, output->nb_samples));
    filtered.sampleBytes = copyFrameBytes(*output);
    return FfmpegFilterReadResult{std::move(filtered), false, std::nullopt};
  }

  void reset() { clearGraph(); }

private:
  std::optional<FfmpegFilterPipelineError> buildGraph(FrameSignature input) {
    clearGraph();

    graph_.reset(avfilter_graph_alloc());
    if (!graph_) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to allocate filter graph", "avfilter_graph_alloc returned null");
    }

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* aformat = avfilter_get_by_name("aformat");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (abuffer == nullptr || aformat == nullptr || abuffersink == nullptr) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "required FFmpeg audio filters are unavailable", "abuffer, aformat, or abuffersink not found");
    }

    if (const auto error = configureSource(abuffer, input)) {
      return error;
    }

    if (const auto error = configureFormat(aformat)) {
      return error;
    }

    int result = avfilter_graph_create_filter(&sink_, abuffersink, "seriona_abuffersink", nullptr, nullptr, graph_.get());
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to create filter sink", result);
    }

    result = avfilter_link(source_, 0, format_, 0);
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to link filter source", result);
    }
    result = avfilter_link(format_, 0, sink_, 0);
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to link filter sink", result);
    }

    result = avfilter_graph_config(graph_.get(), nullptr);
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to configure filter graph", result);
    }

    input_ = input;
    inputClosed_ = false;
    sinkDrained_ = false;
    return std::nullopt;
  }

  std::optional<FfmpegFilterPipelineError> configureSource(const AVFilter* abuffer, FrameSignature input) {
    const char* sampleFormatName = av_get_sample_fmt_name(toPackedAvSampleFormat(input.sampleFormat));
    if (sampleFormatName == nullptr) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "input sample format is unsupported", "av_get_sample_fmt_name returned null");
    }

    ChannelLayoutHolder layout(input.channelCount);
    if (av_channel_layout_check(&layout.layout) == 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "input channel layout is invalid", "av_channel_layout_default produced an invalid layout");
    }

    std::array<char, 128> channelLayout{};
    const int describeResult = av_channel_layout_describe(&layout.layout, channelLayout.data(), channelLayout.size());
    if (describeResult < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to describe input channel layout", describeResult);
    }

    const std::string args = "time_base=1/" + std::to_string(input.sampleRate) + ":sample_rate=" + std::to_string(input.sampleRate) + ":sample_fmt=" + sampleFormatName + ":channel_layout=" + channelLayout.data();
    const int result = avfilter_graph_create_filter(&source_, abuffer, "seriona_abuffer", args.c_str(), nullptr, graph_.get());
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to create filter source", result);
    }

    return std::nullopt;
  }

  std::optional<FfmpegFilterPipelineError> configureFormat(const AVFilter* aformat) {
    const char* sampleFormatName = av_get_sample_fmt_name(toPackedAvSampleFormat(target_.sampleFormat));
    if (sampleFormatName == nullptr) {
      return makeError(PlaybackErrorCode::UnsupportedFormat, "target sample format is unsupported", "av_get_sample_fmt_name returned null");
    }

    ChannelLayoutHolder layout(target_.channelCount);
    if (av_channel_layout_check(&layout.layout) == 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "target channel layout is invalid", "av_channel_layout_default produced an invalid layout");
    }
    std::array<char, 128> channelLayout{};
    const int describeResult = av_channel_layout_describe(&layout.layout, channelLayout.data(), channelLayout.size());
    if (describeResult < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to describe target channel layout", describeResult);
    }
    const std::string args = "sample_fmts=" + std::string(sampleFormatName) + ":sample_rates=" + std::to_string(target_.sampleRate) + ":channel_layouts=" + channelLayout.data();
    const int result = avfilter_graph_create_filter(&format_, aformat, "seriona_aformat", args.c_str(), nullptr, graph_.get());
    if (result < 0) {
      return makeError(PlaybackErrorCode::FormatNegotiationFailed, "failed to create format conversion filter", result);
    }

    return std::nullopt;
  }

  std::optional<FfmpegFilterPipelineError> fillInputFrame(AVFrame& avFrame, const FfmpegAudioFrame& frame) {
    avFrame.nb_samples = static_cast<int>(frame.frameCount);
    avFrame.format = toPackedAvSampleFormat(frame.sampleFormat);
    avFrame.sample_rate = static_cast<int>(frame.sampleRate);
    avFrame.time_base = AVRational{1, static_cast<int>(frame.sampleRate)};
    avFrame.pts = frame.pts;
    av_channel_layout_default(&avFrame.ch_layout, frame.channelCount);

    int result = av_frame_get_buffer(&avFrame, 0);
    if (result < 0) {
      return makeError(PlaybackErrorCode::DecodeFailed, "failed to allocate filter input frame samples", result);
    }

    result = av_frame_make_writable(&avFrame);
    if (result < 0) {
      return makeError(PlaybackErrorCode::DecodeFailed, "filter input frame is not writable", result);
    }

    const auto expectedBytes = static_cast<std::size_t>(frame.frameCount) * frame.channelCount * bytesPerSample(frame.sampleFormat);
    std::memcpy(avFrame.data[0], frame.sampleBytes.data(), expectedBytes);
    return std::nullopt;
  }

  void clearGraph() {
    graph_.reset();
    source_ = nullptr;
    format_ = nullptr;
    sink_ = nullptr;
    input_ = {};
    inputClosed_ = false;
    sinkDrained_ = false;
  }

  FfmpegFilterTargetFormat target_{};
  FilterGraphPtr graph_{};
  AVFilterContext* source_{nullptr};
  AVFilterContext* format_{nullptr};
  AVFilterContext* sink_{nullptr};
  FrameSignature input_{};
  bool configured_{false};
  bool inputClosed_{false};
  bool sinkDrained_{false};
};

FfmpegFilterPipeline::FfmpegFilterPipeline() : impl_(std::make_unique<Impl>()) {}

FfmpegFilterPipeline::~FfmpegFilterPipeline() = default;

FfmpegFilterPipeline::FfmpegFilterPipeline(FfmpegFilterPipeline&&) noexcept = default;

FfmpegFilterPipeline& FfmpegFilterPipeline::operator=(FfmpegFilterPipeline&&) noexcept = default;

std::optional<FfmpegFilterPipelineError> FfmpegFilterPipeline::configure(FfmpegFilterTargetFormat target) {
  return impl_->configure(target);
}

std::optional<FfmpegFilterPipelineError> FfmpegFilterPipeline::pushFrame(const FfmpegAudioFrame& frame) {
  return impl_->pushFrame(frame);
}

std::optional<FfmpegFilterPipelineError> FfmpegFilterPipeline::signalEndOfInput() {
  return impl_->signalEndOfInput();
}

FfmpegFilterReadResult FfmpegFilterPipeline::readFrame() { return impl_->readFrame(); }

void FfmpegFilterPipeline::reset() { impl_->reset(); }

}
