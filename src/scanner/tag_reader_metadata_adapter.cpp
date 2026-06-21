#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <Tag.hpp>
#include <TagReader.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::optional<std::uint32_t> nonZero(std::uint32_t value) {
  if (value == 0U) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<std::uint16_t> nonZero16(std::uint32_t value) {
  if (value == 0U) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::optional<std::uint64_t> fileSizeOf(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return std::nullopt;
  }
  return size;
}

[[nodiscard]] std::chrono::milliseconds toMilliseconds(std::chrono::microseconds value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value);
}

[[nodiscard]] cache::CachedUserStats userStatsFrom(const RawTagMetadata& raw) {
  cache::CachedUserStats stats{};
  stats.playCount = raw.playCount;
  if (raw.rating != 0U) {
    stats.rating = raw.rating;
  }
  if (raw.lastPlayed != std::chrono::system_clock::time_point{}) {
    stats.lastPlayed = raw.lastPlayed;
  }
  return stats;
}

[[nodiscard]] std::vector<LyricLine> mapLyrics(const std::vector<RawTagLyricLine>& rawLyrics) {
  std::vector<LyricLine> lines;
  lines.reserve(rawLyrics.size());
  for (const auto& lyric : rawLyrics) {
    lines.push_back({.timestamp = toMilliseconds(lyric.timestamp), .text = lyric.text});
  }
  return lines;
}

[[nodiscard]] RawTagMetadata rawFromMusicTag(const MusicTag& tag) {
  RawTagMetadata raw{};
  raw.title = tag.title();
  raw.genre = tag.genre();
  raw.artist = tag.artist();
  raw.album = tag.album();
  raw.albumArtist = tag.albumArtist();
  raw.composer = tag.composer();
  raw.year = tag.year();
  raw.trackNumber = tag.trackNumber();
  raw.discNumber = tag.discNumber();
  raw.filePath = tag.filePath();
  raw.coverPath = tag.coverPath();
  raw.duration = std::chrono::microseconds{tag.duration()};
  raw.offset = std::chrono::microseconds{tag.offset()};
  raw.lastModified = tag.lastModified();
  raw.sampleRate = tag.sampleRate();
  raw.bitDepth = tag.bitDepth();
  raw.bitRate = tag.bitRate();
  raw.channels = tag.channels();
  raw.format = tag.format();
  raw.playCount = tag.playCount();
  raw.rating = tag.rating();
  raw.lastPlayed = tag.lastPlayed();
  for (const auto& lyric : tag.lyrics().lyrics()) {
    raw.embeddedLyrics.push_back({.timestamp = lyric.timestamp(), .text = std::string{lyric.text()}});
  }
  return raw;
}

[[nodiscard]] ScannerError metadataReadError(const std::filesystem::path& path, const std::exception& error) {
  return {.code = ScannerErrorCode::MetadataReadFailed,
          .message = "TagReader metadata read failed",
          .detail = error.what(),
          .path = path};
}

}

RawTagMetadata ProductionTagMetadataReader::read(const std::filesystem::path& path,
                                                const std::filesystem::path& coverExportDir) {
  return rawFromMusicTag(TagReader::Read(path, coverExportDir));
}

MappedTagMetadata mapRawTagMetadata(const RawTagMetadata& raw,
                                    std::string contentHash,
                                    std::optional<cache::CachedUserStats> cachedUserStats,
                                    bool externalLyricsOverrideActive) {
  MappedTagMetadata result{};
  auto& cached = result.cachedSong;
  auto& metadata = cached.metadata;
  metadata.trackId = raw.filePath.generic_string();
  metadata.filePath = raw.filePath;
  metadata.title = raw.title;
  metadata.artist = raw.artist;
  metadata.album = raw.album;
  metadata.albumArtist = raw.albumArtist;
  metadata.genre = raw.genre;
  metadata.trackNumber = nonZero(raw.trackNumber);
  metadata.discNumber = nonZero(raw.discNumber);
  metadata.year = nonZero(raw.year);
  metadata.sampleRate = nonZero(raw.sampleRate);
  metadata.bitDepth = nonZero16(raw.bitDepth);
  metadata.channels = nonZero16(raw.channels);
  metadata.fileSizeBytes = fileSizeOf(raw.filePath);
  metadata.fileMtime = raw.lastModified;
  metadata.contentHash = std::move(contentHash);
  metadata.sourceFilePath = raw.filePath;
  metadata.offset = toMilliseconds(raw.offset);
  metadata.duration = toMilliseconds(raw.duration);
  metadata.logicalTrackId = raw.filePath.generic_string();
  cached.embeddedLyrics = mapLyrics(raw.embeddedLyrics);
  if (!externalLyricsOverrideActive && !cached.embeddedLyrics.empty()) {
    metadata.effectiveLyricsSource = LyricsSource::EmbeddedTag;
    metadata.effectiveLyrics = cached.embeddedLyrics;
  }
  cached.userStats = cachedUserStats.value_or(userStatsFrom(raw));
  result.composer = raw.composer;
  result.coverPath = raw.coverPath;
  result.bitRate = raw.bitRate;
  result.format = raw.format;
  return result;
}

std::vector<TagReaderSuccess> readTagMetadataBatch(TagMetadataReader& reader,
                                                  const std::vector<std::filesystem::path>& paths,
                                                  const std::filesystem::path& coverExportDir,
                                                  std::string_view contentHashSeed,
                                                  std::vector<TagReaderFailure>& failures) {
  std::vector<TagReaderSuccess> successes;
  successes.reserve(paths.size());
  for (const auto& path : paths) {
    try {
      auto raw = reader.read(path, coverExportDir);
      successes.push_back({.metadata = mapRawTagMetadata(raw, std::string{contentHashSeed} + ":" + path.generic_string(),
                                                         std::nullopt, false)});
    } catch (const std::exception& error) {
      failures.push_back({.error = metadataReadError(path, error)});
    }
  }
  return successes;
}

}
