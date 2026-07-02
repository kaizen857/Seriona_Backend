#include "seriona/audio/waveform_generator.h"

#include "waveform_ffmpeg.h"
#include "waveform_internal.h"

#include <BS_thread_pool.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr auto kPacketBatchFormats = std::array<std::string_view, 8>{
    "mov",
    "mp4",
    "m4a",
    "3gp",
    "3g2",
    "mj2",
    "matroska",
    "webm",
};

constexpr double kSilentRmsThreshold = 1.0e-9;

[[nodiscard]] bool charactersEqualIgnoringCase(char lhs, char rhs) {
  const auto lowerLhs = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs)));
  const auto lowerRhs = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs)));
  return lowerLhs == lowerRhs;
}

[[nodiscard]] bool containsIgnoringCase(std::string_view value, std::string_view token) {
  if (token.empty()) {
    return true;
  }
  if (value.size() < token.size()) {
    return false;
  }

  for (std::size_t offset = 0; offset <= value.size() - token.size(); ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < token.size(); ++index) {
      if (!charactersEqualIgnoringCase(value[offset + index], token[index])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] BS::concurrency_t normalizeThreadCount(const seriona::audio::WaveformConfig& config) {
  constexpr auto minThreadCount = BS::concurrency_t{1};
  if (config.threadCount <= 0) {
    const auto detected = std::thread::hardware_concurrency();
    return detected == 0U ? minThreadCount : static_cast<BS::concurrency_t>(detected);
  }

  const auto requested = static_cast<unsigned long long>(config.threadCount);
  const auto maxThreadCount = static_cast<unsigned long long>(std::numeric_limits<BS::concurrency_t>::max());
  const auto bounded = std::min(requested, maxThreadCount);
  return std::max(minThreadCount, static_cast<BS::concurrency_t>(bounded));
}

[[nodiscard]] std::runtime_error schedulerError(std::string_view context,
                                                std::size_t taskIndex,
                                                const std::exception& error) {
  return std::runtime_error{std::string{context} + " task " + std::to_string(taskIndex) + " failed: " + error.what()};
}

[[nodiscard]] std::runtime_error schedulerUnknownError(std::string_view context, std::size_t taskIndex) {
  return std::runtime_error{std::string{context} + " task " + std::to_string(taskIndex) + " failed with unknown exception"};
}

[[nodiscard]] std::runtime_error publicWaveformBuildError(const std::filesystem::path& filepath,
                                                          const std::exception& error) {
  return std::runtime_error{"failed to build waveform for '" + filepath.string() + "': " + error.what()};
}

void mergeBarData(const std::vector<seriona::audio::detail::BarData>& source,
                  std::vector<seriona::audio::detail::BarData>& destination,
                  std::string_view context,
                  std::size_t taskIndex) {
  if (source.size() != destination.size()) {
    throw std::runtime_error{std::string{context} + " task " + std::to_string(taskIndex) +
                             " returned invalid bar count " + std::to_string(source.size()) +
                             ", expected " + std::to_string(destination.size())};
  }

  for (std::size_t index = 0; index < destination.size(); ++index) {
    destination[index].sumSquares += source[index].sumSquares;
    destination[index].actualCount += source[index].actualCount;
  }
}

}

namespace seriona::audio {

std::vector<int> buildAudioWaveform(const std::string& filepath,
                                    int barCount,
                                    int totalWidth,
                                    int& barWidth,
                                    int maxHeight,
                                    std::int64_t startTimeUS,
                                    std::int64_t endTimeUS,
                                    const WaveformConfig& config) {
  const bool hasValidShape = detail::normalizeShape(barCount, totalWidth, maxHeight, barWidth);
  if (barCount <= 0) {
    return {};
  }

  auto zeroBars = std::vector<int>(static_cast<std::size_t>(barCount), 0);
  if (!hasValidShape) {
    return zeroBars;
  }

  const auto path = std::filesystem::path{filepath};
  try {
    auto input = detail::openWaveformInput(path);
    auto& stream = detail::findBestAudioStream(*input);
    const auto timeRange = detail::normalizeTimeRange(startTimeUS,
                                                      endTimeUS,
                                                      detail::streamDurationUs(*input, stream));
    if (!timeRange.hasDuration) {
      return zeroBars;
    }

    auto bars = std::vector<detail::BarData>{};
    switch (detail::selectWaveformStrategy(detail::formatName(*input))) {
    case detail::WaveformStrategy::SeekChunks:
      bars = detail::processAudioChunksStrategyAWithThreadPool(path, barCount, timeRange, config);
      break;
    case detail::WaveformStrategy::PacketBatches:
      bars = detail::processPacketBatchStrategyB(detail::StrategyBPacketBatchRequest{
          .filepath = path,
          .barCount = barCount,
          .timeRange = timeRange,
          .config = config,
      });
      break;
    }

    return detail::mapBarsToHeights(bars, maxHeight, config);
  } catch (const std::exception& error) {
    throw publicWaveformBuildError(path, error);
  }
}

namespace detail {

bool normalizeShape(int barCount, int totalWidth, int maxHeight, int& barWidth) {
  barWidth = 0;
  if (barCount <= 0 || totalWidth <= 0 || maxHeight <= 0) {
    return false;
  }

  barWidth = std::max(1, (totalWidth / barCount) - 2);
  return true;
}

WaveformTimeRange normalizeTimeRange(std::int64_t startTimeUS,
                                     std::int64_t endTimeUS,
                                     std::int64_t durationUS) {
  if (durationUS <= 0) {
    return {};
  }

  const auto normalizedStart = std::clamp(startTimeUS, std::int64_t{0}, durationUS);
  const auto normalizedEnd = endTimeUS <= 0 ? durationUS : std::clamp(endTimeUS, std::int64_t{0}, durationUS);
  if (normalizedStart >= normalizedEnd) {
    return WaveformTimeRange{
        .startTimeUS = normalizedStart,
        .endTimeUS = normalizedStart,
        .hasDuration = false,
    };
  }

  return WaveformTimeRange{
      .startTimeUS = normalizedStart,
      .endTimeUS = normalizedEnd,
      .hasDuration = true,
  };
}

WaveformStrategy selectWaveformStrategy(std::string_view formatName) {
  for (const auto container : kPacketBatchFormats) {
    if (containsIgnoringCase(formatName, container)) {
      return WaveformStrategy::PacketBatches;
    }
  }

  return WaveformStrategy::SeekChunks;
}

std::vector<BarData> scheduleWaveformBarTasks(int barCount,
                                              std::vector<WaveformBarTask> tasks,
                                              const WaveformConfig& config,
                                              std::string_view context) {
  auto merged = std::vector<BarData>(barCount > 0 ? static_cast<std::size_t>(barCount) : 0U);
  if (merged.empty() || tasks.empty()) {
    return merged;
  }

  BS::thread_pool pool{normalizeThreadCount(config)};
  auto futures = std::vector<std::future<std::vector<BarData>>>{};
  futures.reserve(tasks.size());
  for (auto& task : tasks) {
    futures.push_back(pool.submit_task([task = std::move(task)] { return task(); }));
  }

  std::exception_ptr firstFailure;
  auto failedTaskIndex = std::size_t{0};
  for (std::size_t index = 0; index < futures.size(); ++index) {
    try {
      const auto bars = futures[index].get();
      if (firstFailure == nullptr) {
        mergeBarData(bars, merged, context, index);
      }
    } catch (const std::exception& error) {
      if (firstFailure == nullptr) {
        failedTaskIndex = index;
        firstFailure = std::make_exception_ptr(schedulerError(context, index, error));
      }
    } catch (...) {
      if (firstFailure == nullptr) {
        failedTaskIndex = index;
        firstFailure = std::make_exception_ptr(schedulerUnknownError(context, index));
      }
    }
  }

  if (firstFailure != nullptr) {
    try {
      std::rethrow_exception(firstFailure);
    } catch (const std::runtime_error&) {
      throw;
    } catch (const std::exception& error) {
      throw schedulerError(context, failedTaskIndex, error);
    }
  }

  return merged;
}

std::vector<BarData> processAudioChunksStrategyAWithThreadPool(const std::filesystem::path& filepath,
                                                               int barCount,
                                                               const WaveformTimeRange& timeRange,
                                                               const WaveformConfig& config) {
  if (barCount <= 0 || !timeRange.hasDuration) {
    return std::vector<BarData>(barCount > 0 ? static_cast<std::size_t>(barCount) : 0U);
  }

  const auto chunkCount = static_cast<int>(normalizeThreadCount(config));
  auto tasks = std::vector<WaveformBarTask>{};
  tasks.reserve(static_cast<std::size_t>(chunkCount));
  for (int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
    tasks.push_back([request = StrategyAChunkRequest{
                         .filepath = filepath,
                         .barCount = barCount,
                         .timeRange = timeRange,
                         .chunkIndex = chunkIndex,
                         .chunkCount = chunkCount,
                         .config = config,
                     }] { return processAudioChunkStrategyA(request); });
  }

  return scheduleWaveformBarTasks(barCount, std::move(tasks), config, "strategy A waveform thread pool");
}

std::vector<int> mapBarsToHeights(const std::vector<BarData>& finalBarsData,
                                  int maxHeight,
                                  const WaveformConfig& config) {
  auto heights = std::vector<int>(finalBarsData.size(), 0);
  const auto dbFloor = static_cast<double>(config.dbFloor);
  const auto dbCeiling = static_cast<double>(config.dbCeiling);
  const auto dbRange = dbCeiling - dbFloor;
  if (maxHeight <= 0 || !std::isfinite(dbFloor) || !std::isfinite(dbCeiling) || dbRange <= 0.0) {
    return heights;
  }

  for (std::size_t index = 0; index < finalBarsData.size(); ++index) {
    const auto& bar = finalBarsData[index];
    if (bar.actualCount == 0) {
      continue;
    }

    const auto meanSquare = std::max(0.0, bar.sumSquares / static_cast<double>(bar.actualCount));
    const auto rms = std::sqrt(meanSquare);
    auto db = dbFloor;
    if (!std::isnan(rms) && rms >= kSilentRmsThreshold) {
      db = 20.0 * std::log10(rms);
    }

    const auto clippedDb = std::clamp(db, dbFloor, dbCeiling);
    const auto normalizedHeight = (clippedDb - dbFloor) / dbRange;
    heights[index] = std::max(2, static_cast<int>(normalizedHeight * maxHeight));
  }

  return heights;
}

}

}
