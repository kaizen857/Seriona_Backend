#include "seriona/audio/clock/playback_clock.h"

#include <utility>

namespace seriona::audio {

void PlaybackClock::reset(std::string trackId,
                          std::uint32_t sampleRate,
                          std::chrono::milliseconds basePosition) {
  trackId_ = std::move(trackId);
  sampleRate_ = sampleRate;
  basePosition_ = basePosition;
  frozenPosition_ = basePosition;
  submittedFrames_ = 0U;
  consumedFrames_ = 0U;
  baseConsumedFrames_ = 0U;
  underrunCount_ = 0U;
  underrunFrames_ = 0U;
  seekCount_ = 0U;
  ++version_;
  paused_ = true;
}

void PlaybackClock::submitFrames(std::uint64_t frameCount) noexcept { submittedFrames_ += frameCount; }

void PlaybackClock::consumeFrames(std::uint64_t frameCount) noexcept { consumedFrames_ += frameCount; }

void PlaybackClock::reportUnderrun(std::uint64_t silenceFrames) noexcept {
  ++underrunCount_;
  underrunFrames_ += silenceFrames;
}

void PlaybackClock::pause() {
  if (paused_) {
    return;
  }

  frozenPosition_ = positionFromFrames();
  paused_ = true;
  ++version_;
}

void PlaybackClock::resume() {
  if (!paused_) {
    return;
  }

  basePosition_ = frozenPosition_;
  baseConsumedFrames_ = consumedFrames_;
  paused_ = false;
  ++version_;
}

void PlaybackClock::seek(std::chrono::milliseconds position) {
  basePosition_ = position;
  frozenPosition_ = position;
  baseConsumedFrames_ = consumedFrames_;
  ++seekCount_;
  ++version_;
}

PlaybackClockSnapshot PlaybackClock::snapshot() const {
  PlaybackClockSnapshot result{};
  result.trackId = trackId_;
  result.position = paused_ ? frozenPosition_ : positionFromFrames();
  result.sampledAt = std::chrono::steady_clock::now();
  result.version = version_;
  result.continuous = !paused_;
  return result;
}

PlaybackClockCounters PlaybackClock::counters() const noexcept {
  return PlaybackClockCounters{submittedFrames_, consumedFrames_, underrunCount_, underrunFrames_, seekCount_};
}

std::uint32_t PlaybackClock::sampleRate() const noexcept { return sampleRate_; }

bool PlaybackClock::paused() const noexcept { return paused_; }

std::chrono::milliseconds PlaybackClock::positionFromFrames() const noexcept {
  if (consumedFrames_ <= baseConsumedFrames_) {
    return basePosition_;
  }

  return basePosition_ + framesToDuration(consumedFrames_ - baseConsumedFrames_);
}

std::chrono::milliseconds PlaybackClock::framesToDuration(std::uint64_t frames) const noexcept {
  if (sampleRate_ == 0U) {
    return std::chrono::milliseconds{0};
  }

  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::duration<double>(static_cast<double>(frames) / static_cast<double>(sampleRate_)));
}

}
