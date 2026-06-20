#include "seriona/scanner/scan_scheduler.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::vector<std::string> taskNames(std::vector<ScanTaskResult> results) {
  std::ranges::sort(results, {}, &ScanTaskResult::taskName);
  std::vector<std::string> names;
  names.reserve(results.size());
  std::ranges::transform(results, std::back_inserter(names), &ScanTaskResult::taskName);
  return names;
}

[[nodiscard]] ScanProgress progressWithScanned(const std::uint64_t filesScanned) noexcept {
  ScanProgress progress{};
  progress.filesScanned = filesScanned;
  return progress;
}

TEST_CASE("scan scheduler completes submitted tasks and fans in results") {
  ScanScheduler scheduler{{.workerCount = 2, .queueCapacity = 4}};
  std::atomic_int total{0};

  CHECK(scheduler.submit("one", [&total](ScanCancellationToken) { total.fetch_add(1); }));
  CHECK(scheduler.submit("two", [&total](ScanCancellationToken) { total.fetch_add(2); }));
  CHECK(scheduler.submit("three", [&total](ScanCancellationToken) { total.fetch_add(3); }));
  scheduler.join();

  CHECK(total.load() == 6);
  const auto results = scheduler.drainResults();
  REQUIRE(results.size() == 3U);
  CHECK(std::ranges::all_of(results, [](const ScanTaskResult& result) {
    return result.status == ScanTaskStatus::Completed;
  }));
  CHECK(taskNames(results) == std::vector<std::string>{"one", "three", "two"});
}

TEST_CASE("scan scheduler captures task exceptions without deadlock") {
  ScanScheduler scheduler{{.workerCount = 1, .queueCapacity = 2}};

  CHECK(scheduler.submit("throws", [](ScanCancellationToken) { throw std::runtime_error{"boom"}; }));
  CHECK(scheduler.submit("continues", [](ScanCancellationToken) {}));
  scheduler.join();

  const auto results = scheduler.drainResults();
  REQUIRE(results.size() == 2U);
  const auto failure = std::ranges::find(results, "throws", &ScanTaskResult::taskName);
  REQUIRE(failure != results.end());
  CHECK(failure->status == ScanTaskStatus::Failed);
  REQUIRE(failure->exception != nullptr);
  CHECK_THROWS_WITH_AS(std::rethrow_exception(failure->exception), "boom", std::runtime_error);
  const auto success = std::ranges::find(results, "continues", &ScanTaskResult::taskName);
  REQUIRE(success != results.end());
  CHECK(success->status == ScanTaskStatus::Completed);
}

TEST_CASE("scan scheduler cancels queued tasks and exposes stop token to running task") {
  ScanScheduler scheduler{{.workerCount = 1, .queueCapacity = 4}};
  std::mutex mutex;
  std::condition_variable started;
  bool running{false};
  bool release{false};
  std::atomic_bool observedStop{false};

  CHECK(scheduler.submit("running", [&](ScanCancellationToken token) {
    {
      std::lock_guard lock{mutex};
      running = true;
    }
    started.notify_one();
    while (!token.stopRequested()) {
      std::unique_lock lock{mutex};
      if (started.wait_for(lock, std::chrono::milliseconds{1}, [&release] { return release; })) {
        break;
      }
    }
    observedStop = token.stopRequested();
  }));
  CHECK(scheduler.submit("queued-a", [](ScanCancellationToken) {}));
  CHECK(scheduler.submit("queued-b", [](ScanCancellationToken) {}));

  {
    std::unique_lock lock{mutex};
    REQUIRE(started.wait_for(lock, std::chrono::seconds{1}, [&running] { return running; }));
  }
  scheduler.cancel();
  scheduler.join();

  CHECK(observedStop.load());
  const auto results = scheduler.drainResults();
  REQUIRE(results.size() == 3U);
  CHECK(std::ranges::count_if(results, [](const ScanTaskResult& result) {
    return result.status == ScanTaskStatus::Cancelled;
  }) == 3);
}

TEST_CASE("scan scheduler queue limit rejects predictably without busy spin") {
  ScanScheduler scheduler{{.workerCount = 1, .queueCapacity = 1}};
  std::mutex mutex;
  std::condition_variable started;
  bool running{false};
  bool release{false};

  CHECK(scheduler.submit("running", [&](ScanCancellationToken) {
    {
      std::lock_guard lock{mutex};
      running = true;
    }
    started.notify_one();
    std::unique_lock lock{mutex};
    started.wait(lock, [&release] { return release; });
  }));
  {
    std::unique_lock lock{mutex};
    REQUIRE(started.wait_for(lock, std::chrono::seconds{1}, [&running] { return running; }));
  }

  CHECK(scheduler.trySubmit("queued", [](ScanCancellationToken) {}));
  CHECK_FALSE(scheduler.trySubmit("overflow", [](ScanCancellationToken) {}));

  {
    std::lock_guard lock{mutex};
    release = true;
  }
  started.notify_all();
  scheduler.join();

  const auto results = scheduler.drainResults();
  REQUIRE(results.size() == 2U);
  CHECK(taskNames(results) == std::vector<std::string>{"queued", "running"});
}

TEST_CASE("scan scheduler submit waits for bounded queue capacity") {
  ScanScheduler scheduler{{.workerCount = 1, .queueCapacity = 1}};
  std::mutex mutex;
  std::condition_variable changed;
  bool running{false};
  bool release{false};
  std::atomic_bool submitted{false};

  CHECK(scheduler.submit("running", [&](ScanCancellationToken) {
    {
      std::lock_guard lock{mutex};
      running = true;
    }
    changed.notify_all();
    std::unique_lock lock{mutex};
    changed.wait(lock, [&release] { return release; });
  }));
  {
    std::unique_lock lock{mutex};
    REQUIRE(changed.wait_for(lock, std::chrono::seconds{1}, [&running] { return running; }));
  }
  CHECK(scheduler.trySubmit("queued", [](ScanCancellationToken) {}));

  std::thread producer{[&] {
    submitted = scheduler.submit("blocked", [](ScanCancellationToken) {});
    changed.notify_all();
  }};
  {
    std::unique_lock lock{mutex};
    CHECK_FALSE(changed.wait_for(lock, std::chrono::milliseconds{20}, [&submitted] { return submitted.load(); }));
    release = true;
  }
  changed.notify_all();
  producer.join();
  scheduler.join();

  CHECK(submitted.load());
  const auto results = scheduler.drainResults();
  REQUIRE(results.size() == 3U);
  CHECK(taskNames(results) == std::vector<std::string>{"blocked", "queued", "running"});
}

TEST_CASE("progress throttle publishes by first event count delta or time interval") {
  ProgressThrottle throttle{{.interval = std::chrono::milliseconds{250}, .minimumDelta = 3}};
  const auto start = std::chrono::steady_clock::time_point{};

  CHECK(throttle.shouldPublish(progressWithScanned(0), start));
  CHECK_FALSE(throttle.shouldPublish(progressWithScanned(1), start + std::chrono::milliseconds{10}));
  CHECK(throttle.shouldPublish(progressWithScanned(3), start + std::chrono::milliseconds{20}));
  CHECK_FALSE(throttle.shouldPublish(progressWithScanned(4), start + std::chrono::milliseconds{30}));
  CHECK(throttle.shouldPublish(progressWithScanned(4), start + std::chrono::milliseconds{300}));

  throttle.reset();
  CHECK(throttle.shouldPublish(progressWithScanned(0), start));
}

}
}
