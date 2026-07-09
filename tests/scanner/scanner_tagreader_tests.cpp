#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace seriona::scanner {
namespace {

class FakeTagMetadataReader final : public TagMetadataReader {
public:
  explicit FakeTagMetadataReader(std::vector<RawTagMetadata> results) : results_(std::move(results)) {}

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override {
    requestedPaths.push_back(path);
    requestedCoverDirs.push_back(coverExportDir);
    if (throwOnRead) {
      throw std::runtime_error("fake tagreader failure");
    }
    if (next_ >= results_.size()) {
      throw std::runtime_error("fake tagreader exhausted");
    }
    auto result = results_[next_++];
    result.filePath = path;
    return result;
  }

  bool throwOnRead{false};
  std::vector<std::filesystem::path> requestedPaths;
  std::vector<std::filesystem::path> requestedCoverDirs;

private:
  std::vector<RawTagMetadata> results_;
  std::size_t next_{0};
};

[[nodiscard]] RawTagMetadata rawTagFixture() {
  RawTagMetadata raw{};
  raw.title = "Title";
  raw.genre = "Genre";
  raw.artist = "Artist";
  raw.album = "Album";
  raw.albumArtist = "Album Artist";
  raw.composer = "Composer";
  raw.year = 2026;
  raw.trackNumber = 7;
  raw.discNumber = 2;
  raw.embeddedLyrics = {RawTagLyricLine{std::chrono::microseconds{1500}, "embedded one"},
                        RawTagLyricLine{std::chrono::microseconds{2500}, "embedded two"}};
  raw.filePath = "music/song.flac";
  raw.coverPath = "covers/song.png";
  raw.thumbnailPath = "covers/thumbnails/song.png";
  raw.duration = std::chrono::microseconds{1234567};
  raw.offset = std::chrono::microseconds{9876};
  raw.lastModified = std::filesystem::file_time_type{std::chrono::nanoseconds{42}};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.bitRate = 320000;
  raw.channels = 2;
  raw.format = "flac";
  raw.playCount = 5;
  raw.rating = 4;
  raw.lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{777}};
  return raw;
}

TEST_CASE("tagreader adapter maps raw metadata lyrics technical fields and initial stats") {
  const auto mapped = mapRawTagMetadata(rawTagFixture(), "content-hash", std::nullopt, false);
  const auto& metadata = mapped.metadata;

  CHECK(metadata.title == "Title");
  CHECK(metadata.genre == "Genre");
  CHECK(metadata.artist == "Artist");
  CHECK(metadata.album == "Album");
  CHECK(metadata.albumArtist == "Album Artist");
  CHECK(mapped.composer == "Composer");
  CHECK(metadata.year == 2026U);
  CHECK(metadata.trackNumber == 7U);
  CHECK(metadata.discNumber == 2U);
  CHECK(metadata.filePath == std::filesystem::path{"music/song.flac"});
  CHECK(mapped.coverPath == std::filesystem::path{"covers/song.png"});
  CHECK(metadata.artworkPath == std::filesystem::path{"covers/song.png"});
  CHECK(metadata.thumbnailPath == std::filesystem::path{"covers/thumbnails/song.png"});
  CHECK(metadata.duration == std::chrono::milliseconds{1234});
  CHECK(metadata.offset == std::chrono::milliseconds{9});
  CHECK(metadata.fileMtime == std::filesystem::file_time_type{std::chrono::nanoseconds{42}});
  CHECK(metadata.sampleRate == 48000U);
  CHECK(metadata.bitDepth == 24U);
  CHECK(mapped.bitRate == 320000U);
  CHECK(metadata.channels == 2U);
  CHECK(mapped.format == "flac");
  CHECK(metadata.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(mapped.embeddedLyrics.size() == 2U);
  CHECK(mapped.embeddedLyrics[0].timestamp == std::chrono::milliseconds{1});
  CHECK(mapped.embeddedLyrics[0].text == "embedded one");
  CHECK(metadata.effectiveLyrics[1].text == "embedded two");
}

TEST_CASE("tagreader adapter preserves zero offset for plain files") {
  auto raw = rawTagFixture();
  raw.offset = std::chrono::microseconds{0};

  const auto mapped = mapRawTagMetadata(raw, "content-hash", std::nullopt, false);

  REQUIRE(mapped.metadata.offset.has_value());
  CHECK(*mapped.metadata.offset == std::chrono::milliseconds{0});
  CHECK(mapped.metadata.duration == std::chrono::milliseconds{1234});
}

TEST_CASE("tagreader adapter preserves cached user stats and respects external lrc override") {
  TagUserStats cachedStats{};
  cachedStats.playCount = 99;
  cachedStats.rating = 1;
  cachedStats.lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{12345}};

  const auto mapped = mapRawTagMetadata(rawTagFixture(), "content-hash", cachedStats, true);

  CHECK(mapped.metadata.effectiveLyricsSource == LyricsSource::None);
  CHECK(mapped.metadata.effectiveLyrics.empty());
  REQUIRE(mapped.embeddedLyrics.size() == 2U);
}

TEST_CASE("tagreader batch reader captures per-file exceptions and continues") {
  FakeTagMetadataReader successReader{{rawTagFixture(), rawTagFixture()}};
  std::vector<TagReaderFailure> failures;

  const auto successes = readTagMetadataBatch(successReader, {"first.flac", "second.flac"}, "covers", "hash", failures);

  REQUIRE(successes.size() == 2U);
  CHECK(failures.empty());
  CHECK(successReader.requestedPaths[0] == std::filesystem::path{"first.flac"});
  CHECK(successReader.requestedCoverDirs[0] == std::filesystem::path{"covers"});
  CHECK(successes[0].metadata.metadata.contentHash == "hash:first.flac");

  FakeTagMetadataReader failingReader{{rawTagFixture()}};
  failingReader.throwOnRead = true;
  const auto failedSuccesses = readTagMetadataBatch(failingReader, {"broken.flac", "also-broken.flac"}, "covers", "hash", failures);

  CHECK(failedSuccesses.empty());
  REQUIRE(failures.size() == 2U);
  CHECK(failures[0].error.code == ScannerErrorCode::MetadataReadFailed);
  CHECK(failures[0].error.path == std::filesystem::path{"broken.flac"});
  CHECK(failures[1].error.path == std::filesystem::path{"also-broken.flac"});
}

}
}
