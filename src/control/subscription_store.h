#pragma once

#include "seriona/control/control_contracts.h"

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace seriona::control {

struct SubscriptionExceptionReport {
  std::size_t subscriptionId{0};
  std::size_t totalExceptionCount{0};
  std::exception_ptr exception{};
};

using SubscriptionExceptionReporter = std::function<void(const SubscriptionExceptionReport&)>;

// 投递模式：Async（默认）经常驻投递 worker 线程执行订阅回调（生产：不阻塞
// 控制器事件循环）；Sync 在 publish/invokeSubscriber 调用线程内同步执行
// （测试 inline 模式：回调可见性可预测，避免等待异步投递的偶发超时）。
enum class SubscriptionDeliveryMode { Async, Sync };

template <typename Snapshot>
class SubscriptionStore {
public:
  using Callback = std::function<void(const Snapshot&)>;

  explicit SubscriptionStore(SubscriptionExceptionReporter exceptionReporter = {},
                              SubscriptionDeliveryMode mode = SubscriptionDeliveryMode::Async);
  ~SubscriptionStore();

  SubscriptionStore(const SubscriptionStore&) = delete;
  SubscriptionStore& operator=(const SubscriptionStore&) = delete;

  [[nodiscard]] SubscriptionHandle subscribe(Callback callback, std::optional<Snapshot> initialSnapshot = std::nullopt);
  void unsubscribe(std::size_t subscriptionId) noexcept;
  void publish(const Snapshot& snapshot);
  [[nodiscard]] std::size_t subscriberCount() const;
  [[nodiscard]] std::size_t exceptionCount() const;
  void clear() noexcept;

private:
  struct Subscriber {
    Callback callback{};
    bool active{true};
  };

  struct State {
    mutable std::mutex mutex{};
    std::unordered_map<std::size_t, Subscriber> subscribers{};
    std::size_t nextSubscriptionId{1};
    std::size_t exceptionCount{0};
    SubscriptionExceptionReporter exceptionReporter{};
    SubscriptionDeliveryMode mode{SubscriptionDeliveryMode::Async};
  };

  [[nodiscard]] Callback callbackFor(std::size_t subscriptionId) const;
  void invokeSubscriber(std::size_t subscriptionId, const Snapshot& snapshot);
  void reportException(std::size_t subscriptionId, std::exception_ptr exception) noexcept;

  std::shared_ptr<State> state_;
};

using PlayerStateSubscriptionStore = SubscriptionStore<PlayerStateSnapshot>;
using LibraryStateSubscriptionStore = SubscriptionStore<LibraryStateSnapshot>;
using DomainNotificationSubscriptionStore = SubscriptionStore<ControlDomainNotification>;

extern template class SubscriptionStore<PlayerStateSnapshot>;
extern template class SubscriptionStore<LibraryStateSnapshot>;
extern template class SubscriptionStore<ControlDomainNotification>;

}
