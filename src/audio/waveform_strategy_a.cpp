#include "waveform_internal.h"

#include "waveform_ffmpeg.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio::detail {

namespace {

constexpr auto kMicrosecondsTimeBase = AVRational{1, 1'000'000};

struct StrategyAChunkSampleRange {
  std::int64_t startSample{0};
  std::int64_t endSample{0};
};

[[nodiscard]] std::runtime_error strategyADecodeError(std::string message, int ffmpegCode) {
  return std::runtime_error{std::move(message) + ": " + ffmpegErrorDetail(ffmpegCode)};
}

[[nodiscard]] std::runtime_error strategyADecodeError(std::string message, std::string detail) {
  return std::runtime_error{std::move(message) + ": " + std::move(detail)};
}

[[nodiscard]] std::int64_t rescaleTimeUsToSample(std::int64_t timeUs, int sampleRate, AVRounding rounding) {
  return av_rescale_q_rnd(timeUs, kMicrosecondsTimeBase, AVRational{1, sampleRate}, rounding);
}

[[nodiscard]] int normalizedChunkCount(int chunkCount) { return std::max(1, chunkCount); }

[[nodiscard]] StrategyAChunkSampleRange chunkSampleRange(std::int64_t globalStartSample,
                                                         std::int64_t globalEndSample,
                                                         int chunkIndex,
                                                         int chunkCount) {
  if (chunkIndex < 0 || chunkIndex >= chunkCount) {
    throw std::invalid_argument{"strategy A chunk index is outside chunk count"};
  }

  const auto windowSamples = globalEndSample - globalStartSample;
  if (windowSamples <= 0) {
    return {};
  }

  const auto baseSamples = windowSamples / chunkCount;
  const auto remainderSamples = windowSamples % chunkCount;
  const auto startOffset = (baseSamples * chunkIndex) + std::min<std::int64_t>(chunkIndex, remainderSamples);
  const auto nextIndex = static_cast<std::int64_t>(chunkIndex) + 1;
  const auto endOffset = (baseSamples * nextIndex) + std::min<std::int64_t>(nextIndex, remainderSamples);
  return StrategyAChunkSampleRange{
      .startSample = globalStartSample + startOffset,
      .endSample = globalStartSample + endOffset,
  };
}

[[nodiscard]] std::int64_t seekPrerollStartSample(std::int64_t globalStartSample,
                                                  const StrategyAChunkSampleRange& chunkRange,
                                                  int sampleRate) {
  const auto prerollSamples = static_cast<std::int64_t>(std::max(1, sampleRate));
  if (chunkRange.startSample <= globalStartSample + prerollSamples) {
    return globalStartSample;
  }
  return chunkRange.startSample - prerollSamples;
}

[[nodiscard]] int frameChannelCount(const AVFrame& frame, int fallbackChannels) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
  if (frame.ch_layout.nb_channels > 0) {
    return frame.ch_layout.nb_channels;
  }
#else
  if (frame.channels > 0) {
    return frame.channels;
  }
#endif

  return std::max(1, fallbackChannels);
}

[[nodiscard]] WaveformKernelSampleFormat kernelSampleFormat(AVSampleFormat format) {
  switch (format) {
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    return WaveformKernelSampleFormat::Float32;
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    return WaveformKernelSampleFormat::Int16;
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
    return WaveformKernelSampleFormat::Int32;
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    return WaveformKernelSampleFormat::UInt8;
  default:
    break;
  }

  const char* formatName = av_get_sample_fmt_name(format);
  throw strategyADecodeError("unsupported strategy A waveform sample format",
                             formatName == nullptr ? "unknown" : formatName);
}

[[nodiscard]] WaveformKernelSampleLayout kernelSampleLayout(AVSampleFormat format) {
  return av_sample_fmt_is_planar(format) != 0 ? WaveformKernelSampleLayout::Planar
                                             : WaveformKernelSampleLayout::Interleaved;
}

