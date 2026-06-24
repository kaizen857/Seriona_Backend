#include "subscription_store.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace seriona::control {

namespace {

void discardUnhandledException(const std::exception_ptr& exception) noexcept {
  (void)exception;
}

class SubscriptionDeliveryWorker {
public:
  SubscriptionDeliveryWorker() : worker_([this] { run(); }) {}

  ~SubscriptionDeliveryWorker() { stop(); }

  SubscriptionDeliveryWorker(const SubscriptionDeliveryWorker&) = delete;
  SubscriptionDeliveryWorker& operator=(const SubscriptionDeliveryWorker&) = delete;

  void submit(std::function<void()> delivery) {
    {
      std::lock_guard lock{mutex_};
      if (stopping_) {
        return;
      }
      deliveries_.push_back(std::move(delivery));
    }
    ready_.notify_one();
  }

  void waitUntilIdle() {
    if (std::this_thread::get_id() == worker_.get_id()) {
      return;
    }
    auto barrier = std::make_shared<std::promise<void>>();
    auto finished = barrier->get_future();
    submit([barrier] { barrier->set_value(); });
    finished.wait();
  }

  void stop() noexcept {
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
      deliveries_.clear();
    }
    ready_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  void run() {
    while (true) {
      std::function<void()> delivery;
      {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return stopping_ || !deliveries_.empty(); });
        if (stopping_ && deliveries_.empty()) {
          return;
        }
        delivery = std::move(deliveries_.front());
        deliveries_.pop_front();
      }
      delivery();
    }
  }

  std::mutex mutex_{};
  std::condition_variable ready_{};
  std::deque<std::function<void()>> deliveries_{};
  std::thread worker_{};
  bool stopping_{false};
};

std::mutex& deliveryRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<const void*, std::shared_ptr<SubscriptionDeliveryWorker>>& deliveryRegistry() {
  static std::unordered_map<const void*, std::shared_ptr<SubscriptionDeliveryWorker>> registry;
  return registry;
}

std::shared_ptr<SubscriptionDeliveryWorker> deliveryWorkerFor(const void* key) {
  std::lock_guard lock{deliveryRegistryMutex()};
  auto& worker = deliveryRegistry()[key];
  if (!worker) {
    worker = std::make_shared<SubscriptionDeliveryWorker>();
  }
  return worker;
}

