#include "seriona/audio/events/audio_event_dispatcher.h"

#include <doctest.h>

#include <utility>
#include <vector>

namespace seriona::audio {
namespace {

class FakeSink {
public:
  BackendEventSink callback() {
    return [this](BackendEvent event) { events.push_back(std::move(event)); };
  }

  std::vector<BackendEvent> events;
};

}

TEST_CASE("audio_event_dispatcher clear sink prevents future delivery") {
  AudioEventDispatcher dispatcher;
  FakeSink sink;
  dispatcher.setEventSink(sink.callback());

  dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Ready});
  REQUIRE(sink.events.size() == 1);
  CHECK(dispatcher.hasEventSink());

  dispatcher.clearEventSink();
  CHECK_FALSE(dispatcher.hasEventSink());
  dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Playing});

  CHECK(sink.events.size() == 1);
  CHECK(dispatcher.nextVersion() == 2);
}

TEST_CASE("audio_event_dispatcher shutdown is idempotent and blocks delivery") {
  AudioEventDispatcher dispatcher;
  FakeSink sink;
  dispatcher.setEventSink(sink.callback());

  dispatcher.shutdown();
  dispatcher.shutdown();
  PlaybackError error{};
  error.code = PlaybackErrorCode::DecodeFailed;
  error.message = "decode failed";
  error.detail = "shutdown";
  dispatcher.dispatch(BackendEventType::PlaybackError, error);

  CHECK(sink.events.empty());
  CHECK_FALSE(dispatcher.hasEventSink());
  CHECK(dispatcher.nextVersion() == 1);
}

}
