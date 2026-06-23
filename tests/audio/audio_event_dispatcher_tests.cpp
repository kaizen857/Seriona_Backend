#include "seriona/audio/events/audio_event_dispatcher.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
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

TEST_CASE("audio_event_dispatcher assigns unique versions under concurrent dispatch") {
  AudioEventDispatcher dispatcher;
  std::mutex eventsMutex;
  std::vector<BackendEvent> events;
  dispatcher.setEventSink([&](BackendEvent event) {
    std::lock_guard lock{eventsMutex};
    events.push_back(std::move(event));
  });

  constexpr int threadCount = 8;
  constexpr int eventsPerThread = 1000;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(threadCount);

  for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
    threads.emplace_back([&] {
      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int eventIndex = 0; eventIndex < eventsPerThread; ++eventIndex) {
        dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Playing});
      }
    });
  }

  while (ready.load(std::memory_order_acquire) != threadCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  for (auto& thread : threads) {
    thread.join();
  }

  std::vector<std::uint64_t> versions;
  {
    std::lock_guard lock{eventsMutex};
    REQUIRE(events.size() == static_cast<std::size_t>(threadCount * eventsPerThread));
    versions.reserve(events.size());
    for (const auto& event : events) {
      versions.push_back(event.monotonicVersion);
    }
  }

  std::ranges::sort(versions);
  for (std::size_t index = 0; index < versions.size(); ++index) {
    CHECK(versions[index] == index + 1U);
  }
  CHECK(dispatcher.nextVersion() == static_cast<std::uint64_t>(threadCount * eventsPerThread + 1));
}

TEST_CASE("audio_event_dispatcher invokes sink outside dispatcher synchronization") {
  AudioEventDispatcher dispatcher;
  std::atomic<int> delivered{0};
  dispatcher.setEventSink([&](BackendEvent) {
    delivered.fetch_add(1, std::memory_order_acq_rel);
    dispatcher.clearEventSink();
    CHECK_FALSE(dispatcher.hasEventSink());
  });

  dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Playing});
  dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Paused});

  CHECK(delivered.load(std::memory_order_acquire) == 1);
  CHECK(dispatcher.nextVersion() == 2);
}

}
