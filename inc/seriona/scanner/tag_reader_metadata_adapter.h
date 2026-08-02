#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <TagReader.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::scanner {

// Internal metadata-read request; options pass through to the TagReader overload verbatim.
struct TagReadRequest {
  std::filesystem::path path;
  std::filesystem::path coverExportDir;
  CoverProcessingOptions options{};
};

// Scanner-wide cover policy: thumbnail PNGs only; cover failures never remove songs/lyrics/CUE tracks.
[[nodiscard]] inline TagReadRequest thumbnailOnlyRequest(std::filesystem::path path, std::filesystem::path coverExportDir) {
  CoverProcessingOptions options;
  options.mode = CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly;
  options.failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Ignore;
  return TagReadRequest{.path = std::move(path),
                        .coverExportDir = std::move(coverExportDir),
                        .options = options};
}

struct RawTagLyricLine {
  std::chrono::microseconds timestamp{0};
  std::string text;
};

struct RawTagMetadata {
  std::string title;
  std::string genre;
  std::string artist;
  std::string album;
  std::string albumArtist;
  std::string composer;
  std::uint16_t year{0};
  std::uint16_t trackNumber{0};
  std::uint16_t discNumber{0};
  std::vector<RawTagLyricLine> embeddedLyrics;
  std::filesystem::path filePath;
  std::filesystem::path coverPath;
  std::filesystem::path thumbnailPath;
  std::chrono::microseconds duration{0};
  std::chrono::microseconds offset{0};
  std::filesystem::file_time_type lastModified{};
  std::uint32_t sampleRate{0};
  std::uint32_t bitDepth{0};
  std::uint32_t bitRate{0};
  std::uint8_t channels{0};
  std::string format;
  std::uint32_t playCount{0};
  std::uint8_t rating{0};
  std::chrono::system_clock::time_point lastPlayed{};
};

struct TagUserStats {
  std::uint64_t playCount{0};
  std::optional<std::uint32_t> rating;
  std::optional<std::chrono::system_clock::time_point> lastPlayed;
};

struct MappedTagMetadata {
  SongMetadata metadata;
  std::vector<LyricLine> embeddedLyrics;
  std::vector<LyricLine> externalLyrics;
  TagUserStats userStats;
  std::string composer;
  std::filesystem::path coverPath;
  std::uint32_t bitRate{0};
  std::string format;
};

struct TagReaderSuccess {
  MappedTagMetadata metadata;
};

struct TagReaderFailure {
  ScannerError error;
};

class TagMetadataReader {
public:
  virtual ~TagMetadataReader() = default;
  [[nodiscard]] virtual RawTagMetadata read(const TagReadRequest& request) = 0;
  [[nodiscard]] virtual std::vector<RawTagMetadata> readCueSheet(const TagReadRequest& request) = 0;
};

class ProductionTagMetadataReader final : public TagMetadataReader {
public:
  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override;
  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest& request) override;
};

[[nodiscard]] MappedTagMetadata mapRawTagMetadata(const RawTagMetadata& raw,
                                                  std::string contentHash,
                                                  std::optional<TagUserStats> cachedUserStats,
                                                  bool externalLyricsOverrideActive);
[[nodiscard]] std::vector<TagReaderSuccess> readTagMetadataBatch(TagMetadataReader& reader,
                                                                 const std::vector<std::filesystem::path>& paths,
                                                                 const std::filesystem::path& coverExportDir,
                                                                 std::string_view contentHashSeed,
                                                                 std::vector<TagReaderFailure>& failures);
[[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const std::filesystem::path& cuePath,
                                                       const std::filesystem::path& coverExportDir);

}
