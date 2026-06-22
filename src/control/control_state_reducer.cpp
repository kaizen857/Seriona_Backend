#include "control_state_reducer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace seriona::control {
namespace {

constexpr std::size_t kRecentNotificationLimit = 32;

[[nodiscard]] PlaybackCapabilities defaultCapabilities() noexcept {
  return PlaybackCapabilities{.canPlay = true,
                              .canPause = true,
                              .canStop = true,
                              .canSeek = true,
                              .canSkipNext = true,
                              .canSkipPrevious = true,
                              .canSetRepeat = true,
                              .canSetShuffle = true,
                              .canSetVolume = true,
                              .canSelectTrack = true};
}

[[nodiscard]] PlaybackStatus mapPlaybackState(audio::PlaybackState state) noexcept {
  switch (state) {
  case audio::PlaybackState::Idle:
  case audio::PlaybackState::Ready:
  case audio::PlaybackState::Stopped:
    return PlaybackStatus::Stopped;
  case audio::PlaybackState::Loading:
    return PlaybackStatus::Loading;
  case audio::PlaybackState::Playing:
  case audio::PlaybackState::Draining:
    return PlaybackStatus::Playing;
  case audio::PlaybackState::Paused:
    return PlaybackStatus::Paused;
  case audio::PlaybackState::Error:
    return PlaybackStatus::Error;
  }
  return PlaybackStatus::Stopped;
}

[[nodiscard]] std::string playbackErrorCode(audio::PlaybackErrorCode code) {
  switch (code) {
  case audio::PlaybackErrorCode::OpenFailed:
    return "OpenFailed";
  case audio::PlaybackErrorCode::UnsupportedFormat:
    return "UnsupportedFormat";
  case audio::PlaybackErrorCode::DeviceUnavailable:
    return "DeviceUnavailable";
  case audio::PlaybackErrorCode::FormatNegotiationFailed:
    return "FormatNegotiationFailed";
  case audio::PlaybackErrorCode::DecodeFailed:
    return "DecodeFailed";
  case audio::PlaybackErrorCode::BufferUnderrun:
    return "BufferUnderrun";
  case audio::PlaybackErrorCode::SeekFailed:
    return "SeekFailed";
  }
  return "Unknown";
}

[[nodiscard]] bool sameTrack(const TrackIdentity& lhs, const TrackIdentity& rhs) {
  return lhs.trackId == rhs.trackId && lhs.filePath == rhs.filePath;
}

[[nodiscard]] TrackIdentity identityFromSong(const scanner::SongMetadata& song) {
  return TrackIdentity{.trackId = song.trackId, .filePath = song.filePath, .sourceId = {}, .libraryId = {}};
}

[[nodiscard]] audio::TrackPlaybackRequest requestFromSong(const scanner::SongMetadata& song) {
  return audio::TrackPlaybackRequest{.trackId = song.trackId,
                                     .filePath = song.filePath,
                                     .title = song.title,
                                     .artist = song.artist,
                                     .offset = song.offset,
                                     .duration = song.duration,
                                     .sampleRate = song.sampleRate,
                                     .bitDepth = song.bitDepth,
                                     .channels = song.channels,
                                     .format = {}};
}

[[nodiscard]] TrackIdentity identityFromRequest(const audio::TrackPlaybackRequest& request) {
  return TrackIdentity{.trackId = request.trackId, .filePath = request.filePath, .sourceId = {}, .libraryId = {}};
}

[[nodiscard]] DisplayMetadata displayFromRequest(const audio::TrackPlaybackRequest& request) {
  return DisplayMetadata{.title = request.title, .artist = request.artist, .album = {}, .albumArtist = {}, .genre = {}};
}

[[nodiscard]] bool isTrackNode(const scanner::PlaylistNode& node) noexcept {
  return node.kind == scanner::PlaylistNodeKind::Track && node.song.has_value() && !node.song->trackId.empty() &&
         !node.song->filePath.empty();
}

[[nodiscard]] ControlIntent makeIntent(ControlIntentKind kind) {
  ControlIntent intent{};
  intent.kind = kind;
  return intent;
}

[[nodiscard]] ControlIntent makeTrackIntent(audio::TrackPlaybackRequest request) {
  auto intent = makeIntent(ControlIntentKind::LoadTrack);
  intent.track = std::move(request);
  return intent;
}

[[nodiscard]] ControlIntent makeSeekIntent(std::chrono::milliseconds position) {
  auto intent = makeIntent(ControlIntentKind::Seek);
  intent.position = position;
  return intent;
}

[[nodiscard]] ControlIntent makeVolumeIntent(float volume) {
  auto intent = makeIntent(ControlIntentKind::SetVolume);
  intent.volume = volume;
  return intent;
}

[[nodiscard]] ControlIntent makeMutedIntent(bool muted) {
  auto intent = makeIntent(ControlIntentKind::SetMuted);
  intent.muted = muted;
  return intent;
}

[[nodiscard]] ControlDomainNotification makeNotification(ControlDomainNotificationKind kind, std::string message) {
  ControlDomainNotification notification{};
  notification.kind = kind;
  notification.message = std::move(message);
  return notification;
}

[[nodiscard]] ControlDomainNotification makeScanNotification(ControlDomainNotificationKind kind, std::string message,
                                                            LibraryScanStatus status) {
  auto notification = makeNotification(kind, std::move(message));
  notification.scanStatus = status;
  return notification;
}

[[nodiscard]] ControlDomainNotification makeErrorNotification(ControlDomainNotificationKind kind, MediaControllerErrorCode code,
                                                             std::string message) {
  auto notification = makeNotification(kind, std::move(message));
  notification.errorCode = code;
  return notification;
}

}

