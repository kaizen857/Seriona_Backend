#include "scanner_test_harness.h"

#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace seriona::scanner {
namespace {

class ConcurrentEntryProbe {
public:
  void enter(std::size_t index) {
    active_.fetch_add(1U, std::memory_order_relaxed);
    recordActivePeak();
    if (index < expectedBlockedEntries_) {
      std::unique_lock lock{mutex_};
      ++blockedEntries_;
      if (blockedEntries_ == expectedBlockedEntries_) {
        release_ = true;
        changed_.notify_all();
      }
      changed_.wait(lock, [this] { return release_; });
    }
  }

  void leave() noexcept { active_.fetch_sub(1U, std::memory_order_relaxed); }

  [[nodiscard]] std::size_t maxActive() const noexcept { return maxActive_.load(std::memory_order_relaxed); }

private:
  void recordActivePeak() noexcept {
    auto observed = active_.load(std::memory_order_relaxed);
    auto peak = maxActive_.load(std::memory_order_relaxed);
    while (observed > peak && !maxActive_.compare_exchange_weak(peak, observed, std::memory_order_relaxed)) {
    }
  }

  static constexpr auto expectedBlockedEntries_ = 4U;
  std::atomic_size_t active_{0U};
  std::atomic_size_t maxActive_{0U};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t blockedEntries_{0U};
  bool release_{false};
};

class ProbeGuard {
public:
  ProbeGuard(ConcurrentEntryProbe& probe, std::size_t index) : probe_{probe} { probe_.enter(index); }
  ~ProbeGuard() { probe_.leave(); }

  ProbeGuard(const ProbeGuard&) = delete;
  ProbeGuard& operator=(const ProbeGuard&) = delete;

private:
  ConcurrentEntryProbe& probe_;
};

[[nodiscard]] std::string trackName(std::size_t index) {
  auto number = std::to_string(index);
  while (number.size() < 4U) {
    number.insert(number.begin(), '0');
  }
  return "track-" + number + ".flac";
}

[[nodiscard]] SongMetadata stressMetadata(const WorkerTask& task) {
  SongMetadata metadata{};
  metadata.filePath = task.filePath;
  metadata.trackId = task.filePath.generic_string();
  metadata.logicalTrackId = metadata.trackId;
  metadata.sourceFilePath = task.filePath;
  metadata.title = task.filePath.stem().generic_string();
  metadata.artist = "Phase 2 TSAN Artist";
  metadata.album = "Phase 2 TSAN Stress";
  metadata.duration = std::chrono::milliseconds{120000};
  metadata.sampleRate = 48000;
  metadata.bitDepth = 24;
  metadata.channels = 2;
  return metadata;
}

[[nodiscard]] std::vector<WorkerTask> makeTasks(const std::filesystem::path& root) {
  constexpr auto songCount = 1000U;
  std::vector<WorkerTask> tasks;
  tasks.reserve(songCount);
  for (auto index = 0U; index < songCount; ++index) {
    tasks.push_back(WorkerTask{.rootPath = root, .filePath = root / trackName(index), .cachedLocation = std::nullopt});
  }
  return tasks;
}

}

TEST_CASE("scanner phase2 worker pool survives deterministic tsan stress") {
  constexpr auto songCount = 1000U;
  constexpr auto repeatCount = 10U;
  test::TempScannerRoot temp{"scanner-phase2-tsan-stress"};
  for (auto index = 0U; index < songCount; ++index) {
    static_cast<void>(test::writeAudioFixture(temp.path(), trackName(index)));
  }

  std::atomic_size_t totalReads{0U};
  for (auto repeat = 0U; repeat < repeatCount; ++repeat) {
    ConcurrentEntryProbe probe;
    ScannerWorkerPool workerPool{ScannerWorkerPool::Config{.workerCount = 4U,
                                                           .tagReaderSlots = 4,
                                                           .tagReader = [&probe, &totalReads](const WorkerTask& task) {
                                                             const auto index = totalReads.fetch_add(1U, std::memory_order_relaxed);
                                                             ProbeGuard guard{probe, index % songCount};
                                                             return stressMetadata(task);
                                                           }}};

    workerPool.submitBatch(makeTasks(temp.path()));
    const auto results = workerPool.waitAll();
    const auto stats = workerPool.statsSnapshot();

    REQUIRE(results.size() == songCount);
    CHECK(workerPool.errorsSnapshot().empty());
    CHECK(stats.submittedTasks == songCount);
    CHECK(stats.cacheHits == 0U);
    CHECK(stats.scannedFiles == songCount);
    CHECK(probe.maxActive() == 4U);
    for (auto index = 0U; index < songCount; ++index) {
      REQUIRE(results[index].metadata.has_value());
      CHECK(results[index].filePath == temp.path() / trackName(index));
      CHECK(results[index].metadata->filePath == temp.path() / trackName(index));
      CHECK(results[index].metadata->title == (temp.path() / trackName(index)).stem().generic_string());
      CHECK(!results[index].metadata->contentHash.empty());
    }
  }

  CHECK(totalReads.load(std::memory_order_relaxed) == songCount * repeatCount);
}

}
