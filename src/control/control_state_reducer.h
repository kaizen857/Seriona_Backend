#pragma once

#include "playback_context_builder.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace seriona::control {

class ShuffleHistory {
public:
  explicit ShuffleHistory(std::size_t maxSize = 50);
  
  void push(const TrackIdentity& track);
  std::optional<TrackIdentity> pop();
  [[nodiscard]] bool contains(const TrackIdentity& track) const;
  void clear();
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  
private:
  std::deque<TrackIdentity> history_;
  std::size_t maxSize_;
};

enum class ControlIntentKind : std::uint8_t {
  LoadTrack,
  Play,
  Pause,
  Resume,
  Stop,
  Seek,
  SetVolume,
  SetMuted,
  ResolveArtwork,
  // Appended at the end: existing enumerators keep their ordinal positions.
  ConfigureOutput,
  // 过渡参数配置意图（T1）：转发 TransitionConfig 至音频服务，不触发重载。
  SetTransitionConfig,
};

struct ControlIntent {
  ControlIntentKind kind{ControlIntentKind::Play};
  std::optional<audio::TrackPlaybackRequest> track;
  std::optional<std::chrono::milliseconds> position;
  std::optional<float> volume;
  std::optional<bool> muted;
  std::optional<ArtworkResolveRequest> artworkRequest;
  // Last member on purpose: appended fields never disturb designated or
  // value-initialization of existing intents.
  std::optional<audio::AudioOutputConfig> outputConfig;
  // SetTransitionConfig 载荷（T1）。追加末尾保持序列化兼容。
  std::optional<audio::TransitionConfig> transitionConfig;
};

struct ControlReduction {
  MediaControllerCommandResult result{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}};
  std::vector<ControlIntent> intents{};
  std::vector<ControlDomainNotification> notifications{};
  bool playerStateChanged{false};
  bool libraryStateChanged{false};
};

class ControlStateReducer {
public:
  explicit ControlStateReducer(MediaControllerOptions options = {});

  [[nodiscard]] const PlayerStateSnapshot& playerState() const noexcept;
  [[nodiscard]] const LibraryStateSnapshot& libraryState() const noexcept;
  [[nodiscard]] const std::vector<ControlDomainNotification>& recentNotifications() const noexcept;

  ControlReduction reduceCommand(const MediaControlCommand& command);
  ControlReduction reduceAudioEvent(const audio::BackendEvent& event);
  ControlReduction reduceScannerEvent(const scanner::ScannerEvent& event);
  ControlReduction reduceArtworkResolved(const ArtworkResolveResultView& result);

private:
  struct PlayableTrack {
    TrackIdentity identity{};
    audio::TrackPlaybackRequest request{};
    DisplayMetadata display{};
    std::optional<ArtworkRef> artwork{};
    std::filesystem::path artworkSourcePath;
    std::filesystem::path fallbackThumbnailPath;
    // 曲目所属容器节点（目录/专辑等）；用于“当前文件夹第一首（不含子文件夹）”回绕。
    std::optional<std::string> parentNodeId{};
  };

  struct PlaybackContextState {
    PlaybackContextDescriptor descriptor{};
    std::vector<PlayableTrack> order{};
    std::size_t index{0};
  };

