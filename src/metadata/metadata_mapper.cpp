#include "metadata_mapper.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace seriona::metadata {

MetadataBackendCapabilities metadataMapperCapabilities() {
  return MetadataBackendCapabilities{};
}

namespace {

[[nodiscard]] std::string encodeTrackId(std::string_view trackId) {
  std::ostringstream stream;
  stream << std::hex << std::nouppercase << std::setfill('0');
  for (const auto byte : trackId) {
    stream << std::setw(2) << static_cast<int>(static_cast<unsigned char>(byte));
  }
  return stream.str();
}

[[nodiscard]] std::int64_t toMicroseconds(std::chrono::milliseconds value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

[[nodiscard]] MetadataFieldSet mapFields(const std::optional<control::DisplayMetadata>& display) {
  MetadataFieldSet fields{};
  if (!display) {
    return fields;
  }

  fields.title = display->title;
  fields.artist = display->artist;
  fields.album = display->album;
  fields.albumArtist = display->albumArtist;
  fields.genre = display->genre;
  return fields;
}

[[nodiscard]] MetadataMprisSnapshotDto mapMprisSnapshot(const control::PlayerStateSnapshot& snapshot) {
  MetadataMprisSnapshotDto dto{};
  dto.trackObjectPath = snapshot.currentTrack ? MetadataTrackObjectPath{makeMprisTrackObjectPath(*snapshot.currentTrack)}
                                              : MetadataTrackObjectPath{makeMprisNoTrackObjectPath()};
  dto.fields = mapFields(snapshot.display);
  dto.positionMicros = toMicroseconds(snapshot.timeline.position);
  dto.durationMicros = snapshot.timeline.duration ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.duration)}
                                                  : std::nullopt;
  dto.bufferedMicros = snapshot.timeline.buffered ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.buffered)}
                                                  : std::nullopt;
  dto.seekableFromMicros = snapshot.timeline.seekableFrom
                               ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.seekableFrom)}
                               : std::nullopt;
  dto.seekableToMicros = snapshot.timeline.seekableTo
                             ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.seekableTo)}
                             : std::nullopt;
  dto.canPlay = snapshot.capabilities.canPlay;
  dto.canPause = snapshot.capabilities.canPause;
  dto.canStop = snapshot.capabilities.canStop;
  dto.canSeek = snapshot.capabilities.canSeek;
  dto.canSkipNext = snapshot.capabilities.canSkipNext;
  dto.canSkipPrevious = snapshot.capabilities.canSkipPrevious;
  dto.canSetRepeat = snapshot.capabilities.canSetRepeat;
  dto.canSetShuffle = snapshot.capabilities.canSetShuffle;
  dto.shuffle = snapshot.shuffle;
  dto.muted = snapshot.muted;
  dto.volume = snapshot.volume;
  return dto;
}

[[nodiscard]] MetadataWindowsSnapshotDto mapWindowsSnapshot(const control::PlayerStateSnapshot& snapshot) {
  MetadataWindowsSnapshotDto dto{};
  dto.fields = mapFields(snapshot.display);
  dto.positionMicros = toMicroseconds(snapshot.timeline.position);
  dto.durationMicros = snapshot.timeline.duration ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.duration)}
                                                  : std::nullopt;
  dto.bufferedMicros = snapshot.timeline.buffered ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.buffered)}
                                                  : std::nullopt;
  dto.seekableFromMicros = snapshot.timeline.seekableFrom
                               ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.seekableFrom)}
                               : std::nullopt;
  dto.seekableToMicros = snapshot.timeline.seekableTo
                             ? std::optional<std::int64_t>{toMicroseconds(*snapshot.timeline.seekableTo)}
                             : std::nullopt;
  dto.canPlay = snapshot.capabilities.canPlay;
  dto.canPause = snapshot.capabilities.canPause;
  dto.canStop = snapshot.capabilities.canStop;
  dto.canSeek = snapshot.capabilities.canSeek;
  dto.canSkipNext = snapshot.capabilities.canSkipNext;
  dto.canSkipPrevious = snapshot.capabilities.canSkipPrevious;
  dto.canSetRepeat = snapshot.capabilities.canSetRepeat;
  dto.canSetShuffle = snapshot.capabilities.canSetShuffle;
  dto.shuffle = snapshot.shuffle;
  return dto;
}

}

MetadataPlatformSnapshotDto mapPlayerStateSnapshot(const control::PlayerStateSnapshot& snapshot) {
  return MetadataPlatformSnapshotDto{.mpris = mapMprisSnapshot(snapshot), .windows = mapWindowsSnapshot(snapshot)};
}

std::string makeMprisTrackObjectPath(const control::TrackIdentity& track) {
  if (track.trackId.empty()) {
    return makeMprisNoTrackObjectPath();
  }

  return std::string{kMprisTrackObjectPathRoot} + "/" + encodeTrackId(track.trackId);
}

std::string makeMprisNoTrackObjectPath() {
  return std::string{kMprisNoTrackObjectPath};
}

}
