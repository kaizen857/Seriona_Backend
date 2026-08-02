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
};

struct ControlIntent {
  ControlIntentKind kind{ControlIntentKind::Play};
  std::optional<audio::TrackPlaybackRequest> track;
  std::optional<std::chrono::milliseconds> position;
  std::optional<float> volume;
  std::optional<bool> muted;
  std::optional<ArtworkResolveRequest> artworkRequest;
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
  [[nodiscard]] std::optional<PlayableTrack> shuffledTrack(const std::vector<PlayableTrack>& tracks);
  [[nodiscard]] std::optional<PlayableTrack> previousTrack();
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

  PlayerStateSnapshot player_{};
  LibraryStateSnapshot library_{};
  std::optional<TrackIdentity> selectedTrack_{};
  std::optional<PlaybackContextState> playbackContext_{};
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
