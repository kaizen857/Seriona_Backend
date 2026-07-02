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
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace seriona::audio::detail {

namespace {

constexpr auto kMicrosecondsTimeBase = AVRational{1, 1'000'000};

[[nodiscard]] std::runtime_error scalarDecodeError(std::string message, int ffmpegCode) {
  return std::runtime_error{std::move(message) + ": " + ffmpegErrorDetail(ffmpegCode)};
}

[[nodiscard]] std::runtime_error scalarDecodeError(std::string message, std::string detail) {
  return std::runtime_error{std::move(message) + ": " + std::move(detail)};
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

template <typename Sample>
[[nodiscard]] double normalizedSample(Sample value) {
  if constexpr (std::is_same_v<Sample, std::uint8_t>) {
    return (static_cast<double>(value) - 128.0) / 128.0;
  } else if constexpr (std::is_same_v<Sample, std::int16_t>) {
    return static_cast<double>(value) / 32768.0;
  } else if constexpr (std::is_same_v<Sample, std::int32_t>) {
    return static_cast<double>(value) / 2147483648.0;
  } else if constexpr (std::is_same_v<Sample, std::int64_t>) {
    return static_cast<double>(static_cast<long double>(value) / 9223372036854775808.0L);
  } else {
    return static_cast<double>(value);
  }
}

template <typename Sample>
[[nodiscard]] double sumPackedChannelEnergy(const AVFrame& frame,
                                            int sampleOffset,
                                            int sampleCount,
                                            int channels) {
  if (frame.extended_data == nullptr || frame.extended_data[0] == nullptr) {
    throw scalarDecodeError("decoded frame is missing packed sample data", "extended_data[0] is null");
  }

  const auto* samples = reinterpret_cast<const Sample*>(frame.extended_data[0]);
  double sumSquares = 0.0;
  for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
    double frameEnergy = 0.0;
    const auto frameOffset = static_cast<std::size_t>(sampleOffset + sampleIndex) * static_cast<std::size_t>(channels);
    for (int channel = 0; channel < channels; ++channel) {
      const double value = normalizedSample(samples[frameOffset + static_cast<std::size_t>(channel)]);
      frameEnergy += value * value;
    }
    sumSquares += frameEnergy / static_cast<double>(channels);
  }

  return sumSquares;
}

template <typename Sample>
[[nodiscard]] double sumPlanarChannelEnergy(const AVFrame& frame,
                                            int sampleOffset,
                                            int sampleCount,
                                            int channels) {
  if (frame.extended_data == nullptr) {
    throw scalarDecodeError("decoded frame is missing planar sample data", "extended_data is null");
  }

  double sumSquares = 0.0;
  for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
    double frameEnergy = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
      if (frame.extended_data[channel] == nullptr) {
        throw scalarDecodeError("decoded frame is missing planar channel data", "extended_data channel is null");
      }
      const auto* samples = reinterpret_cast<const Sample*>(frame.extended_data[channel]);
      const double value = normalizedSample(samples[sampleOffset + sampleIndex]);
      frameEnergy += value * value;
    }
    sumSquares += frameEnergy / static_cast<double>(channels);
  }

  return sumSquares;
}

[[nodiscard]] double sumAverageChannelEnergy(const AVFrame& frame,
                                             int sampleOffset,
                                             int sampleCount,
                                             int fallbackChannels) {
  if (sampleCount <= 0) {
    return 0.0;
  }

  const auto format = static_cast<AVSampleFormat>(frame.format);
  const int channels = frameChannelCount(frame, fallbackChannels);
  if (channels <= 0) {
    throw scalarDecodeError("decoded frame has invalid channel count", std::to_string(channels));
  }

  switch (format) {
  case AV_SAMPLE_FMT_U8:
    return sumPackedChannelEnergy<std::uint8_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_U8P:
    return sumPlanarChannelEnergy<std::uint8_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S16:
    return sumPackedChannelEnergy<std::int16_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S16P:
    return sumPlanarChannelEnergy<std::int16_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S32:
    return sumPackedChannelEnergy<std::int32_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S32P:
    return sumPlanarChannelEnergy<std::int32_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_FLT:
    return sumPackedChannelEnergy<float>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_FLTP:
    return sumPlanarChannelEnergy<float>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_DBL:
    return sumPackedChannelEnergy<double>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_DBLP:
    return sumPlanarChannelEnergy<double>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S64:
    return sumPackedChannelEnergy<std::int64_t>(frame, sampleOffset, sampleCount, channels);
  case AV_SAMPLE_FMT_S64P:
    return sumPlanarChannelEnergy<std::int64_t>(frame, sampleOffset, sampleCount, channels);
  default:
    break;
  }

  const char* formatName = av_get_sample_fmt_name(format);
  throw scalarDecodeError("unsupported scalar waveform sample format", formatName == nullptr ? "unknown" : formatName);
}

[[nodiscard]] std::int64_t rescaleTimeUsToSample(std::int64_t timeUs, int sampleRate, AVRounding rounding) {
  return av_rescale_q_rnd(timeUs, kMicrosecondsTimeBase, AVRational{1, sampleRate}, rounding);
}

