#include "seriona/scanner/worker_pool.h"

#include <doctest.h>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace seriona::scanner {

TEST_CASE("scanner worker pool constructs with explicit concurrency limits") {
  const ScannerWorkerPool::Config config{.workerCount = 2, .tagReaderSlots = 1};

  ScannerWorkerPool pool{config};

  CHECK(pool.config().workerCount == 2);
  CHECK(pool.config().tagReaderSlots == 1);
}

TEST_CASE("scanner worker pool empty batch waits with empty results") {
  ScannerWorkerPool pool{ScannerWorkerPool::Config{.workerCount = 2, .tagReaderSlots = 1}};

  pool.submitBatch({});
  const std::vector<WorkerResult> results = pool.waitAll();

  CHECK(results.empty());
}

TEST_CASE("scanner worker pool destructs with no queued work") {
  const ScannerWorkerPool::Config config{.workerCount = 1, .tagReaderSlots = 1};

  { ScannerWorkerPool pool{config}; }

  CHECK(true);
}

}
