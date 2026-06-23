#include "seriona/audio/events/audio_event_dispatcher.h"

#include <doctest.h>

#include <atomic>
#include <mutex>
#include <thread>
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

TEST_CASE("audio_event_dispatcher supports concurrent set clear and dispatch") {
  AudioEventDispatcher dispatcher;
  std::mutex eventsMutex;
  std::vector<BackendEvent> events;
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};

  auto sink = [&eventsMutex, &events](BackendEvent event) {
    std::lock_guard lock{eventsMutex};
    events.push_back(std::move(event));
  };

  std::thread sinkMutator{[&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int index = 0; index < 2000; ++index) {
      dispatcher.setEventSink(sink);
      dispatcher.clearEventSink();
    }
    dispatcher.setEventSink(sink);
    done.store(true, std::memory_order_release);
  }};

  std::thread dispatcherThread{[&] {
    start.store(true, std::memory_order_release);
    while (!done.load(std::memory_order_acquire)) {
      dispatcher.dispatch(BackendEventType::PlaybackStateChanged, PlaybackStateChanged{PlaybackState::Playing});
    }
  }};

  sinkMutator.join();
  dispatcherThread.join();
  dispatcher.shutdown();

  std::vector<std::uint64_t> versions;
  {
    std::lock_guard lock{eventsMutex};
    versions.reserve(events.size());
    for (const auto& event : events) {
      versions.push_back(event.monotonicVersion);
    }
  }

  for (std::size_t index = 1; index < versions.size(); ++index) {
    CHECK(versions[index] > versions[index - 1]);
  }
  CHECK_FALSE(dispatcher.hasEventSink());
}

}
