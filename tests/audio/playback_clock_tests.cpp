#include "seriona/audio/clock/playback_clock.h"

#include <doctest.h>

#include <chrono>

using namespace std::chrono_literals;

namespace seriona::audio {

TEST_CASE("playback_clock derives position from consumed frames") {
  PlaybackClock clock;
  clock.reset("track-1", 48'000);
  clock.submitFrames(24'000U);
  clock.resume();
  clock.consumeFrames(12'000U);

  const auto snapshot = clock.snapshot();

  CHECK(snapshot.trackId == "track-1");
  CHECK(snapshot.position == 250ms);
  CHECK(snapshot.continuous);
  CHECK(clock.counters().submittedFrames == 24'000U);
  CHECK(clock.counters().consumedFrames == 12'000U);
}

TEST_CASE("playback_clock freezes while paused and resumes from frozen frame base") {
  PlaybackClock clock;
  clock.reset("track-1", 1'000);
  clock.resume();
  clock.consumeFrames(250U);
  clock.pause();
  const auto paused = clock.snapshot();

  clock.consumeFrames(500U);
  const auto stillPaused = clock.snapshot();
  clock.resume();
  clock.consumeFrames(125U);
  const auto resumed = clock.snapshot();

  CHECK(paused.position == 250ms);
  CHECK_FALSE(paused.continuous);
  CHECK(stillPaused.position == paused.position);
  CHECK_FALSE(stillPaused.continuous);
  CHECK(resumed.position == 375ms);
  CHECK(resumed.continuous);
}

TEST_CASE("playback_clock seek rebases position and increments version") {
  PlaybackClock clock;
  clock.reset("track-1", 1'000);
  clock.resume();
  clock.consumeFrames(500U);
  const auto before = clock.snapshot();

  clock.seek(42s);
  clock.consumeFrames(250U);
  const auto after = clock.snapshot();

  CHECK(before.position == 500ms);
  CHECK(after.position == 42'250ms);
  CHECK(after.version > before.version);
  CHECK(after.continuous);
  CHECK(clock.counters().seekCount == 1U);
}

TEST_CASE("playback_clock reset supports nonzero seek base") {
  PlaybackClock clock;
  clock.reset("track-2", 44'100, 30s);
  clock.resume();
  clock.consumeFrames(44'100U);

  const auto snapshot = clock.snapshot();

  CHECK(snapshot.trackId == "track-2");
  CHECK(snapshot.position == 31s);
  CHECK(snapshot.continuous);
}

TEST_CASE("playback_clock tracks underrun accounting independently from consumed frames") {
  PlaybackClock clock;
  clock.reset("track-1", 48'000);
  clock.reportUnderrun(256U);
  clock.reportUnderrun(128U);

  const auto counters = clock.counters();

  CHECK(counters.underrunCount == 2U);
  CHECK(counters.underrunFrames == 384U);
  CHECK(counters.consumedFrames == 0U);
}

}
