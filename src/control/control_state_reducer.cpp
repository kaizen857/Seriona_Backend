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

[[nodiscard]] ControlIntent makeSetTransitionConfigIntent(const audio::TransitionConfig& config) {
  auto intent = makeIntent(ControlIntentKind::SetTransitionConfig);
  intent.transitionConfig = config;
  return intent;
}

// 过渡参数数值域（用户裁定表；与前端任务 2/12 校验完全一致）：
// 交叉长度 0-10000ms；传送/seek/手动短交叉 0-3000ms；预加载 0-5000ms。
// 0 = 时长 0 = 该淡变即时完成（等效关闭），不做下界钳制。
[[nodiscard]] bool inTransitionRange(std::chrono::milliseconds value, std::chrono::milliseconds max) {
  return value.count() >= 0 && value <= max;
}

[[nodiscard]] bool validTransitionMode(int mode) { return mode >= 0 && mode <= 2; }

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

// T10 失效域（裁定基线⑦）：重叠窗口（EndApproaching 记录 pendingAdvance 至
// AdvanceCompleted 提交）内收到以下命令类 = 窗口内失效操作 → 立即中止过渡并重新调度；
// 窗口外（无 pending = 已提交/未进入窗口）这些命令走普通路径、零影响。
[[nodiscard]] bool isAdvanceWindowInvalidator(MediaControlCommandKind kind) {
  switch (kind) {
  case MediaControlCommandKind::Pause:
  case MediaControlCommandKind::Stop:
  case MediaControlCommandKind::TogglePlayPause:
  case MediaControlCommandKind::SeekTo:
  case MediaControlCommandKind::SeekBy:
  case MediaControlCommandKind::SkipNext:
  case MediaControlCommandKind::SkipPrevious:
  case MediaControlCommandKind::SelectTrack:
  case MediaControlCommandKind::SetRepeatMode:
  case MediaControlCommandKind::SetShuffle:
  case MediaControlCommandKind::PlayNextTrack:
  case MediaControlCommandKind::ClearPlayQueue:
  case MediaControlCommandKind::RemoveFromQueue:
  case MediaControlCommandKind::ConfigureOutput:
  case MediaControlCommandKind::SetTransitionConfig:
  case MediaControlCommandKind::StartPlaybackFromContext:
    return true;
  default:
    return false;
  }
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

  // T10 N1：reject 回补标志每命令复位（事件路径 abort 的置位不得跨命令残留生效）。
  carryAbortTransitionOnReject_ = false;

  // T10 失效域门控（裁定基线⑦）：pendingAdvance 在窗（EndApproaching→提交）内收到
  // 失效操作类命令 → 立即中止过渡（撤第二源/预解码槽 + 服务侧重新武装），随后照常
  // 执行本命令自己的重调度。窗口外（无 pending = 已提交或未进入窗口）零影响。
  if (isAdvanceWindowInvalidator(command.kind)) {
    abortPendingAdvance(reduction);
  }

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
      ++queueVersion_;
      syncQueueSnapshot();
      markPlayerChanged(reduction);
      spdlog::debug("play queue entry added at front: '{}' (queue size {})", track->identity.trackId, playbackQueue_.size());
      return reduction;
    }
  case MediaControlCommandKind::ClearPlayQueue:
    if (!playbackQueue_.empty()) {
      playbackQueue_.clear();
      ++queueVersion_;
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
      ++queueVersion_;
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
  case MediaControlCommandKind::SetTransitionConfig:
    return handleSetTransitionConfig(reduction, command);
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
  // T10：留存当前输出模式（pendingAdvance 版本 token 组成部分；配置命令本身在窗内
  // 已由门控中止过渡——此处仅镜像状态供提交校验）。
  outputMode_ = config.outputMode;
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
  // T10：ConfigureOutput 强制整轨重载 = 任何在途预解码/重叠作废（LoadTrack 将重置
  // 服务侧武装与槽）；窗口内的本命令已由门控发过 abort，这里兜底清账本防悬挂。
  pendingAdvance_.reset();
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

ControlReduction ControlStateReducer::handleSetTransitionConfig(ControlReduction& reduction, const MediaControlCommand& command) {
  if (!command.transitionConfig.has_value()) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig requires a transition config");
  }
  const auto& config = *command.transitionConfig;
  if (!validTransitionMode(static_cast<int>(config.autoAdvanceFadeMode))) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig auto advance fade mode is out of range (0-2)");
  }
  if (!validTransitionMode(static_cast<int>(config.manualAdvanceFadeMode))) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig manual advance fade mode is out of range (0-2)");
  }
  if (!inTransitionRange(config.crossfadeMs, std::chrono::milliseconds{10000})) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig crossfade length is out of range (0-10000 ms)");
  }
  if (!inTransitionRange(config.transportFadeMs, std::chrono::milliseconds{3000})) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig transport fade length is out of range (0-3000 ms)");
  }
  if (!inTransitionRange(config.seekFadeMs, std::chrono::milliseconds{3000})) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig seek fade length is out of range (0-3000 ms)");
  }
  if (!inTransitionRange(config.manualShortCrossfadeMs, std::chrono::milliseconds{3000})) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig manual short crossfade length is out of range (0-3000 ms)");
  }
  if (!inTransitionRange(config.gaplessPreloadMs, std::chrono::milliseconds{5000})) {
    return reject(MediaControllerErrorCode::InvalidCommand, "SetTransitionConfig gapless preload lead is out of range (0-5000 ms)");
  }

  // 与 ConfigureOutput 语义隔离：仅生成单意图转发配置，不触发任何重载
  // （无 LoadTrack/Seek/Play 尾意图）、不改播放快照。
  reduction.intents.push_back(makeSetTransitionConfigIntent(config));
  // T8：留存决策表输入（EndApproaching → PrepareNext 的档位/预加载判定来源）。
  transitionConfig_ = config;
  // T10：过渡配置变更版本递增（pendingAdvance 版本 token 组成；窗口内本命令已由
  // 门控中止过渡，服务侧以新配置重新武装）。
  ++transitionConfigVersion_;
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
	            // T10（Metis 缺口 1b）：推进逻辑抽取为共享 commitAdvance（临时队列/RepeatOne/
	            // shuffle/RepeatAll/nextTrack + 索引推进 + 快照发布）。无预载普通自然结束路径
	            // 行为逐事件一致（本分支语义与抽取前等价——回归锁定）。
	            commitAdvance(reduction, AdvanceEventSource::PlaybackEnded, payload.finalClock.sampledAt);
	          } else if constexpr (std::is_same_v<Payload, audio::AdvanceCompleted>) {
	            // T10：接管提交（Metis 缺口 1b）。服务在重叠交叉/无缝直切 handoff 完成时发本
	            // 事件（先于新曲 TrackChanged/状态事件）。校验 pendingAdvance 账本：无账本 =
	            // 陈旧/双提交 → 丢弃（双提交防重）；版本 token 或 trackId 失配 = 服务接管了
	            // 未经批准的推进 → abort（撤账本 + 服务侧撤第二源）并按控制器当前决策重发
	            // 普通 LoadTrack（音频层绝不自主选曲，提交语义永远由控制器裁决）。
	            if (!pendingAdvance_.has_value()) {
	              spdlog::debug("ignoring advance-completed for '{}' without pending ledger (stale or double commit)",
	                            payload.trackId);
	              return;
	            }
	            if (payload.trackId != pendingAdvance_->target.identity.trackId || !pendingTokenMatches()) {
	              spdlog::warn("advance-completed validation failed for '{}' (pending target '{}'): aborting transition",
	                           payload.trackId, pendingAdvance_->target.identity.trackId);
	              abortPendingAdvance(reduction);
	              commitAdvance(reduction, AdvanceEventSource::PlaybackEnded, event.timestamp);
	              return;
	            }
	            commitAdvance(reduction, AdvanceEventSource::AdvanceCompleted, event.timestamp);
	          } else if constexpr (std::is_same_v<Payload, audio::EndApproaching>) {
	          handleEndApproaching(reduction);
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
  // T10 N1：本命令帧被失效域门控 abort 过（意图已压入旧 reduction）→ 回补 AbortTransition，
  // 防窗内非法失效命令漏撤服务侧过渡（账本已清但重叠残留 → 后续 AC 落 stale-drop）。
  if (carryAbortTransitionOnReject_) {
    carryAbortTransitionOnReject_ = false;
    ControlIntent intent{};
    intent.kind = ControlIntentKind::AbortTransition;
    reduction.intents.push_back(std::move(intent));
  }
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
  // T10：任何显式切轨都是对在途过渡的否决（裁定基线⑦窗口外硬清理兜底：门控之外的
  // 内部切轨路径同样不能遗留悬挂账本，否则下一首 EndApproaching 会基于过期账本）。
  pendingAdvance_.reset();
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

std::optional<ControlStateReducer::NaturalEndPeek> ControlStateReducer::peekNaturalEndSelection() const {
  // 只读镜像 reduceAudioEvent(PlaybackEnded) 的推进级联（提交在任务 10 统一抽取），
  // 保证预解码目标 = 自然播完时实际会选中的曲目；零副作用（不消费队列/不推进索引/
  // 不消耗 shuffle 随机序列与历史）。
  if (!playbackContext_.has_value() || playbackContext_->order.empty()) {
    return std::nullopt;
  }

  // 1) 临时队列队首（首个可解析条目；提交时 consumeQueueFront 跳过不可解析条目，
  //    这里只扫描不弹出）。
  for (const auto& entry : playbackQueue_) {
    if (const auto track = findPlayableTrackByTrackId(entry.trackId); track.has_value()) {
      return NaturalEndPeek{*track, /*fromTempQueue=*/true, /*repeatSelf=*/false};
    }
  }

  // 2) RepeatOne → 自身无缝重播（裁定基线①；目标=当前上下文曲目）。
  if (player_.repeatMode == RepeatMode::One && selectedTrack_.has_value()) {
    const auto currentIndex = selectedContextIndex();
    if (!currentIndex.has_value()) {
      return std::nullopt;  // 提交级联同路径 stopPlayback
    }
    return NaturalEndPeek{playbackContext_->order[*currentIndex], false, true};
  }

  // 3) shuffle 激活 → 随机选取不可预解码（决策表：不 armed 不发；降级=自然硬切）。
  if (player_.shuffle) {
    return std::nullopt;
  }

  // 4) 定位当前曲在上下文 order 中的位置（提交级联 nextTrack/selectedContextIndex 同构）：
  //    上下文漂移时仅临时队列路径允许从冻结 index 继续。
  const auto currentIndex = selectedContextIndex();
  if (selectedTrack_.has_value() && !currentIndex.has_value()) {
    if (!playingQueuedTrack_) {
      return std::nullopt;
    }
  }
  if (!selectedTrack_.has_value()) {
    return NaturalEndPeek{playbackContext_->order.front(), false, false};
  }
  const auto& order = playbackContext_->order;

  // 5) 队列末端（最后一首）：RepeatAll → 回绕容器直属第一首（提交级联
  //    firstTrackOfCurrentFolder 同构，仅返回不写 index）；其余 → 无下一曲。
  if (currentIndex.has_value() && *currentIndex + 1U >= order.size()) {
    if (player_.repeatMode != RepeatMode::All) {
      return std::nullopt;
    }
    std::optional<std::string> containerNodeId;
    if (playbackContext_->descriptor.scope == PlaybackContextScope::Folder) {
      containerNodeId = playbackContext_->descriptor.folderNodeId;
    } else if (library_.libraryTree.has_value() && library_.libraryTree->rootNodeId.has_value()) {
      containerNodeId = library_.libraryTree->rootNodeId;
    }
    if (!containerNodeId.has_value()) {
      return NaturalEndPeek{order.front(), false, false};
    }
    const auto firstIt = std::find_if(order.begin(), order.end(), [&](const PlayableTrack& track) {
      return track.parentNodeId.has_value() && *track.parentNodeId == *containerNodeId;
    });
    if (firstIt == order.end()) {
      return std::nullopt;
    }
    return NaturalEndPeek{*firstIt, false, false};
  }

  // 6) 顺序推进：nextTrack(true) 的只读镜像（含 RepeatAll 在 order 末端回绕 index 0）。
  auto nextIndex = currentIndex.value_or(playbackContext_->index) + 1U;
  if (nextIndex >= order.size()) {
    if (player_.repeatMode != RepeatMode::All) {
      return std::nullopt;
    }
    nextIndex = 0U;
  }
  return NaturalEndPeek{order[nextIndex], false, false};
}

bool ControlStateReducer::sharesCueFileWithCurrent(const PlayableTrack& candidate) const {
  if (!selectedTrack_.has_value() || !playbackContext_.has_value()) {
    return false;
  }
  const auto currentIndex = selectedContextIndex();
  if (!currentIndex.has_value()) {
    return false;
  }
  const auto& current = playbackContext_->order[*currentIndex];
  // 同一 .cue 文件相邻轨道：identity.filePath = scanner 的 cue 路径（CUE 派生曲的
  // filePath 语义），request.boundedSegment 标识段请求；候选由顺序邻接天然保证连续。
  return current.request.boundedSegment && candidate.request.boundedSegment &&
         current.identity.filePath == candidate.identity.filePath;
}

void ControlStateReducer::handleEndApproaching(ControlReduction& reduction) {
  // 仅稳定播放/暂停态受理：Stopped/Loading 等状态下的在途事件为陈旧预告。
  if (player_.playback.state != PlaybackStatus::Playing && player_.playback.state != PlaybackStatus::Paused) {
    return;
  }
  if (!selectedTrack_.has_value()) {
    return;
  }
  const auto peek = peekNaturalEndSelection();
  if (!peek.has_value()) {
    return;
  }
  // T10（裁定基线⑦）：记录待提交推进账本——本预告对应的自然结束推进候选 + 版本
  // token 快照（临时队列/输出模式/过渡配置变更版本 + 重复/随机模式）。服务侧接管
  // （AdvanceCompleted）到达时据此校验；窗内任何失效操作先经门控 abort 清账本。
  pendingAdvance_ = PendingAdvance{
      .target = peek->track,
      .queueVersion = queueVersion_,
      .outputMode = outputMode_,
      .repeatMode = player_.repeatMode,
      .shuffle = player_.shuffle,
      .transitionConfigVersion = transitionConfigVersion_,
  };

  audio::PrepareNextMeta meta{};
  const auto mode = transitionConfig_.autoAdvanceFadeMode;
  if (peek->fromTempQueue) {
    // 临时队列队首：提交级联的首选、确定性直插——按"普通下一曲"档位行处理。
    meta.kind = mode == audio::AutoAdvanceFadeMode::Off ? audio::PrepareNextKind::SeamlessDirect
                                                        : audio::PrepareNextKind::Crossfade;
  } else if (peek->repeatSelf) {
    // RepeatOne → 自身无缝重播（裁定基线①）：kind=无缝直切，无交叉（不受档位影响）。
    meta.kind = audio::PrepareNextKind::SeamlessDirect;
  } else {
    // 顺序推进（含 RepeatAll 回绕）：CUE 无间隙组判定 + 档位决策表（裁定）。
    meta.isGaplessGroup = sharesCueFileWithCurrent(peek->track);
    switch (mode) {
    case audio::AutoAdvanceFadeMode::Off:
      // 仅预加载>0 时会收到预告（服务侧武装条件）——就绪时无缝直切（裁定基线③）。
      meta.kind = audio::PrepareNextKind::SeamlessDirect;
      break;
    case audio::AutoAdvanceFadeMode::ExceptGaplessGroup:
      // 除 CUE 邻曲/无间隙组外交叉：组内尽力无缝直切。
      meta.kind = meta.isGaplessGroup ? audio::PrepareNextKind::SeamlessDirect
                                      : audio::PrepareNextKind::Crossfade;
      break;
    case audio::AutoAdvanceFadeMode::All:
      // 全交叉：对 CUE 组也交叉（按字面，例外仅限自动档）。
      meta.kind = audio::PrepareNextKind::Crossfade;
      break;
    }
  }

  ControlIntent intent{};
  intent.kind = ControlIntentKind::PrepareNext;
  intent.track = peek->track.request;
  intent.prepareNextMeta = meta;
  reduction.intents.push_back(std::move(intent));
  spdlog::debug("end approaching: prepare next '{}' (kind={}, gaplessGroup={})",
                peek->track.identity.trackId,
                meta.kind == audio::PrepareNextKind::Crossfade ? "crossfade" : "seamless",
                meta.isGaplessGroup);
}

void ControlStateReducer::stopPlayback(ControlReduction& reduction) {
  // T10：停止 = 无条件终止在途过渡（残留账本防悬挂）。
  pendingAdvance_.reset();
  visibleStateDuringSeek_.reset();
  currentTrackOffset_.reset();
  player_.playback.state = PlaybackStatus::Stopped;
  player_.timeline.position = std::chrono::milliseconds{0};
  spdlog::debug("state: {}", playbackStatusName(PlaybackStatus::Stopped));
  markPlayerChanged(reduction);
}

void ControlStateReducer::abortPendingAdvance(ControlReduction& reduction) {
  if (!pendingAdvance_.has_value()) {
    return;
  }
  pendingAdvance_.reset();
  spdlog::debug("aborting in-flight advance: pushing AbortTransition intent");
  ControlIntent intent{};
  intent.kind = ControlIntentKind::AbortTransition;
  reduction.intents.push_back(std::move(intent));
  // T10 N1：标记本命令帧已压 abort 意图。若 handler 载荷校验失败走 reject()（返回全新
  // reduction、丢弃已压意图），reject() 据此回补，保证窗内非法失效命令也撤服务侧过渡。
  carryAbortTransitionOnReject_ = true;
}

bool ControlStateReducer::pendingTokenMatches() const {
  if (!pendingAdvance_.has_value()) {
    return false;
  }
  return pendingAdvance_->queueVersion == queueVersion_ &&
         pendingAdvance_->outputMode == outputMode_ &&
         pendingAdvance_->repeatMode == player_.repeatMode &&
         pendingAdvance_->shuffle == player_.shuffle &&
         pendingAdvance_->transitionConfigVersion == transitionConfigVersion_;
}

void ControlStateReducer::commitAdvance(ControlReduction& reduction,
                                        AdvanceEventSource source,
                                        std::chrono::steady_clock::time_point sampledAt) {
  // T10：提交级联（抽取自原 PlaybackEnded 分支；裁定基线⑦/⑧）。窗口内状态无变化时
  // 重算结果必与账本目标一致（peek 同构保证），故 AC 与 PBE 共用同一条级联，仅
  // 选曲应用方式不同（applyCommittedTrack 按 source 分发）。
  pendingAdvance_.reset();
  if (const auto queued = consumeQueueFront(); queued.has_value()) {
    // 临时队列优先（T7）：消费队列头部，播放上下文 index 冻结不动；
    // 队列空后由文件夹序列从冻结 index 的下一曲继续。
    applyCommittedTrack(reduction, *queued, source);
    playingQueuedTrack_ = true;
  } else if (player_.repeatMode == RepeatMode::One) {
    if (const auto track = selectedTrack_.has_value() ? this->selectedPlaybackContextTrack() : std::nullopt; track.has_value()) {
      applyCommittedTrack(reduction, *track, source);
    } else {
      stopPlayback(reduction);
    }
  } else if (player_.shuffle && playbackContext_.has_value() && !playbackContext_->order.empty()) {
    if (const auto track = shuffledTrack(playbackContext_->order, /*reshuffleWhenExhausted=*/true); track.has_value()) {
      applyCommittedTrack(reduction, *track, source);
    } else if (const auto current = selectedPlaybackContextTrack(); current.has_value()) {
      applyCommittedTrack(reduction, *current, source);
    } else {
      stopPlayback(reduction);
    }
  } else if (isLastTrackInContext()) {
    addNotification(reduction, makeNotification(ControlDomainNotificationKind::PlaybackEnded, "Playback ended"));
    if (player_.repeatMode == RepeatMode::All) {
      if (const auto track = firstTrackOfCurrentFolder(); track.has_value()) {
        applyCommittedTrack(reduction, *track, source);
      } else {
        stopPlayback(reduction);
      }
    } else {
      stopPlayback(reduction);
    }
  } else if (const auto track = nextTrack(true); track.has_value()) {
    applyCommittedTrack(reduction, *track, source);
  } else {
    stopPlayback(reduction);
  }
  markPlayerChanged(reduction, sampledAt);
}

void ControlStateReducer::applyCommittedTrack(ControlReduction& reduction,
                                              const PlayableTrack& track,
                                              AdvanceEventSource source) {
  if (source == AdvanceEventSource::PlaybackEnded) {
    selectTrack(reduction, track, true);
    return;
  }
  // AdvanceCompleted：音频服务已完成接管（trackId/版本 token 已在上游校验），曲目在
  // 服务侧已加载并输出。此处只镜像 selectTrack 的状态应用，但不产生 LoadTrack/Play
  // 意图（避免重复加载）——artwork 解析仍需推进（新曲目封面）。
  visibleStateDuringSeek_ = PlaybackStatus::Playing;
  playingQueuedTrack_ = false;
  selectedTrack_ = track.identity;
  currentTrackOffset_ = track.request.offset;
  player_.currentTrack = track.identity;
  player_.display = track.display;
  player_.artwork = track.artwork;
  player_.timeline.position = std::chrono::milliseconds{0};
  player_.timeline.duration = track.request.duration;
  player_.playback.state = PlaybackStatus::Playing;
  player_.playback.errorCode.reset();
  player_.playback.errorMessage.reset();
  spdlog::debug("advance completed: committed track '{}'", track.identity.trackId);
  ++artworkGeneration_;
  ArtworkResolveRequest artworkRequest{};
  artworkRequest.generation = artworkGeneration_;
  artworkRequest.identity = track.identity;
  artworkRequest.artworkSourcePath = track.artworkSourcePath;
  artworkRequest.fallbackThumbnailPath = track.fallbackThumbnailPath;
  reduction.intents.push_back(makeArtworkResolveIntent(std::move(artworkRequest)));
}

}
