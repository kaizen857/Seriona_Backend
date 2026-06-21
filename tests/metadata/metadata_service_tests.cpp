#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"
#include "metadata_service_testing.h"
#include "metadata_synchronizer.h"

namespace {

using Clock = std::chrono::steady_clock;

struct SnapshotFixture {
  std::uint64_t version{0};
  std::chrono::milliseconds position{0};
  seriona::control::PlaybackStatus status{seriona::control::PlaybackStatus::Stopped};
  std::string trackId{"track-01"};
  std::optional<std::string> title{std::string{"Song"}};
  std::optional<std::filesystem::path> artworkPath{std::filesystem::path{"covers/track-01.jpg"}};
  bool canSetRepeat{true};
};

seriona::control::PlayerStateSnapshot buildSnapshot(const SnapshotFixture& fixture) {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = fixture.version;
  snapshot.freshness.sampledAt = Clock::time_point{std::chrono::milliseconds{static_cast<std::int64_t>(fixture.version)}};
  snapshot.currentTrack = seriona::control::TrackIdentity{
      .trackId = fixture.trackId,
      .filePath = std::filesystem::path{"music/track-01.flac"},
      .sourceId = "source-a",
      .libraryId = "library-a",
  };
  snapshot.display = seriona::control::DisplayMetadata{.title = fixture.title.value_or("Song"),
                                                        .artist = "Artist",
                                                        .album = "Album",
                                                        .albumArtist = "Album Artist",
                                                        .genre = "Genre"};
  snapshot.artwork = seriona::control::ArtworkRef{.localPath = fixture.artworkPath,
                                                  .uri = std::string{"file:///covers/track-01.jpg"},
                                                  .contentHash = std::string{"hash-01"}};
  snapshot.playback.state = fixture.status;
  snapshot.capabilities = seriona::control::PlaybackCapabilities{.canPlay = true,
                                                                 .canPause = true,
                                                                 .canStop = true,
                                                                 .canSeek = true,
                                                                 .canSkipNext = true,
                                                                 .canSkipPrevious = true,
                                                                 .canSetRepeat = fixture.canSetRepeat,
                                                                 .canSetShuffle = true,
                                                                 .canSetVolume = true,
                                                                 .canSelectTrack = true};
  snapshot.timeline.position = fixture.position;
  snapshot.timeline.duration = std::chrono::milliseconds{3000};
  snapshot.timeline.buffered = std::chrono::milliseconds{1500};
  snapshot.timeline.seekableFrom = std::chrono::milliseconds{0};
  snapshot.timeline.seekableTo = std::chrono::milliseconds{3000};
  return snapshot;
}

}

namespace {

struct ServiceFixture {
  seriona::metadata::MetadataSharingOptions options{};
  seriona::metadata::PlatformMediaState state{.controlState = buildSnapshot(SnapshotFixture{.version = 1U,
                                                                                           .position = std::chrono::milliseconds{0},
                                                                                           .status = seriona::control::PlaybackStatus::Playing}),
                                              .timelineUpdateInterval = std::chrono::milliseconds{1000}};
};

struct CommandRecorder {
  std::vector<seriona::control::MediaControlCommand> commands{};

  seriona::control::MediaControlCommandSink sink() {
    return [this](const seriona::control::MediaControlCommand& command) { commands.push_back(command); };
  }
};

}

TEST_CASE("metadata synchronizer suppresses half-second playback ticks") {
  seriona::metadata::MetadataSynchronizer synchronizer{};

  const auto first = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 1U,
                                                                            .position = std::chrono::milliseconds{0},
                                                                            .status = seriona::control::PlaybackStatus::Playing}));
  const auto second = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 2U,
                                                                             .position = std::chrono::milliseconds{500},
                                                                             .status = seriona::control::PlaybackStatus::Playing}));

  CHECK(first.emitMetadata);
  CHECK(first.emitTimeline);
  CHECK_FALSE(second.emitMetadata);
  CHECK_FALSE(second.emitTimeline);
}

TEST_CASE("metadata synchronizer emits timeline only on one second playback ticks") {
  seriona::metadata::MetadataSynchronizer synchronizer{};

  const auto first = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 1U,
                                                                            .position = std::chrono::milliseconds{0},
                                                                            .status = seriona::control::PlaybackStatus::Playing}));
  const auto second = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 2U,
                                                                             .position = std::chrono::milliseconds{1000},
                                                                             .status = seriona::control::PlaybackStatus::Playing}));

  CHECK(first.emitMetadata);
  CHECK(first.emitTimeline);
  CHECK_FALSE(second.emitMetadata);
  CHECK(second.emitTimeline);
}

