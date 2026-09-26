#include "seriona/audio/playback_state_machine.h"

#include <doctest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

namespace seriona::audio {
namespace {

struct SeekFixture {
  PlaybackStateMachine machine;
  std::vector<BackendEvent> events;

  explicit SeekFixture(PlaybackState initial) {
    machine.setEventSink([this](BackendEvent event) { events.push_back(std::move(event)); });
    machine.loadTrack(TrackPlaybackRequest{.trackId = "first", .filePath = "generated.wav", .offset = 1s});
    machine.completeLoad();
    if (initial != PlaybackState::Ready) machine.play();
    if (initial == PlaybackState::Paused) machine.pause();
    events.clear();
  }
};

}

TEST_CASE("seek transaction restores each stable transport state on success and cancellation") {
  for (const auto initial : {PlaybackState::Ready, PlaybackState::Paused, PlaybackState::Playing}) {
    for (const bool cancel : {false, true}) {
      CAPTURE(initial);
      CAPTURE(cancel);
      SeekFixture fixture(initial);
      const auto before = fixture.machine.clock();
      const auto generation = fixture.machine.beginSeek(5s);
      CHECK(fixture.machine.state() == PlaybackState::Loading);
      CHECK(fixture.machine.hasPendingSeek());
      if (cancel) fixture.machine.cancelSeek(PlaybackErrorCode::SeekFailed, "seek failed", "controlled failure");
      else fixture.machine.completeSeek(generation);

      CHECK(fixture.machine.state() == initial);
      CHECK_FALSE(fixture.machine.hasPendingSeek());
      CHECK(fixture.machine.clock().position == (cancel ? 1s : 5s));
      CHECK(fixture.machine.clock().continuous == (initial == PlaybackState::Playing));
      CHECK(fixture.machine.clock().trackId == "first");
      REQUIRE(fixture.events.size() == 3);
      CHECK(std::get<PlaybackStateChanged>(fixture.events.front().payload).state == PlaybackState::Loading);
      if (cancel) {
        CHECK(fixture.machine.clock().version == before.version);
        CHECK(fixture.machine.clock().sampledAt == before.sampledAt);
        CHECK(std::get<PlaybackStateChanged>(fixture.events[1].payload).state == initial);
        const auto& error = std::get<PlaybackError>(fixture.events[2].payload);
        CHECK(error.code == PlaybackErrorCode::SeekFailed);
        CHECK(error.message == "seek failed");
        CHECK(error.detail == "controlled failure");
      } else {
        const auto& seek = std::get<PositionDiscontinuity>(fixture.events[1].payload);
        CHECK(seek.before.position == 1s);
        CHECK(seek.after.position == 5s);
        CHECK(seek.after.continuous == (initial == PlaybackState::Playing));
        CHECK(seek.after.version == generation);
        CHECK(seek.reason == "seek");
        CHECK(std::get<PlaybackStateChanged>(fixture.events[2].payload).state == initial);
      }
      const auto count = fixture.events.size();
      fixture.machine.completeSeek(generation);
      CHECK(fixture.events.size() == count);
    }
  }
}

TEST_CASE("seek transaction paused targets support both resume and play including repeated boundaries") {
  for (const bool resume : {false, true}) {
    SeekFixture fixture(PlaybackState::Paused);
    for (const auto target : {5s, 0s, 12s, 5s}) {
      const auto generation = fixture.machine.beginSeek(target);
      fixture.machine.completeSeek(generation);
      CHECK(fixture.machine.state() == PlaybackState::Paused);
      CHECK(fixture.machine.clock().position == target);
      CHECK_FALSE(fixture.machine.clock().continuous);
    }
    fixture.events.clear();
    if (resume) fixture.machine.resume();
    else fixture.machine.play();
    CHECK(fixture.machine.state() == PlaybackState::Playing);
    CHECK(fixture.machine.clock().position == 5s);
    CHECK(fixture.machine.clock().continuous);
    REQUIRE(fixture.events.size() == 1);
    CHECK(fixture.events.front().type == BackendEventType::PlaybackStateChanged);
  }
}

TEST_CASE("seek transaction invalidation never restores or reports an obsolete completion") {
  for (const int invalidation : {0, 1, 2, 3, 4}) {
    CAPTURE(invalidation);
    SeekFixture fixture(PlaybackState::Paused);
    const auto old = fixture.machine.beginSeek(5s);
    switch (invalidation) {
    case 0: fixture.machine.stop(); break;
    case 1: fixture.machine.loadTrack(TrackPlaybackRequest{.trackId = "second", .filePath = "other.wav"}); break;
    case 2: fixture.machine.fail(PlaybackErrorCode::DecodeFailed, "failure"); break;
    case 3: fixture.machine.pause(); break;
    case 4: fixture.machine.cancelSeek(PlaybackErrorCode::SeekFailed, "failure"); break;
    }
    const auto state = fixture.machine.state();
    const auto clock = fixture.machine.clock();
    const auto count = fixture.events.size();
    fixture.machine.completeSeek(old);
    CHECK_FALSE(fixture.machine.hasPendingSeek());
    CHECK(fixture.machine.state() == state);
    CHECK(fixture.machine.clock().position == clock.position);
    CHECK(fixture.machine.clock().trackId == clock.trackId);
    CHECK(fixture.events.size() == count);

    if (invalidation == 1) fixture.machine.completeLoad();
    if (fixture.machine.state() == PlaybackState::Ready || fixture.machine.state() == PlaybackState::Paused) {
      const auto next = fixture.machine.beginSeek(8s);
      fixture.machine.completeSeek(old);
      CHECK(fixture.machine.state() == PlaybackState::Loading);
      CHECK(fixture.machine.hasPendingSeek());
      fixture.machine.completeSeek(next);
      CHECK(fixture.machine.clock().position == 8s);
      CHECK_FALSE(fixture.machine.hasPendingSeek());
    }
  }
}

}
