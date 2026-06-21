#pragma once

#include "metadata_service_backend.h"

#include <memory>
#include <string>

namespace seriona::metadata::detail {

class WindowsSmtcServiceBackend final : public MetadataServiceBackend {
public:
  WindowsSmtcServiceBackend() = default;

  [[nodiscard]] MetadataBackendKind kind() const override;
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override;
  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override;
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override;
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override;
  [[nodiscard]] MetadataSyncResult stop() override;

private:
  [[nodiscard]] static MetadataSyncResult makeUnsupportedResult(std::string message);
};

[[nodiscard]] std::unique_ptr<MetadataServiceBackend> makeWindowsMetadataServiceBackend();

}
