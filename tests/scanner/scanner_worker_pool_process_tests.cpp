#include "seriona/scanner/song_identity.h"
#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] WorkerTask taskFor(std::filesystem::path path) {
  return WorkerTask{.rootPath = "music", .filePath = std::move(path), .cachedLocation = std::nullopt};
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
  cached.lyricsSource = LyricsSource::ExternalLrc;
  cached.externalLrcPath = "music/cached.lrc";
  cached.externalLrcMtimeNs = 99;
  return cached;
}

}

TEST_CASE("scanner worker pool processTask returns cached metadata without tag reader work") {
  std::atomic<std::size_t> readCount{0};
  auto cached = cachedLocationFixture();
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [&readCount](const WorkerTask&) -> SongMetadata {
                                                     ++readCount;
                                                     throw std::runtime_error{"unexpected tag read"};
                                                   }}};

  pool.submitBatch({WorkerTask{.rootPath = cached.rootPath, .filePath = cached.filePath, .cachedLocation = cached}});
  const auto results = pool.waitAll();

  REQUIRE(results.size() == 1U);
  REQUIRE(results[0].metadata.has_value());
  CHECK(readCount.load() == 0U);
  CHECK(results[0].metadata->contentHash == "cached-content-id");
  CHECK(results[0].metadata->filePath == std::filesystem::path{"music/cached.flac"});
  CHECK(results[0].metadata->sourceFilePath == std::filesystem::path{"music/cached-source.flac"});
  CHECK(results[0].metadata->offset == std::chrono::milliseconds{250});
  CHECK(results[0].metadata->effectiveLyricsSource == LyricsSource::ExternalLrc);
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

}