[[nodiscard]] std::int64_t frameBaseSample(const AVFrame& frame,
                                           std::int64_t nextFallbackSample,
                                           AVRational streamTimeBase,
                                           int sampleRate) {
  if (frame.pts != AV_NOPTS_VALUE) {
    return av_rescale_q(frame.pts, streamTimeBase, AVRational{1, sampleRate});
  }
  if (nextFallbackSample >= 0) {
    return nextFallbackSample;
  }
  return 0;
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

[[nodiscard]] bool accumulateFrameBars(const AVFrame& frame,
                                       int fallbackChannels,
                                       std::int64_t baseSample,
                                       std::int64_t startSample,
                                       std::int64_t endSample,
                                       std::int64_t windowSampleCount,
                                       std::vector<BarData>& bars) {
  if (frame.nb_samples <= 0) {
    return true;
  }
  if (baseSample >= endSample) {
    return false;
  }

  const auto frameEndSample = baseSample + frame.nb_samples;
  if (frameEndSample <= startSample) {
    return true;
  }

  const auto processStartSample = std::max(baseSample, startSample);
  const auto processEndSample = std::min(frameEndSample, endSample);
  if (processStartSample >= processEndSample) {
    return processEndSample < endSample;
  }

  auto consumedSamples = std::int64_t{0};
  const auto firstFrameOffset = processStartSample - baseSample;
  const auto processSampleCount = processEndSample - processStartSample;
  while (consumedSamples < processSampleCount) {
    const auto absoluteSample = processStartSample + consumedSamples;
    const auto relativeSample = absoluteSample - startSample;
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
      throw scalarDecodeError("scalar waveform sample span is invalid", std::to_string(count));
    }

    const auto frameOffset = firstFrameOffset + consumedSamples;
    if (frameOffset < 0 || frameOffset > std::numeric_limits<int>::max()) {
      throw scalarDecodeError("scalar waveform frame offset is invalid", std::to_string(frameOffset));
    }

    const auto sampleCount = static_cast<int>(count);
    bars[barIndex].sumSquares += sumAverageChannelEnergy(frame, static_cast<int>(frameOffset), sampleCount, fallbackChannels);
    bars[barIndex].actualCount += static_cast<std::uint64_t>(count);
    consumedSamples += count;
  }

  return processEndSample < endSample;
}

}

std::vector<BarData> buildScalarWaveformBars(const std::filesystem::path& filepath,
                                             int barCount,
                                             const WaveformTimeRange& timeRange) {
  auto bars = std::vector<BarData>(barCount > 0 ? static_cast<std::size_t>(barCount) : 0U);
  if (bars.empty() || !timeRange.hasDuration) {
    return bars;
  }

  auto input = openWaveformInput(filepath);
  auto& stream = findBestAudioStream(*input);
  auto decoder = openDecoderForStream(stream);

  const int waveformSampleRate = sampleRate(*decoder);
  const int waveformChannels = channelCount(*decoder);
  if (waveformSampleRate <= 0) {
    throw scalarDecodeError("audio stream has invalid sample rate", std::to_string(waveformSampleRate));
  }
  if (waveformChannels <= 0) {
    throw scalarDecodeError("audio stream has invalid channel count", std::to_string(waveformChannels));
  }

  const auto startSample = rescaleTimeUsToSample(timeRange.startTimeUS, waveformSampleRate, AV_ROUND_DOWN);
  const auto endSample = rescaleTimeUsToSample(timeRange.endTimeUS, waveformSampleRate, AV_ROUND_UP);
  if (startSample >= endSample) {
    return bars;
  }

  const auto streamTimeBase = timeBase(stream);
  if (streamTimeBase.den == 0 || streamTimeBase.num == 0) {
    throw scalarDecodeError("audio stream has invalid time base", "time_base numerator or denominator is zero");
  }

  const auto seekTimestamp = av_rescale_q_rnd(startSample,
                                             AVRational{1, waveformSampleRate},
                                             streamTimeBase,
                                             AV_ROUND_DOWN);
  const int seekResult = av_seek_frame(input.get(), stream.index, seekTimestamp, AVSEEK_FLAG_BACKWARD);
  if (seekResult < 0) {
    throw scalarDecodeError("failed to seek scalar waveform decode", seekResult);
  }
  avcodec_flush_buffers(decoder.get());

  WaveformPacketPtr packet{av_packet_alloc()};
  if (!packet) {
    throw scalarDecodeError("failed to allocate scalar waveform packet", "av_packet_alloc returned null");
  }
  WaveformFramePtr frame{av_frame_alloc()};
  if (!frame) {
    throw scalarDecodeError("failed to allocate scalar waveform frame", "av_frame_alloc returned null");
  }

  const auto windowSampleCount = endSample - startSample;
  auto nextFallbackSample = std::int64_t{-1};
  auto receiveFrames = [&]() {
    while (true) {
      const int receiveResult = avcodec_receive_frame(decoder.get(), frame.get());
      if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
        return true;
      }
      if (receiveResult < 0) {
        throw scalarDecodeError("failed to decode scalar waveform frame", receiveResult);
      }

      const auto baseSample = frameBaseSample(*frame, nextFallbackSample, streamTimeBase, waveformSampleRate);
      const bool needsMore = accumulateFrameBars(*frame,
                                                 waveformChannels,
                                                 baseSample,
                                                 startSample,
                                                 endSample,
                                                 windowSampleCount,
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
      throw scalarDecodeError("failed to read scalar waveform packet", readResult);
    }

    if (packet->stream_index == stream.index) {
      static_cast<void>(stripTrailingId3v1TagIfPresent(*packet, *input, stream));
      if (packet->size > 0) {
        const int sendResult = avcodec_send_packet(decoder.get(), packet.get());
        if (sendResult < 0) {
          av_packet_unref(packet.get());
          throw scalarDecodeError("failed to send scalar waveform packet", sendResult);
        }
        needsMore = receiveFrames();
      }
    }
    av_packet_unref(packet.get());
  }

  if (needsMore) {
    const int drainResult = avcodec_send_packet(decoder.get(), nullptr);
    if (drainResult < 0 && drainResult != AVERROR_EOF) {
      throw scalarDecodeError("failed to drain scalar waveform decoder", drainResult);
    }
    static_cast<void>(receiveFrames());
  }

  return bars;
}

}
