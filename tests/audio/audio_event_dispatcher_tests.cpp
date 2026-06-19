#include "seriona/audio/events/audio_event_dispatcher.h"

#include <doctest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

TrackPlaybackRequest request(std::string id) {
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

}

TEST_CASE("audio_event_dispatcher delivers ordered value events") {
  AudioEventDispatcher dispatcher;
  FakeSink sink;
  dispatcher.setEventSink(sink.callback());

  TrackPlaybackRequest callerOwnedRequest = request("track-1");
  dispatcher.dispatch(BackendEventType::TrackChanged, TrackChanged{callerOwnedRequest});
  callerOwnedRequest.trackId = "mutated-after-dispatch";
  callerOwnedRequest.title = "Mutated";

  PlaybackClockSnapshot clock{};
  clock.trackId = "track-1";
  clock.position = 42s;
  clock.sampledAt = std::chrono::steady_clock::now();
  clock.version = 7;
  dispatcher.dispatch(BackendEventType::PlaybackPositionUpdated, PlaybackPositionUpdated{clock});
  clock.trackId = "mutated-clock";
  clock.position = 0ms;

  REQUIRE(sink.events.size() == 2);

  CHECK(sink.events[0].type == BackendEventType::TrackChanged);
  CHECK(sink.events[0].sourceModule == BackendSourceModule::AudioPlaybackService);
  CHECK(sink.events[0].monotonicVersion == 1);
  CHECK(sink.events[0].timestamp != std::chrono::steady_clock::time_point{});
  CHECK(std::get<TrackChanged>(sink.events[0].payload).request.trackId == "track-1");
  CHECK(std::get<TrackChanged>(sink.events[0].payload).request.title == "Fixture");

  CHECK(sink.events[1].type == BackendEventType::PlaybackPositionUpdated);
  CHECK(sink.events[1].monotonicVersion == 2);
  CHECK(sink.events[1].timestamp != std::chrono::steady_clock::time_point{});
  CHECK(sink.events[1].timestamp >= sink.events[0].timestamp);
  CHECK(std::get<PlaybackPositionUpdated>(sink.events[1].payload).clock.trackId == "track-1");
  CHECK(std::get<PlaybackPositionUpdated>(sink.events[1].payload).clock.position == 42s);
  CHECK(dispatcher.nextVersion() == 3);
}

TEST_CASE("audio_event_dispatcher normalizes explicit backend events") {
  AudioEventDispatcher dispatcher{BackendSourceModule::AudioPlayer};
  FakeSink sink;
  dispatcher.setEventSink(sink.callback());

  BackendEvent event{};
  event.type = BackendEventType::PlaybackStateChanged;
  event.sourceModule = BackendSourceModule::AudioPlaybackService;
  event.monotonicVersion = 999;
  event.timestamp = std::chrono::steady_clock::time_point{};
  event.payload = PlaybackStateChanged{PlaybackState::Playing};

  dispatcher.dispatch(event);
  event.payload = PlaybackStateChanged{PlaybackState::Stopped};

  REQUIRE(sink.events.size() == 1);
  CHECK(sink.events[0].type == BackendEventType::PlaybackStateChanged);
  CHECK(sink.events[0].sourceModule == BackendSourceModule::AudioPlayer);
  CHECK(sink.events[0].monotonicVersion == 1);
  CHECK(sink.events[0].timestamp != std::chrono::steady_clock::time_point{});
  CHECK(std::get<PlaybackStateChanged>(sink.events[0].payload).state == PlaybackState::Playing);
}

}
