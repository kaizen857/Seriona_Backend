#include "seriona/scanner/worker_pool.h"

#include "seriona/scanner/song_identity.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <BS_thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace seriona::scanner {
namespace {

constexpr std::size_t kBatchSubmitSize = 64;

using PendingResults = std::vector<std::future<WorkerResult>>;

std::ptrdiff_t normalizeTagReaderSlots(std::ptrdiff_t slots) {
  return std::max<std::ptrdiff_t>(slots, 1);
}

BS::concurrency_t normalizeWorkerCount(std::size_t workerCount) {
  const auto bounded = std::min<std::size_t>(workerCount, std::numeric_limits<BS::concurrency_t>::max());
  return static_cast<BS::concurrency_t>(std::max<std::size_t>(bounded, 1));
}

[[nodiscard]] std::ptrdiff_t boundedTagReaderSlots(std::ptrdiff_t slots, std::size_t workerCount) {
  const auto normalizedWorkerCount = normalizeWorkerCount(workerCount);
  return std::min<std::ptrdiff_t>(normalizeTagReaderSlots(slots), static_cast<std::ptrdiff_t>(normalizedWorkerCount));
}

[[nodiscard]] ScannerError errorFromException(const WorkerTask& task, const std::exception& error) {
  return {.code = ScannerErrorCode::MetadataReadFailed,
          .message = "worker task failed",
          .detail = error.what(),
          .path = task.filePath};
}

[[nodiscard]] ScannerError errorFromUnknownException(const WorkerTask& task) {
  return {.code = ScannerErrorCode::MetadataReadFailed,
          .message = "worker task failed",
          .detail = "unknown exception",
          .path = task.filePath};
}

class TagReaderSlotGuard {
public:
  explicit TagReaderSlotGuard(std::counting_semaphore<>& slots) : slots_{slots} { slots_.acquire(); }
  ~TagReaderSlotGuard() { slots_.release(); }

  TagReaderSlotGuard(const TagReaderSlotGuard&) = delete;
  TagReaderSlotGuard& operator=(const TagReaderSlotGuard&) = delete;

private:
  std::counting_semaphore<>& slots_;
};

[[nodiscard]] std::optional<std::filesystem::file_time_type> fileTimeFromNanoseconds(std::int64_t value) {
  return std::filesystem::file_time_type{std::chrono::nanoseconds{value}};
}

[[nodiscard]] SongMetadata metadataFromCacheLocation(const cache::CachedLocation& cached) {
  SongMetadata metadata{};
  metadata.trackId = cached.locationId;
  metadata.filePath = cached.filePath;
  metadata.fileSizeBytes = cached.fileSizeBytes;
  metadata.fileMtime = fileTimeFromNanoseconds(cached.fileMtimeNs);
  metadata.contentHash = cached.contentId;
  metadata.effectiveLyricsSource = cached.lyricsSource;
  metadata.sourceFilePath = cached.sourceFilePath.empty() ? cached.filePath : cached.sourceFilePath;
  metadata.offset = cached.cueTrackOffset;
  metadata.logicalTrackId = cached.locationId;
  metadata.artworkPath = cached.artworkPath;
  metadata.externalLyricsPath = cached.externalLrcPath;
  if (cached.externalLrcMtimeNs) {
    metadata.externalLyricsMtime = fileTimeFromNanoseconds(*cached.externalLrcMtimeNs);
  }
  return metadata;
}

[[nodiscard]] SongMetadata readProductionMetadata(const WorkerTask& task) {
  ProductionTagMetadataReader reader;
  const auto raw = reader.read(task.filePath, task.rootPath);
  return mapRawTagMetadata(raw, {}, std::nullopt, false).metadata;
}

void assignContentIdentity(SongMetadata& metadata) {
  metadata.contentHash = computeContentId(metadata.duration.value_or(std::chrono::milliseconds{0}), metadata.title,
                                          metadata.artist);
}

[[nodiscard]] double averageMilliseconds(std::chrono::nanoseconds total, std::uint64_t count) {
  if (count == 0) {
    return 0.0;
  }
  return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(total).count()) / 1000.0 /
         static_cast<double>(count);
}

}