template <typename Sample>
[[nodiscard]] std::vector<const void*> kernelPlanes(const AVFrame& frame,
                                                    WaveformKernelSampleLayout layout,
                                                    int sampleOffset,
                                                    int channelCount) {
  if (frame.extended_data == nullptr) {
    throw strategyADecodeError("strategy A decoded frame is missing sample data", "extended_data is null");
  }

  auto planes = std::vector<const void*>(layout == WaveformKernelSampleLayout::Planar ? static_cast<std::size_t>(channelCount) : 1U,
                                         nullptr);
  switch (layout) {
  case WaveformKernelSampleLayout::Interleaved: {
    if (frame.extended_data[0] == nullptr) {
      throw strategyADecodeError("strategy A decoded frame is missing interleaved samples", "extended_data[0] is null");
    }
    const auto* samples = reinterpret_cast<const Sample*>(frame.extended_data[0]);
    planes[0] = samples + (static_cast<std::ptrdiff_t>(sampleOffset) * channelCount);
    return planes;
  }
  case WaveformKernelSampleLayout::Planar:
    for (int channel = 0; channel < channelCount; ++channel) {
      if (frame.extended_data[channel] == nullptr) {
        throw strategyADecodeError("strategy A decoded frame is missing planar channel data",
                                   "extended_data channel is null");
      }
      const auto* samples = reinterpret_cast<const Sample*>(frame.extended_data[channel]);
      planes[static_cast<std::size_t>(channel)] = samples + sampleOffset;
    }
    return planes;
  }

  throw strategyADecodeError("strategy A decoded frame has unknown sample layout", "unknown layout");
}

template <typename Sample>
[[nodiscard]] WaveformKernelResult computeTypedFrameSegment(const AVFrame& frame,
                                                           WaveformKernelSampleFormat sampleFormat,
                                                           WaveformKernelSampleLayout layout,
                                                           int sampleOffset,
                                                           int sampleCount,
                                                           int channelCount,
                                                           int decimation,
                                                           const WaveformConfig& config) {
  auto planes = kernelPlanes<Sample>(frame, layout, sampleOffset, channelCount);
  const auto input = WaveformKernelInput{
      .planes = planes.data(),
      .sampleFormat = sampleFormat,
      .layout = layout,
      .frameCount = sampleCount,
      .channelCount = channelCount,
      .decimation = decimation,
  };
  return computeWaveformKernel(input, config);
}

[[nodiscard]] WaveformKernelResult computeFrameSegment(const AVFrame& frame,
                                                       int sampleOffset,
                                                       int sampleCount,
                                                       int fallbackChannels,
                                                       int decimation,
                                                       const WaveformConfig& config) {
  if (sampleCount <= 0) {
    return {};
  }

  const auto format = static_cast<AVSampleFormat>(frame.format);
  const auto sampleFormat = kernelSampleFormat(format);
  const auto layout = kernelSampleLayout(format);
  const int channels = frameChannelCount(frame, fallbackChannels);
  if (channels <= 0) {
    throw strategyADecodeError("strategy A decoded frame has invalid channel count", std::to_string(channels));
  }

  switch (format) {
  case AV_SAMPLE_FMT_FLT:
  case AV_SAMPLE_FMT_FLTP:
    return computeTypedFrameSegment<float>(frame, sampleFormat, layout, sampleOffset, sampleCount, channels, decimation, config);
  case AV_SAMPLE_FMT_S16:
  case AV_SAMPLE_FMT_S16P:
    return computeTypedFrameSegment<std::int16_t>(frame, sampleFormat, layout, sampleOffset, sampleCount, channels, decimation, config);
  case AV_SAMPLE_FMT_S32:
  case AV_SAMPLE_FMT_S32P:
    return computeTypedFrameSegment<std::int32_t>(frame, sampleFormat, layout, sampleOffset, sampleCount, channels, decimation, config);
  case AV_SAMPLE_FMT_U8:
  case AV_SAMPLE_FMT_U8P:
    return computeTypedFrameSegment<std::uint8_t>(frame, sampleFormat, layout, sampleOffset, sampleCount, channels, decimation, config);
  default:
    break;
  }

  throw strategyADecodeError("strategy A decoded frame has unsupported sample format", "unreachable format dispatch");
}

[[nodiscard]] std::int64_t frameBaseSample(const AVFrame& frame,
                                           std::int64_t nextFallbackSample,
                                           AVRational streamTimeBase,
                                           int sampleRate) {
  if (frame.pts != AV_NOPTS_VALUE) {
    return av_rescale_q(frame.pts, streamTimeBase, AVRational{1, sampleRate});
  }
  return nextFallbackSample;
}

