#include "seriona/metadata/metadata_contracts.h"

#include "metadata_service_backend.h"

#include "metadata_synchronizer.h"

#if defined(__linux__) && !defined(__APPLE__)
#include "metadata_mpris_private.h"
#endif

#ifdef _WIN32
#include "metadata_windows_private.h"
#endif

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace seriona::metadata {

namespace {

struct CommandSinkState {
  std::optional<control::MediaControlCommandSink> sink{};
};

[[nodiscard]] MetadataSyncResult makeFailureResult(std::string code, std::string message) {
  MetadataSyncResult result{};
  result.accepted = false;
  result.errorCode = std::move(code);
  result.message = std::move(message);
  return result;
}

class NoopMetadataServiceBackend final : public MetadataServiceBackend {
public:
  explicit NoopMetadataServiceBackend(MetadataBackendKind kind, MetadataBackendCapabilities capabilities)
      : kind_(kind), capabilities_(capabilities) {}

  [[nodiscard]] MetadataBackendKind kind() const override { return kind_; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return capabilities_; }

  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    commandSinkState_->sink = std::move(callback);
    const auto subscriptionId = nextSubscriptionId_++;
    auto active = std::make_shared<bool>(true);
    auto commandSinkState = commandSinkState_;
    return control::SubscriptionHandle{subscriptionId, [commandSinkState, active]() mutable {
                                         if (!*active) {
                                           return;
                                         }
                                         *active = false;
                                         commandSinkState->sink = std::nullopt;
                                       }};
  }

  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override {
    if (started_) {
      return makeAcceptedResult(state, false);
    }

    started_ = true;
    return makeAcceptedResult(state, true);
  }

  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override {
    if (!started_) {
      return makeFailureResult("metadata.backend.stopped", "metadata backend update requested after stop");
    }

    return makeAcceptedResult(state, true);
  }

  [[nodiscard]] MetadataSyncResult stop() override {
    if (!started_) {
      return makeAcceptedResult(PlatformMediaState{}, false);
    }

    started_ = false;
    commandSinkState_->sink = std::nullopt;
    return makeAcceptedResult(PlatformMediaState{}, false);
  }

protected:
  [[nodiscard]] MetadataSyncResult makeAcceptedResult(const PlatformMediaState& state, bool changed) const {
    return MetadataSyncResult{.accepted = true,
                              .changed = changed,
                              .state = state,
                              .errorCode = std::nullopt,
                              .message = {}};
  }

  MetadataBackendKind kind_;
  MetadataBackendCapabilities capabilities_;
  std::shared_ptr<CommandSinkState> commandSinkState_{std::make_shared<CommandSinkState>()};
  std::size_t nextSubscriptionId_{1};
  bool started_{false};
};

class UnavailableMetadataServiceBackend final : public MetadataServiceBackend {
public:
  UnavailableMetadataServiceBackend(MetadataBackendKind kind,
                                    MetadataBackendCapabilities capabilities,
                                    std::string code,
                                    std::string message)
      : kind_(kind), capabilities_(capabilities), code_(std::move(code)), message_(std::move(message)) {}

  [[nodiscard]] MetadataBackendKind kind() const override { return kind_; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return capabilities_; }
  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink) override {
    return control::SubscriptionHandle{0U, []() {}};
  }
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState&) override { return makeFailureResult(code_, message_); }
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState&) override {
    return makeFailureResult("metadata.backend.unavailable", "metadata backend is unavailable");
  }
  [[nodiscard]] MetadataSyncResult stop() override {
    return makeFailureResult("metadata.backend.unavailable", "metadata backend is unavailable");
  }

private:
  MetadataBackendKind kind_;
  MetadataBackendCapabilities capabilities_;
  std::string code_;
  std::string message_;
};

class MetadataSharingServiceImpl final : public MetadataSharingService {
public:
  MetadataSharingServiceImpl(MetadataBackendKind kind, std::unique_ptr<MetadataServiceBackend> backend)
      : kind_(kind), backend_(std::move(backend)) {}

  ~MetadataSharingServiceImpl() override {
    const auto result = stopBackendIfNeeded();
    (void)result;
  }

