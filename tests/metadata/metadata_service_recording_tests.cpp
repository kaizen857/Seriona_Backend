#include <doctest/doctest.h>

#include <chrono>

#include "seriona/control/control_contracts.h"
#include "metadata_service_testing.h"

namespace {

seriona::control::PlayerStateSnapshot buildSnapshot(std::uint64_t version, std::chrono::milliseconds position) {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = version;
  snapshot.freshness.sampledAt = std::chrono::steady_clock::time_point{std::chrono::milliseconds{static_cast<std::int64_t>(version)}};
  snapshot.currentTrack = seriona::control::TrackIdentity{.trackId = "track-01",
                                                          .filePath = std::filesystem::path{"music/track-01.flac"},
                                                          .sourceId = "source-a",
                                                          .libraryId = "library-a"};
  snapshot.playback.state = seriona::control::PlaybackStatus::Playing;
  snapshot.capabilities = seriona::control::PlaybackCapabilities{.canPlay = true,
                                                                 .canPause = true,
                                                                 .canStop = true,
                                                                 .canSeek = true,
                                                                 .canSkipNext = true,
                                                                 .canSkipPrevious = true,
                                                                 .canSetRepeat = true,
                                                                 .canSetShuffle = true,
                                                                 .canSetVolume = true,
                                                                 .canSelectTrack = true};
  snapshot.timeline.position = position;
  snapshot.timeline.duration = std::chrono::milliseconds{3000};
  snapshot.timeline.buffered = std::chrono::milliseconds{1500};
  return snapshot;
}

}

TEST_CASE("metadata recording backend captures structured start update and stop evidence") {
  const auto hooks = seriona::metadata::makeMetadataServiceTestHooks();
  auto service = seriona::metadata::makeRecordingMetadataSharingService(
      seriona::metadata::MetadataSharingOptions{}, hooks);

  CHECK(service->start(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot(1U, std::chrono::milliseconds{0}),
                                                             .timelineUpdateInterval = std::chrono::milliseconds{1000}}).accepted);
  CHECK(service->update(seriona::metadata::PlatformMediaState{.controlState = buildSnapshot(2U, std::chrono::milliseconds{1000}),
                                                              .timelineUpdateInterval = std::chrono::milliseconds{1000}}).accepted);
  CHECK(service->stop().accepted);

  REQUIRE(hooks->records.size() == 3U);
  CHECK(hooks->records[0].kind == seriona::metadata::MetadataServiceRecordKind::Start);
  CHECK(hooks->records[0].state.controlState.freshness.version == 1U);
  CHECK(hooks->records[1].kind == seriona::metadata::MetadataServiceRecordKind::Update);
  CHECK(hooks->records[1].state.controlState.freshness.version == 2U);
  CHECK(hooks->records[1].result.changed);
  CHECK(hooks->records[2].kind == seriona::metadata::MetadataServiceRecordKind::Stop);
}