ControlStateReducer::ControlStateReducer(MediaControllerOptions options) : shuffleRandom_{options.shuffleSeed} {
  player_.capabilities = defaultCapabilities();
}

const PlayerStateSnapshot& ControlStateReducer::playerState() const noexcept {
  return player_;
}

const LibraryStateSnapshot& ControlStateReducer::libraryState() const noexcept {
  return library_;
}

const std::vector<ControlDomainNotification>& ControlStateReducer::recentNotifications() const noexcept {
  return recentNotifications_;
}

ControlReduction ControlStateReducer::reduceCommand(const MediaControlCommand& command) {
  auto reduction = accept();

  switch (command.kind) {
  case MediaControlCommandKind::Play:
    if (selectedTrack_.has_value()) {
      reduction.intents.push_back(makeIntent(ControlIntentKind::Play));
      player_.playback.state = PlaybackStatus::Playing;
      markPlayerChanged(reduction);
      return reduction;
    }
    if (const auto track = firstPlayableTrack(); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    stopPlayback(reduction);
    return reject(MediaControllerErrorCode::NoPlayableTrack, "No playable track is available in the current library");
  case MediaControlCommandKind::Pause:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Pause));
    player_.playback.state = PlaybackStatus::Paused;
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::Stop:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Stop));
    stopPlayback(reduction);
    return reduction;
  case MediaControlCommandKind::TogglePlayPause:
    reduction.intents.push_back(makeIntent(player_.playback.state == PlaybackStatus::Playing ? ControlIntentKind::Pause
                                                                                              : ControlIntentKind::Play));
    player_.playback.state = player_.playback.state == PlaybackStatus::Playing ? PlaybackStatus::Paused : PlaybackStatus::Playing;
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SeekTo:
    if (!command.position.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SeekTo requires an absolute position");
    }
    player_.timeline.position = clampPosition(*command.position);
    reduction.intents.push_back(makeSeekIntent(player_.timeline.position));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SeekBy:
    if (!command.delta.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SeekBy requires a delta");
    }
    player_.timeline.position = clampPosition(player_.timeline.position + *command.delta);
    reduction.intents.push_back(makeSeekIntent(player_.timeline.position));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SetVolume:
    if (!command.volume.has_value() || std::isnan(*command.volume)) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SetVolume requires a finite volume");
    }
    player_.volume = std::clamp(*command.volume, 0.0F, 1.0F);
    reduction.intents.push_back(makeVolumeIntent(player_.volume));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SetMuted:
    if (!command.muted.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SetMuted requires a muted value");
    }
    player_.muted = *command.muted;
    reduction.intents.push_back(makeMutedIntent(player_.muted));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SetRepeatMode:
    if (!command.repeatMode.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SetRepeatMode requires a repeat mode");
    }
    player_.repeatMode = *command.repeatMode;
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SetShuffle:
    if (!command.shuffle.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SetShuffle requires a shuffle value");
    }
    player_.shuffle = *command.shuffle;
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SkipNext:
    if (player_.repeatMode == RepeatMode::One && selectedTrack_.has_value()) {
      const auto track = findPlayableTrack(*selectedTrack_);
      if (track.has_value()) {
        selectTrack(reduction, *track, true);
        return reduction;
      }
    }
    if (const auto track = nextTrack(true); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    stopPlayback(reduction);
    return reduction;
  case MediaControlCommandKind::SkipPrevious:
    if (player_.repeatMode == RepeatMode::One && selectedTrack_.has_value()) {
      const auto track = findPlayableTrack(*selectedTrack_);
      if (track.has_value()) {
        selectTrack(reduction, *track, true);
        return reduction;
      }
    }
    if (const auto track = nextTrack(false); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    stopPlayback(reduction);
    return reduction;
  case MediaControlCommandKind::SelectTrack:
    if (!command.track.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SelectTrack requires a track identity");
    }
    if (const auto track = findPlayableTrack(*command.track); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    return reject(MediaControllerErrorCode::TrackNotInLibrary, "Selected track is not present in the current library");
  }

  return reject(MediaControllerErrorCode::InvalidCommand, "Unsupported media control command");
}

ControlReduction ControlStateReducer::reduceAudioEvent(const audio::BackendEvent& event) {
  auto& lastVersion = event.sourceModule == audio::BackendSourceModule::AudioPlayer ? lastAudioPlayerVersion_ : lastAudioServiceVersion_;
  if (event.monotonicVersion <= lastVersion) {
    return {};
  }
  lastVersion = event.monotonicVersion;

  auto reduction = accept();
  player_.freshness.sampledAt = event.timestamp;

  std::visit(
      [&](const auto& payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, audio::PlaybackStateChanged>) {
          player_.playback.state = mapPlaybackState(payload.state);
          if (player_.playback.state != PlaybackStatus::Error) {
            player_.playback.errorCode.reset();
            player_.playback.errorMessage.reset();
          }
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::TrackChanged>) {
          selectedTrack_ = identityFromRequest(payload.request);
          player_.currentTrack = selectedTrack_;
          player_.display = displayFromRequest(payload.request);
          player_.timeline.position = payload.request.offset.value_or(std::chrono::milliseconds{0});
          player_.timeline.duration = payload.request.duration;
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::PlaybackPositionUpdated>) {
          player_.timeline.position = clampPosition(payload.clock.position);
          markPlayerChanged(reduction, payload.clock.sampledAt);
        } else if constexpr (std::is_same_v<Payload, audio::PositionDiscontinuity>) {
          player_.timeline.position = clampPosition(payload.after.position);
          markPlayerChanged(reduction, payload.after.sampledAt);
        } else if constexpr (std::is_same_v<Payload, audio::PlaybackEnded>) {
          player_.timeline.position = clampPosition(payload.finalClock.position);
          addNotification(reduction, makeNotification(ControlDomainNotificationKind::PlaybackEnded, "Playback ended"));
          if (player_.repeatMode == RepeatMode::One) {
            if (const auto track = selectedTrack_.has_value() ? findPlayableTrack(*selectedTrack_) : std::nullopt; track.has_value()) {
              selectTrack(reduction, *track, true);
            }
          } else if (const auto track = nextTrack(true); track.has_value()) {
            selectTrack(reduction, *track, true);
          } else {
            stopPlayback(reduction);
          }
          markPlayerChanged(reduction, payload.finalClock.sampledAt);
        } else if constexpr (std::is_same_v<Payload, audio::OutputFormatChanged>) {
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::OutputModeFallback>) {
          addNotification(reduction, makeNotification(ControlDomainNotificationKind::OutputModeFallback, payload.reason));
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::PlaybackError>) {
          player_.playback.state = PlaybackStatus::Error;
          player_.playback.errorCode = playbackErrorCode(payload.code);
          player_.playback.errorMessage = payload.message;
          if (payload.clock.has_value()) {
            player_.timeline.position = clampPosition(payload.clock->position);
          }
          addNotification(reduction,
                          makeErrorNotification(ControlDomainNotificationKind::PlaybackError,
                                                MediaControllerErrorCode::BackendRejected,
                                                payload.message));
          markPlayerChanged(reduction, event.timestamp);
        }
      },
      event.payload);

  return reduction;
}

