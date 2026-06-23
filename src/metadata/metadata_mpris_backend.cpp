#include "metadata_mpris_private.h"

#include <utility>

namespace seriona::metadata::detail {

namespace {

class LinuxMprisServiceBackend final : public MetadataServiceBackend {
public:
  explicit LinuxMprisServiceBackend(std::unique_ptr<IMprisBus> bus)
      : adapter_(std::move(bus)) {}

  [[nodiscard]] MetadataBackendKind kind() const override { return MetadataBackendKind::Linux; }
  [[nodiscard]] MetadataBackendCapabilities capabilities() const override {
    return MetadataBackendCapabilities{.canPublishMetadata = true,
                                      .canPublishTimeline = true,
                                      .canReceiveCommands = true,
                                      .requiresPlatformExtension = false,
                                      .hasPlatformExtension = false};
  }
  [[nodiscard]] control::SubscriptionHandle registerCommandCallback(control::MediaControlCommandSink callback) override {
    adapter_.setCommandSink(std::move(callback));
    auto commandSinkState = adapter_.commandSinkState();
    return control::SubscriptionHandle{1U, [commandSinkState]() { commandSinkState->clear(); }};
  }
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override { return adapter_.start(state); }
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override { return adapter_.update(state); }
  [[nodiscard]] MetadataSyncResult stop() override { return adapter_.stop(); }

private:
  LinuxMprisAdapter adapter_;
};

}

std::unique_ptr<MetadataServiceBackend> makeLinuxMetadataServiceBackend(std::unique_ptr<IMprisBus> bus) {
  return std::make_unique<LinuxMprisServiceBackend>(std::move(bus));
}

}