TEST_CASE("metadata synchronizer emits metadata once for title artwork and capability changes") {
  seriona::metadata::MetadataSynchronizer synchronizer{};

  const auto first = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 1U,
                                                                            .position = std::chrono::milliseconds{0},
                                                                            .status = seriona::control::PlaybackStatus::Playing}));
  const auto titleChanged = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 2U,
                                                                                   .position = std::chrono::milliseconds{250},
                                                                                   .status = seriona::control::PlaybackStatus::Playing,
                                                                                   .title = std::string{"Song 2"}}));
  const auto artworkChanged = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 3U,
                                                                                      .position = std::chrono::milliseconds{450},
                                                                                      .status = seriona::control::PlaybackStatus::Playing,
                                                                                      .title = std::string{"Song 2"},
                                                                                      .artworkPath = std::filesystem::path{"covers/track-02.jpg"}}));
  const auto capabilityChanged = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 4U,
                                                                                         .position = std::chrono::milliseconds{650},
                                                                                         .status = seriona::control::PlaybackStatus::Playing,
                                                                                         .title = std::string{"Song 2"},
                                                                                         .artworkPath = std::filesystem::path{"covers/track-02.jpg"},
                                                                                         .canSetRepeat = false}));
  const auto repeatedTick = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 5U,
                                                                                   .position = std::chrono::milliseconds{850},
                                                                                   .status = seriona::control::PlaybackStatus::Playing,
                                                                                   .title = std::string{"Song 2"},
                                                                                   .artworkPath = std::filesystem::path{"covers/track-02.jpg"},
                                                                                   .canSetRepeat = false}));

  CHECK(first.emitMetadata);
  CHECK(first.emitTimeline);
  CHECK(titleChanged.emitMetadata);
  CHECK_FALSE(titleChanged.emitTimeline);
  CHECK(artworkChanged.emitMetadata);
  CHECK_FALSE(artworkChanged.emitTimeline);
  CHECK(capabilityChanged.emitMetadata);
  CHECK_FALSE(capabilityChanged.emitTimeline);
  CHECK_FALSE(repeatedTick.emitMetadata);
  CHECK_FALSE(repeatedTick.emitTimeline);
}

TEST_CASE("metadata synchronizer emits immediate timeline updates for pause seek resume track change and stop") {
  seriona::metadata::MetadataSynchronizer synchronizer{};

  const auto playing = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 1U,
                                                                              .position = std::chrono::milliseconds{0},
                                                                              .status = seriona::control::PlaybackStatus::Playing}));
  const auto paused = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 2U,
                                                                             .position = std::chrono::milliseconds{150},
                                                                             .status = seriona::control::PlaybackStatus::Paused}));
  const auto seeking = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 3U,
                                                                              .position = std::chrono::milliseconds{700},
                                                                              .status = seriona::control::PlaybackStatus::Seeking}));
  const auto resumed = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 4U,
                                                                             .position = std::chrono::milliseconds{1000},
                                                                             .status = seriona::control::PlaybackStatus::Playing}));
  const auto stopped = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 5U,
                                                                             .position = std::chrono::milliseconds{0},
                                                                             .status = seriona::control::PlaybackStatus::Stopped}));
  const auto trackChanged = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 6U,
                                                                                   .position = std::chrono::milliseconds{0},
                                                                                   .status = seriona::control::PlaybackStatus::Playing,
                                                                                   .trackId = "track-02"}));

  CHECK(playing.emitTimeline);
  CHECK(paused.emitTimeline);
  CHECK(seeking.emitTimeline);
  CHECK(resumed.emitTimeline);
  CHECK(stopped.emitTimeline);
  CHECK(trackChanged.emitTimeline);
}

TEST_CASE("metadata synchronizer ignores stale snapshots by freshness only") {
  seriona::metadata::MetadataSynchronizer synchronizer{};

  const auto fresh = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 2U,
                                                                            .position = std::chrono::milliseconds{0},
                                                                            .status = seriona::control::PlaybackStatus::Playing}));
  const auto staleVersion = synchronizer.synchronize(buildSnapshot(SnapshotFixture{.version = 1U,
                                                                                     .position = std::chrono::milliseconds{1000},
                                                                                     .status = seriona::control::PlaybackStatus::Playing,
                                                                                     .title = std::string{"Stale"}}));

  CHECK(fresh.emitMetadata);
  CHECK(fresh.emitTimeline);
  CHECK_FALSE(staleVersion.emitMetadata);
  CHECK_FALSE(staleVersion.emitTimeline);
}

