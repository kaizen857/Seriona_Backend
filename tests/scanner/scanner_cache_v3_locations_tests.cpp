#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace seriona::scanner::cache {
namespace {

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] SongMetadata contentFixture(std::string contentId) {
  SongMetadata metadata{};
  metadata.trackId = std::move(contentId);
  metadata.title = "Song";
  metadata.artist = "Artist";
  metadata.album = "Album";
  metadata.albumArtist = "Album Artist";
  metadata.genre = "Genre";
  metadata.duration = std::chrono::milliseconds{180000};
  return metadata;
}

void execSql(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    const auto detail = std::string{message == nullptr ? sqlite3_errmsg(db) : message};
    sqlite3_free(message);
    throw std::runtime_error{detail};
  }
}

void seedRoot(const std::filesystem::path& dbPath, const std::string& rootPath) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(dbPath.generic_string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  execSql(db, "PRAGMA foreign_keys=ON;");
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, R"sql(
INSERT INTO scan_roots(root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms)
VALUES(?1, 'hash', 0, 'incremental', 0, 1);
)sql", -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(statement, 1, rootPath.c_str(), static_cast<int>(rootPath.size()), SQLITE_TRANSIENT) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(db);
}

void seedContentAndRoot(SQLiteCacheV3& cache, const std::filesystem::path& dbPath, const std::string& contentId,
                        const std::string& rootPath) {
  cache.upsertContent(contentId, contentFixture(contentId));
  seedRoot(dbPath, rootPath);
}

[[nodiscard]] CachedLocation locationFixture(std::string locationId, std::string contentId, std::string rootPath,
                                             std::string filePath) {
  return CachedLocation{.locationId = std::move(locationId),
                        .contentId = std::move(contentId),
                        .rootPath = std::move(rootPath),
                        .filePath = std::move(filePath),
                        .fileSizeBytes = 4096U,
                        .fileMtimeNs = 123456789,
                        .sourceFilePath = "disc.flac",
                        .cueTrackOffset = std::chrono::milliseconds{45000},
                        .artworkPath = std::filesystem::path{"cover.png"},
                        .lyricsSource = LyricsSource::ExternalLrc,
                        .externalLrcPath = std::filesystem::path{"song.lrc"},
                        .externalLrcMtimeNs = 987654321,
                        .discoveredAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{1000}},
                        .scannedAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{2000}}};
}

}

TEST_CASE("sqlite cache v3 upserts and loads a location") {
  test::TempScannerRoot temp{"scanner-cache-v3-location-insert"};
  auto cache = openCache(temp.dbPath());
  seedContentAndRoot(cache, temp.dbPath(), "content-1", "/music");

  cache.upsertLocation(locationFixture("location-1", "content-1", "/music", "/music/song.flac"));
  const auto loaded = cache.loadLocation("location-1");

  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-1");
  CHECK(loaded->filePath == std::filesystem::path{"/music/song.flac"});
  CHECK(loaded->fileSizeBytes == 4096U);
  CHECK(loaded->lyricsSource == LyricsSource::ExternalLrc);
  CHECK(loaded->cueTrackOffset == std::chrono::milliseconds{45000});
  CHECK(loaded->externalLrcMtimeNs == 987654321);
}

TEST_CASE("sqlite cache v3 loads locations by root path") {
  test::TempScannerRoot temp{"scanner-cache-v3-location-root"};
  auto cache = openCache(temp.dbPath());
  seedContentAndRoot(cache, temp.dbPath(), "content-2", "/music");
  seedRoot(temp.dbPath(), "/other");
  cache.upsertLocation(locationFixture("location-b", "content-2", "/music", "/music/b.flac"));
  cache.upsertLocation(locationFixture("location-a", "content-2", "/music", "/music/a.flac"));
  cache.upsertLocation(locationFixture("location-x", "content-2", "/other", "/other/x.flac"));

  const auto loaded = cache.loadLocationsByRoot("/music");

  REQUIRE(loaded.size() == 2U);
  CHECK(loaded[0].locationId == "location-a");
  CHECK(loaded[1].locationId == "location-b");
}

TEST_CASE("sqlite cache v3 prunes deleted locations by root") {
  test::TempScannerRoot temp{"scanner-cache-v3-location-prune"};
  auto cache = openCache(temp.dbPath());
  seedContentAndRoot(cache, temp.dbPath(), "content-3", "/music");
  seedRoot(temp.dbPath(), "/other");
  cache.upsertLocation(locationFixture("keep", "content-3", "/music", "/music/keep.flac"));
  cache.upsertLocation(locationFixture("drop", "content-3", "/music", "/music/drop.flac"));
  cache.upsertLocation(locationFixture("other", "content-3", "/other", "/other/keep.flac"));

  cache.pruneDeletedLocations("/music", {"keep"});

  CHECK(cache.loadLocation("keep").has_value());
  CHECK_FALSE(cache.loadLocation("drop").has_value());
  CHECK(cache.loadLocation("other").has_value());
}

TEST_CASE("sqlite cache v3 rejects locations with invalid content foreign key") {
  test::TempScannerRoot temp{"scanner-cache-v3-location-invalid-content"};
  auto cache = openCache(temp.dbPath());
  seedRoot(temp.dbPath(), "/music");

  CHECK_THROWS_AS(cache.upsertLocation(locationFixture("location-4", "missing", "/music", "/music/song.flac")), std::runtime_error);
}

TEST_CASE("sqlite cache v3 replaces a changed location id for the same file path") {
  test::TempScannerRoot temp{"scanner-cache-v3-location-replaced-path"};
  auto cache = openCache(temp.dbPath());
  seedContentAndRoot(cache, temp.dbPath(), "content-5", "/music");
  cache.upsertLocation(locationFixture("location-5a", "content-5", "/music", "/music/song.flac"));

  cache.upsertLocation(locationFixture("location-5b", "content-5", "/music", "/music/song.flac"));

  CHECK_FALSE(cache.loadLocation("location-5a").has_value());
  const auto loaded = cache.loadLocation("location-5b");
  REQUIRE(loaded.has_value());
  CHECK(loaded->filePath == std::filesystem::path{"/music/song.flac"});
}

}
