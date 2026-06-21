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

class MetadataSharingServiceImpl final : public MetadataSharingService {
public:
  MetadataSharingServiceImpl(MetadataBackendKind kind, std::unique_ptr<MetadataServiceBackend> backend)
      : kind_(kind), backend_(std::move(backend)) {}

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