std::string formatWorkerPoolStatsBreakdown(const WorkerPoolStatsSnapshot& stats) {
  std::ostringstream output;
  output << "Worker Pool Phase Breakdown\n";
  output << "submittedTasks=" << stats.submittedTasks << '\n';
  output << "cacheHits=" << stats.cacheHits << '\n';
  output << "scannedFiles=" << stats.scannedFiles << '\n';
  output << "tagReaderTimeMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(stats.tagReaderTime).count()
         << '\n';
  output << "hashTimeMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(stats.hashTime).count() << '\n';
  output << "avgTagReaderMs=" << averageMilliseconds(stats.tagReaderTime, stats.scannedFiles) << '\n';
  output << "avgHashMs=" << averageMilliseconds(stats.hashTime, stats.scannedFiles) << '\n';
  return output.str();
}

std::size_t getOptimalWorkerCount() noexcept {
  const auto detected = std::thread::hardware_concurrency();
  if (detected == 0U) {
    return 1U;
  }
  return std::max<std::size_t>(1U, static_cast<std::size_t>(detected));
}

std::ptrdiff_t getOptimalTagReaderLimit(std::size_t workerCount) noexcept {
  const auto normalizedWorkerCount = normalizeWorkerCount(workerCount == 0U ? getOptimalWorkerCount() : workerCount);
  const auto preferred = std::max<BS::concurrency_t>(1U, normalizedWorkerCount / 2U);
  return static_cast<std::ptrdiff_t>(preferred);
}

class ScannerWorkerPool::Impl {
public:
  explicit Impl(Config config)
      : config_{.workerCount = static_cast<std::size_t>(normalizeWorkerCount(config.workerCount == 0U ? getOptimalWorkerCount() : config.workerCount)),
                .tagReaderSlots = boundedTagReaderSlots(config.tagReaderSlots == 0 ? getOptimalTagReaderLimit(config.workerCount) : config.tagReaderSlots,
                                                        config.workerCount == 0U ? getOptimalWorkerCount() : config.workerCount),
                .tagReader = std::move(config.tagReader)},
        workers_{normalizeWorkerCount(config_.workerCount)},
        tagReaderSlots_{config_.tagReaderSlots} {
    if (!config_.tagReader) {
      config_.tagReader = readProductionMetadata;
    }
  }

  [[nodiscard]] const Config& config() const noexcept { return config_; }

  [[nodiscard]] bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

  [[nodiscard]] WorkerPoolStatsSnapshot statsSnapshot() const noexcept {
    return WorkerPoolStatsSnapshot{.submittedTasks = submittedTasks_.load(std::memory_order_relaxed),
                                   .cacheHits = cacheHits_.load(std::memory_order_relaxed),
                                   .scannedFiles = scannedFiles_.load(std::memory_order_relaxed),
                                   .tagReaderTime = std::chrono::nanoseconds{
                                       tagReaderTimeNs_.load(std::memory_order_relaxed)},
                                   .hashTime = std::chrono::nanoseconds{hashTimeNs_.load(std::memory_order_relaxed)}};
  }

  void submitBatch(std::vector<WorkerTask> tasks) {
    if (tasks.empty()) {
      return;
    }

    std::lock_guard lock{mutex_};
    if (cancelled_.load(std::memory_order_acquire)) {
      throw std::runtime_error{"scanner worker pool is cancelled"};
    }

    submittedTasks_.fetch_add(tasks.size(), std::memory_order_relaxed);
    pending_.reserve(pending_.size() + tasks.size());
    for (auto batchStart = tasks.begin(); batchStart != tasks.end();) {
      const auto remaining = static_cast<std::size_t>(std::distance(batchStart, tasks.end()));
      const auto batchCount = std::min(kBatchSubmitSize, remaining);
      const auto batchEnd = std::next(batchStart, static_cast<std::ptrdiff_t>(batchCount));
      for (auto task = batchStart; task != batchEnd; ++task) {
        pending_.push_back(workers_.submit_task([this, task = std::move(*task)] { return processTask(task); }));
      }
      batchStart = batchEnd;
    }
  }

  [[nodiscard]] std::vector<WorkerResult> waitAll() {
    auto pending = takePending();
    waitForCompletion(pending);

    std::vector<WorkerResult> results;
    results.reserve(pending.size());
    std::vector<ScannerError> errors;
    for (auto& future : pending) {
      auto result = future.get();
      if (result.error) {
        errors.push_back(*result.error);
      }
      results.push_back(std::move(result));
    }
    storeErrors(std::move(errors));
    return results;
  }

