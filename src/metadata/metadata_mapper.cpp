#include "metadata_mapper.h"

#include "spdlog/spdlog.h"

#include <chrono>
#include <filesystem>
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

[[nodiscard]] std::string fileUriFromPath(const std::filesystem::path& path) {
  // generic_string() 在 Windows 按 CP_ACP 转换（不可表示字符抛异常）；generic_u8string()
  // 恒为 UTF-8，Qt 的 file:// 解析按 UTF-8 解码，POSIX 上字节级不变。
  const auto utf8 = path.generic_u8string();
  return std::string{"file://"} + std::string{utf8.begin(), utf8.end()};
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

[[nodiscard]] std::optional<std::uint16_t> mapTrackNumber(const std::optional<control::TrackIdentity>& track) {
  if (!track) {
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] MetadataTrackIdentityDto mapTrackIdentity(const std::optional<control::TrackIdentity>& track) {
  MetadataTrackIdentityDto dto{};
  if (!track) {
    return dto;
  }

  dto.trackId = track->trackId;
  dto.filePath = track->filePath;
  dto.fileUri = fileUriFromPath(track->filePath);
  dto.sourceId = track->sourceId;
  dto.libraryId = track->libraryId;
  dto.trackNumber = mapTrackNumber(track);
  return dto;
}

[[nodiscard]] MetadataArtworkRefDto mapArtwork(const std::optional<control::ArtworkRef>& artwork) {
  MetadataArtworkRefDto dto{};
  if (!artwork) {
    return dto;
  }

  dto.localPath = artwork->localPath;
  dto.uri = artwork->uri;
  dto.contentHash = artwork->contentHash;
  return dto;
}

[[nodiscard]] MetadataCapabilitySetDto mapCapabilities(const control::PlaybackCapabilities& capabilities) {
  return MetadataCapabilitySetDto{.canPlay = capabilities.canPlay,
                                  .canPause = capabilities.canPause,
                                  .canStop = capabilities.canStop,
                                  .canSeek = capabilities.canSeek,
                                  .canSkipNext = capabilities.canSkipNext,
                                  .canSkipPrevious = capabilities.canSkipPrevious,
                                  .canSetRepeat = capabilities.canSetRepeat,
                                  .canSetShuffle = capabilities.canSetShuffle,
                                  .canSetVolume = capabilities.canSetVolume};
}

[[nodiscard]] MetadataMprisSnapshotDto mapMprisSnapshot(const control::PlayerStateSnapshot& snapshot) {
  MetadataMprisSnapshotDto dto{};
  dto.track = mapTrackIdentity(snapshot.currentTrack);
  dto.trackObjectPath = snapshot.currentTrack ? MetadataTrackObjectPathDto{makeMprisTrackObjectPath(*snapshot.currentTrack)}
                                              : MetadataTrackObjectPathDto{makeMprisNoTrackObjectPath()};
  dto.artwork = mapArtwork(snapshot.artwork);
  dto.fields = mapFields(snapshot.display);
  dto.playbackStatus = snapshot.playback.state;
  dto.repeatMode = snapshot.repeatMode;
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
  dto.capabilities = mapCapabilities(snapshot.capabilities);
  dto.shuffle = snapshot.shuffle;
  dto.muted = snapshot.muted;
  dto.volume = snapshot.volume;
  return dto;
}

[[nodiscard]] MetadataWindowsSnapshotDto mapWindowsSnapshot(const control::PlayerStateSnapshot& snapshot) {
  MetadataWindowsSnapshotDto dto{};
  dto.track = mapTrackIdentity(snapshot.currentTrack);
  dto.artwork = mapArtwork(snapshot.artwork);
  dto.fields = mapFields(snapshot.display);
  dto.playbackStatus = snapshot.playback.state;
  dto.repeatMode = snapshot.repeatMode;
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
  dto.capabilities = mapCapabilities(snapshot.capabilities);
  dto.capabilities.canSetVolume = false;
  dto.shuffle = snapshot.shuffle;
  return dto;
}

}

MetadataPlatformSnapshotDto mapPlayerStateSnapshot(const control::PlayerStateSnapshot& snapshot) {
  spdlog::debug("metadata mapper: mapping player state snapshot");
  auto result = MetadataPlatformSnapshotDto{.mpris = mapMprisSnapshot(snapshot), .windows = mapWindowsSnapshot(snapshot)};
  spdlog::debug("metadata mapper: snapshot mapped (track={})",
                result.mpris.track.trackId.empty() ? "none" : result.mpris.track.trackId);
  return result;
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
