#pragma once

#include "seriona/audio/audio_contracts.h"

#include <chrono>
#include <cstdint>

namespace seriona::audio {

class AudioEventDispatcher {
public:
  AudioEventDispatcher() = default;
  explicit AudioEventDispatcher(BackendSourceModule sourceModule);

  void setEventSink(BackendEventSink sink);
  void clearEventSink();
  void shutdown();

  void dispatch(BackendEventType type, PlaybackEvent payload);
  void dispatch(BackendEvent event);

  [[nodiscard]] std::uint64_t nextVersion() const;
  [[nodiscard]] bool hasEventSink() const;

private:
  [[nodiscard]] BackendEvent prepare(BackendEventType type, PlaybackEvent payload);
  [[nodiscard]] BackendEvent prepare(BackendEvent event);

  BackendEventSink sink_{};
  BackendSourceModule sourceModule_{BackendSourceModule::AudioPlaybackService};
  std::uint64_t eventVersion_{0};
};

}
