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

struct MetadataTrackObjectPath {
  std::string value;
};

struct MetadataMprisSnapshotDto {
  MetadataTrackObjectPath trackObjectPath{std::string{kMprisNoTrackObjectPath}};
  MetadataFieldSet fields{};
  std::int64_t positionMicros{0};
  std::optional<std::int64_t> durationMicros;
  std::optional<std::int64_t> bufferedMicros;
  std::optional<std::int64_t> seekableFromMicros;
  std::optional<std::int64_t> seekableToMicros;
  bool canPlay{false};
  bool canPause{false};
  bool canStop{false};
  bool canSeek{false};
  bool canSkipNext{false};
  bool canSkipPrevious{false};
  bool canSetRepeat{false};
  bool canSetShuffle{false};
  bool shuffle{false};
  bool muted{false};
  float volume{1.0F};
};

struct MetadataWindowsSnapshotDto {
  MetadataFieldSet fields{};
  std::int64_t positionMicros{0};
  std::optional<std::int64_t> durationMicros;
  std::optional<std::int64_t> bufferedMicros;
  std::optional<std::int64_t> seekableFromMicros;
  std::optional<std::int64_t> seekableToMicros;
  bool canPlay{false};
  bool canPause{false};
  bool canStop{false};
  bool canSeek{false};
  bool canSkipNext{false};
  bool canSkipPrevious{false};
  bool canSetRepeat{false};
  bool canSetShuffle{false};
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
