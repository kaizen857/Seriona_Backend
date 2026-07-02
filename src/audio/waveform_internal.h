#pragma once

#include "seriona/audio/waveform_generator.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::audio {

namespace detail {

struct WaveformBuildRequest {
  std::string filepath;
  int barCount{0};
  int totalWidth{0};
  int maxHeight{0};
  std::int64_t startTimeUS{0};
  std::int64_t endTimeUS{0};
  WaveformConfig config{};
};

struct BarData {
  double sumSquares{0.0};
  std::uint64_t actualCount{0};
};

struct ChunkResult {
  std::vector<BarData> bars;
};

struct WaveformTimeRange {
  std::int64_t startTimeUS{0};
  std::int64_t endTimeUS{0};
  bool hasDuration{false};
};

enum class WaveformStrategy {
  SeekChunks,
  PacketBatches,
};

enum class WaveformKernelSampleFormat {
  Float32,
  Int16,
  Int32,
  UInt8,
};

enum class WaveformKernelSampleLayout {
  Interleaved,
  Planar,
};

enum class WaveformKernelBackend {
  Scalar,
  Avx2,
};

struct WaveformKernelInput {
  const void* const* planes{nullptr};
  WaveformKernelSampleFormat sampleFormat{WaveformKernelSampleFormat::Float32};
  WaveformKernelSampleLayout layout{WaveformKernelSampleLayout::Interleaved};
  std::int64_t frameCount{0};
  int channelCount{0};
  int decimation{1};
};

struct WaveformKernelResult {
  double sumSquares{0.0};
  std::uint64_t actualCount{0};
  WaveformKernelBackend backend{WaveformKernelBackend::Scalar};
};

struct StrategyAChunkRequest {
  std::filesystem::path filepath;
  int barCount{0};
  WaveformTimeRange timeRange{};
  int chunkIndex{0};
  int chunkCount{1};
  WaveformConfig config{};
};

struct StrategyBPacketBatchRequest {
  std::filesystem::path filepath;
  int barCount{0};
  WaveformTimeRange timeRange{};
  WaveformConfig config{};
};

using WaveformBarTask = std::function<std::vector<BarData>()>;

[[nodiscard]] bool normalizeShape(int barCount, int totalWidth, int maxHeight, int& barWidth);
[[nodiscard]] WaveformTimeRange normalizeTimeRange(std::int64_t startTimeUS,
                                                   std::int64_t endTimeUS,
                                                   std::int64_t durationUS);
[[nodiscard]] WaveformStrategy selectWaveformStrategy(std::string_view formatName);
[[nodiscard]] int waveformDecimationForSampleRate(int sampleRate);
[[nodiscard]] bool supportsAvx2();
[[nodiscard]] bool waveformKernelCanUseAvx2(const WaveformKernelInput& input);
[[nodiscard]] WaveformKernelResult computeScalarWaveformKernel(const WaveformKernelInput& input);
[[nodiscard]] WaveformKernelResult computeAvx2WaveformKernel(const WaveformKernelInput& input);
[[nodiscard]] WaveformKernelResult computeWaveformKernel(const WaveformKernelInput& input, const WaveformConfig& config);
[[nodiscard]] std::vector<BarData> scheduleWaveformBarTasks(int barCount,
                                                            std::vector<WaveformBarTask> tasks,
                                                            const WaveformConfig& config,
                                                            std::string_view context);
[[nodiscard]] std::vector<BarData> processAudioChunksStrategyAWithThreadPool(const std::filesystem::path& filepath,
                                                                             int barCount,
                                                                             const WaveformTimeRange& timeRange,
                                                                             const WaveformConfig& config);
[[nodiscard]] std::vector<BarData> processAudioChunkStrategyA(const StrategyAChunkRequest& request);
[[nodiscard]] std::vector<BarData> processPacketBatchStrategyB(const StrategyBPacketBatchRequest& request);
[[nodiscard]] std::vector<BarData> buildScalarWaveformBars(const std::filesystem::path& filepath,
                                                           int barCount,
                                                           const WaveformTimeRange& timeRange);
[[nodiscard]] std::vector<int> mapBarsToHeights(const std::vector<BarData>& finalBarsData,
                                                int maxHeight,
                                                const WaveformConfig& config);

}
}
