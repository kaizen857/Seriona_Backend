#include "metadata_mpris_private.h"

#include "spdlog/spdlog.h"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] control::RepeatMode repeatModeFromLoopStatus(const std::string& status) {
  if (status == "Track") {
    return control::RepeatMode::One;
  }
  if (status == "Playlist") {
    return control::RepeatMode::All;
  }
  return control::RepeatMode::Off;
}

[[nodiscard]] std::string playbackStatusText(control::PlaybackStatus status) {
  switch (status) {
    case control::PlaybackStatus::Playing:
      return "Playing";
    case control::PlaybackStatus::Paused:
      return "Paused";
    case control::PlaybackStatus::Stopped:
    case control::PlaybackStatus::Loading:
    case control::PlaybackStatus::Seeking:
    case control::PlaybackStatus::Buffering:
    case control::PlaybackStatus::Error:
      return "Stopped";
  }
  return "Stopped";
}

[[nodiscard]] std::string fileUri(const std::filesystem::path& path) {
  return std::string{"file://"} + path.generic_string();
}

[[nodiscard]] std::string artworkUrl(const std::optional<control::ArtworkRef>& artwork) {
  if (!artwork) {
    return {};
  }
  if (artwork->uri.has_value() && !artwork->uri->empty()) {
    return *artwork->uri;
  }
  if (!artwork->localPath.has_value() || artwork->localPath->empty()) {
    return {};
  }
  const auto path = artwork->localPath->is_absolute() ? *artwork->localPath : std::filesystem::absolute(*artwork->localPath);
  return fileUri(path);
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

[[nodiscard]] bool sameOptionalInt64(const std::optional<std::int64_t>& left,
                                     const std::optional<std::int64_t>& right) {
  return left == right;
}

[[nodiscard]] bool sameCapabilities(const MetadataCapabilitySetDto& left,
                                    const MetadataCapabilitySetDto& right) {
  return left.canPlay == right.canPlay && left.canPause == right.canPause && left.canStop == right.canStop &&
         left.canSeek == right.canSeek && left.canSkipNext == right.canSkipNext &&
         left.canSkipPrevious == right.canSkipPrevious && left.canSetRepeat == right.canSetRepeat &&
         left.canSetShuffle == right.canSetShuffle && left.canSetVolume == right.canSetVolume;
}

[[nodiscard]] bool sameTrackIdentity(const MetadataTrackIdentityDto& left,
                                     const MetadataTrackIdentityDto& right) {
  return left.trackId == right.trackId && left.filePath == right.filePath && left.fileUri == right.fileUri &&
         left.sourceId == right.sourceId && left.libraryId == right.libraryId && left.trackNumber == right.trackNumber;
}

[[nodiscard]] bool sameFields(const MetadataFieldSet& left, const MetadataFieldSet& right) {
  return left.title == right.title && left.artist == right.artist && left.album == right.album &&
         left.albumArtist == right.albumArtist && left.genre == right.genre;
}

[[nodiscard]] bool isPlayingPositionOnlyUpdate(const MprisSnapshotRecord& previous,
                                               const MprisSnapshotRecord& current) {
  const auto& previousMpris = previous.snapshot.mpris;
  const auto& currentMpris = current.snapshot.mpris;
  return previous.trackObjectPath == current.trackObjectPath &&
         previousMpris.playbackStatus == control::PlaybackStatus::Playing &&
         currentMpris.playbackStatus == control::PlaybackStatus::Playing &&
         previousMpris.positionMicros != currentMpris.positionMicros &&
         sameTrackIdentity(previousMpris.track, currentMpris.track) && sameFields(previousMpris.fields, currentMpris.fields) &&
         previous.artUrl == current.artUrl && previous.loopStatus == current.loopStatus && previous.canControl == current.canControl &&
         sameOptionalInt64(previousMpris.durationMicros, currentMpris.durationMicros) &&
         sameOptionalInt64(previousMpris.bufferedMicros, currentMpris.bufferedMicros) &&
         sameOptionalInt64(previousMpris.seekableFromMicros, currentMpris.seekableFromMicros) &&
         sameOptionalInt64(previousMpris.seekableToMicros, currentMpris.seekableToMicros) &&
         previousMpris.repeatMode == currentMpris.repeatMode && previousMpris.shuffle == currentMpris.shuffle &&
         previousMpris.muted == currentMpris.muted && previousMpris.volume == currentMpris.volume &&
         sameCapabilities(previousMpris.capabilities, currentMpris.capabilities);
}

[[nodiscard]] bool shouldEmitPropertiesChangedSignal(const std::optional<MprisSnapshotRecord>& previous,
                                                     const MprisSnapshotRecord& current) {
  if (!previous) {
    return true;
  }
  return !isPlayingPositionOnlyUpdate(*previous, current);
}

class NullMprisObject final : public IMprisObject {
public:
  void registerModel(const MprisObjectModel& model) override { model_ = model; }
  void registerCommandHandlers(const MprisCommandHandlers& handlers) override { handlers_ = handlers; }
  void publish(const MprisSnapshotRecord& snapshot, bool) override { snapshot_ = snapshot; }

  [[nodiscard]] const MprisCommandHandlers& handlers() const { return handlers_; }

private:
  MprisObjectModel model_{};
  MprisCommandHandlers handlers_{};
  MprisSnapshotRecord snapshot_{};
};

class SdbusMprisObject final : public IMprisObject {
public:
  SdbusMprisObject(sdbus::IConnection& connection, std::string_view objectPath)
      : object_(sdbus::createObject(connection, sdbus::ObjectPath{std::string{objectPath}})) {}

  void registerModel(const MprisObjectModel& model) override {
    model_ = model;
    registerRootInterface();
    registerPlayerInterface();
  }

  void registerCommandHandlers(const MprisCommandHandlers& handlers) override { handlers_ = handlers; }

  void publish(const MprisSnapshotRecord& snapshot, bool emitPropertiesChanged) override {
    {
      std::lock_guard lock{mutex_};
      snapshot_ = snapshot;
    }
    if (emitPropertiesChanged) {
      object_->emitPropertiesChangedSignal(kMprisPlayerInterface);
    }
  }

private:
  [[nodiscard]] MprisSnapshotRecord snapshot() const {
    std::lock_guard lock{mutex_};
    return snapshot_;
  }

  void registerRootInterface() {
    object_->addVTable(
               sdbus::registerMethod("Raise").implementedAs([] {}),
               sdbus::registerMethod("Quit").implementedAs([] {}),
               sdbus::registerProperty("CanQuit").withGetter([] { return false; }),
               sdbus::registerProperty("CanRaise").withGetter([] { return false; }),
               sdbus::registerProperty("HasTrackList").withGetter([] { return false; }),
               sdbus::registerProperty("Identity").withGetter([] { return std::string{"seriona"}; }),
               sdbus::registerProperty("DesktopEntry").withGetter([] { return std::string{"seriona"}; }),
               sdbus::registerProperty("SupportedUriSchemes").withGetter([] { return std::vector<std::string>{"file"}; }),
               sdbus::registerProperty("SupportedMimeTypes").withGetter([] { return std::vector<std::string>{}; }))
        .forInterface(model_.rootInterface);
  }

  void registerPlayerInterface() {
    object_->addVTable(
               sdbus::registerMethod("Next").implementedAs([this] { dispatch(handlers_.next); }),
               sdbus::registerMethod("Previous").implementedAs([this] { dispatch(handlers_.previous); }),
               sdbus::registerMethod("Pause").implementedAs([this] { dispatch(handlers_.pause); }),
               sdbus::registerMethod("PlayPause").implementedAs([this] { dispatch(handlers_.playPause); }),
               sdbus::registerMethod("Stop").implementedAs([this] { dispatch(handlers_.stop); }),
               sdbus::registerMethod("Play").implementedAs([this] { dispatch(handlers_.play); }),
               sdbus::registerMethod("Seek").implementedAs([this](std::int64_t offset) { dispatchSeek(offset); }),
               sdbus::registerMethod("SetPosition").implementedAs([this](sdbus::ObjectPath trackId, std::int64_t position) {
                 dispatchSetPosition(trackId, position);
               }),
               sdbus::registerMethod("OpenUri").implementedAs([](const std::string&) {}),
               sdbus::registerProperty("PlaybackStatus").withGetter([this] { return playbackStatus(); }),
               sdbus::registerProperty("LoopStatus").withGetter([this] { return snapshot().loopStatus; }).withSetter([this](const std::string& value) {
                 dispatchSetRepeat(value);
               }),
               sdbus::registerProperty("Rate").withGetter([] { return 1.0; }).withSetter([](double) {}),
               sdbus::registerProperty("Shuffle").withGetter([this] { return snapshot().snapshot.mpris.shuffle; }).withSetter([this](bool value) {
                 dispatchSetShuffle(value);
               }),
               sdbus::registerProperty("Metadata").withGetter([this] { return metadata(); }),
               sdbus::registerProperty("Volume").withGetter([this] { return static_cast<double>(snapshot().snapshot.mpris.volume); }).withSetter([this](double value) {
                 dispatchSetVolume(value);
               }),
               sdbus::registerProperty("Position").withGetter([this] { return snapshot().snapshot.mpris.positionMicros; }),
               sdbus::registerProperty("MinimumRate").withGetter([] { return 1.0; }),
               sdbus::registerProperty("MaximumRate").withGetter([] { return 1.0; }),
               sdbus::registerProperty("CanGoNext").withGetter([this] { return snapshot().snapshot.mpris.capabilities.canSkipNext; }),
               sdbus::registerProperty("CanGoPrevious").withGetter([this] { return snapshot().snapshot.mpris.capabilities.canSkipPrevious; }),
               sdbus::registerProperty("CanPlay").withGetter([this] { return snapshot().snapshot.mpris.capabilities.canPlay; }),
               sdbus::registerProperty("CanPause").withGetter([this] { return snapshot().snapshot.mpris.capabilities.canPause; }),
               sdbus::registerProperty("CanSeek").withGetter([this] { return snapshot().snapshot.mpris.capabilities.canSeek; }),
               sdbus::registerProperty("CanControl").withGetter([this] { return snapshot().canControl; }))
        .forInterface(model_.playerInterface);
  }

  [[nodiscard]] std::string playbackStatus() const { return playbackStatusText(snapshot().snapshot.mpris.playbackStatus); }

  [[nodiscard]] std::map<std::string, sdbus::Variant> metadata() const {
    const auto current = snapshot();
    const auto& mpris = current.snapshot.mpris;
    auto values = std::map<std::string, sdbus::Variant>{};
    values.emplace("mpris:trackid", sdbus::Variant{sdbus::ObjectPath{current.trackObjectPath}});
    if (mpris.durationMicros) {
      values.emplace("mpris:length", sdbus::Variant{*mpris.durationMicros});
    }
    if (!current.artUrl.empty()) {
      values.emplace("mpris:artUrl", sdbus::Variant{current.artUrl});
    }
    if (!mpris.track.fileUri.empty()) {
      values.emplace("xesam:url", sdbus::Variant{mpris.track.fileUri});
    }
    if (mpris.fields.title) {
      values.emplace("xesam:title", sdbus::Variant{*mpris.fields.title});
    }
    if (mpris.fields.artist) {
      values.emplace("xesam:artist", sdbus::Variant{std::vector<std::string>{*mpris.fields.artist}});
    }
    if (mpris.fields.album) {
      values.emplace("xesam:album", sdbus::Variant{*mpris.fields.album});
    }
    if (mpris.fields.albumArtist) {
      values.emplace("xesam:albumArtist", sdbus::Variant{std::vector<std::string>{*mpris.fields.albumArtist}});
    }
    if (mpris.fields.genre) {
      values.emplace("xesam:genre", sdbus::Variant{std::vector<std::string>{*mpris.fields.genre}});
    }
    return values;
  }

  static void dispatch(const std::function<bool()>& handler) {
    if (handler) {
      static_cast<void>(handler());
    }
  }

  void dispatchSeek(std::int64_t offset) const {
    if (handlers_.seekBy) {
      static_cast<void>(handlers_.seekBy(std::chrono::microseconds{offset}));
    }
  }

  void dispatchSetPosition(const std::string& trackId, std::int64_t position) const {
    if (handlers_.setPosition) {
      static_cast<void>(handlers_.setPosition(trackId, std::chrono::microseconds{position}));
    }
  }

  void dispatchSetVolume(double volume) const {
    if (handlers_.setVolume) {
      static_cast<void>(handlers_.setVolume(static_cast<float>(volume)));
    }
  }

  void dispatchSetRepeat(const std::string& loopStatus) const {
    if (handlers_.setRepeatMode) {
      static_cast<void>(handlers_.setRepeatMode(repeatModeFromLoopStatus(loopStatus)));
    }
  }

  void dispatchSetShuffle(bool shuffle) const {
    if (handlers_.setShuffle) {
      static_cast<void>(handlers_.setShuffle(shuffle));
    }
  }

  std::unique_ptr<sdbus::IObject> object_;
  mutable std::mutex mutex_{};
  MprisObjectModel model_{};
  MprisCommandHandlers handlers_{};
  MprisSnapshotRecord snapshot_{};
};

class SdbusMprisBus final : public IMprisBus {
public:
  SdbusMprisBus() : connection_(sdbus::createSessionBusConnection()) {}

  ~SdbusMprisBus() override {
    if (eventLoopStarted_) {
      try {
        connection_->leaveEventLoop();
      } catch (const sdbus::Error& error) {
        spdlog::error("MPRIS DBus error: {}", error.what());
      }
    }
  }

  void requestName(std::string_view name) override {
    try {
      connection_->requestName(sdbus::ServiceName{std::string{name}});
    } catch (const sdbus::Error& error) {
      spdlog::error("MPRIS DBus error: {}", error.what());
      throw;
    }
    if (!eventLoopStarted_) {
      connection_->enterEventLoopAsync();
      eventLoopStarted_ = true;
    }
  }
  void addObjectManager(std::string_view objectPath) override { connection_->addObjectManager(sdbus::ObjectPath{std::string{objectPath}}); }
  [[nodiscard]] std::unique_ptr<IMprisObject> createObject(std::string_view objectPath) override {
    return std::make_unique<SdbusMprisObject>(*connection_, objectPath);
  }

private:
  std::unique_ptr<sdbus::IConnection> connection_;
  bool eventLoopStarted_{false};
};

[[nodiscard]] MprisSnapshotRecord toSnapshotRecord(const MetadataPlatformSnapshotDto& snapshot) {
  return MprisSnapshotRecord{.snapshot = snapshot,
                             .trackObjectPath = snapshot.mpris.trackObjectPath.value,
                             .artUrl = artworkUrl((snapshot.mpris.artwork.localPath || snapshot.mpris.artwork.uri)
                                                      ? std::optional<control::ArtworkRef>{control::ArtworkRef{.localPath = snapshot.mpris.artwork.localPath,
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

void CommandSinkState::set(control::MediaControlCommandSink sink) {
  std::lock_guard lock{mutex_};
  sink_ = std::move(sink);
}

void CommandSinkState::clear() {
  std::lock_guard lock{mutex_};
  sink_.reset();
}

std::optional<control::MediaControlCommandSink> CommandSinkState::snapshot() const {
  std::lock_guard lock{mutex_};
  return sink_;
}

void LinuxMprisAdapter::setCommandSink(control::MediaControlCommandSink sink) { commandSinkState_->set(std::move(sink)); }

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
  {
    std::lock_guard lock{stateMutex_};
    currentState_ = state;
  }
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
  spdlog::info("MPRIS object exported on session bus");
  started_ = true;
  publishCurrentSnapshot(state);
  return MetadataSyncResult{.accepted = true, .changed = true, .state = state, .errorCode = std::nullopt, .message = {}};
}

MetadataSyncResult LinuxMprisAdapter::update(const PlatformMediaState& state) {
  if (!started_) {
    return makeFailureResult("metadata.backend.stopped", "metadata backend update requested after stop");
  }
  {
    std::lock_guard lock{stateMutex_};
    currentState_ = state;
  }
  publishCurrentSnapshot(state);
  return MetadataSyncResult{.accepted = true, .changed = true, .state = state, .errorCode = std::nullopt, .message = {}};
}

MetadataSyncResult LinuxMprisAdapter::stop() {
  spdlog::debug("metadata mpris backend stopping");
  started_ = false;
  {
    std::lock_guard lock{stateMutex_};
    currentState_.reset();
  }
  lastPublishedSnapshot_.reset();
  return MetadataSyncResult{.accepted = true, .changed = false, .state = {}, .errorCode = std::nullopt, .message = {}};
}

bool LinuxMprisAdapter::setPosition(const std::string& trackObjectPath, std::chrono::microseconds position) {
  const auto state = currentStateSnapshot();
  if (!state || !state->controlState.currentTrack) {
    spdlog::debug("MPRIS SetPosition rejected: no current track");
    return false;
  }
  const auto& controlState = state->controlState;
  if (makeMprisTrackObjectPath(*controlState.currentTrack) != trackObjectPath) {
    spdlog::warn("MPRIS SetPosition rejected: invalid track id {}", trackObjectPath);
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink || !controlState.capabilities.canSeek || position < std::chrono::microseconds{0}) {
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SeekTo;
  command.position = std::chrono::duration_cast<std::chrono::milliseconds>(position);
  command.track = controlState.currentTrack;
  (*sink)(command);
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
  const auto state = currentStateSnapshot();
  if (!state || !commandAllowed(kind, state->controlState.capabilities)) {
    spdlog::debug("MPRIS command rejected: not permitted by capabilities");
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink) {
    spdlog::debug("MPRIS command rejected: no command sink");
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = kind;
  command.position = position;
  (*sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSeekBy(std::chrono::microseconds delta) {
  const auto state = currentStateSnapshot();
  if (!state || !commandAllowed(control::MediaControlCommandKind::SeekBy, state->controlState.capabilities) || delta < std::chrono::microseconds{0}) {
    spdlog::debug("MPRIS SeekBy rejected: not permitted by capabilities or invalid delta");
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink) {
    spdlog::debug("MPRIS command rejected: no command sink");
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SeekBy;
  command.delta = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
  (*sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetVolume(float volume) {
  const auto state = currentStateSnapshot();
  if (!state || !commandAllowed(control::MediaControlCommandKind::SetVolume, state->controlState.capabilities)) {
    spdlog::debug("MPRIS SetVolume rejected: not permitted by capabilities");
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink) {
    spdlog::debug("MPRIS command rejected: no command sink");
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetVolume;
  command.volume = volume;
  (*sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetRepeat(control::RepeatMode repeatMode) {
  const auto state = currentStateSnapshot();
  if (!state || !commandAllowed(control::MediaControlCommandKind::SetRepeatMode, state->controlState.capabilities)) {
    spdlog::debug("MPRIS SetRepeatMode rejected: not permitted by capabilities");
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink) {
    spdlog::debug("MPRIS command rejected: no command sink");
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetRepeatMode;
  command.repeatMode = repeatMode;
  (*sink)(command);
  return true;
}

bool LinuxMprisAdapter::dispatchSetShuffle(bool shuffle) {
  const auto state = currentStateSnapshot();
  if (!state || !commandAllowed(control::MediaControlCommandKind::SetShuffle, state->controlState.capabilities)) {
    spdlog::debug("MPRIS SetShuffle rejected: not permitted by capabilities");
    return false;
  }
  const auto sink = commandSinkState_->snapshot();
  if (!sink) {
    spdlog::debug("MPRIS command rejected: no command sink");
    return false;
  }

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::SetShuffle;
  command.shuffle = shuffle;
  (*sink)(command);
  return true;
}

std::optional<PlatformMediaState> LinuxMprisAdapter::currentStateSnapshot() const {
  std::lock_guard lock{stateMutex_};
  return currentState_;
}

void LinuxMprisAdapter::publishCurrentSnapshot(const PlatformMediaState& state) {
  if (!object_) {
    return;
  }
  const auto record = toSnapshotRecord(mapPlayerStateSnapshot(state.controlState));
  const auto emitProps = shouldEmitPropertiesChangedSignal(lastPublishedSnapshot_, record);
  if (emitProps) {
    spdlog::debug("MPRIS property publish: properties changed");
  }
  object_->publish(record, emitProps);
  lastPublishedSnapshot_ = record;
}

}
