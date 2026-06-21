#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "seriona/control/control_contracts.h"
#include "metadata_mapper.h"

namespace {

seriona::control::PlayerStateSnapshot buildSnapshot() {
  seriona::control::PlayerStateSnapshot snapshot{};
  snapshot.freshness.version = 99U;
  snapshot.freshness.sampledAt = std::chrono::steady_clock::now();
  snapshot.currentTrack = seriona::control::TrackIdentity{
      .trackId = "song/01",
      .filePath = std::filesystem::path{"music/song.flac"},
      .sourceId = "library-a",
      .libraryId = "library-b",
  };
  snapshot.display = seriona::control::DisplayMetadata{
      .title = "Song",
      .artist = "Artist",
      .album = "Album",
      .albumArtist = "Album Artist",
      .genre = "Genre",
  };
  snapshot.playback.state = seriona::control::PlaybackStatus::Playing;
  snapshot.playback.errorCode = std::string{"internal-code"};
  snapshot.playback.errorMessage = std::string{"internal error summary"};
  snapshot.repeatMode = seriona::control::RepeatMode::All;
  snapshot.shuffle = true;
  snapshot.capabilities = seriona::control::PlaybackCapabilities{
      .canPlay = true,
      .canPause = true,
      .canStop = true,
      .canSeek = true,
      .canSkipNext = true,
      .canSkipPrevious = true,
      .canSetRepeat = true,
      .canSetShuffle = true,
      .canSetVolume = true,
      .canSelectTrack = true,
  };
  snapshot.timeline.position = std::chrono::milliseconds{1234};
  snapshot.timeline.duration = std::chrono::milliseconds{5678};
  snapshot.timeline.buffered = std::chrono::milliseconds{3456};
  snapshot.timeline.seekableFrom = std::chrono::milliseconds{100};
  snapshot.timeline.seekableTo = std::chrono::milliseconds{5000};
  snapshot.volume = 0.75F;
  snapshot.muted = true;
  return snapshot;
}

}

TEST_CASE("metadata mapper exposes a concrete capability baseline") {
  const auto capabilities = seriona::metadata::metadataMapperCapabilities();

  CHECK_FALSE(capabilities.canPublishMetadata);
  CHECK_FALSE(capabilities.canPublishTimeline);
  CHECK_FALSE(capabilities.canReceiveCommands);
  CHECK_FALSE(capabilities.requiresPlatformExtension);
  CHECK_FALSE(capabilities.hasPlatformExtension);
}

TEST_CASE("metadata mapper forwards supported fields and drops internal-only snapshot data") {
  const auto dto = seriona::metadata::mapPlayerStateSnapshot(buildSnapshot());

  CHECK(dto.mpris.fields.title == std::optional<std::string>{"Song"});
  CHECK(dto.mpris.fields.artist == std::optional<std::string>{"Artist"});
  CHECK(dto.mpris.fields.album == std::optional<std::string>{"Album"});
  CHECK(dto.mpris.fields.albumArtist == std::optional<std::string>{"Album Artist"});
  CHECK(dto.mpris.fields.genre == std::optional<std::string>{"Genre"});
  CHECK(dto.mpris.positionMicros == 1'234'000);
  CHECK(dto.mpris.durationMicros == std::optional<std::int64_t>{5'678'000});
  CHECK(dto.mpris.bufferedMicros == std::optional<std::int64_t>{3'456'000});
  CHECK(dto.mpris.seekableFromMicros == std::optional<std::int64_t>{100'000});
  CHECK(dto.mpris.seekableToMicros == std::optional<std::int64_t>{5'000'000});
  CHECK(dto.mpris.canPlay);
  CHECK(dto.mpris.canPause);
  CHECK(dto.mpris.canStop);
  CHECK(dto.mpris.canSeek);
  CHECK(dto.mpris.canSkipNext);
  CHECK(dto.mpris.canSkipPrevious);
  CHECK(dto.mpris.canSetRepeat);
  CHECK(dto.mpris.canSetShuffle);
  CHECK(dto.mpris.shuffle);
  CHECK(dto.mpris.muted);
  CHECK(dto.mpris.volume == doctest::Approx(0.75F));

  CHECK(dto.windows.fields.title == std::optional<std::string>{"Song"});
  CHECK(dto.windows.positionMicros == 1'234'000);
  CHECK(dto.windows.durationMicros == std::optional<std::int64_t>{5'678'000});
  CHECK(dto.windows.canPlay);
  CHECK(dto.windows.canPause);
  CHECK(dto.windows.canStop);
  CHECK(dto.windows.canSeek);
  CHECK(dto.windows.canSkipNext);
  CHECK(dto.windows.canSkipPrevious);
  CHECK(dto.windows.canSetRepeat);
  CHECK(dto.windows.canSetShuffle);
  CHECK(dto.windows.shuffle);
}

TEST_CASE("metadata mapper emits the no-track sentinel for missing track identities") {
  const auto dto = seriona::metadata::mapPlayerStateSnapshot(seriona::control::PlayerStateSnapshot{});

  CHECK(dto.mpris.trackObjectPath.value == seriona::metadata::makeMprisNoTrackObjectPath());
  CHECK(dto.windows.positionMicros == 0);
  CHECK_FALSE(dto.windows.durationMicros.has_value());
}

TEST_CASE("metadata mapper generates non-reserved MPRIS object paths for track identities") {
  const seriona::control::TrackIdentity track{
      .trackId = "disc 1/song-01",
      .filePath = std::filesystem::path{"music/song.flac"},
      .sourceId = "library-a",
      .libraryId = "library-b",
  };

  const auto path = seriona::metadata::makeMprisTrackObjectPath(track);

  CHECK(path.rfind("/org/mpris", 0) != 0);
  CHECK(path.rfind(seriona::metadata::kMprisTrackObjectPathRoot, 0) == 0);
  CHECK(path != seriona::metadata::makeMprisNoTrackObjectPath());
}

TEST_CASE("metadata mapper treats empty track ids as the no-track sentinel") {
  const seriona::control::TrackIdentity track{
      .trackId = {},
      .filePath = std::filesystem::path{"music/song.flac"},
      .sourceId = "library-a",
      .libraryId = "library-b",
  };

  CHECK(seriona::metadata::makeMprisTrackObjectPath(track) == seriona::metadata::makeMprisNoTrackObjectPath());
}
