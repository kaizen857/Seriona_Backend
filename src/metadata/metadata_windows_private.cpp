#include "metadata_windows_private.h"

#include <optional>
#include <utility>

namespace seriona::metadata::detail {

namespace {

[[nodiscard]] MetadataSyncResult makeAcceptedResult(const PlatformMediaState& state, bool changed) {
  return MetadataSyncResult{.accepted = true, .changed = changed, .state = state, .errorCode = std::nullopt, .message = {}};
}

[[nodiscard]] std::string describeWindowsSmtcFlow() {
  return "ISystemMediaTransportControlsInterop::GetForWindow(...) -> DisplayUpdater.Type = Music -> MusicProperties -> Thumbnail = RandomAccessStreamReference -> Update()";
}

struct WindowsArtworkFlowShape {
  std::string localArtworkFlow{"StorageFile::GetFileFromPathAsync(absPath) + CreateFromFile(file)"};
  std::string uriArtworkFlow{"CreateFromUri only for supported app/http(s) schemes"};
};

}

MetadataBackendKind WindowsSmtcServiceBackend::kind() const { return MetadataBackendKind::Windows; }

MetadataBackendCapabilities WindowsSmtcServiceBackend::capabilities() const {
  return MetadataBackendCapabilities{.canPublishMetadata = false,
                                     .canPublishTimeline = false,
                                     .canReceiveCommands = false,
                                     .requiresPlatformExtension = true,
                                     .hasPlatformExtension = false};
}

control::SubscriptionHandle WindowsSmtcServiceBackend::registerCommandCallback(control::MediaControlCommandSink) {
  return control::SubscriptionHandle{0U, []() {}};
}

MetadataSyncResult WindowsSmtcServiceBackend::start(const PlatformMediaState& state) {
  [[maybe_unused]] const auto flowShape = describeWindowsSmtcFlow();
  [[maybe_unused]] const WindowsArtworkFlowShape artworkShape{};
  return makeAcceptedResult(state, false);
}

MetadataSyncResult WindowsSmtcServiceBackend::update(const PlatformMediaState& state) {
  return makeAcceptedResult(state, false);
}

MetadataSyncResult WindowsSmtcServiceBackend::stop() {
  return makeAcceptedResult(PlatformMediaState{}, false);
}

MetadataSyncResult WindowsSmtcServiceBackend::makeUnsupportedResult(std::string message) {
  MetadataSyncResult result{};
  result.accepted = false;
  result.errorCode = "metadata.backend.windows.unsupported";
  result.message = std::move(message);
  return result;
}

std::unique_ptr<MetadataServiceBackend> makeWindowsMetadataServiceBackend() {
  return std::make_unique<WindowsSmtcServiceBackend>();
}

}
