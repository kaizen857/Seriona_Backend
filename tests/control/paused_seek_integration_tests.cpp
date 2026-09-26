#include "paused_seek_test_support.h"

namespace seriona::control::test {

TEST_CASE("harness teardown joins copied callbacks before destroying controller including construction failure") {
  for (const auto teardown : {0, 1, 2}) {
    CAPTURE(teardown);
    bool controllerAliveAtJoin = false;
    std::atomic<bool> copiedCallbackFinished{false};
    auto prepare = [&](PausedSeekHarness& harness) {
      harness.device->onDestroy = [&harness, &controllerAliveAtJoin] {
        controllerAliveAtJoin = static_cast<bool>(harness.controller);
      };
      harness.audio->setEventSink([&, sink = harness.controller->audioEventSink()](audio::BackendEvent event) {
        sink(std::move(event));
        copiedCallbackFinished = true;
      });
      harness.audio->gate.arm();
      harness.audio->seek(5s);
      REQUIRE(harness.audio->gate.wait());
      if (teardown == 2) throw std::runtime_error("harness construction failure");
    };
    if (teardown == 2) {
      CHECK_THROWS_WITH(PausedSeekHarness(0ms, prepare), "harness construction failure");
    } else {
      PausedSeekHarness harness(0ms, prepare);
      if (teardown == 1) {
        harness.shutdown();
        harness.shutdown();
      }
    }
    CHECK(copiedCallbackFinished);
    CHECK(controllerAliveAtJoin);
  }
}

TEST_CASE("seek observer joins its worker during constructor failure with an in-flight callback") {
  SeekPcmFixture fixture;
  std::weak_ptr<audio::AudioPlaybackService> service;
  struct FailingConstruction {
    ObservedSeekAudio observer;
    FailingConstruction(const std::filesystem::path& path, std::weak_ptr<audio::AudioPlaybackService>& weak)
        : observer(audio::makeAudioPlaybackService(std::make_unique<ManualSeekDevice>())) {
      weak = observer.inner;
      observer.setEventSink([](audio::BackendEvent) {});
      observer.gate.arm();
      observer.loadTrack(audio::TrackPlaybackRequest{.trackId = "construction", .filePath = path});
      if (!observer.gate.wait()) throw std::runtime_error("gate timeout");
      throw std::runtime_error("controlled constructor failure");
    }
  };
  CHECK_THROWS_WITH(FailingConstruction(fixture.path(), service), "controlled constructor failure");
  CHECK(service.expired());
}

TEST_CASE("production paused seek resumes target PCM without reloading with either command ordering") {
  for (const auto offset : {0ms, 2000ms}) {
    for (const bool delayed : {false, true}) {
      for (const auto command : {MediaControlCommandKind::Play, MediaControlCommandKind::TogglePlayPause}) {
        CAPTURE(offset.count());
        CAPTURE(delayed);
        CAPTURE(command);
        PausedSeekHarness harness(offset);
        if (delayed) harness.audio->gate.arm();
        harness.command(MediaControlCommandKind::SeekTo, 5s);
        if (delayed) {
          REQUIRE(harness.audio->gate.wait());
          harness.command(command);
          CHECK(harness.audio->resumes == 1);
          harness.audio->gate.release();
        } else {
          const auto sought = harness.settle();
          CHECK(sought.position == offset + 5s);
          CHECK_FALSE(sought.continuous);
          CHECK_FALSE(harness.device->active());
          CHECK(harness.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);
          harness.command(command);
        }
        const auto resumed = harness.settle();
        CHECK(resumed.position == offset + 5s);
        CHECK(resumed.continuous);
        CHECK(harness.audio->loads == 1);
        CHECK(harness.audio->resumes == 1);
        CHECK(harness.audio->errors == 0);
        CHECK(harness.controller->playerStateSnapshot().timeline.position == 5s);
        harness.expectPcm(0.5F);
        CHECK(harness.settle().position == offset + 5010ms);
      }
    }
  }
}

TEST_CASE("production paused seek supports direct audio resume and play after repeated targets") {
  for (const bool resume : {false, true}) {
    PausedSeekHarness harness;
    for (const auto target : {5s, 0s, 12s, 5s}) {
      harness.command(MediaControlCommandKind::SeekTo, target);
      CHECK(harness.settle().position == target);
      CHECK(harness.audio->state == audio::PlaybackState::Paused);
      CHECK_FALSE(harness.device->active());
    }
    if (resume) harness.audio->resume();
    else harness.audio->play();
    CHECK(harness.settle().position == 5s);
    CHECK(harness.audio->errors == 0);
    CHECK(harness.audio->loads == 1);
    harness.expectPcm(0.5F);
    CHECK(harness.settle().position == 5010ms);
  }
}

TEST_CASE("production queued stop or replacement wins over paused seek completion") {
  for (const bool replace : {false, true}) {
    PausedSeekHarness harness;
    harness.audio->gate.arm();
    harness.command(MediaControlCommandKind::SeekTo, 5s);
    REQUIRE(harness.audio->gate.wait());
    if (replace) {
      harness.audio->loadTrack(audio::TrackPlaybackRequest{.trackId = "replacement", .filePath = harness.fixture.path(), .offset = 1s});
    } else harness.command(MediaControlCommandKind::Stop);
    harness.audio->gate.release();
    const auto clock = harness.settle();
    CHECK_FALSE(clock.continuous);
    CHECK_FALSE(harness.device->active());
    CHECK(harness.audio->errors == 0);
    if (replace) {
      CHECK(clock.trackId == "replacement");
      CHECK(clock.position == 1s);
      CHECK(harness.audio->state == audio::PlaybackState::Ready);
    } else {
      CHECK(harness.audio->state == audio::PlaybackState::Stopped);
      CHECK(harness.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
    }
  }
}

TEST_CASE("production output reload after paused seek preserves pause and target") {
  PausedSeekHarness harness;
  harness.command(MediaControlCommandKind::SeekTo, 5s);
  harness.settle();
  MediaControlCommand config;
  config.kind = MediaControlCommandKind::ConfigureOutput;
  config.outputConfig = harness.output;
  REQUIRE(harness.controller->submitCommand(config).accepted);
  CHECK(harness.settle().position == 5s);
  CHECK(harness.audio->state == audio::PlaybackState::Paused);
  CHECK_FALSE(harness.device->active());
  CHECK(harness.audio->loads == 2);
  harness.command(MediaControlCommandKind::TogglePlayPause);
  CHECK(harness.settle().position == 5s);
  CHECK(harness.audio->loads == 2);
  CHECK(harness.audio->errors == 0);
  harness.expectPcm(0.5F);
}

}