  [[nodiscard]] std::vector<PlayableTrack> playableTracks() const;
  [[nodiscard]] std::optional<PlayableTrack> firstPlayableTrack() const;
  [[nodiscard]] std::optional<PlayableTrack> findPlayableTrack(const TrackIdentity& identity) const;
  [[nodiscard]] std::optional<PlaybackContextDescriptor> defaultContextDescriptorForTrack(const TrackIdentity& identity) const;
  [[nodiscard]] std::optional<PlaybackContextState> buildPlaybackContextState(PlaybackContextDescriptor descriptor,
                                                                              PlaybackContextBuildStatus* status = nullptr) const;
  [[nodiscard]] std::optional<std::size_t> selectedContextIndex() const;
  [[nodiscard]] std::optional<PlayableTrack> selectedPlaybackContextTrack();
  [[nodiscard]] bool activateTrackWithDefaultContext(ControlReduction& reduction, const TrackIdentity& identity, bool startPlayback);
  [[nodiscard]] std::optional<PlayableTrack> nextTrack(bool forward);
  [[nodiscard]] std::optional<PlayableTrack> shuffledTrack(const std::vector<PlayableTrack>& tracks,
                                                          bool reshuffleWhenExhausted = false);
  [[nodiscard]] std::optional<PlayableTrack> previousTrack();
  [[nodiscard]] std::optional<PlayableTrack> findPlayableTrackByTrackId(const std::string& trackId) const;
  // 消费临时队列队首（跳过不可解析条目）；队列为空返回 nullopt。不改播放上下文 index。
  [[nodiscard]] std::optional<PlayableTrack> consumeQueueFront();
  // 将临时队列同步进 PlayerStateSnapshot::queueEntries（跨端契约）。
  void syncQueueSnapshot();
  // 当前选中曲目是否为播放上下文的最后一首（不含子文件夹干扰的判断由 order 决定）。
  [[nodiscard]] bool isLastTrackInContext() const;
  // 当前播放上下文容器（文件夹 / 根）直属的第一首，不含子文件夹。
  [[nodiscard]] std::optional<PlayableTrack> firstTrackOfCurrentFolder();
  [[nodiscard]] std::vector<PlayableTrack> filterOutHistory(const std::vector<PlayableTrack>& candidates) const;
  [[nodiscard]] std::chrono::milliseconds clampPosition(std::chrono::milliseconds position) const;

  ControlReduction accept();
  ControlReduction reject(MediaControllerErrorCode code, std::string message);
  void markPlayerChanged(ControlReduction& reduction, std::chrono::steady_clock::time_point sampledAt = {});
  void addNotification(ControlReduction& reduction, ControlDomainNotification notification);
  void reconcilePlaybackContextAfterSnapshot(ControlReduction& reduction);
  void selectFirstTrackWhenIdle(ControlReduction& reduction);
  void selectTrack(ControlReduction& reduction, const PlayableTrack& track, bool startPlayback);
  void stopPlayback(ControlReduction& reduction);
  ControlReduction handleConfigureOutput(ControlReduction& reduction, const MediaControlCommand& command);
  // SetTransitionConfig：校验过渡参数并生成单意图（不重载、不改快照）。
  ControlReduction handleSetTransitionConfig(ControlReduction& reduction, const MediaControlCommand& command);

  PlayerStateSnapshot player_{};
  LibraryStateSnapshot library_{};
  std::optional<TrackIdentity> selectedTrack_{};
  std::optional<PlaybackContextState> playbackContext_{};
  // 临时播放队列（T7）：不持久化（新实例即空）；消费期间播放上下文 index 冻结，
  // 队列空后才从冻结位置推进文件夹序列。
  std::deque<QueueEntry> playbackQueue_{};
  // 当前曲目是否来自临时队列（T7）：仅此时 nextTrack 允许从冻结 index 继续；
  // 普通"上下文漂移"（选中曲目不在 order）保持旧语义（返回空，停止播放）。
  bool playingQueuedTrack_{false};
  std::optional<PlaybackStatus> visibleStateDuringSeek_{};
  std::optional<std::chrono::milliseconds> currentTrackOffset_{};
  std::uint64_t lastAudioPlayerVersion_{0};
  std::uint64_t lastAudioServiceVersion_{0};
  std::uint64_t lastScannerVersion_{0};
  std::vector<ControlDomainNotification> recentNotifications_{};
  std::mt19937_64 shuffleRandom_;
  ShuffleHistory shuffleHistory_;
  std::size_t shuffleHistorySize_{50};
  std::uint64_t artworkGeneration_{0};
};

}