TEST_CASE("metadata service selects noop backend explicitly") {
  const auto service = seriona::metadata::makeMetadataSharingService(seriona::metadata::MetadataSharingOptions{.backendKind = seriona::metadata::MetadataBackendKind::Noop,
                                                                                                              .platformExtension = nullptr});

  REQUIRE(service != nullptr);
  CHECK(service->backendKind() == seriona::metadata::MetadataBackendKind::Noop);
  CHECK_FALSE(service->capabilities().canPublishMetadata);
  CHECK_FALSE(service->capabilities().canPublishTimeline);
  CHECK_FALSE(service->capabilities().canReceiveCommands);
}

TEST_CASE("metadata service degrades Windows backend without host through capabilities") {
  const auto service = seriona::metadata::makeMetadataSharingService(seriona::metadata::MetadataSharingOptions{.backendKind = seriona::metadata::MetadataBackendKind::Windows,
                                                                                                              .platformExtension = nullptr});

  CHECK(service->backendKind() == seriona::metadata::MetadataBackendKind::Windows);
  CHECK(service->capabilities().requiresPlatformExtension);
  CHECK_FALSE(service->capabilities().hasPlatformExtension);
  CHECK_FALSE(service->capabilities().canPublishMetadata);
  CHECK_FALSE(service->capabilities().canPublishTimeline);
}

TEST_CASE("metadata service keeps start stop idempotent") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  ServiceFixture fixture{};
  auto service = seriona::metadata::makeRecordingMetadataSharingService(fixture.options, hooks);

  const auto firstStart = service->start(fixture.state);
  const auto secondStart = service->start(fixture.state);
  const auto firstStop = service->stop();
  const auto secondStop = service->stop();

  CHECK(firstStart.accepted);
  CHECK(secondStart.accepted);
  CHECK(firstStop.accepted);
  CHECK(secondStop.accepted);
}

TEST_CASE("metadata service reports update after stop as rejected") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  ServiceFixture fixture{};
  auto service = seriona::metadata::makeRecordingMetadataSharingService(fixture.options, hooks);

  CHECK(service->start(fixture.state).accepted);
  CHECK(service->stop().accepted);

  const auto updateAfterStop = service->update(fixture.state);

  CHECK_FALSE(updateAfterStop.accepted);
  CHECK_FALSE(updateAfterStop.changed);
  CHECK(updateAfterStop.errorCode.has_value());
}

TEST_CASE("metadata service registers and unregisters command callbacks") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  ServiceFixture fixture{};
  auto service = seriona::metadata::makeRecordingMetadataSharingService(fixture.options, hooks);
  CommandRecorder recorder{};

  const auto handle = service->registerCommandCallback(recorder.sink());
  REQUIRE(handle.unsubscribe);
  CHECK(hooks->commandRegistrations == 1U);

  handle.unsubscribe();
  handle.unsubscribe();
  CHECK(handle.subscriptionId != 0U);
  CHECK(hooks->commandUnregistrations == 1U);
}

TEST_CASE("metadata service reports backend start failure explicitly") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  hooks->failStart = true;
  ServiceFixture fixture{};
  auto service = seriona::metadata::makeRecordingMetadataSharingService(fixture.options, hooks);

  const auto result = service->start(fixture.state);

  CHECK_FALSE(result.accepted);
  CHECK(result.errorCode.has_value());
  CHECK(result.message == "metadata backend start failed");
}

TEST_CASE("metadata service recording backend captures timeline and metadata updates") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  ServiceFixture fixture{};
  auto service = seriona::metadata::makeRecordingMetadataSharingService(fixture.options, hooks);

  const auto start = service->start(fixture.state);
  const auto update = service->update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot(SnapshotFixture{.version = 2U,
                                                                                                                        .position = std::chrono::milliseconds{1000},
                                                                                                                        .status = seriona::control::PlaybackStatus::Playing,
                                                                                                                        .title = std::string{"Song 2"}}),
                                                                              .timelineUpdateInterval = std::chrono::milliseconds{1000}});

  CHECK(start.accepted);
  CHECK(update.accepted);
  CHECK(update.changed);
  REQUIRE(hooks->results.size() >= 2U);
  CHECK(hooks->results.back().changed);
}