  [[nodiscard]] MetadataBackendKind backendKind() const override { return kind_; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return backend_->capabilities(); }
  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    return backend_->registerCommandCallback(std::move(callback));
  }

  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override {
    bool alreadyStarted = false;
    {
      std::lock_guard lock{mutex_};
      alreadyStarted = started_ && !stopping_;
    }

    if (alreadyStarted) {
      std::lock_guard backendLock{backendMutex_};
      return backend_->start(state);
    }

    MetadataSyncResult result{};
    {
      std::lock_guard backendLock{backendMutex_};
      result = backend_->start(state);
    }
    if (!result.accepted) {
      return result;
    }

    std::lock_guard lock{mutex_};
    started_ = true;
    stopping_ = false;
    if (!worker_.joinable()) {
      worker_ = std::thread{[this] { runWorker(); }};
    }
    return result;
  }

  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override {
    {
      std::lock_guard lock{mutex_};
      if (!started_ || stopping_) {
        return makeFailureResult("metadata.backend.stopped", "metadata backend update requested after stop");
      }
      pendingUpdate_ = state;
    }
    changed_.notify_one();
    return MetadataSyncResult{.accepted = true,
                              .changed = true,
                              .state = state,
                              .errorCode = std::nullopt,
                              .message = {}};
  }

  [[nodiscard]] MetadataSyncResult stop() override {
    return stopBackendIfNeeded();
  }

private:
  [[nodiscard]] MetadataSyncResult stopBackendIfNeeded() {
    const auto shouldStopBackend = stopWorker();
    if (!shouldStopBackend) {
      return MetadataSyncResult{.accepted = true,
                                .changed = false,
                                .state = PlatformMediaState{},
                                .errorCode = std::nullopt,
                                .message = {}};
    }

    std::lock_guard backendLock{backendMutex_};
    return backend_->stop();
  }

  [[nodiscard]] bool stopWorker() {
    bool shouldStopBackend = false;
    {
      std::lock_guard lock{mutex_};
      shouldStopBackend = started_;
      started_ = false;
      stopping_ = true;
    }
    changed_.notify_one();

    if (worker_.joinable()) {
      worker_.join();
    }

    {
      std::lock_guard lock{mutex_};
      pendingUpdate_.reset();
      stopping_ = false;
    }
    return shouldStopBackend;
  }

  void runWorker() {
    while (true) {
      auto state = nextUpdate();
      if (!state) {
        return;
      }

      std::lock_guard backendLock{backendMutex_};
      const auto result = backend_->update(*state);
      (void)result;
    }
  }

  [[nodiscard]] std::optional<PlatformMediaState> nextUpdate() {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [this] { return pendingUpdate_.has_value() || stopping_; });
    if (pendingUpdate_.has_value()) {
      auto state = pendingUpdate_;
      pendingUpdate_.reset();
      return state;
    }
    return std::nullopt;
  }

  MetadataBackendKind kind_;
  std::unique_ptr<MetadataServiceBackend> backend_;
  std::mutex mutex_{};
  std::mutex backendMutex_{};
  std::condition_variable changed_{};
  std::thread worker_{};
  std::optional<PlatformMediaState> pendingUpdate_{};
  bool started_{false};
  bool stopping_{false};
};

}

[[nodiscard]] std::unique_ptr<MetadataServiceBackend> makeMetadataServiceBackendFromOptions(const MetadataSharingOptions& options) {
  if (options.backendKind == MetadataBackendKind::Windows) {
    const auto capabilities = MetadataBackendCapabilities{.requiresPlatformExtension = true,
                                                          .hasPlatformExtension = options.platformExtension != nullptr};
    if (options.platformExtension == nullptr) {
      return std::make_unique<UnavailableMetadataServiceBackend>(
          MetadataBackendKind::Windows,
          capabilities,
          "metadata.backend.windows.host_missing",
          "windows metadata backend requires a platform host extension");
    }
#ifdef _WIN32
    return detail::makeWindowsMetadataServiceBackend();
#else
    return std::make_unique<NoopMetadataServiceBackend>(MetadataBackendKind::Windows, capabilities);
#endif
  }

  if (options.backendKind == MetadataBackendKind::Linux) {
#if defined(__linux__) && !defined(__APPLE__)
    return detail::makeLinuxMetadataServiceBackend();
#else
    return std::make_unique<NoopMetadataServiceBackend>(MetadataBackendKind::Noop, MetadataBackendCapabilities{});
#endif
  }

  return std::make_unique<NoopMetadataServiceBackend>(MetadataBackendKind::Noop, MetadataBackendCapabilities{});
}

std::unique_ptr<MetadataSharingService> makeMetadataSharingServiceFromBackend(
    MetadataBackendKind kind, std::unique_ptr<MetadataServiceBackend> backend) {
  return std::make_unique<MetadataSharingServiceImpl>(kind, std::move(backend));
}

std::unique_ptr<MetadataSharingService> makeMetadataSharingService(const MetadataSharingOptions& options) {
  return makeMetadataSharingServiceFromBackend(options.backendKind, makeMetadataServiceBackendFromOptions(options));
}

}
