#pragma once

#include "seriona/control/control_contracts.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace seriona::metadata {

enum class MetadataBackendKind {
  Noop,
  Linux,
  Windows,
};

struct MetadataBackendCapabilities {
  bool canPublishMetadata{false};
  bool canPublishTimeline{false};
  bool canReceiveCommands{false};
  bool requiresPlatformExtension{false};
  bool hasPlatformExtension{false};
};

struct PlatformMediaState {
  control::PlayerStateSnapshot controlState{};
  std::chrono::milliseconds timelineUpdateInterval{1000};
};

struct MetadataSyncResult {
  bool accepted{false};
  bool changed{false};
  PlatformMediaState state{};
  std::optional<std::string> errorCode;
  std::string message;
};

struct MetadataSharingOptions {
  MetadataBackendKind backendKind{MetadataBackendKind::Noop};
  std::chrono::milliseconds timelineUpdateInterval{1000};
  std::shared_ptr<void> platformExtension;
  bool allowNoopFallback{true};
};

class MetadataSharingService {
public:
  virtual ~MetadataSharingService() = default;

  [[nodiscard]] virtual MetadataBackendKind backendKind() const = 0;
  [[nodiscard]] virtual MetadataBackendCapabilities capabilities() const = 0;
  [[nodiscard]] virtual control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) = 0;
  [[nodiscard]] virtual MetadataSyncResult start(const PlatformMediaState& state) = 0;
  [[nodiscard]] virtual MetadataSyncResult update(const PlatformMediaState& state) = 0;
  [[nodiscard]] virtual MetadataSyncResult stop() = 0;
};

[[nodiscard]] std::unique_ptr<MetadataSharingService> makeMetadataSharingService(const MetadataSharingOptions& options);

}
