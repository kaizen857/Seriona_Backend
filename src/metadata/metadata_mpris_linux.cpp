#include "metadata_mpris_private.h"

#include <sdbus-c++/sdbus-c++.h>

#include <utility>

namespace seriona::metadata::detail {

namespace {

[[nodiscard]] std::string loopStatusText(control::RepeatMode mode) {
  switch (mode) {
    case control::RepeatMode::Off:
      return "None";
    case control::RepeatMode::One:
      return "Track";
    case control::RepeatMode::All:
      return "Playlist";
  }
  return "None";
}

[[nodiscard]] std::string fileUri(const std::filesystem::path& path) {
  return std::string{"file://"} + path.generic_string();
}

[[nodiscard]] std::string artworkUrl(const std::optional<control::ArtworkRef>& artwork) {
  if (!artwork || !artwork->localPath) {
    return {};
  }
  return fileUri(*artwork->localPath);
}

[[nodiscard]] bool capabilitiesAllowControl(const control::PlaybackCapabilities& capabilities) {
  return capabilities.canPlay || capabilities.canPause || capabilities.canStop || capabilities.canSeek ||
         capabilities.canSkipNext || capabilities.canSkipPrevious || capabilities.canSetRepeat || capabilities.canSetShuffle ||
         capabilities.canSetVolume;
}

class NullMprisObject final : public IMprisObject {
public:
  void registerModel(const MprisObjectModel& model) override { model_ = model; }
  void publish(const MprisSnapshotRecord& snapshot) override { snapshot_ = snapshot; }

private:
  MprisObjectModel model_{};
  MprisSnapshotRecord snapshot_{};
};

class SdbusMprisBus final : public IMprisBus {
public:
  SdbusMprisBus() : connection_(sdbus::createSessionBusConnection()) {}

