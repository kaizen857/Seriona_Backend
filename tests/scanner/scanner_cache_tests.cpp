#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_scanner_cache.h"

#include <doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>

namespace seriona::scanner::cache {
namespace {

[[nodiscard]] std::filesystem::file_time_type fileTime(const std::int64_t ticks) {
  return std::filesystem::file_time_type{std::chrono::nanoseconds{ticks}};
}

[[nodiscard]] CachedSong songWithLyrics(std::string title, std::string trackId, std::string hash) {
  CachedSong song{};
  song.metadata.trackId = std::move(trackId);
  song.metadata.filePath = "artist/song.flac";
  song.metadata.title = std::move(title);
  song.metadata.artist = "Artist";
  song.metadata.album = "Album";
  song.metadata.albumArtist = "Album Artist";
  song.metadata.genre = "Genre";
  song.metadata.trackNumber = 7U;
  song.metadata.discNumber = 1U;
  song.metadata.year = 2026U;
  song.metadata.sampleRate = 48000U;
  song.metadata.bitDepth = 24U;
  song.metadata.channels = 2U;
  song.metadata.fileSizeBytes = 4096U;
  song.metadata.fileMtime = fileTime(1000);
  song.metadata.contentHash = std::move(hash);
  song.metadata.effectiveLyricsSource = LyricsSource::ExternalLrc;
  song.metadata.externalLyricsPath = "artist/song.lrc";
  song.metadata.externalLyricsHash = "lrc-hash-1";
  song.metadata.externalLyricsMtime = fileTime(2000);
  song.metadata.sourceFilePath = "artist/song.flac";
  song.metadata.offset = std::chrono::milliseconds{5};
  song.metadata.duration = std::chrono::milliseconds{180000};
  song.metadata.logicalTrackId = "artist/song.flac#main";
  song.metadata.artworkPath = "artist/covers/song.png";
  song.embeddedLyrics = {LyricLine{std::chrono::milliseconds{10}, "embedded one"},
                         LyricLine{std::chrono::milliseconds{20}, "embedded two"}};
  song.externalLyrics = {LyricLine{std::chrono::milliseconds{30}, "external one"},
                         LyricLine{std::chrono::milliseconds{40}, "external two"}};
  song.metadata.effectiveLyrics = song.externalLyrics;
  return song;
}

[[nodiscard]] CachedRoot rootFixture(const std::filesystem::path& rootPath) {
  CachedRoot root{};
  root.rootPath = rootPath;
  root.directoryHash = "root-hash";
  root.directories.push_back({.relativePath = "artist", .hash = "dir-hash", .mtime = fileTime(3000)});
  root.songs.push_back(songWithLyrics("Song", "track-1", "audio-hash-1"));
  root.errors.push_back({.code = ScannerErrorCode::MetadataReadFailed,
                         .message = "tag failed",
                         .detail = "fixture detail",
                         .path = std::filesystem::path{"artist/bad.flac"}});
  return root;
}

[[nodiscard]] SQLiteScannerCache openCache(const std::filesystem::path& dbPath) {
  return SQLiteScannerCache{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] SQLiteScannerCache openCacheWithPolicy(const std::filesystem::path& dbPath, CacheMaintenancePolicy policy) {
  return SQLiteScannerCache{ScannerCacheConfig{.databasePath = dbPath,
                                               .busyTimeout = std::chrono::milliseconds{25},
                                               .maintenancePolicy = policy}};
}

TEST_CASE("sqlite scanner cache migrates schema and enables WAL pragmas") {
  test::TempScannerRoot temp{"scanner-cache-migrate"};
  auto cache = openCache(temp.dbPath());

  CHECK(cache.schemaVersion() == 2);
  CHECK(cache.journalMode() == "wal");
}

TEST_CASE("sqlite scanner cache round-trips full metadata lyrics directories and errors") {
  test::TempScannerRoot temp{"scanner-cache-roundtrip"};
  auto cache = openCache(temp.dbPath());
  const auto expected = rootFixture(temp.path());

  cache.saveRoot(expected);
  const auto loaded = cache.loadRoot(temp.path());

  REQUIRE(loaded.has_value());
  CHECK(loaded->directoryHash == "root-hash");
  REQUIRE(loaded->directories.size() == 1U);
  CHECK(loaded->directories[0].relativePath == std::filesystem::path{"artist"});
  CHECK(loaded->directories[0].hash == "dir-hash");
  REQUIRE(loaded->songs.size() == 1U);
  const auto& song = loaded->songs[0];
  CHECK(song.metadata.title == "Song");
  CHECK(song.metadata.artist == "Artist");
  CHECK(song.metadata.albumArtist == "Album Artist");
  CHECK(song.metadata.trackNumber == 7U);
  CHECK(song.metadata.bitDepth == 24U);
  CHECK(song.metadata.fileSizeBytes == 4096U);
  CHECK(song.metadata.contentHash == "audio-hash-1");
  CHECK(song.metadata.artworkPath == std::filesystem::path{"artist/covers/song.png"});
  CHECK(song.metadata.effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(song.metadata.externalLyricsPath == std::filesystem::path{"artist/song.lrc"});
  CHECK(song.metadata.externalLyricsHash == "lrc-hash-1");
  REQUIRE(song.embeddedLyrics.size() == 2U);
  REQUIRE(song.externalLyrics.size() == 2U);
  CHECK(song.embeddedLyrics[0].text == "embedded one");
  CHECK(song.externalLyrics[0].text == "external one");
  CHECK(song.metadata.effectiveLyrics[0].text == "external one");
  REQUIRE(loaded->errors.size() == 1U);
  CHECK(loaded->errors[0].code == ScannerErrorCode::MetadataReadFailed);
  CHECK(loaded->errors[0].path == std::filesystem::path{"artist/bad.flac"});
}

TEST_CASE("sqlite scanner cache preserves embedded lyrics when external override changes or is deleted") {
  test::TempScannerRoot temp{"scanner-cache-lrc-state"};
  auto cache = openCache(temp.dbPath());
  auto root = rootFixture(temp.path());
  cache.saveRoot(root);

  root.songs[0].metadata.effectiveLyricsSource = LyricsSource::EmbeddedTag;
  root.songs[0].metadata.effectiveLyrics = root.songs[0].embeddedLyrics;
  root.songs[0].metadata.externalLyricsPath = std::nullopt;
  root.songs[0].metadata.externalLyricsHash = std::nullopt;
  root.songs[0].metadata.externalLyricsMtime = std::nullopt;
  root.songs[0].externalLyrics.clear();
  cache.saveRoot(root);
  const auto loaded = cache.loadRoot(temp.path());

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->songs.size() == 1U);
  CHECK(loaded->songs[0].metadata.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(loaded->songs[0].embeddedLyrics.size() == 2U);
  CHECK(loaded->songs[0].metadata.effectiveLyrics[0].text == "embedded one");
  CHECK(loaded->songs[0].externalLyrics.empty());
}

TEST_CASE("sqlite scanner cache updates hashes and preserves explicit user stats across refresh") {
  test::TempScannerRoot temp{"scanner-cache-stats"};
  auto cache = openCache(temp.dbPath());
  auto root = rootFixture(temp.path());
  cache.saveRoot(root);
  const auto lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{123456}};
  cache.updateUserStats(temp.path(), "track-1", {.playCount = 9U, .rating = 4U, .lastPlayed = lastPlayed});

  root.songs[0] = songWithLyrics("Song refreshed", "track-1", "audio-hash-2");
  root.songs[0].metadata.externalLyricsHash = "lrc-hash-2";
  root.songs[0].metadata.effectiveLyrics = root.songs[0].externalLyrics;
  cache.saveRoot(root);
  const auto loaded = cache.loadRoot(temp.path());

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->songs.size() == 1U);
  CHECK(loaded->songs[0].metadata.title == "Song refreshed");
  CHECK(loaded->songs[0].metadata.contentHash == "audio-hash-2");
  CHECK(loaded->songs[0].metadata.externalLyricsHash == "lrc-hash-2");
  CHECK(loaded->songs[0].userStats.playCount == 9U);
  CHECK(loaded->songs[0].userStats.rating == 4U);
  CHECK(loaded->songs[0].userStats.lastPlayed == lastPlayed);
}

TEST_CASE("sqlite scanner cache prunes deleted songs and checkpoints WAL") {
  test::TempScannerRoot temp{"scanner-cache-prune"};
  auto cache = openCache(temp.dbPath());
  auto root = rootFixture(temp.path());
  root.songs.push_back(songWithLyrics("Second", "track-2", "audio-hash-second"));
  root.songs[1].metadata.filePath = "artist/second.flac";
  cache.saveRoot(root);

  cache.pruneMissingSongs(temp.path(), {"track-2"});
  const auto checkpoint = cache.checkpointPassive();
  const auto loaded = cache.loadRoot(temp.path());

  CHECK(checkpoint.resultCode == SQLITE_OK);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->songs.size() == 1U);
  CHECK(loaded->songs[0].metadata.trackId == "track-2");
}

TEST_CASE("sqlite scanner cache saveRoot prunes songs absent from replacement root") {
  test::TempScannerRoot temp{"scanner-cache-save-prune"};
  auto cache = openCache(temp.dbPath());
  auto root = rootFixture(temp.path());
  root.songs.push_back(songWithLyrics("Second", "track-2", "audio-hash-second"));
  root.songs[1].metadata.filePath = "artist/second.flac";
  cache.saveRoot(root);

  root.songs.erase(root.songs.begin());
  cache.saveRoot(root);
  const auto loaded = cache.loadRoot(temp.path());

  REQUIRE(loaded.has_value());
  REQUIRE(loaded->songs.size() == 1U);
  CHECK(loaded->songs[0].metadata.trackId == "track-2");
}

TEST_CASE("sqlite scanner cache reports busy deterministically while writer transaction is held") {
  test::TempScannerRoot temp{"scanner-cache-busy"};
  auto first = openCache(temp.dbPath());
  auto second = openCache(temp.dbPath());
  auto transaction = first.beginWriter();

  CHECK_THROWS_AS(second.saveRoot(rootFixture(temp.path())), std::runtime_error);

  transaction.commit();
}

TEST_CASE("sqlite scanner cache reports capacity and checkpoint policy decisions without large fixtures") {
  test::TempScannerRoot temp{"scanner-cache-policy"};
  auto cache = openCacheWithPolicy(temp.dbPath(), CacheMaintenancePolicy{.softDatabaseBytes = 0,
                                                                         .hardDatabaseBytes = 1,
                                                                         .passiveCheckpointWalBytes = 0,
                                                                         .maxCachedRoots = 8});
  cache.saveRoot(rootFixture(temp.path()));

  const auto decision = cache.maintenanceDecision();

  CHECK(decision.databaseBytes > 0U);
  CHECK(decision.checkpointRecommended);
  CHECK(decision.cleanupRecommended);
  CHECK(decision.vacuumRecommended);
}

TEST_CASE("sqlite scanner cache maintenance checkpoints and prunes roots by policy") {
  test::TempScannerRoot first{"scanner-cache-policy-first"};
  test::TempScannerRoot second{"scanner-cache-policy-second"};
  const auto dbPath = first.dbPath();
  auto cache = openCacheWithPolicy(dbPath, CacheMaintenancePolicy{.softDatabaseBytes = 0,
                                                                  .hardDatabaseBytes = 1,
                                                                  .passiveCheckpointWalBytes = 0,
                                                                  .maxCachedRoots = 1});
  cache.saveRoot(rootFixture(first.path()));
  cache.saveRoot(rootFixture(second.path()));

  const auto result = cache.maintainCache();

  CHECK(result.checkpoint.has_value());
  CHECK(result.checkpoint->resultCode == SQLITE_OK);
  CHECK(result.rootsRemoved == 1U);
  CHECK_FALSE(cache.loadRoot(first.path()).has_value());
  CHECK(cache.loadRoot(second.path()).has_value());
}

}
}