[[nodiscard]] std::size_t barIndexForRelativeSample(std::int64_t relativeSample,
                                                    std::int64_t windowSampleCount,
                                                    std::size_t barCount) {
  const auto scaled = (static_cast<long double>(relativeSample) * static_cast<long double>(barCount)) /
                      static_cast<long double>(windowSampleCount);
  const auto index = static_cast<std::size_t>(std::floor(std::max<long double>(0.0L, scaled)));
  return std::min(barCount - 1U, index);
}

[[nodiscard]] std::int64_t nextBarBoundaryRelative(std::size_t barIndex,
                                                   std::int64_t windowSampleCount,
                                                   std::size_t barCount) {
  const auto scaled = (static_cast<long double>(barIndex + 1U) * static_cast<long double>(windowSampleCount)) /
                      static_cast<long double>(barCount);
  const auto boundary = static_cast<std::int64_t>(std::ceil(scaled));
  return std::clamp(boundary, std::int64_t{1}, windowSampleCount);
}

[[nodiscard]] bool accumulateStrategyAFrameBars(const AVFrame& frame,
                                                int fallbackChannels,
                                                std::int64_t baseSample,
                                                const StrategyAChunkSampleRange& chunkRange,
                                                std::int64_t globalStartSample,
                                                std::int64_t globalEndSample,
                                                int decimation,
                                                const WaveformConfig& config,
                                                std::vector<BarData>& bars) {
  if (frame.nb_samples <= 0) {
    return true;
  }
  if (baseSample >= chunkRange.endSample) {
    return false;
  }

  const auto frameEndSample = baseSample + frame.nb_samples;
  if (frameEndSample <= chunkRange.startSample) {
    return true;
  }

  const auto processStartSample = std::max(baseSample, chunkRange.startSample);
  const auto processEndSample = std::min(frameEndSample, chunkRange.endSample);
  if (processStartSample >= processEndSample) {
    return processEndSample < chunkRange.endSample;
  }

  const auto windowSampleCount = globalEndSample - globalStartSample;
  auto consumedSamples = std::int64_t{0};
  const auto firstFrameOffset = processStartSample - baseSample;
  const auto processSampleCount = processEndSample - processStartSample;
  while (consumedSamples < processSampleCount) {
    const auto absoluteSample = processStartSample + consumedSamples;
    const auto relativeSample = absoluteSample - globalStartSample;
    if (relativeSample < 0 || relativeSample >= windowSampleCount) {
      return relativeSample < windowSampleCount;
    }

    const auto barIndex = barIndexForRelativeSample(relativeSample, windowSampleCount, bars.size());
    auto nextBoundary = nextBarBoundaryRelative(barIndex, windowSampleCount, bars.size());
    if (nextBoundary <= relativeSample) {
      nextBoundary = relativeSample + 1;
    }

    const auto count = std::min(processSampleCount - consumedSamples, nextBoundary - relativeSample);
    if (count <= 0 || count > std::numeric_limits<int>::max()) {
      throw strategyADecodeError("strategy A sample span is invalid", std::to_string(count));
    }

    const auto frameOffset = firstFrameOffset + consumedSamples;
    if (frameOffset < 0 || frameOffset > std::numeric_limits<int>::max()) {
      throw strategyADecodeError("strategy A frame offset is invalid", std::to_string(frameOffset));
    }

    const auto result = computeFrameSegment(frame,
                                            static_cast<int>(frameOffset),
                                            static_cast<int>(count),
                                            fallbackChannels,
                                            decimation,
                                            config);
    bars[barIndex].sumSquares += result.sumSquares;
    bars[barIndex].actualCount += result.actualCount;
    consumedSamples += count;
  }

  return processEndSample < chunkRange.endSample;
}

}