  void requestName(std::string_view name) override { connection_->requestName(sdbus::ServiceName{std::string{name}}); }
  void addObjectManager(std::string_view objectPath) override { connection_->addObjectManager(sdbus::ObjectPath{std::string{objectPath}}); }
  [[nodiscard]] std::unique_ptr<IMprisObject> createObject(std::string_view) override { return std::make_unique<NullMprisObject>(); }

private:
  std::unique_ptr<sdbus::IConnection> connection_;
};

[[nodiscard]] MprisSnapshotRecord toSnapshotRecord(const MetadataPlatformSnapshotDto& snapshot) {
  return MprisSnapshotRecord{.snapshot = snapshot,
                             .trackObjectPath = snapshot.mpris.trackObjectPath.value,
                             .artUrl = artworkUrl(snapshot.mpris.artwork.localPath ? std::optional<control::ArtworkRef>{control::ArtworkRef{.localPath = snapshot.mpris.artwork.localPath,
                                                                                                                                      .uri = snapshot.mpris.artwork.uri,
                                                                                                                                      .contentHash = snapshot.mpris.artwork.contentHash}}
                                                                               : std::nullopt),
                             .loopStatus = loopStatusText(snapshot.mpris.repeatMode),
                             .canControl = snapshot.mpris.capabilities.canPlay || snapshot.mpris.capabilities.canPause ||
                                           snapshot.mpris.capabilities.canStop || snapshot.mpris.capabilities.canSeek ||
                                           snapshot.mpris.capabilities.canSkipNext || snapshot.mpris.capabilities.canSkipPrevious ||
                                           snapshot.mpris.capabilities.canSetRepeat || snapshot.mpris.capabilities.canSetShuffle ||
                                           snapshot.mpris.capabilities.canSetVolume};
}

}
LinuxMprisAdapter::LinuxMprisAdapter(std::unique_ptr<IMprisBus> bus)
    : bus_(std::move(bus)) {}

void LinuxMprisAdapter::setCommandSink(control::MediaControlCommandSink sink) { commandSink_ = std::move(sink); }

MetadataSyncResult LinuxMprisAdapter::start(const PlatformMediaState& state) {
  currentState_ = state;
  if (!bus_) {
    bus_ = std::make_unique<SdbusMprisBus>();
  }
  if (!object_) {
    object_ = bus_->createObject(kMprisObjectPath);
    object_->registerModel(model_);
  }
  bus_->requestName(kMprisBusName);
  bus_->addObjectManager(kMprisObjectPath);
  started_ = true;
  publishCurrentSnapshot();
  return MetadataSyncResult{.accepted = true, .changed = true, .state = state, .errorCode = std::nullopt, .message = {}};
}

MetadataSyncResult LinuxMprisAdapter::update(const PlatformMediaState& state) {
  if (!started_) {
    return makeFailureResult("metadata.backend.stopped", "metadata backend update requested after stop");
  }
  currentState_ = state;
  publishCurrentSnapshot();
  return MetadataSyncResult{.accepted = true, .changed = true, .state = state, .errorCode = std::nullopt, .message = {}};
}

MetadataSyncResult LinuxMprisAdapter::stop() {
  started_ = false;
  currentState_.reset();
  lastPublishedSnapshot_.reset();
  return MetadataSyncResult{.accepted = true, .changed = false, .state = {}, .errorCode = std::nullopt, .message = {}};
}

bool LinuxMprisAdapter::setPosition(const std::string& trackObjectPath, std::chrono::microseconds position) {
  if (!currentState_ || !currentState_->controlState.currentTrack) {
    return false;
  }
  if (makeMprisTrackObjectPath(*currentState_->controlState.currentTrack) != trackObjectPath) {
    return false;
  }
  if (!commandSink_) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SeekTo;
  command.position = std::chrono::duration_cast<std::chrono::milliseconds>(position);
  command.track = currentState_->controlState.currentTrack;
  commandSink_(command);
  return true;
}

MetadataSyncResult LinuxMprisAdapter::makeFailureResult(std::string code, std::string message) {
  MetadataSyncResult result{};
  result.accepted = false;
  result.errorCode = std::move(code);
  result.message = std::move(message);
  return result;
}

MetadataSyncResult LinuxMprisAdapter::makeAcceptedResult(const PlatformMediaState& state, bool changed) {
  return MetadataSyncResult{.accepted = true, .changed = changed, .state = state, .errorCode = std::nullopt, .message = {}};
}

std::string LinuxMprisAdapter::loopStatusFromRepeatMode(control::RepeatMode repeatMode) { return loopStatusText(repeatMode); }

std::string LinuxMprisAdapter::artUrlFromArtwork(const std::optional<control::ArtworkRef>& artwork) { return artworkUrl(artwork); }

bool LinuxMprisAdapter::canControlFromCapabilities(const control::PlaybackCapabilities& capabilities) {
  return capabilitiesAllowControl(capabilities);
}

MprisSnapshotRecord LinuxMprisAdapter::toSnapshotRecord(const MetadataPlatformSnapshotDto& snapshot) {
  return ::seriona::metadata::detail::toSnapshotRecord(snapshot);
}

void LinuxMprisAdapter::publishCurrentSnapshot() {
  if (!currentState_ || !object_) {
    return;
  }
  const auto record = toSnapshotRecord(mapPlayerStateSnapshot(currentState_->controlState));
  object_->publish(record);
  lastPublishedSnapshot_ = record;
}

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
    return control::SubscriptionHandle{1U, [this]() { adapter_.setCommandSink({}); }};
  }
  [[nodiscard]] MetadataSyncResult start(const PlatformMediaState& state) override { return adapter_.start(state); }
  [[nodiscard]] MetadataSyncResult update(const PlatformMediaState& state) override { return adapter_.update(state); }
  [[nodiscard]] MetadataSyncResult stop() override { return adapter_.stop(); }

private:
  LinuxMprisAdapter adapter_;
};

std::unique_ptr<MetadataServiceBackend> makeLinuxMetadataServiceBackend(std::unique_ptr<IMprisBus> bus) {
  return std::make_unique<LinuxMprisServiceBackend>(std::move(bus));
}

}
