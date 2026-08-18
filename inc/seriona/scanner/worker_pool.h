#pragma once

#include "seriona/scanner/cache/sqlite_cache.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

struct WorkerTask {
  std::filesystem::path rootPath;
  std::filesystem::path filePath;
  std::string locationId;
  std::optional<cache::CachedLocation> cachedLocation;
  std::size_t nodeIndex{0};
};

struct WorkerResult {
  std::filesystem::path filePath;
  std::optional<SongMetadata> metadata;
  std::optional<ScannerError> error;
};

struct WorkerPoolStatsSnapshot {
  std::uint64_t submittedTasks{0};
  std::uint64_t completedTasks{0};
  std::uint64_t cacheHits{0};
  std::uint64_t scannedFiles{0};
  std::chrono::nanoseconds tagReaderTime{0};
};

[[nodiscard]] std::string formatWorkerPoolStatsBreakdown(const WorkerPoolStatsSnapshot& stats);
[[nodiscard]] std::size_t getOptimalWorkerCount() noexcept;
[[nodiscard]] std::ptrdiff_t getOptimalTagReaderLimit(std::size_t workerCount) noexcept;

class ScannerWorkerPool {
public:
  struct Config {
    std::size_t workerCount{1};
    std::ptrdiff_t tagReaderSlots{1};
    std::function<SongMetadata(const WorkerTask&)> tagReader{};
  };

  explicit ScannerWorkerPool(Config config);
  ~ScannerWorkerPool();

  ScannerWorkerPool(const ScannerWorkerPool&) = delete;
  ScannerWorkerPool& operator=(const ScannerWorkerPool&) = delete;
  ScannerWorkerPool(ScannerWorkerPool&&) noexcept;
  ScannerWorkerPool& operator=(ScannerWorkerPool&&) noexcept;

  [[nodiscard]] const Config& config() const noexcept;
  void submitBatch(std::vector<WorkerTask> batch);
  // Called from the waitAll thread with the finished task count (success or failure).
  using ProgressCallback = std::function<void(std::uint64_t completed)>;
  [[nodiscard]] std::vector<WorkerResult> waitAll(ProgressCallback progressCallback = {});
  void cancel();
  [[nodiscard]] bool isCancelled() const noexcept;
  [[nodiscard]] std::vector<ScannerError> errorsSnapshot() const;
  [[nodiscard]] WorkerPoolStatsSnapshot statsSnapshot() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}
