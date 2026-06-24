#include "seriona/scanner/scan_scheduler.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace seriona::scanner {
namespace {

struct QueuedTask {
  std::string name;
  ScanTask task;
};

[[nodiscard]] std::size_t normalizedWorkerCount(const std::size_t workerCount) noexcept {
  return std::max<std::size_t>(1U, workerCount);
}

[[nodiscard]] std::size_t normalizedQueueCapacity(const std::size_t queueCapacity) noexcept {
  return std::max<std::size_t>(1U, queueCapacity);
}

}

struct ScanScheduler::Impl {
  explicit Impl(ScanSchedulerConfig config)
      : workerCount(normalizedWorkerCount(config.workerCount)),
        queueCapacity(normalizedQueueCapacity(config.queueCapacity)) {
    workers.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
      workers.emplace_back([this] { workerLoop(); });
    }
  }

  ~Impl() {
    cancel();
    join();
  }

  std::size_t workerCount{1};
  std::size_t queueCapacity{1};
  std::deque<QueuedTask> queue{};
  std::vector<ScanTaskResult> results{};
  std::vector<std::thread> workers{};
  mutable std::mutex mutex{};
  std::condition_variable queueChanged{};
  std::condition_variable capacityChanged{};
  std::condition_variable idleChanged{};
  std::atomic_bool stopping{false};
  std::size_t activeWorkers{0};

  [[nodiscard]] bool trySubmit(std::string taskName, ScanTask task) {
    std::lock_guard lock{mutex};
    if (stopping.load() || queue.size() >= queueCapacity) {
      spdlog::debug("scheduler trySubmit '{}' rejected (stopping={} q={}/{})", taskName, stopping.load(), queue.size(),
                    queueCapacity);
      return false;
    }
    queue.push_back({.name = std::move(taskName), .task = std::move(task)});
    spdlog::debug("scheduler trySubmit '{}' accepted (q={}/{})", queue.back().name, queue.size(), queueCapacity);
    queueChanged.notify_one();
    return true;
  }

  [[nodiscard]] bool submit(std::string taskName, ScanTask task) {
    std::unique_lock lock{mutex};
    capacityChanged.wait(lock, [this] { return stopping.load() || queue.size() < queueCapacity; });
    if (stopping.load()) {
      spdlog::debug("scheduler submit '{}' rejected (stopping)", taskName);
      return false;
    }
    queue.push_back({.name = std::move(taskName), .task = std::move(task)});
    spdlog::debug("scheduler submit '{}' accepted (q={}/{})", queue.back().name, queue.size(), queueCapacity);
    lock.unlock();
    queueChanged.notify_one();
    return true;
  }

  void cancel() noexcept {
    {
      std::lock_guard lock{mutex};
      stopping.store(true);
      spdlog::debug("scheduler cancelling {} queued tasks", queue.size());
      while (!queue.empty()) {
        auto taskName = std::move(queue.front().name);
        queue.pop_front();
        results.push_back({.status = ScanTaskStatus::Cancelled, .taskName = std::move(taskName)});
      }
    }
    queueChanged.notify_all();
    capacityChanged.notify_all();
    idleChanged.notify_all();
  }

  void join() {
    {
      std::unique_lock lock{mutex};
      spdlog::debug("scheduler waiting for idle (q={} active={})", queue.size(), activeWorkers);
      idleChanged.wait(lock, [this] { return queue.empty() && activeWorkers == 0; });
      stopping.store(true);
    }
    spdlog::debug("scheduler joining {} workers", workers.size());
    queueChanged.notify_all();
    capacityChanged.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  [[nodiscard]] bool cancellationRequested() const noexcept {
    std::lock_guard lock{mutex};
    return stopping.load();
  }

  [[nodiscard]] std::vector<ScanTaskResult> drainResults() {
    std::lock_guard lock{mutex};
    auto drained = std::move(results);
    results.clear();
    return drained;
  }

  void workerLoop() {
    while (true) {
      QueuedTask queued;
      {
        std::unique_lock lock{mutex};
        queueChanged.wait(lock, [this] { return stopping.load() || !queue.empty(); });
        if (queue.empty()) {
          if (stopping.load()) {
            return;
          }
          continue;
        }
        queued = std::move(queue.front());
        queue.pop_front();
        ++activeWorkers;
        capacityChanged.notify_one();
      }

      ScanTaskResult result{.status = ScanTaskStatus::Completed, .taskName = queued.name};
      try {
        if (cancellationRequested()) {
          result.status = ScanTaskStatus::Cancelled;
        } else {
          queued.task(ScanCancellationToken{&stopping});
          if (cancellationRequested()) {
            result.status = ScanTaskStatus::Cancelled;
          }
        }
      } catch (...) {
        result.status = ScanTaskStatus::Failed;
        result.exception = std::current_exception();
      }

      {
        std::lock_guard lock{mutex};
        results.push_back(std::move(result));
        --activeWorkers;
      }
      idleChanged.notify_all();
    }
  }
};

ScanCancellationToken::ScanCancellationToken(const std::atomic_bool* stopRequested) noexcept : stopRequested_(stopRequested) {}

bool ScanCancellationToken::stopRequested() const noexcept {
  return stopRequested_ != nullptr && stopRequested_->load();
}

ScanScheduler::ScanScheduler(ScanSchedulerConfig config) : impl_(std::make_unique<Impl>(config)) {}

ScanScheduler::~ScanScheduler() = default;

bool ScanScheduler::trySubmit(std::string taskName, ScanTask task) {
  return impl_->trySubmit(std::move(taskName), std::move(task));
}

bool ScanScheduler::submit(std::string taskName, ScanTask task) {
  return impl_->submit(std::move(taskName), std::move(task));
}

void ScanScheduler::cancel() noexcept {
  impl_->cancel();
}

void ScanScheduler::join() {
  impl_->join();
}

bool ScanScheduler::cancellationRequested() const noexcept {
  return impl_->cancellationRequested();
}

std::vector<ScanTaskResult> ScanScheduler::drainResults() {
  return impl_->drainResults();
}

ProgressThrottle::ProgressThrottle(ProgressThrottleConfig config) : config_(config) {}

bool ProgressThrottle::shouldPublish(const ScanProgress& progress,
                                     const std::chrono::steady_clock::time_point now) noexcept {
  const auto currentCount = progress.filesScanned + progress.filesSkipped + progress.errors;
  if (!lastPublishedAt_.has_value()) {
    lastPublishedAt_ = now;
    lastPublishedCount_ = currentCount;
    return true;
  }

  const auto countDelta = currentCount > lastPublishedCount_ ? currentCount - lastPublishedCount_ : 0U;
  const auto timeDelta = now - *lastPublishedAt_;
  if (countDelta < config_.minimumDelta && timeDelta < config_.interval) {
    return false;
  }

  lastPublishedAt_ = now;
  lastPublishedCount_ = currentCount;
  return true;
}

void ProgressThrottle::reset() noexcept {
  lastPublishedAt_.reset();
  lastPublishedCount_ = 0;
}

}
