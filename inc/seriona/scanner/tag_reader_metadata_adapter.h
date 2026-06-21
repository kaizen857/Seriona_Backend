#pragma once

#include "seriona/scanner/cache/sqlite_scanner_cache.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::scanner {

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

struct MappedTagMetadata {
  cache::CachedSong cachedSong;
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
  [[nodiscard]] virtual RawTagMetadata read(const std::filesystem::path& path,
                                            const std::filesystem::path& coverExportDir) = 0;
};

class ProductionTagMetadataReader final : public TagMetadataReader {
public:
  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override;
};

[[nodiscard]] MappedTagMetadata mapRawTagMetadata(const RawTagMetadata& raw,
                                                  std::string contentHash,
                                                  std::optional<cache::CachedUserStats> cachedUserStats,
                                                  bool externalLyricsOverrideActive);
[[nodiscard]] std::vector<TagReaderSuccess> readTagMetadataBatch(TagMetadataReader& reader,
                                                                 const std::vector<std::filesystem::path>& paths,
                                                                 const std::filesystem::path& coverExportDir,
                                                                 std::string_view contentHashSeed,
                                                                 std::vector<TagReaderFailure>& failures);

}
