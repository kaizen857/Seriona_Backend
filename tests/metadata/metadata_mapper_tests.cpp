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
  snapshot.artwork = seriona::control::ArtworkRef{
      .localPath = std::filesystem::path{"covers/song.jpg"},
      .uri = std::string{"file:///covers/song.jpg"},
      .contentHash = std::string{"abc123"},
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

  CHECK(dto.mpris.track.trackId == "song/01");
  CHECK(dto.mpris.track.filePath == std::filesystem::path{"music/song.flac"});
  CHECK(dto.mpris.track.fileUri == "file://music/song.flac");
  CHECK(dto.mpris.track.sourceId == "library-a");
  CHECK(dto.mpris.track.libraryId == "library-b");
  CHECK_FALSE(dto.mpris.track.trackNumber.has_value());
  CHECK(dto.mpris.artwork.localPath == std::optional<std::filesystem::path>{std::filesystem::path{"covers/song.jpg"}});
  CHECK(dto.mpris.artwork.uri == std::optional<std::string>{"file:///covers/song.jpg"});
  CHECK(dto.mpris.artwork.contentHash == std::optional<std::string>{"abc123"});
  CHECK(dto.mpris.fields.title == std::optional<std::string>{"Song"});
  CHECK(dto.mpris.fields.artist == std::optional<std::string>{"Artist"});
  CHECK(dto.mpris.fields.album == std::optional<std::string>{"Album"});
  CHECK(dto.mpris.fields.albumArtist == std::optional<std::string>{"Album Artist"});
  CHECK(dto.mpris.fields.genre == std::optional<std::string>{"Genre"});
  CHECK(dto.mpris.playbackStatus == seriona::control::PlaybackStatus::Playing);
  CHECK(dto.mpris.repeatMode == seriona::control::RepeatMode::All);
  CHECK(dto.mpris.positionMicros == 1'234'000);
  CHECK(dto.mpris.durationMicros == std::optional<std::int64_t>{5'678'000});
  CHECK(dto.mpris.bufferedMicros == std::optional<std::int64_t>{3'456'000});
  CHECK(dto.mpris.seekableFromMicros == std::optional<std::int64_t>{100'000});
  CHECK(dto.mpris.seekableToMicros == std::optional<std::int64_t>{5'000'000});
  CHECK(dto.mpris.capabilities.canPlay);
  CHECK(dto.mpris.capabilities.canPause);
  CHECK(dto.mpris.capabilities.canStop);
  CHECK(dto.mpris.capabilities.canSeek);
  CHECK(dto.mpris.capabilities.canSkipNext);
  CHECK(dto.mpris.capabilities.canSkipPrevious);
  CHECK(dto.mpris.capabilities.canSetRepeat);
  CHECK(dto.mpris.capabilities.canSetShuffle);
  CHECK(dto.mpris.capabilities.canSetVolume);
  CHECK(dto.mpris.shuffle);
  CHECK(dto.mpris.muted);
  CHECK(dto.mpris.volume == doctest::Approx(0.75F));
  CHECK(dto.mpris.track.trackId != std::string{"internal-code"});

  CHECK(dto.windows.track.trackId == "song/01");
  CHECK(dto.windows.track.filePath == std::filesystem::path{"music/song.flac"});
  CHECK(dto.windows.track.fileUri == "file://music/song.flac");
  CHECK(dto.windows.track.sourceId == "library-a");
  CHECK(dto.windows.track.libraryId == "library-b");
  CHECK(dto.windows.artwork.localPath == std::optional<std::filesystem::path>{std::filesystem::path{"covers/song.jpg"}});
  CHECK(dto.windows.artwork.uri == std::optional<std::string>{"file:///covers/song.jpg"});
  CHECK(dto.windows.artwork.contentHash == std::optional<std::string>{"abc123"});
  CHECK(dto.windows.playbackStatus == seriona::control::PlaybackStatus::Playing);
  CHECK(dto.windows.repeatMode == seriona::control::RepeatMode::All);
  CHECK(dto.windows.fields.title == std::optional<std::string>{"Song"});
  CHECK(dto.windows.positionMicros == 1'234'000);
  CHECK(dto.windows.durationMicros == std::optional<std::int64_t>{5'678'000});
  CHECK(dto.windows.capabilities.canPlay);
  CHECK(dto.windows.capabilities.canPause);
  CHECK(dto.windows.capabilities.canStop);
  CHECK(dto.windows.capabilities.canSeek);
  CHECK(dto.windows.capabilities.canSkipNext);
  CHECK(dto.windows.capabilities.canSkipPrevious);
  CHECK(dto.windows.capabilities.canSetRepeat);
  CHECK(dto.windows.capabilities.canSetShuffle);
  CHECK_FALSE(dto.windows.capabilities.canSetVolume);
  CHECK(dto.windows.shuffle);
  CHECK_FALSE(dto.windows.track.trackId == "internal-code");
}

TEST_CASE("metadata mapper emits the no-track sentinel for missing track identities") {
  const auto dto = seriona::metadata::mapPlayerStateSnapshot(seriona::control::PlayerStateSnapshot{});

  CHECK(dto.mpris.trackObjectPath.value == seriona::metadata::makeMprisNoTrackObjectPath());
  CHECK(dto.windows.positionMicros == 0);
  CHECK_FALSE(dto.windows.durationMicros.has_value());
  CHECK_FALSE(dto.windows.capabilities.canSetVolume);
  CHECK_FALSE(dto.windows.capabilities.canPause);
  CHECK_FALSE(dto.windows.track.trackNumber.has_value());
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
