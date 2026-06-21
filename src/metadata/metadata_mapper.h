#pragma once

#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace seriona::metadata {

constexpr const char* kMprisNoTrackObjectPath = "/org/mpris/MediaPlayer2/TrackList/NoTrack";
constexpr const char* kMprisTrackObjectPathRoot = "/com/seriona/metadata/track";

struct MetadataFieldSet {
  std::optional<std::string> title;
  std::optional<std::string> artist;
  std::optional<std::string> album;
  std::optional<std::string> albumArtist;
  std::optional<std::string> genre;
};

struct MetadataTrackIdentityDto {
  std::string trackId;
  std::filesystem::path filePath;
  std::string fileUri;
  std::string sourceId;
  std::string libraryId;
  std::optional<std::uint16_t> trackNumber;
};

struct MetadataArtworkRefDto {
  std::optional<std::filesystem::path> localPath;
  std::optional<std::string> uri;
  std::optional<std::string> contentHash;
};

struct MetadataTrackObjectPathDto {
  std::string value;
};

struct MetadataCapabilitySetDto {
  bool canPlay{false};
  bool canPause{false};
  bool canStop{false};
  bool canSeek{false};
  bool canSkipNext{false};
  bool canSkipPrevious{false};
  bool canSetRepeat{false};
  bool canSetShuffle{false};
  bool canSetVolume{false};
};

struct MetadataMprisSnapshotDto {
  MetadataTrackIdentityDto track{};
  MetadataTrackObjectPathDto trackObjectPath{std::string{kMprisNoTrackObjectPath}};
  MetadataArtworkRefDto artwork{};
  MetadataFieldSet fields{};
  control::PlaybackStatus playbackStatus{control::PlaybackStatus::Stopped};
  control::RepeatMode repeatMode{control::RepeatMode::Off};
  std::int64_t positionMicros{0};
  std::optional<std::int64_t> durationMicros;
  std::optional<std::int64_t> bufferedMicros;
  std::optional<std::int64_t> seekableFromMicros;
  std::optional<std::int64_t> seekableToMicros;
  MetadataCapabilitySetDto capabilities{};
  bool shuffle{false};
  bool muted{false};
  float volume{1.0F};
};

struct MetadataWindowsSnapshotDto {
  MetadataTrackIdentityDto track{};
  MetadataArtworkRefDto artwork{};
  MetadataFieldSet fields{};
  control::PlaybackStatus playbackStatus{control::PlaybackStatus::Stopped};
  control::RepeatMode repeatMode{control::RepeatMode::Off};
  std::int64_t positionMicros{0};
  std::optional<std::int64_t> durationMicros;
  std::optional<std::int64_t> bufferedMicros;
  std::optional<std::int64_t> seekableFromMicros;
  std::optional<std::int64_t> seekableToMicros;
  MetadataCapabilitySetDto capabilities{};
  bool shuffle{false};
};

struct MetadataPlatformSnapshotDto {
  MetadataMprisSnapshotDto mpris{};
  MetadataWindowsSnapshotDto windows{};
};

[[nodiscard]] MetadataBackendCapabilities metadataMapperCapabilities();
[[nodiscard]] MetadataPlatformSnapshotDto mapPlayerStateSnapshot(const control::PlayerStateSnapshot& snapshot);
[[nodiscard]] std::string makeMprisTrackObjectPath(const control::TrackIdentity& track);
[[nodiscard]] std::string makeMprisNoTrackObjectPath();

}
