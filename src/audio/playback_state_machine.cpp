#include "seriona/audio/playback_state_machine.h"

#include "spdlog/spdlog.h"

#include <utility>

namespace seriona::audio {

namespace {

const char* stateName(PlaybackState state) {
  switch (state) {
  case PlaybackState::Idle: return "Idle";
  case PlaybackState::Loading: return "Loading";
  case PlaybackState::Ready: return "Ready";
  case PlaybackState::Playing: return "Playing";
  case PlaybackState::Paused: return "Paused";
  case PlaybackState::Draining: return "Draining";
  case PlaybackState::Stopped: return "Stopped";
  case PlaybackState::Error: return "Error";
  }
  return "Unknown";
}

} // namespace

void PlaybackStateMachine::setEventSink(BackendEventSink sink) { sink_ = std::move(sink); }

void PlaybackStateMachine::clearEventSink() { sink_ = {}; }

void PlaybackStateMachine::loadTrack(const TrackPlaybackRequest& request) {
  ++generation_;
  currentTrack_ = request;
  hasTrack_ = true;
  pendingSeekBefore_.reset();
  pendingSeekAfter_.reset();
  clock_ = makeClock(request.offset.value_or(std::chrono::milliseconds{0}), false);
  changeState(PlaybackState::Loading);
  emitTrackChanged(currentTrack_);
}

void PlaybackStateMachine::completeLoad() {
  if (state_ != PlaybackState::Loading) {
    emitError(PlaybackErrorCode::OpenFailed, "completeLoad requires Loading state", "illegal transition");
    return;
  }

  changeState(PlaybackState::Ready);
}

void PlaybackStateMachine::fail(PlaybackErrorCode code, std::string message, std::string detail) {
  ++generation_;
  pendingSeekBefore_.reset();
  pendingSeekAfter_.reset();
  changeState(PlaybackState::Error);
  emitError(code, std::move(message), std::move(detail));
}

void PlaybackStateMachine::play() {
  if (state_ == PlaybackState::Ready || state_ == PlaybackState::Paused || state_ == PlaybackState::Stopped) {
    if (!hasTrack_) {
      emitError(PlaybackErrorCode::OpenFailed, "play requires a loaded track", "missing track");
      return;
    }
    clock_.continuous = true;
    changeState(PlaybackState::Playing);
    return;
  }

  emitError(PlaybackErrorCode::OpenFailed, "play is not legal in current state", "illegal transition");
}

void PlaybackStateMachine::pause() {
  // Ready（已加载未播放，时钟已停）→ Paused 合法：ConfigureOutput 重载当前曲目
  // 后控制层补发 pause 以保持暂停，物理上无副作用；重复 pause 仍被拒绝。
  if (state_ == PlaybackState::Playing || state_ == PlaybackState::Draining ||
      state_ == PlaybackState::Ready ||
      (state_ == PlaybackState::Loading && pendingSeekAfter_.has_value())) {
    clock_.continuous = false;
    pendingSeekBefore_.reset();
    pendingSeekAfter_.reset();
    changeState(PlaybackState::Paused);
    return;
  }

  emitError(PlaybackErrorCode::OpenFailed, "pause requires active playback", "illegal transition");
}

void PlaybackStateMachine::resume() {
  if (state_ == PlaybackState::Paused) {
    clock_.continuous = true;
    changeState(PlaybackState::Playing);
    return;
  }

  emitError(PlaybackErrorCode::OpenFailed, "resume requires Paused state", "illegal transition");
}

void PlaybackStateMachine::stop() {
  ++generation_;
  pendingSeekBefore_.reset();
  pendingSeekAfter_.reset();
  clock_.continuous = false;
  changeState(PlaybackState::Stopped);
}

void PlaybackStateMachine::seek(std::chrono::milliseconds position) {
  static_cast<void>(beginSeek(position));
}

std::uint64_t PlaybackStateMachine::beginSeek(std::chrono::milliseconds position) {
  if (state_ != PlaybackState::Ready && state_ != PlaybackState::Playing && state_ != PlaybackState::Paused) {
    emitError(PlaybackErrorCode::SeekFailed, "seek requires a loaded track", "illegal transition");
    return generation_;
  }

  ++generation_;
  const bool wasContinuous = state_ == PlaybackState::Playing;
  pendingSeekBefore_ = clock_;
  pendingSeekAfter_ = makeClock(position, wasContinuous);
  clock_ = *pendingSeekAfter_;
  changeState(PlaybackState::Loading);
  return generation_;
}

void PlaybackStateMachine::cancelSeek(PlaybackErrorCode code, std::string message, std::string detail) {
  if (!pendingSeekBefore_ || !pendingSeekAfter_ || state_ != PlaybackState::Loading) {
    emitError(code, std::move(message), std::move(detail));
    return;
  }

  const auto previous = *pendingSeekBefore_;
  const auto rollbackState = previous.continuous ? PlaybackState::Playing : PlaybackState::Ready;
  clock_ = previous;
  pendingSeekBefore_.reset();
  pendingSeekAfter_.reset();
  changeState(rollbackState);
  emitError(code, std::move(message), std::move(detail));
}

void PlaybackStateMachine::completeSeek(std::uint64_t generation) {
  if (generation != generation_) {
    return;
  }

  completeSeek();
}

void PlaybackStateMachine::completeSeek() {
  if (!pendingSeekBefore_ || !pendingSeekAfter_ || state_ != PlaybackState::Loading) {
    emitError(PlaybackErrorCode::SeekFailed, "completeSeek requires a pending seek", "illegal transition");
    return;
  }

  const bool resumePlayback = pendingSeekAfter_->continuous;
  emitPositionDiscontinuity(*pendingSeekBefore_, *pendingSeekAfter_, "seek");
  pendingSeekBefore_.reset();
  pendingSeekAfter_.reset();
  changeState(resumePlayback ? PlaybackState::Playing : PlaybackState::Ready);
}

void PlaybackStateMachine::naturalEnd() {
  if (state_ != PlaybackState::Playing && state_ != PlaybackState::Draining) {
    emitError(PlaybackErrorCode::DecodeFailed, "naturalEnd requires active playback", "illegal transition");
    return;
  }

  ++generation_;
  clock_.continuous = false;
  changeState(PlaybackState::Draining);
  emitPlaybackEnded();
  changeState(PlaybackState::Stopped);
}

void PlaybackStateMachine::shutdown() {
  stop();
  clearEventSink();
}

PlaybackState PlaybackStateMachine::state() const { return state_; }

std::uint64_t PlaybackStateMachine::generation() const { return generation_; }

PlaybackClockSnapshot PlaybackStateMachine::clock() const { return clock_; }

bool PlaybackStateMachine::hasPendingSeek() const {
  return pendingSeekBefore_.has_value() || pendingSeekAfter_.has_value();
}

void PlaybackStateMachine::changeState(PlaybackState nextState) {
  if (state_ == nextState) {
    return;
  }

  const auto previous = state_;
  state_ = nextState;
  spdlog::debug("playback state: {} -> {}", stateName(previous), stateName(nextState));
  emit(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{nextState});
}

void PlaybackStateMachine::emitTrackChanged(const TrackPlaybackRequest& request) {
  emit(BackendEventType::TrackChanged, TrackChanged{request});
}

void PlaybackStateMachine::emitPositionDiscontinuity(PlaybackClockSnapshot before,
                                                     PlaybackClockSnapshot after,
                                                     std::string reason) {
  emit(BackendEventType::PositionDiscontinuity,
       PositionDiscontinuity{std::move(before), std::move(after), std::move(reason)});
}

void PlaybackStateMachine::emitPlaybackEnded() {
  emit(BackendEventType::PlaybackEnded, PlaybackEnded{currentTrack_, clock_});
}

void PlaybackStateMachine::emitError(PlaybackErrorCode code, std::string message, std::string detail) {
  spdlog::error("playback error (code={}): {} - {}", static_cast<int>(code), message, detail);
  emit(BackendEventType::PlaybackError,
       PlaybackError{code, std::move(message), std::move(detail), clock_});
}

void PlaybackStateMachine::emit(BackendEventType type, PlaybackEvent payload) {
  if (!sink_) {
    return;
  }

  BackendEvent event{};
  event.type = type;
  event.sourceModule = BackendSourceModule::AudioPlaybackService;
  event.monotonicVersion = ++eventVersion_;
  event.timestamp = std::chrono::steady_clock::now();
  event.payload = std::move(payload);
  sink_(std::move(event));
}

PlaybackClockSnapshot PlaybackStateMachine::makeClock(std::chrono::milliseconds position,
                                                       bool continuous) const {
  PlaybackClockSnapshot snapshot{};
  snapshot.trackId = currentTrack_.trackId;
  snapshot.position = position;
  snapshot.sampledAt = std::chrono::steady_clock::now();
  snapshot.version = generation_;
  snapshot.continuous = continuous;
  return snapshot;
}

}
