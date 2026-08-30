#include "waveform_internal.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace seriona::audio::detail {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)

namespace {

[[nodiscard]] int normalizedDecimation(int decimation) { return decimation < 1 ? 1 : decimation; }

[[nodiscard]] std::uint64_t sampledFrameCount(std::int64_t frameCount, int decimation) {
  if (frameCount <= 0) {
    return 0;
  }

  const auto stride = static_cast<std::int64_t>(normalizedDecimation(decimation));
  return static_cast<std::uint64_t>(((frameCount - 1) / stride) + 1);
}

[[nodiscard]] std::invalid_argument kernelInputError(std::string detail) {
  return std::invalid_argument{"invalid AVX2 waveform kernel input: " + std::move(detail)};
}

void validateKernelInput(const WaveformKernelInput& input) {
  if (input.frameCount <= 0) {
    return;
  }
  if (input.channelCount <= 0) {
    throw kernelInputError("channelCount must be positive");
  }
  if (input.planes == nullptr) {
    throw kernelInputError("planes must not be null");
  }

  switch (input.layout) {
  case WaveformKernelSampleLayout::Interleaved:
    if (input.planes[0] == nullptr) {
      throw kernelInputError("interleaved plane must not be null");
    }
    return;
  case WaveformKernelSampleLayout::Planar:
    for (int channel = 0; channel < input.channelCount; ++channel) {
      if (input.planes[channel] == nullptr) {
        throw kernelInputError("planar channel plane must not be null");
      }
    }
    return;
  }

  throw kernelInputError("unknown sample layout");
}

template <typename Sample>
[[nodiscard]] float normalizedSample(Sample value) {
  if constexpr (std::is_same_v<Sample, std::uint8_t>) {
    return (static_cast<float>(value) - 128.0F) * (1.0F / 128.0F);
  } else if constexpr (std::is_same_v<Sample, std::int16_t>) {
    return static_cast<float>(value) * (1.0F / 32768.0F);
  } else if constexpr (std::is_same_v<Sample, std::int32_t>) {
    return static_cast<float>(static_cast<double>(value) * (1.0 / 2147483648.0));
  } else {
    return static_cast<float>(value);
  }
}

template <typename Sample>
[[nodiscard]] const Sample* channelSamples(const WaveformKernelInput& input, int channel) {
  switch (input.layout) {
  case WaveformKernelSampleLayout::Interleaved:
    return reinterpret_cast<const Sample*>(input.planes[0]) + channel;
  case WaveformKernelSampleLayout::Planar:
    return reinterpret_cast<const Sample*>(input.planes[channel]);
  }

  throw kernelInputError("unknown sample layout");
}

template <typename Sample>
[[nodiscard]] int channelStride(const WaveformKernelInput& input) {
  switch (input.layout) {
  case WaveformKernelSampleLayout::Interleaved:
    return input.channelCount;
  case WaveformKernelSampleLayout::Planar:
    return 1;
  }

  throw kernelInputError("unknown sample layout");
}

[[nodiscard]] double horizontalSum(__m256 values) {
  alignas(32) std::array<float, 8> lanes{};
  _mm256_storeu_ps(lanes.data(), values);

  double sum = 0.0;
  for (const float lane : lanes) {
    sum += static_cast<double>(lane);
  }
  return sum;
}

template <typename Sample>
[[nodiscard]] __m256 loadNormalizedGathered(const Sample* samples,
                                            std::int64_t frame,
                                            int stride,
                                            int decimation) {
  alignas(32) std::array<float, 8> lanes{};
  const auto frameStride = static_cast<std::int64_t>(decimation);
  const auto sampleStride = static_cast<std::int64_t>(stride);
  for (int lane = 0; lane < 8; ++lane) {
    const auto offset = (frame + (static_cast<std::int64_t>(lane) * frameStride)) * sampleStride;
    lanes[static_cast<std::size_t>(lane)] = normalizedSample(samples[static_cast<std::ptrdiff_t>(offset)]);
  }

  return _mm256_loadu_ps(lanes.data());
}

template <typename Sample>
[[nodiscard]] __m256 loadNormalized8(const Sample* samples, std::int64_t frame, int stride, int decimation) {
  return loadNormalizedGathered(samples, frame, stride, decimation);
}

template <>
[[nodiscard]] __m256 loadNormalized8<float>(const float* samples, std::int64_t frame, int stride, int decimation) {
  if (stride == 1 && decimation == 1) {
    return _mm256_loadu_ps(samples + frame);
  }

  return loadNormalizedGathered(samples, frame, stride, decimation);
}

template <>
[[nodiscard]] __m256 loadNormalized8<std::int16_t>(const std::int16_t* samples,
                                                   std::int64_t frame,
                                                   int stride,
                                                   int decimation) {
  if (stride == 1 && decimation == 1) {
    const auto raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples + frame));
    const auto wide = _mm256_cvtepi16_epi32(raw);
    const auto floats = _mm256_cvtepi32_ps(wide);
    return _mm256_mul_ps(floats, _mm256_set1_ps(1.0F / 32768.0F));
  }

  return loadNormalizedGathered(samples, frame, stride, decimation);
}

