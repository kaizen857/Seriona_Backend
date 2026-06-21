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

[[nodiscard]] bool commandAllowed(control::MediaControlCommandKind kind, const control::PlaybackCapabilities& capabilities) {
  switch (kind) {
    case control::MediaControlCommandKind::Play:
      return capabilities.canPlay;
    case control::MediaControlCommandKind::Pause:
      return capabilities.canPause;
    case control::MediaControlCommandKind::Stop:
      return capabilities.canStop;
    case control::MediaControlCommandKind::TogglePlayPause:
      return capabilities.canPlay || capabilities.canPause;
    case control::MediaControlCommandKind::SeekTo:
    case control::MediaControlCommandKind::SeekBy:
      return capabilities.canSeek;
    case control::MediaControlCommandKind::SetVolume:
      return capabilities.canSetVolume;
    case control::MediaControlCommandKind::SetMuted:
      return false;
    case control::MediaControlCommandKind::SetRepeatMode:
      return capabilities.canSetRepeat;
    case control::MediaControlCommandKind::SetShuffle:
      return capabilities.canSetShuffle;
    case control::MediaControlCommandKind::SkipNext:
      return capabilities.canSkipNext;
    case control::MediaControlCommandKind::SkipPrevious:
      return capabilities.canSkipPrevious;
    case control::MediaControlCommandKind::SelectTrack:
      return false;
  }
  return false;
}

class NullMprisObject final : public IMprisObject {
public:
  void registerModel(const MprisObjectModel& model) override { model_ = model; }
  void registerCommandHandlers(const MprisCommandHandlers& handlers) override { handlers_ = handlers; }
  void publish(const MprisSnapshotRecord& snapshot) override { snapshot_ = snapshot; }

  [[nodiscard]] const MprisCommandHandlers& handlers() const { return handlers_; }

private:
  MprisObjectModel model_{};
  MprisCommandHandlers handlers_{};
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
    : bus_(std::move(bus)) {
  configureCommandModel();
}

void LinuxMprisAdapter::setCommandSink(control::MediaControlCommandSink sink) { commandSinkState_->sink = std::move(sink); }

void LinuxMprisAdapter::configureCommandModel() {
  commandHandlers_.play = [this]() { return dispatchCommand(control::MediaControlCommandKind::Play, std::nullopt); };
  commandHandlers_.pause = [this]() { return dispatchCommand(control::MediaControlCommandKind::Pause, std::nullopt); };
  commandHandlers_.playPause = [this]() { return dispatchCommand(control::MediaControlCommandKind::TogglePlayPause, std::nullopt); };
  commandHandlers_.stop = [this]() { return dispatchCommand(control::MediaControlCommandKind::Stop, std::nullopt); };
  commandHandlers_.next = [this]() { return dispatchCommand(control::MediaControlCommandKind::SkipNext, std::nullopt); };
  commandHandlers_.previous = [this]() { return dispatchCommand(control::MediaControlCommandKind::SkipPrevious, std::nullopt); };
  commandHandlers_.seekBy = [this](std::chrono::microseconds delta) { return dispatchSeekBy(delta); };
  commandHandlers_.setPosition = [this](const std::string& trackObjectPath, std::chrono::microseconds position) {
    return setPosition(trackObjectPath, position);
  };
  commandHandlers_.setVolume = [this](float volume) { return dispatchSetVolume(volume); };
  commandHandlers_.setRepeatMode = [this](control::RepeatMode repeatMode) { return dispatchSetRepeat(repeatMode); };
  commandHandlers_.setShuffle = [this](bool shuffle) { return dispatchSetShuffle(shuffle); };
}

MetadataSyncResult LinuxMprisAdapter::start(const PlatformMediaState& state) {
  currentState_ = state;
  if (!bus_) {
    bus_ = std::make_unique<SdbusMprisBus>();
  }
  if (!object_) {
    object_ = bus_->createObject(kMprisObjectPath);
    object_->registerModel(model_);
    object_->registerCommandHandlers(commandHandlers_);
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
  if (!commandSinkState_->sink || !currentState_->controlState.capabilities.canSeek || position < std::chrono::microseconds{0}) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SeekTo;
  command.position = std::chrono::duration_cast<std::chrono::milliseconds>(position);
  command.track = currentState_->controlState.currentTrack;
  (*commandSinkState_->sink)(command);
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

bool LinuxMprisAdapter::dispatchCommand(control::MediaControlCommandKind kind, std::optional<std::chrono::milliseconds> position) {
  if (!currentState_ || !commandSinkState_->sink || !commandAllowed(kind, currentState_->controlState.capabilities)) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = kind;
  command.position = position;
  (*commandSinkState_->sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSeekBy(std::chrono::microseconds delta) {
  if (!currentState_ || !commandSinkState_->sink || !commandAllowed(control::MediaControlCommandKind::SeekBy, currentState_->controlState.capabilities) || delta < std::chrono::microseconds{0}) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SeekBy;
  command.delta = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  (*commandSinkState_->sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetVolume(float volume) {
  if (!currentState_ || !commandSinkState_->sink || !commandAllowed(control::MediaControlCommandKind::SetVolume, currentState_->controlState.capabilities)) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetVolume;
  command.volume = volume;
  (*commandSinkState_->sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetRepeat(control::RepeatMode repeatMode) {
  if (!currentState_ || !commandSinkState_->sink || !commandAllowed(control::MediaControlCommandKind::SetRepeatMode, currentState_->controlState.capabilities)) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetRepeatMode;
  command.repeatMode = repeatMode;
  (*commandSinkState_->sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetShuffle(bool shuffle) {
  if (!currentState_ || !commandSinkState_->sink || !commandAllowed(control::MediaControlCommandKind::SetShuffle, currentState_->controlState.capabilities)) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetShuffle;
  command.shuffle = shuffle;
  (*commandSinkState_->sink)(command);
  return true;
}

void LinuxMprisAdapter::publishCurrentSnapshot() {
  if (!currentState_ || !object_) {
    return;
  }
  const auto record = toSnapshotRecord(mapPlayerStateSnapshot(currentState_->controlState));
  object_->publish(record);
  lastPublishedSnapshot_ = record;
}

}
