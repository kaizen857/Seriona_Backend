#include "seriona/audio/events/audio_event_dispatcher.h"

#include <mutex>
#include <utility>

namespace seriona::audio {

AudioEventDispatcher::AudioEventDispatcher(BackendSourceModule sourceModule)
    : sourceModule_(sourceModule) {}

void AudioEventDispatcher::setEventSink(BackendEventSink sink) {
  std::lock_guard lock{mutex_};
  sink_ = std::move(sink);
}

void AudioEventDispatcher::clearEventSink() {
  std::lock_guard lock{mutex_};
  sink_ = {};
}

void AudioEventDispatcher::shutdown() { clearEventSink(); }

void AudioEventDispatcher::dispatch(BackendEventType type, PlaybackEvent payload) {
  BackendEventSink sink;
  BackendEvent event;
  {
    std::lock_guard lock{mutex_};
    if (!sink_) {
      return;
    }
    sink = sink_;
    event = prepare(type, std::move(payload));
  }

  sink(std::move(event));
}

void AudioEventDispatcher::dispatch(BackendEvent event) {
  BackendEventSink sink;
  {
    std::lock_guard lock{mutex_};
    if (!sink_) {
      return;
    }
    sink = sink_;
    event = prepare(std::move(event));
  }

  sink(std::move(event));
}

std::uint64_t AudioEventDispatcher::nextVersion() const {
  std::lock_guard lock{mutex_};
  return eventVersion_ + 1;
}

bool AudioEventDispatcher::hasEventSink() const {
  std::lock_guard lock{mutex_};
  return static_cast<bool>(sink_);
}

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
