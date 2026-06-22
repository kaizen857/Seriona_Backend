#pragma once

#include "seriona/control/control_contracts.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace seriona::control {

class ControlEventLoop {
public:
  explicit ControlEventLoop(MediaControllerOptions options = {});
  ~ControlEventLoop();

  ControlEventLoop(const ControlEventLoop&) = delete;
  ControlEventLoop& operator=(const ControlEventLoop&) = delete;

  void start();
  [[nodiscard]] bool post(std::function<void()> work) noexcept;
  void drainForTests();
  void stop() noexcept;

private:
  void workerMain() noexcept;
  [[nodiscard]] std::deque<std::function<void()>> takeQueuedWork();

  MediaControllerOptions options_{};
  std::mutex mutex_{};
  std::condition_variable workAvailable_{};
  std::deque<std::function<void()>> queue_{};
  std::thread worker_{};
  bool started_{false};
  bool stopping_{false};
};

}
