#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] cache::CachedLocation cachedLocationFor(std::size_t index) {
  cache::CachedLocation cached{};
  cached.locationId = "cached-" + std::to_string(index);
  cached.contentId = "content-" + std::to_string(index);
  cached.rootPath = "music";
  cached.filePath = "music/cached-" + std::to_string(index) + ".flac";
  cached.fileSizeBytes = 4096;
  cached.fileMtimeNs = static_cast<std::int64_t>(index + 1U);
  return cached;
}

[[nodiscard]] WorkerTask cachedTask(std::size_t index) {
  auto cached = cachedLocationFor(index);
  return WorkerTask{.rootPath = cached.rootPath, .filePath = cached.filePath, .cachedLocation = std::move(cached)};
}

[[nodiscard]] WorkerTask scanTask(std::size_t index) {
  return WorkerTask{.rootPath = "music", .filePath = "music/scanned-" + std::to_string(index) + ".flac", .cachedLocation = std::nullopt};
}

[[nodiscard]] std::vector<WorkerTask> mixedBatch(std::size_t cacheHits, std::size_t scans) {
  std::vector<WorkerTask> tasks;
  tasks.reserve(cacheHits + scans);
  for (std::size_t index = 0; index < cacheHits; ++index) {
    tasks.push_back(cachedTask(index));
  }
  for (std::size_t index = 0; index < scans; ++index) {
    tasks.push_back(scanTask(index));
  }
  return tasks;
}

[[nodiscard]] SongMetadata metadataFixture(const std::filesystem::path& path) {
  SongMetadata metadata{};
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.title = path.filename().string();
  metadata.artist = "Stats Artist";
  metadata.duration = std::chrono::milliseconds{50};
  return metadata;
}

}

TEST_CASE("scanner worker pool stats count cache hits across one hundred tasks") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 4,
                                                   .tagReaderSlots = 4,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch(mixedBatch(80, 20));
  const auto results = pool.waitAll();
  const auto stats = pool.statsSnapshot();

  CHECK(results.size() == 100U);
  CHECK(stats.submittedTasks == 100U);
  CHECK(stats.completedTasks == 100U);
  CHECK(stats.cacheHits == 80U);
  CHECK(stats.scannedFiles == 20U);
}

TEST_CASE("scanner worker pool progress callback reports a monotonically increasing completion count") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 4,
                                                   .tagReaderSlots = 4,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath);
                                                   }}};

  std::uint64_t lastReported = 0;
  std::uint64_t reportCount = 0;
  std::vector<std::uint64_t> reports;
  pool.submitBatch(mixedBatch(0, 10));
  const auto results = pool.waitAll([&](std::uint64_t completed) {
    lastReported = completed;
    ++reportCount;
    reports.push_back(completed);
  });

  CHECK(results.size() == 10U);
  REQUIRE(reportCount > 0U);
  REQUIRE(reports.size() == reportCount);
  for (std::size_t index = 1; index < reports.size(); ++index) {
    CHECK(reports[index] >= reports[index - 1]);
  }
  CHECK(lastReported == 10U);
  CHECK(pool.statsSnapshot().completedTasks == 10U);
}

TEST_CASE("scanner worker pool stats accumulate tag reader time across worker tasks") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({scanTask(1), scanTask(2), scanTask(3)});
  const auto results = pool.waitAll();
  const auto stats = pool.statsSnapshot();
  const auto breakdown = formatWorkerPoolStatsBreakdown(stats);

  CHECK(results.size() == 3U);
  CHECK(stats.scannedFiles == 3U);
  CHECK(stats.tagReaderTime > std::chrono::nanoseconds{0});
  CHECK(breakdown.find("Worker Pool Phase Breakdown") != std::string::npos);
  CHECK(breakdown.find("tagReaderTimeMs=") != std::string::npos);
  CHECK(breakdown.find("avgTagReaderMs=") != std::string::npos);
}

TEST_CASE("scanner worker pool stats snapshot remains reusable after multiple batches") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({cachedTask(1), scanTask(1)});
  const auto first = pool.waitAll();
  const auto firstStats = pool.statsSnapshot();

  pool.submitBatch({cachedTask(2), cachedTask(3), scanTask(2)});
  const auto second = pool.waitAll();
  const auto secondStats = pool.statsSnapshot();

  CHECK(first.size() == 2U);
  CHECK(second.size() == 3U);
  CHECK(firstStats.submittedTasks == 2U);
  CHECK(firstStats.cacheHits == 1U);
  CHECK(firstStats.scannedFiles == 1U);
  CHECK(secondStats.submittedTasks == 5U);
  CHECK(secondStats.cacheHits == 3U);
  CHECK(secondStats.scannedFiles == 2U);
  CHECK(secondStats.tagReaderTime >= firstStats.tagReaderTime);
}

}
