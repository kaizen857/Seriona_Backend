#include "control_state_reducer.h"

#include "spdlog/spdlog.h"

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

[[nodiscard]] std::string errorCodeText(MediaControllerErrorCode code) {
  switch (code) {
  case MediaControllerErrorCode::None:
    return "none";
  case MediaControllerErrorCode::ControllerStopped:
    return "controller_stopped";
  case MediaControllerErrorCode::NoPlayableTrack:
    return "no_playable_track";
  case MediaControllerErrorCode::TrackNotInLibrary:
    return "track_not_in_library";
  case MediaControllerErrorCode::InvalidCommand:
    return "invalid_command";
  case MediaControllerErrorCode::BackendRejected:
    return "backend_rejected";
  }
  return "unknown";
}

[[nodiscard]] std::string playbackStatusName(PlaybackStatus status) {
  switch (status) {
  case PlaybackStatus::Stopped:
    return "stopped";
  case PlaybackStatus::Playing:
    return "playing";
  case PlaybackStatus::Paused:
    return "paused";
  case PlaybackStatus::Loading:
    return "loading";
  case PlaybackStatus::Seeking:
    return "seeking";
  case PlaybackStatus::Buffering:
    return "buffering";
  case PlaybackStatus::Error:
    return "error";
  }
  return "unknown";
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
  return lhs.trackId == rhs.trackId;
}

[[nodiscard]] TrackIdentity identityFromSong(const scanner::SongMetadata& song) {
  return TrackIdentity{.trackId = song.trackId, .filePath = song.filePath, .sourceId = {}, .libraryId = {}};
}

[[nodiscard]] bool isCueDerivedTrack(const scanner::SongMetadata& song) {
  return !song.sourceFilePath.empty() && song.sourceFilePath != song.filePath && song.offset.has_value();
}

[[nodiscard]] audio::TrackPlaybackRequest requestFromSong(const scanner::SongMetadata& song) {
  // For CUE tracks, use sourceFilePath (the actual audio file) instead of filePath (the .cue file)
  const auto& actualAudioPath = !song.sourceFilePath.empty() ? song.sourceFilePath : song.filePath;
  
  return audio::TrackPlaybackRequest{.trackId = song.trackId,
                                     .filePath = actualAudioPath,
                                     .title = song.title,
                                     .artist = song.artist,
                                     .offset = song.offset,
                                     .duration = song.duration,
                                     .sampleRate = song.sampleRate,
                                     .bitDepth = song.bitDepth,
                                     .channels = song.channels,
                                     .format = {},
                                     .boundedSegment = isCueDerivedTrack(song)};
}

[[nodiscard]] DisplayMetadata displayFromSong(const scanner::SongMetadata& song) {
  return DisplayMetadata{.title = song.title,
                         .artist = song.artist,
                         .album = song.album,
                         .albumArtist = song.albumArtist,
                         .genre = song.genre};
}

