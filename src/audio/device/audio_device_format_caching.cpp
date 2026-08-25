#include "seriona/audio/device/audio_device_format_enumerator.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::audio {

// CachingDeviceFormatEnumerator 实现：常驻后台刷新线程 + 缓存。
// enumerate() 只做"标记需要刷新 + 返回缓存副本"，微秒级返回。
// 刷新线程被条件变量唤醒后在无锁状态下执行平台枚举（数百毫秒），
// 完成后再持锁写回缓存——因此 enumerate() 永远不被平台枚举阻塞。
struct CachingDeviceFormatEnumerator::Impl {
  static constexpr std::chrono::seconds kTtl{30};
  static constexpr std::chrono::seconds kRetryDelay{5};

  explicit Impl(std::unique_ptr<DeviceFormatEnumerator> platform)
      : platform(std::move(platform)),
        refreshThread([this] { runRefreshLoop(); }) {}

  ~Impl() {
    {
      std::lock_guard lock{mutex};
      stopping = true;
    }
    wake.notify_one();
    if (refreshThread.joinable()) {
      refreshThread.join();
    }
  }

  void runRefreshLoop() {
    std::unique_lock lock{mutex};
    for (;;) {
      wake.wait(lock, [this] { return stopping || stale; });
      if (stopping) {
        return;
      }
      stale = false;
      refreshing = true;
      lock.unlock();
      auto result = platform->enumerate();
      lock.lock();
      refreshing = false;
      if (!stopping) {
        cache = std::move(result);
        cachedAt = std::chrono::steady_clock::now();
        if (!cache.empty()) {
          lastAttemptAt = cachedAt;
        }
      }
    }
  }

  bool needsRefresh() const {
    if (stopping || refreshing) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (cache.empty()) {
      return now - lastAttemptAt >= kRetryDelay;
    }
    return now - cachedAt >= kTtl;
  }

  std::unique_ptr<DeviceFormatEnumerator> platform;
  std::thread refreshThread;
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<DeviceFormatCapabilities> cache;
  std::chrono::steady_clock::time_point cachedAt{};
  std::chrono::steady_clock::time_point lastAttemptAt{};
  bool stale{true};
  bool refreshing{false};
  bool stopping{false};
};

CachingDeviceFormatEnumerator::CachingDeviceFormatEnumerator(std::unique_ptr<DeviceFormatEnumerator> platform)
    : impl_(std::make_unique<Impl>(std::move(platform))) {}

CachingDeviceFormatEnumerator::~CachingDeviceFormatEnumerator() = default;

std::vector<DeviceFormatCapabilities> CachingDeviceFormatEnumerator::enumerate() {
  std::lock_guard lock{impl_->mutex};
  if (impl_->needsRefresh()) {
    impl_->stale = true;
    impl_->wake.notify_one();
  }
  return impl_->cache;
}

}
