#include "subscription_store.h"

#include <utility>

namespace seriona::control {

namespace {

void discardUnhandledException(const std::exception_ptr& exception) noexcept {
  (void)exception;
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
                                std::lock_guard lock{lockedState->mutex};
                                lockedState->subscribers.erase(subscriptionId);
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
    std::lock_guard lock{state_->mutex};
    state_->subscribers.erase(subscriptionId);
  } catch (...) {
    discardUnhandledException(std::current_exception());
  }
}

template <typename Snapshot>
void SubscriptionStore<Snapshot>::publish(const Snapshot& snapshot) {
  std::vector<std::size_t> subscriptionIds;
  {
    std::lock_guard lock{state_->mutex};
    subscriptionIds.reserve(state_->subscribers.size());
    for (const auto& [subscriptionId, subscriber] : state_->subscribers) {
      if (subscriber.active) {
        subscriptionIds.push_back(subscriptionId);
      }
    }
  }

  for (const auto subscriptionId : subscriptionIds) {
    invokeSubscriber(subscriptionId, snapshot);
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

  try {
    callback(snapshot);
  } catch (...) {
    reportException(subscriptionId, std::current_exception());
  }
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
