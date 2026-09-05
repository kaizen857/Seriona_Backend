#include "seriona/scanner/song_identity.h"
#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] WorkerTask taskFor(std::filesystem::path path) {
  return WorkerTask{.rootPath = "music", .filePath = std::move(path), .locationId = {}, .cachedLocation = std::nullopt};
}

[[nodiscard]] SongMetadata metadataFixture(const std::filesystem::path& path) {
  SongMetadata metadata{};
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.title = "  Loud  Title ";
  metadata.artist = "Artist";
  metadata.duration = std::chrono::milliseconds{1234};
  return metadata;
}

[[nodiscard]] cache::CachedLocation cachedLocationFixture() {
  cache::CachedLocation cached{};
  cached.locationId = "location-id";
  cached.contentId = "cached-content-id";
  cached.rootPath = "music";
  cached.filePath = "music/cached.flac";
  cached.fileSizeBytes = 4096;
  cached.fileMtimeNs = 42;
  cached.sourceFilePath = "music/cached-source.flac";
  cached.cueTrackOffset = std::chrono::milliseconds{250};
  cached.artworkPath = "art/cached.png";
  cached.thumbnailPath = "art/thumbnails/cached.png";
  cached.lyricsSource = LyricsSource::ExternalLrc;
  cached.externalLrcPath = "music/cached.lrc";
  cached.externalLrcMtimeNs = 99;
  return cached;
}

}

TEST_CASE("scanner worker pool processTask invokes tagReader even with cachedLocation") {
  std::atomic<std::size_t> readCount{0};
  auto cached = cachedLocationFixture();
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [&readCount, &cached](const WorkerTask& task) -> SongMetadata {
                                                     ++readCount;
                                                     REQUIRE(task.cachedLocation.has_value());
                                                     CHECK(task.cachedLocation->contentId == cached.contentId);
                                                     SongMetadata meta;
                                                     meta.contentHash = task.cachedLocation->contentId;
                                                     meta.filePath = task.filePath;
                                                     return meta;
                                                   }}};

  pool.submitBatch({WorkerTask{.rootPath = cached.rootPath, .filePath = cached.filePath, .locationId = cached.locationId, .cachedLocation = cached}});
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 1U);
  REQUIRE(results[0].metadata.has_value());
  CHECK(readCount.load() == 1U);
  CHECK(results[0].metadata->contentHash == "cached-content-id");
  CHECK(results[0].metadata->filePath == std::filesystem::path{"music/cached.flac"});
}

TEST_CASE("scanner worker pool processTask reads metadata and computes content identity") {
  std::vector<std::filesystem::path> requestedPaths;
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 1,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [&requestedPaths](const WorkerTask& task) {
                                                     requestedPaths.push_back(task.filePath);
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({taskFor("music/miss.flac")});
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 1U);
  REQUIRE(results[0].metadata.has_value());
  CHECK(requestedPaths == std::vector<std::filesystem::path>{"music/miss.flac"});
  CHECK(results[0].metadata->contentHash == computeContentId(std::chrono::milliseconds{1234}, "  Loud  Title ", "Artist"));
  CHECK(results[0].error == std::nullopt);
}

TEST_CASE("scanner worker pool processTask respects tag reader semaphore limit") {
  std::atomic<int> activeReaders{0};
  std::atomic<int> maxActiveReaders{0};
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 4,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [&activeReaders, &maxActiveReaders](const WorkerTask& task) {
                                                     const int active = activeReaders.fetch_add(1) + 1;
                                                     int observed = maxActiveReaders.load();
                                                     while (active > observed &&
                                                            !maxActiveReaders.compare_exchange_weak(observed, active)) {}
                                                     std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                                     activeReaders.fetch_sub(1);
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({taskFor("music/one.flac"), taskFor("music/two.flac"), taskFor("music/three.flac")});
  const auto results = pool.waitAll();

  CHECK(results.size() == 3U);
  CHECK(maxActiveReaders.load() == 1);
}

TEST_CASE("scanner worker pool processTask records tag reader exceptions as worker errors") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 1,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [](const WorkerTask&) -> SongMetadata {
                                                     throw std::runtime_error{"fake read failure"};
                                                   }}};

  pool.submitBatch({taskFor("music/broken.flac")});
  const auto results = pool.waitAll();
  const auto errors = pool.errorsSnapshot();

  REQUIRE(results.size() == 1U);
  CHECK_FALSE(results[0].metadata.has_value());
  REQUIRE(results[0].error.has_value());
  CHECK(results[0].error->detail.find("fake read failure") != std::string::npos);
  CHECK(errors.size() == 1U);
}

TEST_CASE("scanner worker pool WorkerTask nodeIndex is accessible in callback") {
  std::vector<std::size_t> observedIndices;
  std::vector<std::filesystem::path> observedPaths;
  std::mutex observationMutex;
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [&observedIndices, &observedPaths, &observationMutex](const WorkerTask& task) {
                                                     {
                                                       std::lock_guard<std::mutex> lock{observationMutex};
                                                       observedIndices.push_back(task.nodeIndex);
                                                       observedPaths.push_back(task.filePath);
                                                     }
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({WorkerTask{.rootPath = "music", .filePath = "music/first.flac", .locationId = {}, .cachedLocation = std::nullopt, .nodeIndex = 10},
                    WorkerTask{.rootPath = "music", .filePath = "music/second.flac", .locationId = {}, .cachedLocation = std::nullopt, .nodeIndex = 20},
                    WorkerTask{.rootPath = "music", .filePath = "music/third.flac", .locationId = {}, .cachedLocation = std::nullopt, .nodeIndex = 30}});
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 3U);
  REQUIRE(observedIndices.size() == 3U);
  REQUIRE(observedPaths.size() == 3U);
  CHECK(std::find(observedIndices.begin(), observedIndices.end(), 10) != observedIndices.end());
  CHECK(std::find(observedIndices.begin(), observedIndices.end(), 20) != observedIndices.end());
  CHECK(std::find(observedIndices.begin(), observedIndices.end(), 30) != observedIndices.end());
  CHECK(std::find(observedPaths.begin(), observedPaths.end(), std::filesystem::path{"music/first.flac"}) != observedPaths.end());
  CHECK(std::find(observedPaths.begin(), observedPaths.end(), std::filesystem::path{"music/second.flac"}) != observedPaths.end());
  CHECK(std::find(observedPaths.begin(), observedPaths.end(), std::filesystem::path{"music/third.flac"}) != observedPaths.end());
}

}
