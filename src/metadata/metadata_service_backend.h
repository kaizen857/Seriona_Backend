#pragma once

#include "seriona/metadata/metadata_contracts.h"

#include <memory>

namespace seriona::metadata {

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

[[nodiscard]] std::unique_ptr<MetadataServiceBackend> makeMetadataServiceBackendFromOptions(const MetadataSharingOptions& options);
[[nodiscard]] std::unique_ptr<MetadataSharingService> makeMetadataSharingServiceFromBackend(
    MetadataBackendKind kind, std::unique_ptr<MetadataServiceBackend> backend);

}
