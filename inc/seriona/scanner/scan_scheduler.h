#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <vector>

namespace seriona::scanner {

struct ScanSchedulerConfig {
  std::size_t workerCount{1};
  std::size_t queueCapacity{64};
};

enum class ScanTaskStatus {
  Completed,
  Failed,
  Cancelled,
};

struct ScanTaskResult {
  ScanTaskStatus status{ScanTaskStatus::Completed};
  std::string taskName;
  std::exception_ptr exception{};
};

class ScanCancellationToken {
public:
  [[nodiscard]] bool stopRequested() const noexcept;

private:
  friend class ScanScheduler;
  explicit ScanCancellationToken(const std::atomic_bool* stopRequested) noexcept;
  const std::atomic_bool* stopRequested_{};
};

using ScanTask = std::function<void(ScanCancellationToken)>;

class ScanScheduler {
public:
  explicit ScanScheduler(ScanSchedulerConfig config = {});
  ~ScanScheduler();

  ScanScheduler(const ScanScheduler&) = delete;
  ScanScheduler& operator=(const ScanScheduler&) = delete;
  ScanScheduler(ScanScheduler&&) = delete;
  ScanScheduler& operator=(ScanScheduler&&) = delete;

  [[nodiscard]] bool trySubmit(std::string taskName, ScanTask task);
  [[nodiscard]] bool submit(std::string taskName, ScanTask task);
  void cancel() noexcept;
  void join();
  [[nodiscard]] bool cancellationRequested() const noexcept;
  [[nodiscard]] std::vector<ScanTaskResult> drainResults();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ProgressThrottleConfig {
  std::chrono::milliseconds interval{250};
  std::uint64_t minimumDelta{1};
};

class ProgressThrottle {
public:
  explicit ProgressThrottle(ProgressThrottleConfig config = {});
  [[nodiscard]] bool shouldPublish(const ScanProgress& progress,
                                   std::chrono::steady_clock::time_point now) noexcept;
  void reset() noexcept;

private:
  ProgressThrottleConfig config_{};
  std::optional<std::chrono::steady_clock::time_point> lastPublishedAt_{};
  std::uint64_t lastPublishedCount_{0};
};

}