std::vector<BarData> processAudioChunkStrategyA(const StrategyAChunkRequest& request) {
  auto bars = std::vector<BarData>(request.barCount > 0 ? static_cast<std::size_t>(request.barCount) : 0U);
  if (bars.empty() || !request.timeRange.hasDuration) {
    return bars;
  }

  const int chunkCount = normalizedChunkCount(request.chunkCount);
  if (request.chunkIndex < 0 || request.chunkIndex >= chunkCount) {
    throw std::invalid_argument{"strategy A chunk index is outside chunk count"};
  }

  auto input = openWaveformInput(request.filepath);
  auto& stream = findBestAudioStream(*input);
  auto decoder = openDecoderForStream(stream, 1);

  const int waveformSampleRate = sampleRate(*decoder);
  const int waveformChannels = channelCount(*decoder);
  if (waveformSampleRate <= 0) {
    throw strategyADecodeError("strategy A audio stream has invalid sample rate", std::to_string(waveformSampleRate));
  }
  if (waveformChannels <= 0) {
    throw strategyADecodeError("strategy A audio stream has invalid channel count", std::to_string(waveformChannels));
  }

  const auto globalStartSample = rescaleTimeUsToSample(request.timeRange.startTimeUS, waveformSampleRate, AV_ROUND_DOWN);
  const auto globalEndSample = rescaleTimeUsToSample(request.timeRange.endTimeUS, waveformSampleRate, AV_ROUND_UP);
  if (globalStartSample >= globalEndSample) {
    return bars;
  }

  const auto chunkRange = chunkSampleRange(globalStartSample, globalEndSample, request.chunkIndex, chunkCount);
  if (chunkRange.startSample >= chunkRange.endSample) {
    return bars;
  }

  const auto streamTimeBase = timeBase(stream);
  if (streamTimeBase.den == 0 || streamTimeBase.num == 0) {
    throw strategyADecodeError("strategy A audio stream has invalid time base", "time_base numerator or denominator is zero");
  }

  const auto seekStartSample = seekPrerollStartSample(globalStartSample, chunkRange, waveformSampleRate);
  const auto seekTimestamp = av_rescale_q_rnd(seekStartSample,
                                             AVRational{1, waveformSampleRate},
                                             streamTimeBase,
                                             AV_ROUND_DOWN);
  const int seekResult = av_seek_frame(input.get(), stream.index, seekTimestamp, AVSEEK_FLAG_BACKWARD);
  if (seekResult < 0) {
    throw strategyADecodeError("failed to seek strategy A waveform decode", seekResult);
  }
  avcodec_flush_buffers(decoder.get());

  WaveformPacketPtr packet{av_packet_alloc()};
  if (!packet) {
    throw strategyADecodeError("failed to allocate strategy A waveform packet", "av_packet_alloc returned null");
  }
  WaveformFramePtr frame{av_frame_alloc()};
  if (!frame) {
    throw strategyADecodeError("failed to allocate strategy A waveform frame", "av_frame_alloc returned null");
  }

  const int decimation = waveformDecimationForSampleRate(waveformSampleRate);
  auto nextFallbackSample = seekStartSample;
  auto receiveFrames = [&]() {
    while (true) {
      const int receiveResult = avcodec_receive_frame(decoder.get(), frame.get());
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        return true;
      }
      if (receiveResult < 0) {
        throw strategyADecodeError("failed to decode strategy A waveform frame", receiveResult);
      }

      const auto baseSample = frameBaseSample(*frame, nextFallbackSample, streamTimeBase, waveformSampleRate);
      const bool needsMore = accumulateStrategyAFrameBars(*frame,
                                                          waveformChannels,
                                                          baseSample,
                                                          chunkRange,
                                                          globalStartSample,
                                                          globalEndSample,
                                                          decimation,
                                                          request.config,
                                                          bars);
      nextFallbackSample = baseSample + frame->nb_samples;
      av_frame_unref(frame.get());
      if (!needsMore) {
        return false;
      }
    }
  };

  bool needsMore = true;
  while (needsMore) {
    const int readResult = av_read_frame(input.get(), packet.get());
    if (readResult == AVERROR_EOF) {
      break;
    }
    if (readResult < 0) {
      throw strategyADecodeError("failed to read strategy A waveform packet", readResult);
    }

    if (packet->stream_index == stream.index) {
      static_cast<void>(stripTrailingId3v1TagIfPresent(*packet, *input, stream));
      if (packet->size > 0) {
        const int sendResult = avcodec_send_packet(decoder.get(), packet.get());
        if (sendResult < 0) {
          av_packet_unref(packet.get());
          throw strategyADecodeError("failed to send strategy A waveform packet", sendResult);
        }
        needsMore = receiveFrames();
      }
    }
    av_packet_unref(packet.get());
  }

  if (needsMore) {
    const int drainResult = avcodec_send_packet(decoder.get(), nullptr);
    if (drainResult < 0 && drainResult != AVERROR_EOF) {
      throw strategyADecodeError("failed to drain strategy A waveform decoder", drainResult);
    }
    static_cast<void>(receiveFrames());
  }

  return bars;
}

}
