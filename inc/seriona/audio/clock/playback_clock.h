#pragma once

#include "seriona/audio/audio_contracts.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace seriona::audio {

struct PlaybackClockCounters {
  std::uint64_t submittedFrames{0};
  std::uint64_t consumedFrames{0};
  std::uint64_t underrunCount{0};
  std::uint64_t underrunFrames{0};
  std::uint64_t seekCount{0};
};

class PlaybackClock {
public:
  PlaybackClock() = default;

  void reset(std::string trackId, std::uint32_t sampleRate, std::chrono::milliseconds basePosition = std::chrono::milliseconds{0});
  void submitFrames(std::uint64_t frameCount) noexcept;
  void consumeFrames(std::uint64_t frameCount) noexcept;
  void reportUnderrun(std::uint64_t silenceFrames) noexcept;
  void pause();
  void resume();
  void seek(std::chrono::milliseconds position);

  [[nodiscard]] PlaybackClockSnapshot snapshot() const;
  [[nodiscard]] PlaybackClockCounters counters() const noexcept;
  [[nodiscard]] std::uint32_t sampleRate() const noexcept;
  [[nodiscard]] bool paused() const noexcept;

private:
  [[nodiscard]] std::chrono::milliseconds positionFromFrames() const noexcept;
  [[nodiscard]] std::chrono::milliseconds framesToDuration(std::uint64_t frames) const noexcept;

  std::string trackId_;
  std::uint32_t sampleRate_{0};
  std::chrono::milliseconds basePosition_{0};
  std::chrono::milliseconds frozenPosition_{0};
  std::uint64_t submittedFrames_{0};
  std::uint64_t consumedFrames_{0};
  std::uint64_t baseConsumedFrames_{0};
  std::uint64_t underrunCount_{0};
  std::uint64_t underrunFrames_{0};
  std::uint64_t seekCount_{0};
  std::uint64_t version_{0};
  bool paused_{true};
};

}
