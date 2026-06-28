#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] WorkerTask taskFor(std::string name) {
  return WorkerTask{.rootPath = "music", .filePath = std::filesystem::path{"music"} / std::move(name), .cachedLocation = std::nullopt};
}

[[nodiscard]] SongMetadata metadataFixture(const std::filesystem::path& path) {
  SongMetadata metadata{};
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.title = path.filename().string();
  metadata.artist = "Config Artist";
  metadata.duration = std::chrono::milliseconds{77};
  return metadata;
}

class BlockingReader final {
public:
  SongMetadata read(const WorkerTask& task) {
    {
      std::lock_guard lock{mutex_};
      ++started_;
    }
    startedChanged_.notify_all();

    std::unique_lock lock{mutex_};
    releasedChanged_.wait(lock, [this] { return released_; });
    return metadataFixture(task.filePath);
  }

  void waitForStarted(std::size_t count) {
    std::unique_lock lock{mutex_};
    startedChanged_.wait(lock, [this, count] { return started_ >= count; });
  }

  void release() {
    {
      std::lock_guard lock{mutex_};
      released_ = true;
    }
    releasedChanged_.notify_all();
  }

private:
  std::mutex mutex_{};
  std::condition_variable startedChanged_{};
  std::condition_variable releasedChanged_{};
  std::size_t started_{0};
  bool released_{false};
};

}

TEST_CASE("scanner worker pool config normalizes zero values to tuned defaults") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 0,
                                                   .tagReaderSlots = 0,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath);
                                                   }}};

  CHECK(pool.config().workerCount == getOptimalWorkerCount());
  CHECK(pool.config().tagReaderSlots == getOptimalTagReaderLimit(pool.config().workerCount));
  CHECK(pool.config().workerCount >= 1U);
  CHECK(pool.config().tagReaderSlots >= 1);
}

TEST_CASE("scanner worker pool config caps tag reader slots to worker count") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 99,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     return metadataFixture(task.filePath);
                                                   }}};

  CHECK(pool.config().workerCount == 2U);
  CHECK(pool.config().tagReaderSlots == 2);
  CHECK(getOptimalTagReaderLimit(1) == 1);
}

TEST_CASE("scanner worker pool cancel is cooperative and rejects new submissions") {
  BlockingReader reader{};
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 1,
                                                   .tagReaderSlots = 1,
                                                   .tagReader = [&reader](const WorkerTask& task) {
                                                     return reader.read(task);
                                                   }}};

  pool.submitBatch({taskFor("running.flac")});
  reader.waitForStarted(1);

  std::thread cancelThread{[&pool] { pool.cancel(); }};
  reader.release();
  cancelThread.join();

  CHECK(pool.isCancelled());
  CHECK(pool.waitAll().empty());
  CHECK_THROWS_AS(pool.submitBatch({taskFor("rejected.flac")}), std::runtime_error);
}

TEST_CASE("scanner worker pool waitAll collects successes and aggregates task errors") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2,
                                                   .tagReaderSlots = 2,
                                                   .tagReader = [](const WorkerTask& task) {
                                                     if (task.filePath.filename() == "broken.flac") {
                                                       throw std::runtime_error{"config read failed"};
                                                     }
                                                     return metadataFixture(task.filePath);
                                                   }}};

  pool.submitBatch({taskFor("ok-1.flac"), taskFor("broken.flac"), taskFor("ok-2.flac")});
  const auto results = pool.waitAll();
  const auto errors = pool.errorsSnapshot();

  REQUIRE(results.size() == 3U);
  const auto* brokenResult = static_cast<const WorkerResult*>(nullptr);
  std::size_t successes = 0;
  for (const auto& result : results) {
    if (result.metadata.has_value()) {
      ++successes;
    }
    if (result.filePath.filename() == "broken.flac") {
      brokenResult = &result;
    }
  }
  CHECK(successes == 2U);
  REQUIRE(brokenResult != nullptr);
  REQUIRE(brokenResult->error.has_value());
  CHECK(brokenResult->error->detail.find("config read failed") != std::string::npos);
  REQUIRE(errors.size() == 1U);
  REQUIRE(errors[0].path.has_value());
  CHECK(*errors[0].path == std::filesystem::path{"music/broken.flac"});
  CHECK(errors[0].detail.find("config read failed") != std::string::npos);

  pool.submitBatch({taskFor("after-error.flac")});
  const auto next = pool.waitAll();
  CHECK(next.size() == 1U);
  CHECK(pool.errorsSnapshot().empty());
}

}
