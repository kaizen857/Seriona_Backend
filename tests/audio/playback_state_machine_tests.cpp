#include "seriona/audio/playback_state_machine.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

TrackPlaybackRequest request(std::string id = "track-1") {
  TrackPlaybackRequest track{};
  track.trackId = std::move(id);
  track.filePath = "fixture.wav";
  track.title = "Fixture";
  return track;
}

class FakeSink {
public:
  BackendEventSink callback() {
    return [this](BackendEvent event) { events.push_back(std::move(event)); };
  }

  std::vector<BackendEvent> events;
};

std::vector<PlaybackState> stateEvents(const std::vector<BackendEvent>& events) {
  std::vector<PlaybackState> states;
  for (const auto& event : events) {
    if (event.type == BackendEventType::PlaybackStateChanged) {
      states.push_back(std::get<PlaybackStateChanged>(event.payload).state);
    }
  }
  return states;
}

BackendEventType eventTypeAt(const std::vector<BackendEvent>& events, std::size_t index) {
  REQUIRE(index < events.size());
  return events[index].type;
}

}

TEST_CASE("playback_state_machine legal transitions emit ordered events") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request());
  machine.completeLoad();
  machine.play();
  machine.pause();
  machine.resume();
  machine.seek(42s);
  machine.completeSeek();
  machine.naturalEnd();

  CHECK(machine.state() == PlaybackState::Stopped);
  CHECK(eventTypeAt(sink.events, 0) == BackendEventType::PlaybackStateChanged);
  CHECK(eventTypeAt(sink.events, 1) == BackendEventType::TrackChanged);
  CHECK(eventTypeAt(sink.events, 7) == BackendEventType::PositionDiscontinuity);
  CHECK(eventTypeAt(sink.events, 10) == BackendEventType::PlaybackEnded);

  const auto states = stateEvents(sink.events);
  CHECK(states == std::vector<PlaybackState>{PlaybackState::Loading,
                                             PlaybackState::Ready,
                                             PlaybackState::Playing,
                                             PlaybackState::Paused,
                                             PlaybackState::Playing,
                                             PlaybackState::Loading,
                                             PlaybackState::Playing,
                                             PlaybackState::Draining,
                                             PlaybackState::Stopped});

  const auto& discontinuity = std::get<PositionDiscontinuity>(sink.events[7].payload);
  CHECK(discontinuity.before.position == 0ms);
  CHECK(discontinuity.after.position == 42s);
  CHECK(discontinuity.reason == "seek");
}

TEST_CASE("playback_state_machine rejects illegal transitions with value errors") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.play();
  machine.seek(5s);
  machine.resume();

  REQUIRE(sink.events.size() == 3);
  CHECK(machine.state() == PlaybackState::Idle);
  for (const auto& event : sink.events) {
    CHECK(event.type == BackendEventType::PlaybackError);
    CHECK(std::holds_alternative<PlaybackError>(event.payload));
  }
  CHECK(std::get<PlaybackError>(sink.events[1].payload).code == PlaybackErrorCode::SeekFailed);
}

TEST_CASE("playback_state_machine recovers from error by loading a new track") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request("broken"));
  machine.fail(PlaybackErrorCode::DecodeFailed, "decode failed");
  machine.loadTrack(request("recovered"));
  machine.completeLoad();

  CHECK(machine.state() == PlaybackState::Ready);
  CHECK(machine.clock().trackId == "recovered");
  CHECK(machine.hasPendingSeek() == false);

  const auto states = stateEvents(sink.events);
  CHECK(states == std::vector<PlaybackState>{PlaybackState::Loading,
                                             PlaybackState::Error,
                                             PlaybackState::Loading,
                                             PlaybackState::Ready});
}

TEST_CASE("playback_state_machine cancellation clears stale seek before stop") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request());
  machine.completeLoad();
  machine.play();
  machine.seek(10s);
  machine.pause();
  machine.stop();
  machine.completeSeek();

  CHECK(machine.state() == PlaybackState::Stopped);
  CHECK(machine.hasPendingSeek() == false);

  const auto states = stateEvents(sink.events);
  CHECK(states == std::vector<PlaybackState>{PlaybackState::Loading,
                                             PlaybackState::Ready,
                                             PlaybackState::Playing,
                                             PlaybackState::Loading,
                                             PlaybackState::Paused,
                                             PlaybackState::Stopped});

  for (const auto& event : sink.events) {
    CHECK(event.type != BackendEventType::PositionDiscontinuity);
  }
  CHECK(sink.events.back().type == BackendEventType::PlaybackError);
  CHECK(std::get<PlaybackError>(sink.events.back().payload).code == PlaybackErrorCode::SeekFailed);
}

TEST_CASE("playback_state_machine seek rollback restores previous ready state") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request());
  machine.completeLoad();
  machine.seek(10s);
  machine.cancelSeek(PlaybackErrorCode::SeekFailed, "seek failed", "simulated seek failure");

  CHECK(machine.state() == PlaybackState::Ready);
  CHECK(machine.hasPendingSeek() == false);
  CHECK(machine.clock().position == 0ms);
  CHECK(machine.clock().continuous == false);

  CHECK(sink.events.back().type == BackendEventType::PlaybackError);
  CHECK(std::get<PlaybackError>(sink.events.back().payload).code == PlaybackErrorCode::SeekFailed);
}

TEST_CASE("playback_state_machine stale seek completion is ignored after newer stop") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request());
  machine.completeLoad();
  machine.play();
  const auto seekGeneration = machine.beginSeek(10s);
  machine.stop();
  machine.completeSeek(seekGeneration);

  CHECK(machine.state() == PlaybackState::Stopped);
  CHECK(machine.hasPendingSeek() == false);
  CHECK(std::none_of(sink.events.begin(), sink.events.end(), [](const BackendEvent& event) {
    return event.type == BackendEventType::PositionDiscontinuity;
  }));
}

TEST_CASE("playback_state_machine clear sink prevents delivery after shutdown") {
  PlaybackStateMachine machine;
  FakeSink sink;
  machine.setEventSink(sink.callback());

  machine.loadTrack(request());
  machine.completeLoad();
  const auto deliveredBeforeClear = sink.events.size();

  machine.clearEventSink();
  machine.play();
  machine.stop();
  machine.shutdown();

  CHECK(machine.state() == PlaybackState::Stopped);
  CHECK(sink.events.size() == deliveredBeforeClear);
}

}
