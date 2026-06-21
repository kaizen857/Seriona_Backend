#include "metadata_service_testing.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace seriona::metadata {

namespace {

[[nodiscard]] MetadataSyncResult makeFailureResult(std::string code, std::string message) {
  MetadataSyncResult result{};
  result.accepted = false;
  result.errorCode = std::move(code);
  result.message = std::move(message);
  return result;
}

[[nodiscard]] MetadataBackendCapabilities makeCapabilities(MetadataBackendKind kind, bool hasPlatformExtension) {
  MetadataBackendCapabilities capabilities{};
  if (kind == MetadataBackendKind::Windows) {
    capabilities.requiresPlatformExtension = true;
    capabilities.hasPlatformExtension = hasPlatformExtension;
  }
  return capabilities;
}

class MetadataServiceBackend {
public:
  virtual ~MetadataServiceBackend() = default;
  [[nodiscard]] virtual MetadataBackendKind kind() const = 0;
  [[nodiscard]] virtual MetadataBackendCapabilities capabilities() const = 0;
  [[nodiscard]] virtual control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) = 0;
  [[nodiscard]] virtual MetadataSyncResult start(const PlatformMediaState& state) = 0;
  [[nodiscard]] virtual MetadataSyncResult update(const PlatformMediaState& state) = 0;
  [[nodiscard]] virtual MetadataSyncResult stop() = 0;
};

class NoopMetadataServiceBackend final : public MetadataServiceBackend {
public:
  explicit NoopMetadataServiceBackend(MetadataBackendKind kind, MetadataBackendCapabilities capabilities)
      : kind_(kind), capabilities_(capabilities) {}

  [[nodiscard]] MetadataBackendKind kind() const override { return kind_; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return capabilities_; }

  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    commandSink_ = std::move(callback);
    const auto subscriptionId = nextSubscriptionId_++;
    auto active = std::make_shared<bool>(true);
    return control::SubscriptionHandle{subscriptionId, [this, active]() mutable {
                                         if (!*active) {
                                           return;
                                         }
                                         *active = false;
                                         commandSink_ = std::nullopt;
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
    commandSink_ = std::nullopt;
    return makeAcceptedResult(PlatformMediaState{}, false);
  }

private:
  [[nodiscard]] MetadataSyncResult makeAcceptedResult(const PlatformMediaState& state, bool changed) const {
    return MetadataSyncResult{.accepted = true,
                              .changed = changed,
                              .state = state,
                              .errorCode = std::nullopt,
                              .message = {}};
  }

  MetadataBackendKind kind_;
  MetadataBackendCapabilities capabilities_;
  std::optional<control::MediaControlCommandSink> commandSink_{};
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

class RecordingMetadataServiceBackend final : public MetadataServiceBackend {
public:
  RecordingMetadataServiceBackend(std::unique_ptr<MetadataServiceBackend> backend,
                                  std::shared_ptr<MetadataServiceTestHooks> hooks)
      : backend_(std::move(backend)), hooks_(std::move(hooks)) {}

  [[nodiscard]] MetadataBackendKind kind() const override { return backend_->kind(); }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return backend_->capabilities(); }

  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    if (hooks_) {
      ++hooks_->commandRegistrations;
    }

    auto handle = backend_->registerCommandCallback(std::move(callback));
    auto originalUnsubscribe = std::move(handle.unsubscribe);
    auto active = std::make_shared<bool>(true);
    handle.unsubscribe = [this, originalUnsubscribe = std::move(originalUnsubscribe), active]() mutable {
      if (!*active) {
        return;
      }
      *active = false;
      if (hooks_) {
        ++hooks_->commandUnregistrations;
      }
      if (originalUnsubscribe) {
        originalUnsubscribe();
      }
    };
    return handle;
  }

  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override {
    if (hooks_) {
      ++hooks_->startCalls;
    }
    if (hooks_ && hooks_->failStart) {
      const auto result = makeFailureResult("metadata.backend.start_failed", "metadata backend start failed");
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->start(state);
    if (hooks_) {
      hooks_->results.push_back(result);
    }
    return result;
  }

  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override {
    if (hooks_) {
      ++hooks_->updateCalls;
    }
    if (hooks_ && hooks_->failUpdate) {
      const auto result = makeFailureResult("metadata.backend.update_failed", "metadata backend update failed");
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->update(state);
    if (hooks_) {
      hooks_->results.push_back(result);
    }
    return result;
  }

  [[nodiscard]] MetadataSyncResult stop() override {
    if (hooks_) {
      ++hooks_->stopCalls;
    }
    if (hooks_ && hooks_->failStop) {
      const auto result = makeFailureResult("metadata.backend.stop_failed", "metadata backend stop failed");
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->stop();
    if (hooks_) {
      hooks_->results.push_back(result);
    }
    return result;
  }

private:
  std::unique_ptr<MetadataServiceBackend> backend_;
  std::shared_ptr<MetadataServiceTestHooks> hooks_;
};

class MetadataRecordingServiceImpl final : public MetadataSharingService {
public:
  MetadataRecordingServiceImpl(MetadataBackendKind kind,
                               std::unique_ptr<MetadataServiceBackend> backend,
                               std::shared_ptr<MetadataServiceTestHooks> hooks)
      : kind_(kind), backend_(std::move(backend)), hooks_(std::move(hooks)) {}

  [[nodiscard]] MetadataBackendKind backendKind() const override { return kind_; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override { return backend_->capabilities(); }
  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    return backend_->registerCommandCallback(std::move(callback));
  }
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override { return backend_->start(state); }
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override { return backend_->update(state); }
  [[nodiscard]] MetadataSyncResult stop() override { return backend_->stop(); }

private:
  MetadataBackendKind kind_;
  std::unique_ptr<MetadataServiceBackend> backend_;
  std::shared_ptr<MetadataServiceTestHooks> hooks_;
};

}  // namespace

std::unique_ptr<MetadataSharingService> makeRecordingMetadataSharingService(
    const MetadataSharingOptions& options, const std::shared_ptr<MetadataServiceTestHooks>& hooks) {
  auto backend = std::make_unique<NoopMetadataServiceBackend>(options.backendKind,
                                                              makeCapabilities(options.backendKind,
                                                                               options.platformExtension != nullptr));
  return std::make_unique<MetadataRecordingServiceImpl>(options.backendKind,
                                                        std::make_unique<RecordingMetadataServiceBackend>(std::move(backend), hooks),
                                                        hooks);
}

}
