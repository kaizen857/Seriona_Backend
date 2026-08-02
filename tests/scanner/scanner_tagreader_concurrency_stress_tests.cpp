#include "scanner_test_harness.h"

#include "seriona/scanner/tag_reader_metadata_adapter.h"
#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::scanner {
namespace {

constexpr auto kWorkerCount = 16U;
constexpr auto kSongCount = 64U;
constexpr auto kRepeatCount = 3U;
constexpr auto kDefaultConcurrencyLimits = std::array{1, 2, 4, 8, 16};

class ActiveReadProbe {
public:
  void enter() noexcept {
    const auto active = activeReads_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    auto peak = maxActiveReads_.load(std::memory_order_relaxed);
    while (active > peak && !maxActiveReads_.compare_exchange_weak(peak, active, std::memory_order_relaxed)) {
    }
  }

  void leave() noexcept { activeReads_.fetch_sub(1U, std::memory_order_relaxed); }

  [[nodiscard]] std::size_t maxActiveReads() const noexcept { return maxActiveReads_.load(std::memory_order_relaxed); }

private:
  std::atomic_size_t activeReads_{0U};
  std::atomic_size_t maxActiveReads_{0U};
};

class ActiveReadGuard {
public:
  explicit ActiveReadGuard(ActiveReadProbe& probe) : probe_{probe} { probe_.enter(); }
  ~ActiveReadGuard() { probe_.leave(); }

  ActiveReadGuard(const ActiveReadGuard&) = delete;
  ActiveReadGuard& operator=(const ActiveReadGuard&) = delete;

private:
  ActiveReadProbe& probe_;
};

[[nodiscard]] std::string trackName(std::size_t index) {
  auto number = std::to_string(index);
  while (number.size() < 4U) {
    number.insert(number.begin(), '0');
  }
  return "tagreader-stress-" + number + ".flac";
}

[[nodiscard]] std::vector<WorkerTask> makeTasks(const std::filesystem::path& root) {
  std::vector<WorkerTask> tasks;
  tasks.reserve(kSongCount);
  for (auto index = 0U; index < kSongCount; ++index) {
    tasks.push_back(WorkerTask{.rootPath = root, .filePath = root / trackName(index), .cachedLocation = std::nullopt});
  }
  return tasks;
}

[[nodiscard]] std::vector<int> configuredConcurrencyLimits() {
  std::vector<int> limits;
  const auto* configured = std::getenv("SERIONA_TAGREADER_CONCURRENCY");
  if (configured != nullptr) {
    auto value = 0;
    const auto digits = std::string_view{configured};
    const auto* begin = digits.data();
    const auto* end = digits.data() + digits.size();
    if (std::from_chars(begin, end, value).ptr == end && std::ranges::find(kDefaultConcurrencyLimits, value) != kDefaultConcurrencyLimits.end()) {
      limits.push_back(value);
    }
  }
  if (limits.empty()) {
    limits.assign(kDefaultConcurrencyLimits.begin(), kDefaultConcurrencyLimits.end());
  }
  std::ranges::sort(limits);
  limits.erase(std::ranges::unique(limits).begin(), limits.end());
  return limits;
}

void runConcurrencyStress(const int tagReaderSlots) {
  test::TempScannerRoot temp{"scanner-tagreader-concurrency-stress"};
  for (auto index = 0U; index < kSongCount; ++index) {
    static_cast<void>(test::writeAudioFixture(temp.path(), trackName(index)));
  }
  std::filesystem::create_directory(temp.path() / "covers");

  ActiveReadProbe probe;
  ProductionTagMetadataReader tagReader;
  std::atomic_size_t attemptedReads{0U};
  for (auto repeat = 0U; repeat < kRepeatCount; ++repeat) {
    ScannerWorkerPool workerPool{ScannerWorkerPool::Config{.workerCount = kWorkerCount,
                                                           .tagReaderSlots = tagReaderSlots,
                                                           .tagReader = [&probe, &tagReader, &attemptedReads, &temp](const WorkerTask& task) {
                                                             ActiveReadGuard guard{probe};
                                                             attemptedReads.fetch_add(1U, std::memory_order_relaxed);
                                                             const auto raw = tagReader.read(thumbnailOnlyRequest(task.filePath, temp.path() / "covers"));
                                                             return mapRawTagMetadata(raw, "tagreader-stress:" + task.filePath.generic_string(),
                                                                                      std::nullopt, false).metadata;
                                                           }}};

    workerPool.submitBatch(makeTasks(temp.path()));
    const auto results = workerPool.waitAll();
    const auto stats = workerPool.statsSnapshot();

    REQUIRE(results.size() == kSongCount);
    CHECK(workerPool.errorsSnapshot().size() == kSongCount);
    CHECK(stats.submittedTasks == kSongCount);
    CHECK(stats.scannedFiles == 0U);
    CHECK(stats.cacheHits == 0U);
    for (auto index = 0U; index < kSongCount; ++index) {
      CHECK(results[index].filePath == temp.path() / trackName(index));
      CHECK_FALSE(results[index].metadata.has_value());
      REQUIRE(results[index].error.has_value());
      CHECK(results[index].error->code == ScannerErrorCode::MetadataReadFailed);
      REQUIRE(results[index].error->path.has_value());
      CHECK(*results[index].error->path == temp.path() / trackName(index));
    }
  }

  CHECK(attemptedReads.load(std::memory_order_relaxed) == kSongCount * kRepeatCount);
  CHECK(probe.maxActiveReads() <= static_cast<std::size_t>(tagReaderSlots));
  std::cout << "tagreader_concurrency=" << tagReaderSlots << " songs=" << kSongCount
            << " repeats=" << kRepeatCount << " worker_count=" << kWorkerCount
            << " max_active_reads=" << probe.maxActiveReads() << '\n';
}

}

TEST_CASE("tagreader production reader survives concurrent worker pool stress") {
  const auto limits = configuredConcurrencyLimits();
  REQUIRE(!limits.empty());
  for (const auto limit : limits) {
    CAPTURE(limit);
    REQUIRE(limit > 0);
    runConcurrencyStress(limit);
  }
}

}