[[nodiscard]] std::optional<ArtworkRef> artworkFromSong(const scanner::SongMetadata& song) {
  const auto hasArtworkPath = song.artworkPath.has_value() && !song.artworkPath->empty();
  const auto hasThumbnail = song.thumbnailPath.has_value() && !song.thumbnailPath->empty();
  if (!hasArtworkPath && !hasThumbnail) {
    return std::nullopt;
  }
  ArtworkRef ref{};
  // Thumbnail-first: preferred path starts at the thumbnail and is upgraded
  // by the async resolver; legacy full artwork stays available.
  ref.localPath = hasThumbnail ? song.thumbnailPath : song.artworkPath;
  ref.thumbnailPath = hasThumbnail ? song.thumbnailPath : std::nullopt;
  if (!song.contentHash.empty()) {
    ref.contentHash = song.contentHash;
  }
  return ref;
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

[[nodiscard]] bool isContextContainerNode(const scanner::PlaylistNode& node) noexcept {
  return node.kind == scanner::PlaylistNodeKind::Root || node.kind == scanner::PlaylistNodeKind::Directory ||
         node.kind == scanner::PlaylistNodeKind::Album || node.kind == scanner::PlaylistNodeKind::Disc;
}

[[nodiscard]] std::filesystem::path defaultPlaybackRootPath(const scanner::SongMetadata& song) {
  const auto root = song.filePath.root_path();
  if (!root.empty()) {
    return root;
  }
  return std::filesystem::path{"."};
}

[[nodiscard]] MediaControllerErrorCode contextBuildErrorCode(PlaybackContextBuildStatus status) noexcept {
  switch (status) {
  case PlaybackContextBuildStatus::Ready:
    return MediaControllerErrorCode::None;
  case PlaybackContextBuildStatus::InvalidDescriptor:
    return MediaControllerErrorCode::InvalidCommand;
  case PlaybackContextBuildStatus::AnchorNotFound:
    return MediaControllerErrorCode::TrackNotInLibrary;
  case PlaybackContextBuildStatus::ContextNotFound:
  case PlaybackContextBuildStatus::EmptyContext:
    return MediaControllerErrorCode::NoPlayableTrack;
  }
  return MediaControllerErrorCode::InvalidCommand;
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

[[nodiscard]] ControlIntent makeArtworkResolveIntent(ArtworkResolveRequest request) {
  auto intent = makeIntent(ControlIntentKind::ResolveArtwork);
  intent.artworkRequest = std::move(request);
  return intent;
}

// The audio backend confirms the track we already selected when the playback
// request matches on the fields that define the segment (CUE offset/duration
// and the referenced audio source). Title/artist are echoed verbatim by the
// backend and carry no identity information, so they are not compared.
[[nodiscard]] bool matchesRequest(const audio::TrackPlaybackRequest& lhs, const audio::TrackPlaybackRequest& rhs) {
  return lhs.trackId == rhs.trackId && lhs.filePath == rhs.filePath && lhs.offset == rhs.offset &&
         lhs.duration == rhs.duration && lhs.boundedSegment == rhs.boundedSegment;
}

[[nodiscard]] ControlIntent makeSeekIntent(std::chrono::milliseconds position) {
  auto intent = makeIntent(ControlIntentKind::Seek);
  intent.position = position;
  return intent;
}

[[nodiscard]] bool isStableSeekVisibleState(PlaybackStatus state) noexcept {
  return state == PlaybackStatus::Playing || state == PlaybackStatus::Paused;
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

[[nodiscard]] ControlIntent makeConfigureOutputIntent(const audio::AudioOutputConfig& config) {
  auto intent = makeIntent(ControlIntentKind::ConfigureOutput);
  intent.outputConfig = config;
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

ShuffleHistory::ShuffleHistory(std::size_t maxSize) : maxSize_(maxSize) {}

void ShuffleHistory::push(const TrackIdentity& track) {
  history_.push_back(track);
  if (history_.size() > maxSize_) {
    history_.pop_front();
  }
}

std::optional<TrackIdentity> ShuffleHistory::pop() {
  if (history_.empty()) {
    return std::nullopt;
  }
  auto track = history_.back();
  history_.pop_back();
  return track;
}

bool ShuffleHistory::contains(const TrackIdentity& track) const {
  return std::find_if(history_.begin(), history_.end(), [&](const TrackIdentity& item) {
    return sameTrack(item, track);
  }) != history_.end();
}

void ShuffleHistory::clear() {
  history_.clear();
}

std::size_t ShuffleHistory::size() const noexcept {
  return history_.size();
}

bool ShuffleHistory::empty() const noexcept {
  return history_.empty();
}

ControlStateReducer::ControlStateReducer(MediaControllerOptions options) 
    : shuffleRandom_{options.shuffleSeed},
      shuffleHistory_{options.shuffleHistorySize},
      shuffleHistorySize_{options.shuffleHistorySize} {
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
      if (player_.playback.state == PlaybackStatus::Stopped) {
        if (activateTrackWithDefaultContext(reduction, *selectedTrack_, true)) {
          spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Playing));
          return reduction;
        }
      }
      reduction.intents.push_back(makeIntent(player_.playback.state == PlaybackStatus::Paused ? ControlIntentKind::Resume : ControlIntentKind::Play));
      player_.playback.state = PlaybackStatus::Playing;
      spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Playing));
      markPlayerChanged(reduction);
      return reduction;
    }
    if (const auto track = firstPlayableTrack(); track.has_value()) {
      if (!activateTrackWithDefaultContext(reduction, track->identity, true)) {
        selectTrack(reduction, *track, true);
      }
      spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Playing));
      return reduction;
    }
    stopPlayback(reduction);
    return reject(MediaControllerErrorCode::NoPlayableTrack, "No playable track is available in the current library");
  case MediaControlCommandKind::Pause:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Pause));
    player_.playback.state = PlaybackStatus::Paused;
    spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Paused));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::Stop:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Stop));
    stopPlayback(reduction);
    spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Stopped));
    return reduction;
  case MediaControlCommandKind::TogglePlayPause:
    if (player_.playback.state == PlaybackStatus::Stopped && selectedTrack_.has_value()) {
      if (activateTrackWithDefaultContext(reduction, *selectedTrack_, true)) {
        spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Playing));
        return reduction;
      }
    }
    reduction.intents.push_back(makeIntent(player_.playback.state == PlaybackStatus::Playing   ? ControlIntentKind::Pause
                                            : player_.playback.state == PlaybackStatus::Paused ? ControlIntentKind::Resume
                                                                                                : ControlIntentKind::Play));
    player_.playback.state = player_.playback.state == PlaybackStatus::Playing ? PlaybackStatus::Paused : PlaybackStatus::Playing;
    spdlog::debug("state: {}", playbackStatusName(player_.playback.state));
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SeekTo:
    if (!command.position.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SeekTo requires an absolute position");
    }
    if (isStableSeekVisibleState(player_.playback.state)) {
      visibleStateDuringSeek_ = player_.playback.state;
      spdlog::debug("seek visible state suppressed (holding {})", playbackStatusName(*visibleStateDuringSeek_));
    }
    {
      const auto trackPosition = clampPosition(*command.position);
      auto filePosition = trackPosition;
      if (currentTrackOffset_.has_value()) {
        filePosition = trackPosition + *currentTrackOffset_;
      }
      player_.timeline.position = trackPosition;
      reduction.intents.push_back(makeSeekIntent(filePosition));
    }
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SeekBy:
    if (!command.delta.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SeekBy requires a delta");
    }
    if (isStableSeekVisibleState(player_.playback.state)) {
      visibleStateDuringSeek_ = player_.playback.state;
      spdlog::debug("seek visible state suppressed (holding {})", playbackStatusName(*visibleStateDuringSeek_));
    }
    {
      const auto trackPosition = clampPosition(player_.timeline.position + *command.delta);
      auto filePosition = trackPosition;
      if (currentTrackOffset_.has_value()) {
        filePosition = trackPosition + *currentTrackOffset_;
      }
      player_.timeline.position = trackPosition;
      reduction.intents.push_back(makeSeekIntent(filePosition));
    }
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
    shuffleHistory_.clear();
    markPlayerChanged(reduction);
    return reduction;
  case MediaControlCommandKind::SkipNext:
    // 临时队列优先（T7）：next 命令先消费队列头部，播放上下文 index 冻结不动。
    if (const auto queued = consumeQueueFront(); queued.has_value()) {
      selectTrack(reduction, *queued, true);
      playingQueuedTrack_ = true;
      return reduction;
    }
    if (player_.repeatMode == RepeatMode::One && selectedTrack_.has_value()) {
      const auto track = this->selectedPlaybackContextTrack();
      if (track.has_value()) {
        selectTrack(reduction, *track, true);
        return reduction;
      }
    }
    if (player_.shuffle && selectedTrack_.has_value()) {
      shuffleHistory_.push(*selectedTrack_);
    }
    if (const auto track = nextTrack(true); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    stopPlayback(reduction);
    return reduction;
  case MediaControlCommandKind::SkipPrevious: {
    // 如果播放超过 5 秒，上一首等于 seek 到 0
    if (player_.timeline.position > std::chrono::seconds{5}) {
      if (isStableSeekVisibleState(player_.playback.state)) {
        visibleStateDuringSeek_ = player_.playback.state;
        spdlog::debug("seek visible state suppressed (holding {})", playbackStatusName(*visibleStateDuringSeek_));
      }
      player_.timeline.position = std::chrono::milliseconds{0};
      auto filePosition = std::chrono::milliseconds{0};
      if (currentTrackOffset_.has_value()) {
        filePosition = *currentTrackOffset_;
      }
      reduction.intents.push_back(makeSeekIntent(filePosition));
      markPlayerChanged(reduction);
      return reduction;
    }
    // Repeat One 模式
    if (player_.repeatMode == RepeatMode::One && selectedTrack_.has_value()) {
      const auto track = this->selectedPlaybackContextTrack();
      if (track.has_value()) {
        selectTrack(reduction, *track, true);
        return reduction;
      }
    }
    // 尝试上一首
    if (const auto track = nextTrack(false); track.has_value()) {
      selectTrack(reduction, *track, true);
      return reduction;
    }
    // 没有上一首，seek 到 0
    if (isStableSeekVisibleState(player_.playback.state)) {
      visibleStateDuringSeek_ = player_.playback.state;
      spdlog::debug("seek visible state suppressed (holding {})", playbackStatusName(*visibleStateDuringSeek_));
    }
    player_.timeline.position = std::chrono::milliseconds{0};
    auto filePositionFallback = std::chrono::milliseconds{0};
    if (currentTrackOffset_.has_value()) {
      filePositionFallback = *currentTrackOffset_;
    }
    reduction.intents.push_back(makeSeekIntent(filePositionFallback));
    markPlayerChanged(reduction);
    return reduction;
  }
  case MediaControlCommandKind::SelectTrack:
    if (!command.track.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "SelectTrack requires a track identity");
    }
    shuffleHistory_.clear();
    if (activateTrackWithDefaultContext(reduction, *command.track, true)) {
      return reduction;
    }
    return reject(MediaControllerErrorCode::TrackNotInLibrary, "Selected track is not present in the current library");
  case MediaControlCommandKind::PlayNextTrack:
    if (!command.track.has_value() || command.track->trackId.empty()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "PlayNextTrack requires a track identity");
    }
    {
      const auto track = findPlayableTrack(*command.track);
      if (!track.has_value()) {
        return reject(MediaControllerErrorCode::TrackNotInLibrary, "PlayNextTrack target is not present in the current library");
      }
      playbackQueue_.push_front(QueueEntry{.trackId = track->identity.trackId,
                                           .nodeId = track->parentNodeId.value_or(std::string{})});
      syncQueueSnapshot();
      markPlayerChanged(reduction);
      spdlog::debug("play queue entry added at front: '{}' (queue size {})", track->identity.trackId, playbackQueue_.size());
      return reduction;
    }
  case MediaControlCommandKind::ClearPlayQueue:
    if (!playbackQueue_.empty()) {
      playbackQueue_.clear();
      syncQueueSnapshot();
      markPlayerChanged(reduction);
      spdlog::debug("play queue cleared");
    }
    return reduction;
  case MediaControlCommandKind::RemoveFromQueue:
    if (!command.queueIndex.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "RemoveFromQueue requires a queue index");
    }
    if (*command.queueIndex < playbackQueue_.size()) {
      playbackQueue_.erase(playbackQueue_.begin() + static_cast<std::ptrdiff_t>(*command.queueIndex));
      syncQueueSnapshot();
      markPlayerChanged(reduction);
      spdlog::debug("play queue entry removed at index {}", *command.queueIndex);
    }
    return reduction;
  case MediaControlCommandKind::StartPlaybackFromContext: {
    if (!command.playbackContext.has_value()) {
      return reject(MediaControllerErrorCode::InvalidCommand, "StartPlaybackFromContext requires a playback context");
    }
    PlaybackContextBuildStatus status{PlaybackContextBuildStatus::InvalidDescriptor};
    auto context = buildPlaybackContextState(*command.playbackContext, &status);
    if (!context.has_value()) {
      const auto code = contextBuildErrorCode(status);
      return reject(code, "Playback context could not produce a playable order");
    }
    shuffleHistory_.clear();
    playbackContext_ = std::move(*context);
    selectTrack(reduction, playbackContext_->order[playbackContext_->index], true);
    return reduction;
  }
  case MediaControlCommandKind::ConfigureOutput:
    return handleConfigureOutput(reduction, command);
  case MediaControlCommandKind::DeleteTrack:
  case MediaControlCommandKind::DeleteFolder:
    // 删除涉及文件系统与 scanner 缓存，必须经 MediaController（service 层）执行；
    // reducer 不直接做文件系统操作。
    return reject(MediaControllerErrorCode::InvalidCommand, "Delete commands must be handled by MediaController");
  case MediaControlCommandKind::ApplyFolderSortRules:
    return reject(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules must be handled by MediaController");
  }

  return reject(MediaControllerErrorCode::InvalidCommand, "Unsupported media control command");
}