void removeDeliveryWorker(const void* key) noexcept {
  try {
    std::shared_ptr<SubscriptionDeliveryWorker> worker;
    {
      std::lock_guard lock{deliveryRegistryMutex()};
      auto found = deliveryRegistry().find(key);
      if (found == deliveryRegistry().end()) {
        return;
      }
      worker = std::move(found->second);
      deliveryRegistry().erase(found);
    }
    if (worker) {
      worker->stop();
    }
  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

void waitForDeliveryWorkerIdle(const void* key) noexcept {
  try {
    std::shared_ptr<SubscriptionDeliveryWorker> worker;
    {
      std::lock_guard lock{deliveryRegistryMutex()};
      const auto found = deliveryRegistry().find(key);
      if (found == deliveryRegistry().end()) {
        return;
      }
      worker = found->second;
    }
    if (worker) {
      worker->waitUntilIdle();
    }
  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

}

template <typename Snapshot>
SubscriptionStore<Snapshot>::SubscriptionStore(SubscriptionExceptionReporter exceptionReporter)
    : state_(std::make_shared<State>()) {
  state_->exceptionReporter = std::move(exceptionReporter);
}

template <typename Snapshot>
SubscriptionStore<Snapshot>::~SubscriptionStore() {
  clear();
  removeDeliveryWorker(state_.get());
}

template <typename Snapshot>
SubscriptionHandle SubscriptionStore<Snapshot>::subscribe(Callback callback, std::optional<Snapshot> initialSnapshot) {
  if (!callback) {
    return {};
  }

  const auto state = state_;
  std::size_t subscriptionId = 0;
  {
    std::lock_guard lock{state->mutex};
    subscriptionId = state->nextSubscriptionId++;
    state->subscribers.emplace(subscriptionId, Subscriber{.callback = std::move(callback), .active = true});
  }

  SubscriptionHandle handle{.subscriptionId = subscriptionId,
	                            .unsubscribe = [weakState = std::weak_ptr<State>{state}, subscriptionId] {
	                              if (const auto lockedState = weakState.lock()) {
	                                {
	                                  std::lock_guard lock{lockedState->mutex};
	                                  lockedState->subscribers.erase(subscriptionId);
	                                }
	                                waitForDeliveryWorkerIdle(lockedState.get());
	                              }
	                            }};

  if (initialSnapshot) {
    invokeSubscriber(subscriptionId, *initialSnapshot);
  }

  return handle;
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::unsubscribe(std::size_t subscriptionId) noexcept {
	  try {
	    {
	      std::lock_guard lock{state_->mutex};
	      state_->subscribers.erase(subscriptionId);
	    }
	    waitForDeliveryWorkerIdle(state_.get());
	  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::publish(const Snapshot& snapshot) {
  std::vector<std::pair<std::size_t, Callback>> deliveries;
  {
    std::lock_guard lock{state_->mutex};
    deliveries.reserve(state_->subscribers.size());
    for (const auto& [subscriptionId, subscriber] : state_->subscribers) {
      if (subscriber.active) {
        deliveries.emplace_back(subscriptionId, subscriber.callback);
      }
    }
  }

  const auto worker = deliveryWorkerFor(state_.get());
  for (const auto& [subscriptionId, callback] : deliveries) {
    const auto snapshotCopy = snapshot;
    const auto weakState = std::weak_ptr<State>{state_};
    worker->submit([subscriptionId, callback, snapshotCopy, weakState] {
      const auto lockedState = weakState.lock();
      if (!lockedState) {
        return;
      }
      {
        std::lock_guard lock{lockedState->mutex};
        const auto found = lockedState->subscribers.find(subscriptionId);
        if (found == lockedState->subscribers.end() || !found->second.active) {
          return;
        }
      }
      try {
        callback(snapshotCopy);
      } catch (...) {
        SubscriptionExceptionReporter reporter;
        SubscriptionExceptionReport report{.subscriptionId = subscriptionId};
        report.exception = std::current_exception();
        {
          std::lock_guard lock{lockedState->mutex};
          report.totalExceptionCount = ++lockedState->exceptionCount;
          reporter = lockedState->exceptionReporter;
        }
        if (reporter) {
          reporter(report);
        }
      }
    });
  }
}

template <typename Snapshot>
std::size_t SubscriptionStore<Snapshot>::subscriberCount() const {
  std::lock_guard lock{state_->mutex};
  return state_->subscribers.size();
}

template <typename Snapshot>
std::size_t SubscriptionStore<Snapshot>::exceptionCount() const {
  std::lock_guard lock{state_->mutex};
  return state_->exceptionCount;
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::clear() noexcept {
  try {
    std::lock_guard lock{state_->mutex};
    state_->subscribers.clear();
  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

template <typename Snapshot>
typename SubscriptionStore<Snapshot>::Callback SubscriptionStore<Snapshot>::callbackFor(std::size_t subscriptionId) const {
  std::lock_guard lock{state_->mutex};
  const auto found = state_->subscribers.find(subscriptionId);
  if (found == state_->subscribers.end() || !found->second.active) {
    return {};
  }

  return found->second.callback;
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::invokeSubscriber(std::size_t subscriptionId, const Snapshot& snapshot) {
  const auto callback = callbackFor(subscriptionId);
  if (!callback) {
    return;
  }

  const auto worker = deliveryWorkerFor(state_.get());
  const auto snapshotCopy = snapshot;
  const auto weakState = std::weak_ptr<State>{state_};
  worker->submit([subscriptionId, callback, snapshotCopy, weakState] {
    const auto lockedState = weakState.lock();
    if (!lockedState) {
      return;
    }
    {
      std::lock_guard lock{lockedState->mutex};
      const auto found = lockedState->subscribers.find(subscriptionId);
      if (found == lockedState->subscribers.end() || !found->second.active) {
        return;
      }
    }
    try {
      callback(snapshotCopy);
    } catch (...) {
      SubscriptionExceptionReporter reporter;
      SubscriptionExceptionReport report{.subscriptionId = subscriptionId};
      report.exception = std::current_exception();
      {
        std::lock_guard lock{lockedState->mutex};
        report.totalExceptionCount = ++lockedState->exceptionCount;
        reporter = lockedState->exceptionReporter;
      }
      if (reporter) {
        reporter(report);
      }
    }
  });
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::reportException(std::size_t subscriptionId, std::exception_ptr exception) noexcept {
  SubscriptionExceptionReporter reporter;
  SubscriptionExceptionReport report{.subscriptionId = subscriptionId};
  report.exception = std::move(exception);
  try {
    std::lock_guard lock{state_->mutex};
    report.totalExceptionCount = ++state_->exceptionCount;
    reporter = state_->exceptionReporter;
  } catch (...) {
    discardUnhandledException(std::current_exception());
    return;
  }

  if (!reporter) {
    return;
  }

  try {
    reporter(report);
  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

template class SubscriptionStore<PlayerStateSnapshot>;
template class SubscriptionStore<LibraryStateSnapshot>;
template class SubscriptionStore<ControlDomainNotification>;

}
