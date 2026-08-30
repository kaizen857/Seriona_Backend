#include "control_event_loop.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <utility>

namespace seriona::control {

namespace {

void discardUnhandledException(const std::exception_ptr& exception) noexcept {
  (void)exception;
}

}

ControlEventLoop::ControlEventLoop(MediaControllerOptions options) : options_(options) {}

ControlEventLoop::~ControlEventLoop() { stop(); }

void ControlEventLoop::start() {
  std::lock_guard lock{mutex_};
  if (started_ || stopping_) {
    return;
  }

  started_ = true;
  spdlog::info("control event loop started");
  if (!options_.runInlineForTests) {
    worker_ = std::thread{[this] { workerMain(); }};
  }
}

bool ControlEventLoop::post(std::function<void()> work) noexcept {
  if (!work) {
    return true;
  }

  try {
    {
      std::lock_guard lock{mutex_};
      if (stopping_) {
        spdlog::warn("event loop queue rejected: loop is stopping");
        return false;
      }

      queue_.push_back(std::move(work));
    }
    workAvailable_.notify_one();
    return true;
  } catch (...) {
    spdlog::error("event loop queue push failed with exception");
    discardUnhandledException(std::current_exception());
    return false;
  }
}

void ControlEventLoop::drainForTests() {
  if (!options_.runInlineForTests) {
    return;
  }

  for (;;) {
    auto work = takeQueuedWork();
    if (work.empty()) {
      return;
    }

    for (auto& task : work) {
      // 与 workerMain 保持一致：任务抛出的异常不得越过事件循环边界。
      // inline 模式（测试）原先裸调 task()，异常会逃逸到调用方；调用方若处于
      // noexcept 上下文即 std::terminate（macOS CI 上表现为 SIGABRT）。
      try {
        task();
      } catch (...) {
        discardUnhandledException(std::current_exception());
      }
    }
  }
}

void ControlEventLoop::stop() noexcept {
  const auto workerThreadId = worker_.joinable() ? worker_.get_id() : std::thread::id{};
  {
    std::lock_guard lock{mutex_};
    if (!stopping_) {
      stopping_ = true;
      queue_.clear();
    }
  }
  workAvailable_.notify_all();

  if (!worker_.joinable()) {
    spdlog::info("control event loop stopped (no worker)");
    return;
  }

  if (workerThreadId == std::this_thread::get_id()) {
    return;
  }

  worker_.join();
  spdlog::info("control event loop stopped");
}

void ControlEventLoop::workerMain() noexcept {
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock lock{mutex_};
      workAvailable_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }

      work = std::move(queue_.front());
      queue_.pop_front();
    }

    try {
      work();
    } catch (...) {
      discardUnhandledException(std::current_exception());
    }
  }
}

std::deque<std::function<void()>> ControlEventLoop::takeQueuedWork() {
  std::lock_guard lock{mutex_};
  if (stopping_ || queue_.empty()) {
    return {};
  }

  std::deque<std::function<void()>> work;
  work.swap(queue_);
  return work;
}

}