template <>
[[nodiscard]] __m256 loadNormalized8<std::int32_t>(const std::int32_t* samples,
                                                   std::int64_t frame,
                                                   int stride,
                                                   int decimation) {
  if (stride == 1 && decimation == 1) {
    const auto raw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(samples + frame));
    const auto floats = _mm256_cvtepi32_ps(raw);
    return _mm256_mul_ps(floats, _mm256_set1_ps(1.0F / 2147483648.0F));
  }

  return loadNormalizedGathered(samples, frame, stride, decimation);
}

template <>
[[nodiscard]] __m256 loadNormalized8<std::uint8_t>(const std::uint8_t* samples,
                                                   std::int64_t frame,
                                                   int stride,
                                                   int decimation) {
  if (stride == 1 && decimation == 1) {
    const auto raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(samples + frame));
    const auto wide = _mm256_cvtepu8_epi32(raw);
    const auto centered = _mm256_sub_epi32(wide, _mm256_set1_epi32(128));
    const auto floats = _mm256_cvtepi32_ps(centered);
    return _mm256_mul_ps(floats, _mm256_set1_ps(1.0F / 128.0F));
  }

  return loadNormalizedGathered(samples, frame, stride, decimation);
}

template <typename Sample>
[[nodiscard]] double avx2ChannelSumSquares(const Sample* samples,
                                           std::int64_t frameCount,
                                           int stride,
                                           int decimation) {
  const int normalizedStep = normalizedDecimation(decimation);
  auto frame = std::int64_t{0};
  auto sumVector = _mm256_setzero_ps();
  const auto vectorSpan = static_cast<std::int64_t>(7 * normalizedStep);
  for (; frame + vectorSpan < frameCount; frame += static_cast<std::int64_t>(8 * normalizedStep)) {
    const auto values = loadNormalized8(samples, frame, stride, normalizedStep);
    sumVector = _mm256_fmadd_ps(values, values, sumVector);
  }

  double sumSquares = horizontalSum(sumVector);
  for (; frame < frameCount; frame += static_cast<std::int64_t>(normalizedStep)) {
    const double value = normalizedSample(samples[static_cast<std::ptrdiff_t>(frame * static_cast<std::int64_t>(stride))]);
    sumSquares += value * value;
  }

  return sumSquares;
}

template <typename Sample>
[[nodiscard]] WaveformKernelResult computeAvx2TypedKernel(const WaveformKernelInput& input) {
  validateKernelInput(input);
  const auto actualCount = sampledFrameCount(input.frameCount, input.decimation);
  if (actualCount == 0) {
    return WaveformKernelResult{0.0, 0, WaveformKernelBackend::Avx2};
  }

  const int stride = channelStride<Sample>(input);
  double channelSumSquares = 0.0;
  for (int channel = 0; channel < input.channelCount; ++channel) {
    channelSumSquares += avx2ChannelSumSquares(channelSamples<Sample>(input, channel),
                                               input.frameCount,
                                               stride,
                                               input.decimation);
  }

  return WaveformKernelResult{channelSumSquares / static_cast<double>(input.channelCount),
                              actualCount,
                              WaveformKernelBackend::Avx2};
}

}

WaveformKernelResult computeAvx2WaveformKernel(const WaveformKernelInput& input) {
  switch (input.sampleFormat) {
  case WaveformKernelSampleFormat::Float32:
    return computeAvx2TypedKernel<float>(input);
  case WaveformKernelSampleFormat::Int16:
    return computeAvx2TypedKernel<std::int16_t>(input);
  case WaveformKernelSampleFormat::Int32:
    return computeAvx2TypedKernel<std::int32_t>(input);
  case WaveformKernelSampleFormat::UInt8:
    return computeAvx2TypedKernel<std::uint8_t>(input);
  }

  throw kernelInputError("unknown sample format");
}


#else // 非 x86 架构（如 arm64）：AVX2 内核不可用，回落 scalar 实现。
      // 运行期 supportsAvx2() 已返回 false，此定义仅用于满足链接期符号引用。

WaveformKernelResult computeAvx2WaveformKernel(const WaveformKernelInput& input) {
  return computeScalarWaveformKernel(input);
}

#endif

}