  void cancel() {
    cancelled_.store(true, std::memory_order_release);
    auto pending = takePending();
    waitForCompletion(pending);
    std::vector<ScannerError> errors;
    for (auto& future : pending) {
      auto result = future.get();
      if (result.error) {
        errors.push_back(*result.error);
      }
    }
    storeErrors(std::move(errors));
  }

  [[nodiscard]] std::vector<ScannerError> errorsSnapshot() const {
    std::lock_guard lock{mutex_};
    return errors_;
  }

private:
  [[nodiscard]] PendingResults takePending() {
    std::lock_guard lock{mutex_};
    return std::exchange(pending_, {});
  }

  void storeErrors(std::vector<ScannerError> errors) {
    std::lock_guard lock{mutex_};
    errors_ = std::move(errors);
  }

  static void waitForCompletion(PendingResults& pending) {
    std::size_t completed = 0;
    std::vector<bool> observed(pending.size(), false);
    while (completed < pending.size()) {
      for (std::size_t index = 0; index < pending.size(); ++index) {
        if (!observed[index] && pending[index].wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
          observed[index] = true;
          ++completed;
        }
      }
      if (completed < pending.size()) {
        std::this_thread::yield();
      }
    }
  }

  [[nodiscard]] WorkerResult processTask(const WorkerTask& task) {
    try {
      if (task.cachedLocation) {
        cacheHits_.fetch_add(1, std::memory_order_relaxed);
        return WorkerResult{.filePath = task.filePath,
                            .metadata = metadataFromCacheLocation(*task.cachedLocation),
                            .error = std::nullopt};
      }

      TagReaderSlotGuard slot{tagReaderSlots_};
      const auto tagReaderStart = std::chrono::steady_clock::now();
      auto metadata = config_.tagReader(task);
      const auto tagReaderElapsed = std::chrono::steady_clock::now() - tagReaderStart;
      tagReaderTimeNs_.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(tagReaderElapsed).count(),
                                 std::memory_order_relaxed);
      scannedFiles_.fetch_add(1, std::memory_order_relaxed);
      assignContentIdentity(metadata);
      return WorkerResult{.filePath = task.filePath, .metadata = std::move(metadata), .error = std::nullopt};
    } catch (const std::exception& error) {
      return WorkerResult{.filePath = task.filePath, .metadata = std::nullopt, .error = errorFromException(task, error)};
    } catch (...) {
      return WorkerResult{.filePath = task.filePath, .metadata = std::nullopt, .error = errorFromUnknownException(task)};
    }
  }

  Config config_{};
  BS::thread_pool workers_;
  std::counting_semaphore<> tagReaderSlots_;
  std::vector<std::future<WorkerResult>> pending_{};
  mutable std::mutex mutex_{};
  std::vector<ScannerError> errors_{};
  std::atomic<bool> cancelled_{false};
  std::atomic<std::uint64_t> submittedTasks_{0};
  std::atomic<std::uint64_t> cacheHits_{0};
  std::atomic<std::uint64_t> scannedFiles_{0};
  std::atomic<std::uint64_t> tagReaderTimeNs_{0};
  std::atomic<std::uint64_t> hashTimeNs_{0};
};

ScannerWorkerPool::ScannerWorkerPool(Config config) : impl_{std::make_unique<Impl>(config)} {}

ScannerWorkerPool::~ScannerWorkerPool() = default;

ScannerWorkerPool::ScannerWorkerPool(ScannerWorkerPool&&) noexcept = default;

ScannerWorkerPool& ScannerWorkerPool::operator=(ScannerWorkerPool&&) noexcept = default;

const ScannerWorkerPool::Config& ScannerWorkerPool::config() const noexcept { return impl_->config(); }

void ScannerWorkerPool::submitBatch(std::vector<WorkerTask> tasks) { impl_->submitBatch(std::move(tasks)); }

std::vector<WorkerResult> ScannerWorkerPool::waitAll() { return impl_->waitAll(); }

void ScannerWorkerPool::cancel() { impl_->cancel(); }

bool ScannerWorkerPool::isCancelled() const noexcept { return impl_->isCancelled(); }

std::vector<ScannerError> ScannerWorkerPool::errorsSnapshot() const { return impl_->errorsSnapshot(); }

WorkerPoolStatsSnapshot ScannerWorkerPool::statsSnapshot() const noexcept { return impl_->statsSnapshot(); }

}
