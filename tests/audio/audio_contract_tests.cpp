#include "doctest.h"

#include "seriona/audio/audio_contracts.h"

#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace seriona::audio;

PlaybackClockSnapshot makeClock(std::string trackId, std::uint64_t version, std::chrono::milliseconds position) {
  return PlaybackClockSnapshot{
      .trackId = std::move(trackId),
      .position = position,
      .sampledAt = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{version}},
      .version = version,
      .continuous = true,
  };
}

BackendEvent makeStateEvent(PlaybackState state, std::uint64_t version) {
  return BackendEvent{
      .type = BackendEventType::PlaybackStateChanged,
      .sourceModule = BackendSourceModule::AudioPlaybackService,
      .monotonicVersion = version,
      .timestamp = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{version}},
      .payload = PlaybackStateChanged{.state = state},
  };
}

}

TEST_CASE("audio contract symbols are value types") {
  static_assert(std::is_copy_constructible_v<TrackPlaybackRequest>);
  static_assert(std::is_copy_constructible_v<AudioOutputConfig>);
  static_assert(std::is_copy_constructible_v<AudioDeviceFormat>);
  static_assert(std::is_copy_constructible_v<PlaybackClockSnapshot>);
  static_assert(std::is_copy_constructible_v<PlaybackEvent>);
  static_assert(std::is_copy_constructible_v<BackendEvent>);
  static_assert(std::is_same_v<BackendEventSink, std::function<void(BackendEvent)>>);
  static_assert(std::is_abstract_v<AudioPlaybackService>);
  static_assert(std::is_class_v<AudioPlayer>);
}

TEST_CASE("backend event preserves payload and version semantics") {
  const auto request = TrackPlaybackRequest{
      .trackId = "track-1",
      .filePath = "music/track-1.wav",
      .title = "Track One",
      .offset = std::chrono::milliseconds{250},
      .duration = std::chrono::milliseconds{180000},
      .sampleRate = 48000,
      .bitDepth = 24,
      .channels = 2,
      .format = std::string{"wav"},
      .boundedSegment = true,
  };

  const auto clock = makeClock("track-1", 17, std::chrono::milliseconds{1250});
  const auto event = BackendEvent{
      .type = BackendEventType::PlaybackPositionUpdated,
      .sourceModule = BackendSourceModule::AudioPlaybackService,
      .monotonicVersion = 17,
      .timestamp = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{17}},
      .payload = PlaybackPositionUpdated{.clock = clock},
  };

  const auto copied = event;
  const auto moved = BackendEvent{copied};

  CHECK(moved.type == BackendEventType::PlaybackPositionUpdated);
  CHECK(moved.sourceModule == BackendSourceModule::AudioPlaybackService);
  CHECK(moved.monotonicVersion == 17);
  CHECK(std::holds_alternative<PlaybackPositionUpdated>(moved.payload));
  CHECK(std::get<PlaybackPositionUpdated>(moved.payload).clock.trackId == "track-1");
  CHECK(std::get<PlaybackPositionUpdated>(moved.payload).clock.position == std::chrono::milliseconds{1250});

  const auto trackChanged = BackendEvent{
      .type = BackendEventType::TrackChanged,
      .sourceModule = BackendSourceModule::AudioPlayer,
      .monotonicVersion = 18,
      .timestamp = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{18}},
      .payload = TrackChanged{.request = request},
  };

  CHECK(std::holds_alternative<TrackChanged>(trackChanged.payload));
  CHECK(std::get<TrackChanged>(trackChanged.payload).request.title == "Track One");
}

TEST_CASE("backend event sink captures events without raw handles") {
  std::vector<BackendEvent> captured;

  const BackendEventSink sink = [&captured](BackendEvent event) { captured.push_back(std::move(event)); };

  sink(makeStateEvent(PlaybackState::Playing, 41));
  sink(BackendEvent{
      .type = BackendEventType::PlaybackError,
      .sourceModule = BackendSourceModule::AudioPlaybackService,
      .monotonicVersion = 42,
      .timestamp = std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{42}},
      .payload = PlaybackError{
          .code = PlaybackErrorCode::DeviceUnavailable,
          .message = "device missing",
          .detail = "fallback disabled",
          .clock = makeClock("track-2", 42, std::chrono::milliseconds{0}),
      },
  });

  CHECK(captured.size() == 2);
  CHECK(captured[0].monotonicVersion == 41);
  CHECK(std::get<PlaybackStateChanged>(captured[0].payload).state == PlaybackState::Playing);
  CHECK(captured[1].monotonicVersion == 42);
  CHECK(std::get<PlaybackError>(captured[1].payload).code == PlaybackErrorCode::DeviceUnavailable);
  CHECK(std::get<PlaybackError>(captured[1].payload).clock.has_value());
}

TEST_CASE("output contracts retain playback configuration data") {
  const auto requested = AudioOutputConfig{
      .outputMode = AudioOutputMode::Direct,
      .targetSampleRate = 96000,
      .targetSampleFormat = AudioSampleFormat::Float32,
      .targetChannelCount = 2,
      .bufferDuration = std::chrono::milliseconds{80},
      .keepDeviceOpen = true,
      .allowFallback = false,
      .preferredDeviceId = "primary",
  };

  const auto deviceFormat = AudioDeviceFormat{
      .deviceId = "device-1",
      .deviceName = "Primary Device",
      .backendName = "miniaudio",
      .sampleRate = 48000,
      .sampleFormat = AudioSampleFormat::Int24,
      .channelCount = 2,
      .bufferFrames = 512,
      .actualMode = AudioOutputMode::Mixed,
      .fallbackApplied = true,
  };

  CHECK(requested.outputMode == AudioOutputMode::Direct);
  CHECK(requested.targetSampleRate.value() == 96000);
  CHECK(deviceFormat.fallbackApplied);
  CHECK(deviceFormat.actualMode == AudioOutputMode::Mixed);
}
