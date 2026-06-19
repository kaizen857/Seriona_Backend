#include "seriona/audio/events/audio_event_dispatcher.h"

#include <utility>

namespace seriona::audio {

AudioEventDispatcher::AudioEventDispatcher(BackendSourceModule sourceModule)
    : sourceModule_(sourceModule) {}

void AudioEventDispatcher::setEventSink(BackendEventSink sink) { sink_ = std::move(sink); }

void AudioEventDispatcher::clearEventSink() { sink_ = {}; }

void AudioEventDispatcher::shutdown() { clearEventSink(); }

void AudioEventDispatcher::dispatch(BackendEventType type, PlaybackEvent payload) {
  if (!sink_) {
    return;
  }

  sink_(prepare(type, std::move(payload)));
}

void AudioEventDispatcher::dispatch(BackendEvent event) {
  if (!sink_) {
    return;
  }

  sink_(prepare(std::move(event)));
}

std::uint64_t AudioEventDispatcher::nextVersion() const { return eventVersion_ + 1; }

bool AudioEventDispatcher::hasEventSink() const { return static_cast<bool>(sink_); }

BackendEvent AudioEventDispatcher::prepare(BackendEventType type, PlaybackEvent payload) {
  BackendEvent event{};
  event.type = type;
  event.sourceModule = sourceModule_;
  event.monotonicVersion = ++eventVersion_;
  event.timestamp = std::chrono::steady_clock::now();
  event.payload = std::move(payload);
  return event;
}

BackendEvent AudioEventDispatcher::prepare(BackendEvent event) {
  event.sourceModule = sourceModule_;
  event.monotonicVersion = ++eventVersion_;
  event.timestamp = std::chrono::steady_clock::now();
  return event;
}

}
