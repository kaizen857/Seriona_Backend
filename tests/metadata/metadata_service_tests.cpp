#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <optional>

#include "seriona/control/control_contracts.h"
#include "metadata_synchronizer.h"

namespace {

using Clock = std::chrono::steady_clock;

struct SnapshotFixture {
  std::uint64_t version{0};
  std::chrono::milliseconds position{0};
  seriona::control::PlaybackStatus status{seriona::control::PlaybackStatus::Stopped};
  std::optional<std::string> title{std::string{"Song"}};
  std::optional<std::filesystem::path> artworkPath{std::filesystem::path{"covers/track-01.jpg"}};
  bool canSetRepeat{true};
};

seriona::control::PlayerStateSnapshot buildSnapshot(const SnapshotFixture& fixture) {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = fixture.version;
  snapshot.freshness.sampledAt = Clock::time_point{std::chrono::milliseconds{static_cast<std::int64_t>(fixture.version)}};
  snapshot.currentTrack = seriona::control::TrackIdentity{
      .trackId = "track-01",
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

}  // namespace

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

  CHECK(playing.emitTimeline);
  CHECK(paused.emitTimeline);
  CHECK(seeking.emitTimeline);
  CHECK(resumed.emitTimeline);
  CHECK(stopped.emitTimeline);
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