ControlReduction ControlStateReducer::reduceScannerEvent(const scanner::ScannerEvent& event) {
  if (event.monotonicVersion <= lastScannerVersion_) {
    return {};
  }
  lastScannerVersion_ = event.monotonicVersion;

  auto reduction = accept();
  library_.version = event.monotonicVersion;

  switch (event.type) {
  case scanner::ScannerEventType::ScanStarted:
    library_.scanStatus = LibraryScanStatus::Scanning;
    library_.lastError.reset();
    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibraryScanStarted,
                                                    "Library scan started",
                                                    library_.scanStatus));
    break;
  case scanner::ScannerEventType::ProgressUpdated:
  case scanner::ScannerEventType::FileScanned:
    if (const auto* progress = std::get_if<scanner::ScanProgress>(&event.payload)) {
      library_.scanProgress = *progress;
    }
    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibraryScanProgressUpdated,
                                                    "Library scan progress updated",
                                                    library_.scanStatus));
    break;
  case scanner::ScannerEventType::PlaylistSnapshotUpdated:
    if (const auto* snapshot = std::get_if<scanner::PlaylistTreeSnapshot>(&event.payload)) {
      library_.libraryTree = *snapshot;
      library_.version = snapshot->version;
    }
    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibrarySnapshotUpdated,
                                                    "Library snapshot updated",
                                                    library_.scanStatus));
    break;
  case scanner::ScannerEventType::ScanCompleted:
    library_.scanStatus = LibraryScanStatus::Completed;
    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibraryScanCompleted,
                                                    "Library scan completed",
                                                    library_.scanStatus));
    break;
  case scanner::ScannerEventType::ScanStopped:
    library_.scanStatus = LibraryScanStatus::Stopped;
    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibraryScanStopped,
                                                    "Library scan stopped",
                                                    library_.scanStatus));
    break;
  case scanner::ScannerEventType::ScanError:
    library_.scanStatus = LibraryScanStatus::Error;
    if (const auto* error = std::get_if<scanner::ScannerError>(&event.payload)) {
      library_.lastError = *error;
      auto notification = makeErrorNotification(ControlDomainNotificationKind::LibraryScanError,
                                                MediaControllerErrorCode::BackendRejected,
                                                error->message);
      notification.scanStatus = library_.scanStatus;
      addNotification(reduction, std::move(notification));
    } else {
      auto notification = makeErrorNotification(ControlDomainNotificationKind::LibraryScanError,
                                                MediaControllerErrorCode::BackendRejected,
                                                "Library scan error");
      notification.scanStatus = library_.scanStatus;
      addNotification(reduction, std::move(notification));
    }
    break;
  }

  reduction.libraryStateChanged = true;
  return reduction;
}

