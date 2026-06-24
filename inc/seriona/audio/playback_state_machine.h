#pragma once

#include "seriona/audio/audio_contracts.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace seriona::audio {

class PlaybackStateMachine {
public:
  PlaybackStateMachine() = default;

  void setEventSink(BackendEventSink sink);
  void clearEventSink();

  void loadTrack(const TrackPlaybackRequest& request);
  void completeLoad();
  void fail(PlaybackErrorCode code, std::string message, std::string detail = {});
  void play();
  void pause();
  void resume();
  void stop();
  [[nodiscard]] std::uint64_t beginSeek(std::chrono::milliseconds position);
  void seek(std::chrono::milliseconds position);
  void cancelSeek(PlaybackErrorCode code, std::string message, std::string detail = {});
  void completeSeek(std::uint64_t generation);
  void completeSeek();
  void naturalEnd();
  void shutdown();

  [[nodiscard]] PlaybackState state() const;
  [[nodiscard]] std::uint64_t generation() const;
  [[nodiscard]] PlaybackClockSnapshot clock() const;
  [[nodiscard]] bool hasPendingSeek() const;

private:
  void changeState(PlaybackState nextState);
  void emitTrackChanged(const TrackPlaybackRequest& request);
  void emitPositionDiscontinuity(PlaybackClockSnapshot before,
                                 PlaybackClockSnapshot after,
                                 std::string reason);
  void emitPlaybackEnded();
  void emitError(PlaybackErrorCode code, std::string message, std::string detail);
  void emit(BackendEventType type, PlaybackEvent payload);
  [[nodiscard]] PlaybackClockSnapshot makeClock(std::chrono::milliseconds position,
                                                bool continuous) const;

  BackendEventSink sink_{};
  PlaybackState state_{PlaybackState::Idle};
  std::uint64_t eventVersion_{0};
  std::uint64_t generation_{0};
  TrackPlaybackRequest currentTrack_{};
  bool hasTrack_{false};
  PlaybackClockSnapshot clock_{};
  std::optional<PlaybackClockSnapshot> pendingSeekBefore_{};
  std::optional<PlaybackClockSnapshot> pendingSeekAfter_{};
};

}
