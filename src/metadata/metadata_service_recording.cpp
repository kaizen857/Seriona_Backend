#include "metadata_service_testing.h"

#include "metadata_service_backend.h"

#include "metadata_synchronizer.h"

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
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::RegisterCommand,
                                                      .state = {},
                                                      .result = MetadataSyncResult{.accepted = true,
                                                                                   .changed = false,
                                                                                   .state = {},
                                                                                   .errorCode = std::nullopt,
                                                                                   .message = {}}});
    }

    auto handle = backend_->registerCommandCallback(std::move(callback));
    auto originalUnsubscribe = std::move(handle.unsubscribe);
    auto active = std::make_shared<bool>(true);
    auto hooks = hooks_;
    handle.unsubscribe = [hooks, originalUnsubscribe = std::move(originalUnsubscribe), active]() mutable {
      if (!*active) {
        return;
      }
      *active = false;
      if (hooks) {
        ++hooks->commandUnregistrations;
        hooks->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::UnregisterCommand,
                                                       .state = {},
                                                       .result = MetadataSyncResult{.accepted = true,
                                                                                    .changed = false,
                                                                                    .state = {},
                                                                                    .errorCode = std::nullopt,
                                                                                    .message = {}}});
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
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Start, .state = state, .result = result});
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->start(state);
    if (hooks_) {
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Start, .state = state, .result = result});
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
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Update, .state = state, .result = result});
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->update(state);
    if (hooks_) {
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Update, .state = state, .result = result});
      hooks_->results.push_back(result);
    }
    return result;
  }

  [[nodiscard]] MetadataSyncResult stop() override {
    if (hooks_) {
      ++hooks_->stopCalls;
    }
    const auto state = PlatformMediaState{};
    if (hooks_ && hooks_->failStop) {
      const auto result = makeFailureResult("metadata.backend.stop_failed", "metadata backend stop failed");
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Stop, .state = state, .result = result});
      hooks_->results.push_back(result);
      return result;
    }

    const auto result = backend_->stop();
    if (hooks_) {
      hooks_->records.push_back(MetadataServiceRecord{.kind = MetadataServiceRecordKind::Stop, .state = state, .result = result});
      hooks_->results.push_back(result);
    }
    return result;
  }

private:
  std::unique_ptr<MetadataServiceBackend> backend_;
  std::shared_ptr<MetadataServiceTestHooks> hooks_;
};
}

std::unique_ptr<MetadataSharingService> makeRecordingMetadataSharingService(
    const MetadataSharingOptions& options, const std::shared_ptr<MetadataServiceTestHooks>& hooks) {
  return makeMetadataSharingServiceFromBackend(
      options.backendKind,
      std::make_unique<RecordingMetadataServiceBackend>(makeMetadataServiceBackendFromOptions(options), hooks));
}

}