std::vector<ControlStateReducer::PlayableTrack> ControlStateReducer::playableTracks() const {
  std::vector<PlayableTrack> tracks;
  if (!library_.libraryTree.has_value()) {
    return tracks;
  }

  const auto& tree = *library_.libraryTree;
  const auto appendTrack = [&](const scanner::PlaylistNode& node) {
    if (isTrackNode(node)) {
      tracks.push_back(PlayableTrack{.identity = identityFromSong(*node.song), .request = requestFromSong(*node.song)});
    }
  };

  if (!tree.rootNodeId.has_value()) {
    for (const auto& node : tree.nodes) {
      appendTrack(node);
    }
    return tracks;
  }

  std::vector<std::string> pending{*tree.rootNodeId};
  while (!pending.empty()) {
    const auto nodeId = pending.back();
    pending.pop_back();
    const auto nodeIt = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const scanner::PlaylistNode& node) { return node.nodeId == nodeId; });
    if (nodeIt == tree.nodes.end()) {
      continue;
    }
    appendTrack(*nodeIt);
    for (auto childIt = nodeIt->childNodeIds.rbegin(); childIt != nodeIt->childNodeIds.rend(); ++childIt) {
      pending.push_back(*childIt);
    }
  }
  return tracks;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::firstPlayableTrack() const {
  const auto tracks = playableTracks();
  if (tracks.empty()) {
    return std::nullopt;
  }
  return tracks.front();
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::findPlayableTrack(const TrackIdentity& identity) const {
  if (identity.trackId.empty()) {
    return std::nullopt;
  }
  const auto tracks = playableTracks();
  const auto trackIt = std::find_if(tracks.begin(), tracks.end(), [&](const PlayableTrack& track) {
    return track.identity.trackId == identity.trackId && (identity.filePath.empty() || track.identity.filePath == identity.filePath);
  });
  if (trackIt == tracks.end()) {
    return std::nullopt;
  }
  return *trackIt;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::nextTrack(bool forward) {
  const auto tracks = playableTracks();
  if (tracks.empty()) {
    return std::nullopt;
  }
  if (player_.shuffle && tracks.size() > 1U) {
    return shuffledTrack(tracks);
  }
  if (!selectedTrack_.has_value()) {
    return tracks.front();
  }
  const auto currentIt = std::find_if(tracks.begin(), tracks.end(), [&](const PlayableTrack& track) { return sameTrack(track.identity, *selectedTrack_); });
  if (currentIt == tracks.end()) {
    return tracks.front();
  }
  const auto index = static_cast<std::size_t>(std::distance(tracks.begin(), currentIt));
  if (forward) {
    if (index + 1U < tracks.size()) {
      return tracks[index + 1U];
    }
    if (player_.repeatMode == RepeatMode::All) {
      return tracks.front();
    }
  } else {
    if (index > 0U) {
      return tracks[index - 1U];
    }
    if (player_.repeatMode == RepeatMode::All) {
      return tracks.back();
    }
  }
  return std::nullopt;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::shuffledTrack(const std::vector<PlayableTrack>& tracks) {
  if (tracks.empty()) {
    return std::nullopt;
  }
  if (!selectedTrack_.has_value()) {
    std::uniform_int_distribution<std::size_t> distribution{0U, tracks.size() - 1U};
    return tracks[distribution(shuffleRandom_)];
  }
  std::vector<PlayableTrack> candidates;
  std::copy_if(tracks.begin(), tracks.end(), std::back_inserter(candidates), [&](const PlayableTrack& track) {
    return !sameTrack(track.identity, *selectedTrack_);
  });
  if (candidates.empty()) {
    return player_.repeatMode == RepeatMode::All ? std::optional<PlayableTrack>{tracks.front()} : std::nullopt;
  }
  std::uniform_int_distribution<std::size_t> distribution{0U, candidates.size() - 1U};
  return candidates[distribution(shuffleRandom_)];
}

std::chrono::milliseconds ControlStateReducer::clampPosition(std::chrono::milliseconds position) const {
  position = std::max(position, std::chrono::milliseconds{0});
  if (player_.timeline.duration.has_value()) {
    position = std::min(position, *player_.timeline.duration);
  }
  return position;
}

ControlReduction ControlStateReducer::accept() {
  return ControlReduction{.result = MediaControllerCommandResult{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}}};
}

ControlReduction ControlStateReducer::reject(MediaControllerErrorCode code, std::string message) {
  auto reduction = ControlReduction{.result = MediaControllerCommandResult{.accepted = false, .code = code, .message = message}};
  addNotification(reduction, makeErrorNotification(ControlDomainNotificationKind::CommandRejected, code, std::move(message)));
  return reduction;
}

void ControlStateReducer::markPlayerChanged(ControlReduction& reduction, std::chrono::steady_clock::time_point sampledAt) {
  ++player_.freshness.version;
  if (sampledAt != std::chrono::steady_clock::time_point{}) {
    player_.freshness.sampledAt = sampledAt;
  }
  reduction.playerStateChanged = true;
}

void ControlStateReducer::addNotification(ControlReduction& reduction, ControlDomainNotification notification) {
  recentNotifications_.push_back(notification);
  if (recentNotifications_.size() > kRecentNotificationLimit) {
    recentNotifications_.erase(recentNotifications_.begin());
  }
  reduction.notifications.push_back(std::move(notification));
}

void ControlStateReducer::selectTrack(ControlReduction& reduction, const PlayableTrack& track, bool startPlayback) {
  selectedTrack_ = track.identity;
  player_.currentTrack = track.identity;
  player_.display = displayFromRequest(track.request);
  player_.timeline.position = track.request.offset.value_or(std::chrono::milliseconds{0});
  player_.timeline.duration = track.request.duration;
  player_.playback.state = startPlayback ? PlaybackStatus::Playing : PlaybackStatus::Stopped;
  player_.playback.errorCode.reset();
  player_.playback.errorMessage.reset();
  reduction.intents.push_back(makeTrackIntent(track.request));
  if (startPlayback) {
    reduction.intents.push_back(makeIntent(ControlIntentKind::Play));
  }
  markPlayerChanged(reduction);
}

void ControlStateReducer::stopPlayback(ControlReduction& reduction) {
  player_.playback.state = PlaybackStatus::Stopped;
  player_.timeline.position = std::chrono::milliseconds{0};
  markPlayerChanged(reduction);
}

}
