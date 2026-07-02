#include "waveform_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace seriona::audio::detail {

namespace {

[[nodiscard]] int normalizedDecimation(int decimation) { return std::max(1, decimation); }

[[nodiscard]] std::uint64_t sampledFrameCount(std::int64_t frameCount, int decimation) {
  if (frameCount <= 0) {
    return 0;
  }

  const auto stride = static_cast<std::int64_t>(normalizedDecimation(decimation));
  return static_cast<std::uint64_t>(((frameCount - 1) / stride) + 1);
}

[[nodiscard]] std::invalid_argument kernelInputError(std::string detail) {
  return std::invalid_argument{"invalid waveform kernel input: " + std::move(detail)};
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
[[nodiscard]] double normalizedSample(Sample value) {
  if constexpr (std::is_same_v<Sample, std::uint8_t>) {
    return (static_cast<double>(value) - 128.0) / 128.0;
  } else if constexpr (std::is_same_v<Sample, std::int16_t>) {
    return static_cast<double>(value) / 32768.0;
  } else if constexpr (std::is_same_v<Sample, std::int32_t>) {
    return static_cast<double>(value) / 2147483648.0;
  } else {
    return static_cast<double>(value);
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

template <typename Sample>
[[nodiscard]] double scalarChannelSumSquares(const Sample* samples,
                                             std::int64_t frameCount,
                                             int stride,
                                             int decimation) {
  double sumSquares = 0.0;
  for (auto frame = std::int64_t{0}; frame < frameCount; frame += static_cast<std::int64_t>(normalizedDecimation(decimation))) {
    const double value = normalizedSample(samples[static_cast<std::ptrdiff_t>(frame * static_cast<std::int64_t>(stride))]);
    sumSquares += value * value;
  }
  return sumSquares;
}

template <typename Sample>
[[nodiscard]] WaveformKernelResult computeScalarTypedKernel(const WaveformKernelInput& input) {
  validateKernelInput(input);
  const auto actualCount = sampledFrameCount(input.frameCount, input.decimation);
  if (actualCount == 0) {
    return {};
  }

  const int stride = channelStride<Sample>(input);
  double channelSumSquares = 0.0;
  for (int channel = 0; channel < input.channelCount; ++channel) {
    channelSumSquares += scalarChannelSumSquares(channelSamples<Sample>(input, channel),
                                                 input.frameCount,
                                                 stride,
                                                 input.decimation);
  }

  return WaveformKernelResult{channelSumSquares / static_cast<double>(input.channelCount),
                              actualCount,
                              WaveformKernelBackend::Scalar};
}

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
[[nodiscard]] bool msvcSupportsAvx2() {
  int registers[4]{};
  __cpuidex(registers, 0, 0);
  if (registers[0] < 7) {
    return false;
  }

  __cpuidex(registers, 1, 0);
  const bool osUsesXsave = (registers[2] & (1 << 27)) != 0;
  const bool cpuAvx = (registers[2] & (1 << 28)) != 0;
  const bool cpuFma = (registers[2] & (1 << 12)) != 0;
  if (!osUsesXsave || !cpuAvx || !cpuFma) {
    return false;
  }

  const auto xcr0 = _xgetbv(0);
  if ((xcr0 & 0x6) != 0x6) {
    return false;
  }

  __cpuidex(registers, 7, 0);
  return (registers[1] & (1 << 5)) != 0;
}
#endif

}

int waveformDecimationForSampleRate(int sampleRate) {
  if (sampleRate <= 48'000) {
    return 1;
  }

  return std::max(1, sampleRate / 32'000);
}

bool supportsAvx2() {
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) && \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  return msvcSupportsAvx2();
#else
  return false;
#endif
}

bool waveformKernelCanUseAvx2(const WaveformKernelInput& input) {
  if (input.frameCount <= 0 || input.channelCount <= 0 || input.planes == nullptr) {
    return false;
  }

  switch (input.layout) {
  case WaveformKernelSampleLayout::Interleaved:
    if (input.planes[0] == nullptr) {
      return false;
    }
    break;
  case WaveformKernelSampleLayout::Planar:
    for (int channel = 0; channel < input.channelCount; ++channel) {
      if (input.planes[channel] == nullptr) {
        return false;
      }
    }
    break;
  }

  switch (input.sampleFormat) {
  case WaveformKernelSampleFormat::Float32:
  case WaveformKernelSampleFormat::Int16:
  case WaveformKernelSampleFormat::Int32:
  case WaveformKernelSampleFormat::UInt8:
    return true;
  }

  return false;
}

WaveformKernelResult computeScalarWaveformKernel(const WaveformKernelInput& input) {
  switch (input.sampleFormat) {
  case WaveformKernelSampleFormat::Float32:
    return computeScalarTypedKernel<float>(input);
  case WaveformKernelSampleFormat::Int16:
    return computeScalarTypedKernel<std::int16_t>(input);
  case WaveformKernelSampleFormat::Int32:
    return computeScalarTypedKernel<std::int32_t>(input);
  case WaveformKernelSampleFormat::UInt8:
    return computeScalarTypedKernel<std::uint8_t>(input);
  }

  throw kernelInputError("unknown sample format");
}

WaveformKernelResult computeWaveformKernel(const WaveformKernelInput& input, const WaveformConfig& config) {
  if (config.enableSIMD && supportsAvx2() && waveformKernelCanUseAvx2(input)) {
    return computeAvx2WaveformKernel(input);
  }

  return computeScalarWaveformKernel(input);
}

}
