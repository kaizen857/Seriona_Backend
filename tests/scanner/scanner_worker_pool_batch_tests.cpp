#include "seriona/scanner/song_identity.h"
#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] WorkerTask taskFor(std::size_t index) {
  return WorkerTask{.rootPath = "music", .filePath = "music/track-" + std::to_string(index) + ".flac", .cachedLocation = std::nullopt};
}

[[nodiscard]] std::vector<WorkerTask> taskBatch(std::size_t count) {
  std::vector<WorkerTask> tasks;
  tasks.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    tasks.push_back(taskFor(index));
  }
  return tasks;
}

[[nodiscard]] SongMetadata metadataFixture(const std::filesystem::path& path, std::chrono::milliseconds duration) {
  SongMetadata metadata{};
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.title = path.filename().string();
  metadata.artist = "Batch Artist";
  metadata.duration = duration;
  return metadata;
}

class CompletionGate {
public:
  explicit CompletionGate(std::size_t releaseCount) : releaseCount_{releaseCount} {}

  void arriveAndWait() {
    std::unique_lock lock{mutex_};
    ++arrived_;
    maxArrived_ = std::max(maxArrived_, arrived_);
    if (arrived_ >= releaseCount_) {
      released_ = true;
      ready_.notify_all();
      return;
    }
    ready_.wait(lock, [this] { return released_; });
  }

  [[nodiscard]] std::size_t maxArrived() const {
    std::lock_guard lock{mutex_};
    return maxArrived_;
  }

private:
  std::size_t releaseCount_;
  mutable std::mutex mutex_{};
  std::condition_variable ready_{};
  std::size_t arrived_{0};
  std::size_t maxArrived_{0};
  bool released_{false};
};

}

TEST_CASE("scanner worker pool batch collects one hundred submitted results") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 4,
                                                   .tagReaderSlots = 4,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath, std::chrono::milliseconds{10});
                                                   }}};

  pool.submitBatch(taskBatch(100));
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 100U);
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto expectedPath = std::filesystem::path{"music/track-" + std::to_string(index) + ".flac"};
    CHECK(results[index].filePath == expectedPath);
    REQUIRE(results[index].metadata.has_value());
    CHECK(results[index].metadata->contentHash == computeContentId(std::chrono::milliseconds{10},
                                                                    expectedPath.filename().string(), "Batch Artist"));
  }
}

TEST_CASE("scanner worker pool batch clears pending futures after a failed task") {
  std::atomic<std::size_t> reads{0};
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [&reads](const WorkerTask& task) {
                                                     const auto current = reads.fetch_add(1);
                                                     if (current == 1U) {
                                                       throw std::runtime_error{"batch read failed"};
                                                     }
                                                     return metadataFixture(task.filePath, std::chrono::milliseconds{20});
                                                   }}};

  pool.submitBatch(taskBatch(3));
  const auto first = pool.waitAll();
  CHECK(first.size() == 3U);
  CHECK(pool.errorsSnapshot().size() == 1U);

  pool.submitBatch({taskFor(10)});
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 1U);
  CHECK(results[0].filePath == std::filesystem::path{"music/track-10.flac"});
  CHECK(results[0].metadata.has_value());
}

TEST_CASE("scanner worker pool batch can be reused after successful collection") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath, std::chrono::milliseconds{30});
                                                   }}};

  pool.submitBatch(taskBatch(2));
  const auto first = pool.waitAll();
  pool.submitBatch({taskFor(20), taskFor(21), taskFor(22)});
  const auto second = pool.waitAll();

  CHECK(first.size() == 2U);
  REQUIRE(second.size() == 3U);
  CHECK(second[0].filePath == std::filesystem::path{"music/track-20.flac"});
  CHECK(second[2].filePath == std::filesystem::path{"music/track-22.flac"});
}

TEST_CASE("scanner worker pool batch observes concurrent completion without timing dependence") {
  CompletionGate gate{3};
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 3,
                                                   .tagReaderSlots = 3,
                                                   .tagReader = [&gate](const WorkerTask& task) {
                                                     gate.arriveAndWait();
                                                     return metadataFixture(task.filePath, std::chrono::milliseconds{40});
                                                   }}};

  pool.submitBatch(taskBatch(3));
  const auto results = pool.waitAll();

  CHECK(results.size() == 3U);
  CHECK(gate.maxArrived() == 3U);
}

}
