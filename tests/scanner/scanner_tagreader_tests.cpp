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
  const auto& song = mapped.cachedSong;

  CHECK(song.metadata.title == "Title");
  CHECK(song.metadata.genre == "Genre");
  CHECK(song.metadata.artist == "Artist");
  CHECK(song.metadata.album == "Album");
  CHECK(song.metadata.albumArtist == "Album Artist");
  CHECK(mapped.composer == "Composer");
  CHECK(song.metadata.year == 2026U);
  CHECK(song.metadata.trackNumber == 7U);
  CHECK(song.metadata.discNumber == 2U);
  CHECK(song.metadata.filePath == std::filesystem::path{"music/song.flac"});
  CHECK(mapped.coverPath == std::filesystem::path{"covers/song.png"});
  CHECK(song.metadata.duration == std::chrono::milliseconds{1234});
  CHECK(song.metadata.offset == std::chrono::milliseconds{9});
  CHECK(song.metadata.fileMtime == std::filesystem::file_time_type{std::chrono::nanoseconds{42}});
  CHECK(song.metadata.sampleRate == 48000U);
  CHECK(song.metadata.bitDepth == 24U);
  CHECK(mapped.bitRate == 320000U);
  CHECK(song.metadata.channels == 2U);
  CHECK(mapped.format == "flac");
  CHECK(song.metadata.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(song.embeddedLyrics.size() == 2U);
  CHECK(song.embeddedLyrics[0].timestamp == std::chrono::milliseconds{1});
  CHECK(song.embeddedLyrics[0].text == "embedded one");
  CHECK(song.metadata.effectiveLyrics[1].text == "embedded two");
  CHECK(song.userStats.playCount == 5U);
  CHECK(song.userStats.rating == 4U);
  CHECK(song.userStats.lastPlayed == std::chrono::system_clock::time_point{std::chrono::milliseconds{777}});
}

TEST_CASE("tagreader adapter preserves cached user stats and respects external lrc override") {
  cache::CachedUserStats cachedStats{};
  cachedStats.playCount = 99;
  cachedStats.rating = 1;
  cachedStats.lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{12345}};

  const auto mapped = mapRawTagMetadata(rawTagFixture(), "content-hash", cachedStats, true);

  CHECK(mapped.cachedSong.userStats.playCount == 99U);
  CHECK(mapped.cachedSong.userStats.rating == 1U);
  CHECK(mapped.cachedSong.userStats.lastPlayed == std::chrono::system_clock::time_point{std::chrono::milliseconds{12345}});
  CHECK(mapped.cachedSong.metadata.effectiveLyricsSource == LyricsSource::None);
  CHECK(mapped.cachedSong.metadata.effectiveLyrics.empty());
  REQUIRE(mapped.cachedSong.embeddedLyrics.size() == 2U);
}

TEST_CASE("tagreader batch reader captures per-file exceptions and continues") {
  FakeTagMetadataReader successReader{{rawTagFixture(), rawTagFixture()}};
  std::vector<TagReaderFailure> failures;

  const auto successes = readTagMetadataBatch(successReader, {"first.flac", "second.flac"}, "covers", "hash", failures);

  REQUIRE(successes.size() == 2U);
  CHECK(failures.empty());
  CHECK(successReader.requestedPaths[0] == std::filesystem::path{"first.flac"});
  CHECK(successReader.requestedCoverDirs[0] == std::filesystem::path{"covers"});
  CHECK(successes[0].metadata.cachedSong.metadata.contentHash == "hash:first.flac");

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