ControlReduction ControlStateReducer::handleConfigureOutput(ControlReduction& reduction, const MediaControlCommand& command) {
  if (!command.outputConfig.has_value()) {
    return reject(MediaControllerErrorCode::InvalidCommand, "ConfigureOutput requires an output config");
  }
  const auto& config = *command.outputConfig;
  const auto mode = static_cast<int>(config.outputMode);
  if (mode != static_cast<int>(audio::AudioOutputMode::Direct) && mode != static_cast<int>(audio::AudioOutputMode::Mixed)) {
    return reject(MediaControllerErrorCode::InvalidCommand, "ConfigureOutput requires a valid output mode");
  }
  if (config.targetSampleRate.has_value() && (*config.targetSampleRate < 8000U || *config.targetSampleRate > 768000U)) {
    return reject(MediaControllerErrorCode::InvalidCommand, "ConfigureOutput sample rate is out of range (8000-768000)");
  }
  if (config.bufferDuration.count() < 50 || config.bufferDuration.count() > 1000) {
    return reject(MediaControllerErrorCode::InvalidCommand, "ConfigureOutput buffer duration is out of range (50-1000 ms)");
  }

  // 应用配置：转发给音频后端。
  reduction.intents.push_back(makeConfigureOutputIntent(config));

  // 立即重载：仅当存在选中曲目时重载，保持位置（含 CUE 偏移）与播放状态。
  if (!selectedTrack_.has_value()) {
    return reduction;
  }
  auto track = findPlayableTrack(*selectedTrack_);
  if (!track.has_value()) {
    track = selectedPlaybackContextTrack();
  }
  if (!track.has_value()) {
    return reduction;
  }
  reduction.intents.push_back(makeTrackIntent(track->request));
  const auto seekPosition = player_.timeline.position + currentTrackOffset_.value_or(std::chrono::milliseconds{0});
  reduction.intents.push_back(makeSeekIntent(seekPosition));
  switch (player_.playback.state) {
  case PlaybackStatus::Playing:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Play));
    break;
  case PlaybackStatus::Paused:
    reduction.intents.push_back(makeIntent(ControlIntentKind::Pause));
    break;
  case PlaybackStatus::Loading:
  case PlaybackStatus::Buffering:
  case PlaybackStatus::Seeking:
    // 参照 reconcilePlaybackContextAfterSnapshot 的 shouldContinuePlayback 先例：
    // 这些状态重载后应恢复播放，否则会静默停止。
    reduction.intents.push_back(makeIntent(ControlIntentKind::Play));
    break;
  case PlaybackStatus::Stopped:
  case PlaybackStatus::Error:
    break;
  }
  return reduction;
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
          const auto mappedState = mapPlaybackState(payload.state);
          if (visibleStateDuringSeek_.has_value() && mappedState == PlaybackStatus::Loading) {
            spdlog::debug("seek visible state suppressed (holding {})", playbackStatusName(*visibleStateDuringSeek_));
            player_.playback.state = *visibleStateDuringSeek_;
          } else {
            player_.playback.state = mappedState;
            spdlog::debug("state: {}", playbackStatusName(mappedState));
            if (mappedState == PlaybackStatus::Playing || mappedState == PlaybackStatus::Stopped || mappedState == PlaybackStatus::Error) {
              visibleStateDuringSeek_.reset();
            }
          }
          if (player_.playback.state != PlaybackStatus::Error) {
            player_.playback.errorCode.reset();
            player_.playback.errorMessage.reset();
          }
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::TrackChanged>) {
          const auto selected = selectedPlaybackContextTrack();
          if (selected.has_value() && matchesRequest(payload.request, selected->request)) {
            // The audio backend confirms the already-selected track (including
            // CUE offset/duration/source): keep the logical identity, display
            // and artwork instead of replacing them with identityFromRequest.
            spdlog::debug("track changed matches selected track '{}'", payload.request.trackId);
          } else {
            const auto identity = identityFromRequest(payload.request);
            selectedTrack_ = identity;
            player_.currentTrack = selectedTrack_;
            if (const auto track = findPlayableTrack(identity); track.has_value()) {
              player_.display = track->display;
              player_.artwork = track->artwork;
            } else {
              player_.display = displayFromRequest(payload.request);
            }
          }
          player_.timeline.position = payload.request.offset.value_or(std::chrono::milliseconds{0});
          player_.timeline.duration = payload.request.duration;
          markPlayerChanged(reduction, event.timestamp);
        } else if constexpr (std::is_same_v<Payload, audio::PlaybackPositionUpdated>) {
          auto filePosition = payload.clock.position;
          auto trackPosition = filePosition;
          if (currentTrackOffset_.has_value()) {
            trackPosition = std::max(std::chrono::milliseconds{0}, filePosition - *currentTrackOffset_);
          }
          if (player_.timeline.duration.has_value()) {
            trackPosition = std::min(trackPosition, *player_.timeline.duration);
          }
          player_.timeline.position = trackPosition;
          markPlayerChanged(reduction, payload.clock.sampledAt);
        } else if constexpr (std::is_same_v<Payload, audio::PositionDiscontinuity>) {
          auto filePosition = payload.after.position;
          auto trackPosition = filePosition;
          if (currentTrackOffset_.has_value()) {
            trackPosition = std::max(std::chrono::milliseconds{0}, filePosition - *currentTrackOffset_);
          }
          if (player_.timeline.duration.has_value()) {
            trackPosition = std::min(trackPosition, *player_.timeline.duration);
          }
          player_.timeline.position = trackPosition;
          markPlayerChanged(reduction, payload.after.sampledAt);
	        } else if constexpr (std::is_same_v<Payload, audio::PlaybackEnded>) {
	          if (selectedTrack_.has_value() && !sameTrack(identityFromRequest(payload.request), *selectedTrack_)) {
	            spdlog::debug("ignoring stale playback-ended event for track '{}'", payload.request.trackId);
	            return;
	          }
	          visibleStateDuringSeek_.reset();
	          {
            auto filePosition = payload.finalClock.position;
            auto trackPosition = filePosition;
            if (currentTrackOffset_.has_value()) {
              trackPosition = std::max(std::chrono::milliseconds{0}, filePosition - *currentTrackOffset_);
            }
            if (player_.timeline.duration.has_value()) {
              trackPosition = std::min(trackPosition, *player_.timeline.duration);
            }
            player_.timeline.position = trackPosition;
	          }
          if (const auto queued = consumeQueueFront(); queued.has_value()) {
            // 临时队列优先（T7）：消费队列头部，播放上下文 index 冻结不动；
            // 队列空后由文件夹序列从冻结 index 的下一曲继续。
            selectTrack(reduction, *queued, true);
            playingQueuedTrack_ = true;
          } else if (player_.repeatMode == RepeatMode::One) {
            if (const auto track = selectedTrack_.has_value() ? this->selectedPlaybackContextTrack() : std::nullopt; track.has_value()) {
              selectTrack(reduction, *track, true);
            } else {
              stopPlayback(reduction);
            }
          } else if (player_.shuffle && playbackContext_.has_value() && !playbackContext_->order.empty()) {
	            if (const auto track = shuffledTrack(playbackContext_->order, /*reshuffleWhenExhausted=*/true); track.has_value()) {
	              selectTrack(reduction, *track, true);
	            } else if (const auto current = selectedPlaybackContextTrack(); current.has_value()) {
	              selectTrack(reduction, *current, true);
	            } else {
	              stopPlayback(reduction);
	            }
          } else if (isLastTrackInContext()) {
            addNotification(reduction, makeNotification(ControlDomainNotificationKind::PlaybackEnded, "Playback ended"));
	            if (player_.repeatMode == RepeatMode::All) {
	              if (const auto track = firstTrackOfCurrentFolder(); track.has_value()) {
	                selectTrack(reduction, *track, true);
	              } else {
	                stopPlayback(reduction);
	              }
	            } else {
	              stopPlayback(reduction);
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
          spdlog::warn("playback error ({}): {}", playbackErrorCode(payload.code), payload.message);
          visibleStateDuringSeek_.reset();
          // BufferUnderrun 是瞬态欠载：音频层已补静音继续播放、状态机不变；
          // 这里置 Error 会造成控制层与音频层状态错位，后续 Play/Pause 命令
          // 会被音频层判为非法转换而反复报错（按钮失控）。仅记录诊断不置 Error。
          if (payload.code != audio::PlaybackErrorCode::BufferUnderrun) {
            player_.playback.state = PlaybackStatus::Error;
          }
          player_.playback.errorCode = playbackErrorCode(payload.code);
          player_.playback.errorMessage = payload.message;
          if (payload.clock.has_value()) {
            auto filePosition = payload.clock->position;
            auto trackPosition = filePosition;
            if (currentTrackOffset_.has_value()) {
              trackPosition = std::max(std::chrono::milliseconds{0}, filePosition - *currentTrackOffset_);
            }
            if (player_.timeline.duration.has_value()) {
              trackPosition = std::min(trackPosition, *player_.timeline.duration);
            }
            player_.timeline.position = trackPosition;
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
	if (event.type == scanner::ScannerEventType::FileScanned) {
	  return {};
	}

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
	    if (const auto* progress = std::get_if<scanner::ScanProgress>(&event.payload)) {
	      library_.scanProgress = *progress;
	    }
	    addNotification(reduction, makeScanNotification(ControlDomainNotificationKind::LibraryScanProgressUpdated,
	                                                    "Library scan progress updated",
	                                                    library_.scanStatus));
	    break;
	  case scanner::ScannerEventType::FileScanned:
	    return {};
	  case scanner::ScannerEventType::PlaylistSnapshotUpdated:
    if (const auto* snapshot = std::get_if<scanner::PlaylistTreeSnapshot>(&event.payload)) {
      library_.libraryTree = *snapshot;
      library_.version = snapshot->version;
      if (playbackContext_.has_value()) {
        reconcilePlaybackContextAfterSnapshot(reduction);
      } else {
        selectFirstTrackWhenIdle(reduction);
      }
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
  case scanner::ScannerEventType::ScanError: {
    if (const auto* error = std::get_if<scanner::ScannerError>(&event.payload)) {
      library_.lastError = *error;
      // 仅致命错误（根不可用 / 缓存不可用）视为整体扫描失败：
      // 文件级错误（个别文件元数据读取失败等）不终止扫描，若置 Error 会造成
      // UI 状态跳动（扫描失败 → 扫描中 → 扫描完成）并逐条弹出错误通知。
      // 取消（Cancelled）随后由 ScanStopped 正常收尾，也不在此置 Error。
      const bool fatal = error->code == scanner::ScannerErrorCode::RootUnavailable
          || error->code == scanner::ScannerErrorCode::CacheUnavailable;
      if (!fatal) {
        break;
      }
      library_.scanStatus = LibraryScanStatus::Error;
      auto notification = makeErrorNotification(ControlDomainNotificationKind::LibraryScanError,
                                                MediaControllerErrorCode::BackendRejected,
                                                error->message);
      notification.scanStatus = library_.scanStatus;
      addNotification(reduction, std::move(notification));
    }
    break;
  }
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
      tracks.push_back(PlayableTrack{.identity = identityFromSong(*node.song),
                                     .request = requestFromSong(*node.song),
                                     .display = displayFromSong(*node.song),
                                     .artwork = artworkFromSong(*node.song),
                                     .artworkSourcePath = !node.song->sourceFilePath.empty() ? node.song->sourceFilePath : node.song->filePath,
                                     .fallbackThumbnailPath = node.song->thumbnailPath.value_or(std::filesystem::path{}),
                                     .parentNodeId = node.parentNodeId});
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

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::findPlayableTrackByTrackId(const std::string& trackId) const {
  if (trackId.empty()) {
    return std::nullopt;
  }
  const auto tracks = playableTracks();
  const auto trackIt = std::find_if(tracks.begin(), tracks.end(), [&](const PlayableTrack& track) {
    return track.identity.trackId == trackId;
  });
  if (trackIt == tracks.end()) {
    return std::nullopt;
  }
  return *trackIt;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::consumeQueueFront() {
  while (!playbackQueue_.empty()) {
    const auto entry = playbackQueue_.front();
    playbackQueue_.pop_front();
    syncQueueSnapshot();
    if (const auto track = findPlayableTrackByTrackId(entry.trackId); track.has_value()) {
      return track;
    }
    spdlog::debug("play queue entry '{}' is no longer playable; skipped", entry.trackId);
  }
  return std::nullopt;
}

void ControlStateReducer::syncQueueSnapshot() {
  player_.queueEntries.assign(playbackQueue_.begin(), playbackQueue_.end());
}

std::optional<PlaybackContextDescriptor> ControlStateReducer::defaultContextDescriptorForTrack(const TrackIdentity& identity) const {
  if (identity.trackId.empty() || !library_.libraryTree.has_value()) {
    return std::nullopt;
  }

  const auto& tree = *library_.libraryTree;
  const auto trackIt = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const scanner::PlaylistNode& node) {
    return isTrackNode(node) && node.song->trackId == identity.trackId &&
           (identity.filePath.empty() || node.song->filePath == identity.filePath);
  });
  if (trackIt == tree.nodes.end()) {
    return std::nullopt;
  }

  PlaybackContextDescriptor descriptor{};
  descriptor.scope = PlaybackContextScope::Root;
  descriptor.rootPath = defaultPlaybackRootPath(*trackIt->song);
  descriptor.anchorTrack = identityFromSong(*trackIt->song);

  if (trackIt->parentNodeId.has_value()) {
    const auto parentIt = std::find_if(tree.nodes.begin(), tree.nodes.end(), [&](const scanner::PlaylistNode& node) {
      return node.nodeId == *trackIt->parentNodeId;
    });
    const auto parentIsRoot = tree.rootNodeId.has_value() && parentIt != tree.nodes.end() && parentIt->nodeId == *tree.rootNodeId;
    if (parentIt != tree.nodes.end() && !parentIsRoot && isContextContainerNode(*parentIt)) {
      descriptor.scope = PlaybackContextScope::Folder;
      descriptor.folderNodeId = parentIt->nodeId;
    }
  }

  return descriptor;
}

std::optional<ControlStateReducer::PlaybackContextState> ControlStateReducer::buildPlaybackContextState(
    PlaybackContextDescriptor descriptor,
    PlaybackContextBuildStatus* status) const {
  if (!library_.libraryTree.has_value()) {
    if (status != nullptr) {
      *status = PlaybackContextBuildStatus::ContextNotFound;
    }
    return std::nullopt;
  }

  auto result = buildPlaybackContextOrder(*library_.libraryTree, std::move(descriptor));
  if (status != nullptr) {
    *status = result.status;
  }
  if (result.status != PlaybackContextBuildStatus::Ready || !result.anchorIndex.has_value()) {
    return std::nullopt;
  }

  PlaybackContextState state{};
  state.descriptor = std::move(result.context);
  state.index = *result.anchorIndex;
  state.order.reserve(result.order.size());
  for (const auto& item : result.order) {
    state.order.push_back(PlayableTrack{.identity = item.identity,
                                        .request = requestFromSong(item.metadata),
                                        .display = displayFromSong(item.metadata),
                                        .artwork = artworkFromSong(item.metadata),
                                        .artworkSourcePath = !item.metadata.sourceFilePath.empty() ? item.metadata.sourceFilePath : item.metadata.filePath,
                                        .fallbackThumbnailPath = item.metadata.thumbnailPath.value_or(std::filesystem::path{}),
                                        .parentNodeId = item.parentNodeId});
  }
  return state;
}

std::optional<std::size_t> ControlStateReducer::selectedContextIndex() const {
  if (!playbackContext_.has_value() || !selectedTrack_.has_value()) {
    return std::nullopt;
  }
  const auto iterator = std::find_if(playbackContext_->order.begin(), playbackContext_->order.end(), [&](const PlayableTrack& track) {
    return sameTrack(track.identity, *selectedTrack_);
  });
  if (iterator == playbackContext_->order.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(playbackContext_->order.begin(), iterator));
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::selectedPlaybackContextTrack() {
  const auto currentIndex = selectedContextIndex();
  if (!currentIndex.has_value()) {
    return std::nullopt;
  }
  playbackContext_->index = *currentIndex;
  return playbackContext_->order[*currentIndex];
}

bool ControlStateReducer::activateTrackWithDefaultContext(ControlReduction& reduction,
                                                          const TrackIdentity& identity,
                                                          bool startPlayback) {
  const auto descriptor = defaultContextDescriptorForTrack(identity);
  if (!descriptor.has_value()) {
    return false;
  }
  auto context = buildPlaybackContextState(*descriptor);
  if (!context.has_value()) {
    return false;
  }
  playbackContext_ = std::move(*context);
  selectTrack(reduction, playbackContext_->order[playbackContext_->index], startPlayback);
  return true;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::nextTrack(bool forward) {
  if (!playbackContext_.has_value() || playbackContext_->order.empty()) {
    return std::nullopt;
  }
  const auto currentIndex = selectedContextIndex();
  if (selectedTrack_.has_value() && !currentIndex.has_value()) {
    // 当前选中曲目不在播放上下文 order 中。仅当它来自临时队列（playingQueuedTrack_）
    // 时才保留冻结的 playbackContext_->index 继续（A 歌曲1 → 插播 B → B 播完 →
    // A 歌曲2）；普通上下文漂移保持旧语义（返回空，由调用方停止播放）。
    if (!playingQueuedTrack_ || !playbackContext_.has_value()) {
      return std::nullopt;
    }
  } else if (currentIndex.has_value()) {
    playbackContext_->index = *currentIndex;
  }
  const auto& tracks = playbackContext_->order;
  
  // 上一曲逻辑
  if (!forward) {
    // 随机模式：从历史栈返回
    if (player_.shuffle) {
      return previousTrack();
    }
    // 非随机模式：返回列表中的上一个
    if (!selectedTrack_.has_value()) {
      return std::nullopt;
    }
    if (playbackContext_->index == 0U) {
      if (player_.repeatMode == RepeatMode::All) {
        playbackContext_->index = tracks.size() - 1U;
        return tracks.back();
      }
      return std::nullopt;
    }
    --playbackContext_->index;
    return tracks[playbackContext_->index];
  }
  
  // 随机模式 + 下一曲
  if (player_.shuffle && tracks.size() > 1U) {
    return shuffledTrack(tracks);
  }
  
  // 非随机模式：顺序播放
  if (!selectedTrack_.has_value()) {
    playbackContext_->index = 0U;
    return tracks.front();
  }
  if (playbackContext_->index + 1U < tracks.size()) {
    ++playbackContext_->index;
    return tracks[playbackContext_->index];
  }
  if (player_.repeatMode == RepeatMode::All) {
    playbackContext_->index = 0U;
    return tracks.front();
  }
  return std::nullopt;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::shuffledTrack(
    const std::vector<PlayableTrack>& tracks,
    bool reshuffleWhenExhausted) {
  auto candidates = tracks;
  if (candidates.empty()) {
    return std::nullopt;
  }
  
  if (!selectedTrack_.has_value()) {
    std::uniform_int_distribution<std::size_t> distribution{0U, candidates.size() - 1U};
    return candidates[distribution(shuffleRandom_)];
  }
  
  candidates = filterOutHistory(candidates);
  
  std::vector<PlayableTrack> filtered;
  std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(filtered),
               [this](const PlayableTrack& track) {
                 return !sameTrack(track.identity, *selectedTrack_);
               });
  candidates = std::move(filtered);
  
  if (candidates.empty()) {
    if (player_.repeatMode == RepeatMode::All || reshuffleWhenExhausted) {
      shuffleHistory_.clear();
      candidates = tracks;
      candidates = filterOutHistory(candidates);
      
      std::vector<PlayableTrack> filtered2;
      std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(filtered2),
                   [this](const PlayableTrack& track) {
                     return !sameTrack(track.identity, *selectedTrack_);
                   });
      candidates = std::move(filtered2);
      
      if (candidates.empty()) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  
  std::uniform_int_distribution<std::size_t> distribution{0U, candidates.size() - 1U};
  auto selected = candidates[distribution(shuffleRandom_)];
  if (playbackContext_.has_value()) {
    const auto selectedIt = std::find_if(playbackContext_->order.begin(), playbackContext_->order.end(), [&](const PlayableTrack& track) {
      return sameTrack(track.identity, selected.identity);
    });
    if (selectedIt != playbackContext_->order.end()) {
      playbackContext_->index = static_cast<std::size_t>(std::distance(playbackContext_->order.begin(), selectedIt));
    }
  }
  return selected;
}

bool ControlStateReducer::isLastTrackInContext() const {
  if (!playbackContext_.has_value() || playbackContext_->order.empty()) {
    return false;
  }
  const auto currentIndex = selectedContextIndex();
  if (!currentIndex.has_value()) {
    return false;
  }
  return *currentIndex + 1U >= playbackContext_->order.size();
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::firstTrackOfCurrentFolder() {
  if (!playbackContext_.has_value() || playbackContext_->order.empty()) {
    return std::nullopt;
  }

  const auto containerNodeId = [&]() -> std::optional<std::string> {
    if (playbackContext_->descriptor.scope == PlaybackContextScope::Folder) {
      return playbackContext_->descriptor.folderNodeId;
    }
    if (library_.libraryTree.has_value() && library_.libraryTree->rootNodeId.has_value()) {
      return library_.libraryTree->rootNodeId;
    }
    return std::nullopt;
  }();

  if (!containerNodeId.has_value()) {
    return playbackContext_->order.front();
  }

  const auto firstIt = std::find_if(playbackContext_->order.begin(), playbackContext_->order.end(),
                                    [&](const PlayableTrack& track) {
                                      return track.parentNodeId.has_value() &&
                                             *track.parentNodeId == *containerNodeId;
                                    });
  if (firstIt == playbackContext_->order.end()) {
    return std::nullopt;
  }
  playbackContext_->index = static_cast<std::size_t>(std::distance(playbackContext_->order.begin(), firstIt));
  return *firstIt;
}

std::optional<ControlStateReducer::PlayableTrack> ControlStateReducer::previousTrack() {
  if (!playbackContext_.has_value()) {
    return std::nullopt;
  }
  while (auto previousIdentity = shuffleHistory_.pop()) {
    const auto previousIt = std::find_if(playbackContext_->order.begin(), playbackContext_->order.end(), [&](const PlayableTrack& track) {
      return sameTrack(track.identity, *previousIdentity);
    });
    if (previousIt != playbackContext_->order.end()) {
      playbackContext_->index = static_cast<std::size_t>(std::distance(playbackContext_->order.begin(), previousIt));
      return *previousIt;
    }
  }
  return std::nullopt;
}

std::vector<ControlStateReducer::PlayableTrack> ControlStateReducer::filterOutHistory(
    const std::vector<PlayableTrack>& candidates) const {
  std::vector<PlayableTrack> filtered;
  std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(filtered),
               [this](const PlayableTrack& track) {
                 return !shuffleHistory_.contains(track.identity);
               });
  return filtered;
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
  spdlog::warn("command rejected ({}): {}", errorCodeText(code), message);
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

void ControlStateReducer::reconcilePlaybackContextAfterSnapshot(ControlReduction& reduction) {
  if (!playbackContext_.has_value()) {
    selectFirstTrackWhenIdle(reduction);
    return;
  }

  const auto previousDescriptor = playbackContext_->descriptor;
  const auto previousIndex = selectedContextIndex().value_or(playbackContext_->index);
  const auto previousTrack = selectedTrack_;
  const auto previousPlaybackState = player_.playback.state;

  if (!library_.libraryTree.has_value()) {
    playbackContext_.reset();
    shuffleHistory_.clear();
    if (previousPlaybackState != PlaybackStatus::Stopped) {
      reduction.intents.push_back(makeIntent(ControlIntentKind::Stop));
    }
    stopPlayback(reduction);
    return;
  }

  auto result = buildPlaybackContextOrder(*library_.libraryTree, previousDescriptor);
  if (result.status == PlaybackContextBuildStatus::InvalidDescriptor ||
      result.status == PlaybackContextBuildStatus::ContextNotFound ||
      result.status == PlaybackContextBuildStatus::EmptyContext || result.order.empty()) {
    playbackContext_.reset();
    shuffleHistory_.clear();
    if (previousPlaybackState != PlaybackStatus::Stopped) {
      reduction.intents.push_back(makeIntent(ControlIntentKind::Stop));
    }
    stopPlayback(reduction);
    return;
  }

  PlaybackContextState rebuilt{};
  rebuilt.descriptor = std::move(result.context);
  rebuilt.order.reserve(result.order.size());
  for (const auto& item : result.order) {
    rebuilt.order.push_back(PlayableTrack{.identity = item.identity,
                                          .request = requestFromSong(item.metadata),
                                          .display = displayFromSong(item.metadata),
                                          .artwork = artworkFromSong(item.metadata),
                                          .artworkSourcePath = !item.metadata.sourceFilePath.empty() ? item.metadata.sourceFilePath : item.metadata.filePath,
                                          .fallbackThumbnailPath = item.metadata.thumbnailPath.value_or(std::filesystem::path{}),
                                          .parentNodeId = item.parentNodeId});
  }

  std::optional<std::size_t> currentIndex{};
  if (previousTrack.has_value()) {
    const auto currentIt = std::find_if(rebuilt.order.begin(), rebuilt.order.end(), [&](const PlayableTrack& track) {
      return sameTrack(track.identity, *previousTrack);
    });
    if (currentIt != rebuilt.order.end()) {
      currentIndex = static_cast<std::size_t>(std::distance(rebuilt.order.begin(), currentIt));
    }
  }

  const auto reconciledIndex = currentIndex.value_or(std::min(previousIndex, rebuilt.order.size() - 1U));
  rebuilt.index = reconciledIndex;
  rebuilt.descriptor.anchorTrack = rebuilt.order[rebuilt.index].identity;
  const auto currentTrackStillPresent = currentIndex.has_value();
  playbackContext_ = std::move(rebuilt);
  shuffleHistory_.clear();

  if (currentTrackStillPresent) {
    return;
  }

  const auto shouldContinuePlayback = previousPlaybackState == PlaybackStatus::Playing ||
                                      previousPlaybackState == PlaybackStatus::Loading ||
                                      previousPlaybackState == PlaybackStatus::Buffering ||
                                      previousPlaybackState == PlaybackStatus::Seeking;
  selectTrack(reduction, playbackContext_->order[playbackContext_->index], shouldContinuePlayback);
  if (previousPlaybackState == PlaybackStatus::Paused) {
    player_.playback.state = PlaybackStatus::Paused;
    markPlayerChanged(reduction);
  }
}

void ControlStateReducer::selectFirstTrackWhenIdle(ControlReduction& reduction) {
  if (selectedTrack_.has_value() || player_.playback.state == PlaybackStatus::Playing || player_.playback.state == PlaybackStatus::Loading) {
    return;
  }
  const auto track = firstPlayableTrack();
  if (!track.has_value()) {
    return;
  }
  selectedTrack_ = track->identity;
  currentTrackOffset_ = track->request.offset;
  player_.currentTrack = track->identity;
  player_.display = track->display;
  player_.artwork = track->artwork;
  player_.timeline.position = std::chrono::milliseconds{0};
  player_.timeline.duration = track->request.duration;
  player_.playback.state = PlaybackStatus::Stopped;
  player_.playback.errorCode.reset();
  player_.playback.errorMessage.reset();
  markPlayerChanged(reduction);
}

ControlReduction ControlStateReducer::reduceArtworkResolved(const ArtworkResolveResultView& result) {
  auto reduction = accept();
  if (result.generation != artworkGeneration_ || !selectedTrack_.has_value() ||
      !sameTrack(result.identity, *selectedTrack_)) {
    spdlog::debug("stale artwork resolution dropped (generation {} vs {})", result.generation, artworkGeneration_);
    return reduction;
  }
  if (result.outcome.kind != ArtworkResolveOutcomeKind::FullPath || !result.outcome.fullPath.has_value() ||
      result.outcome.fullPath->empty() || !player_.artwork.has_value()) {
    // No-art / cover error / resolver failure keep the thumbnail fallback.
    return reduction;
  }
  player_.artwork->localPath = *result.outcome.fullPath;
  markPlayerChanged(reduction);
  return reduction;
}

void ControlStateReducer::selectTrack(ControlReduction& reduction, const PlayableTrack& track, bool startPlayback) {
  // 切轨抑制（需求 4 按钮锁定）：startPlayback=true 时立即发布乐观 Playing 快照，
  // 并设置可见状态抑制 —— 音频层随后发布的 Loading 会被 reduceAudioEvent 压回
  // Playing（:621-629），直到真实 Playing/Stopped/Error 到达时解除；Error 必放行，
  // 真实错误不会被掩盖。注意不要无条件 reset：那会把刚设置的抑制立即清掉
  // （历史陷阱：seek 抑制变量被 selectTrack 清空）。startPlayback=false 时切轨
  // 不自动播放，无需抑制。
  visibleStateDuringSeek_ = startPlayback ? std::optional<PlaybackStatus>{PlaybackStatus::Playing} : std::nullopt;
  // 除临时队列消费路径外，切轨都回到文件夹序列来源（队列消费路径在 selectTrack
  // 之后重新置位 playingQueuedTrack_）。
  playingQueuedTrack_ = false;
  selectedTrack_ = track.identity;
  currentTrackOffset_ = track.request.offset;
  player_.currentTrack = track.identity;
  player_.display = track.display;
  player_.artwork = track.artwork;
  player_.timeline.position = std::chrono::milliseconds{0};
  player_.timeline.duration = track.request.duration;
  player_.playback.state = startPlayback ? PlaybackStatus::Playing : PlaybackStatus::Stopped;
  player_.playback.errorCode.reset();
  player_.playback.errorMessage.reset();
  spdlog::debug("state: {}", playbackStatusName(player_.playback.state));
  reduction.intents.push_back(makeTrackIntent(track.request));
  ++artworkGeneration_;
  ArtworkResolveRequest artworkRequest{};
  artworkRequest.generation = artworkGeneration_;
  artworkRequest.identity = track.identity;
  artworkRequest.artworkSourcePath = track.artworkSourcePath;
  artworkRequest.fallbackThumbnailPath = track.fallbackThumbnailPath;
  reduction.intents.push_back(makeArtworkResolveIntent(std::move(artworkRequest)));
  if (startPlayback) {
    reduction.intents.push_back(makeIntent(ControlIntentKind::Play));
  }
  markPlayerChanged(reduction);
}

void ControlStateReducer::stopPlayback(ControlReduction& reduction) {
  visibleStateDuringSeek_.reset();
  currentTrackOffset_.reset();
  player_.playback.state = PlaybackStatus::Stopped;
  player_.timeline.position = std::chrono::milliseconds{0};
  spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Stopped));
  markPlayerChanged(reduction);
}

}
