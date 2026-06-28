#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
#include "seriona/scanner/song_identity.h"

#include <doctest.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace seriona::scanner::cache {
namespace {

constexpr auto kRootPath = "/music";
constexpr auto kMovedPath = "/music/archive/disc-b.flac";
constexpr auto kDuration = std::chrono::milliseconds{180000};
constexpr auto kFileSize = std::uint64_t{8192};
constexpr auto kFileMtimeNs = std::int64_t{2000};

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

[[nodiscard]] std::filesystem::file_time_type nsToFileTime(const std::int64_t value) {
  return std::filesystem::file_time_type{std::chrono::nanoseconds{value}};
}

[[nodiscard]] std::filesystem::path backupPath(const std::filesystem::path& dbPath) {
  auto backup = dbPath;
  backup += ".bak";
  return backup;
}

void execSql(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    const auto detail = std::string{message == nullptr ? sqlite3_errmsg(db) : message};
    sqlite3_free(message);
    throw std::runtime_error{detail};
  }
}

class TestDb final {
public:
  explicit TestDb(const std::filesystem::path& path) {
    REQUIRE(sqlite3_open_v2(pathText(path).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
  }

  ~TestDb() { sqlite3_close(db_); }

  TestDb(const TestDb&) = delete;
  TestDb& operator=(const TestDb&) = delete;

  [[nodiscard]] sqlite3* get() const noexcept { return db_; }

private:
  sqlite3* db_{};
};

class Statement final {
public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    REQUIRE(sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) == SQLITE_OK);
  }

  ~Statement() { sqlite3_finalize(statement_); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(const int index, const std::string& value) {
    REQUIRE(sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK);
  }

  void bind(const int index, const std::int64_t value) { REQUIRE(sqlite3_bind_int64(statement_, index, value) == SQLITE_OK); }
  void bindNull(const int index) { REQUIRE(sqlite3_bind_null(statement_, index) == SQLITE_OK); }
  void stepDone() { REQUIRE(sqlite3_step(statement_) == SQLITE_DONE); }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

void createV2Schema(sqlite3* db) {
  execSql(db, R"sql(
CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE roots(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, directory_hash TEXT NOT NULL, updated_at_ms INTEGER NOT NULL);
CREATE TABLE directories(root_id INTEGER NOT NULL, relative_path TEXT NOT NULL, directory_hash TEXT NOT NULL, mtime_ns INTEGER,
  PRIMARY KEY(root_id, relative_path), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE TABLE songs(id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL, track_id TEXT NOT NULL, file_path TEXT NOT NULL,
  title TEXT NOT NULL, artist TEXT NOT NULL, album TEXT NOT NULL, album_artist TEXT NOT NULL, genre TEXT NOT NULL,
  track_number INTEGER, disc_number INTEGER, year INTEGER, sample_rate INTEGER, bit_depth INTEGER, channels INTEGER,
  file_size_bytes INTEGER, file_mtime_ns INTEGER, content_hash TEXT NOT NULL, lyrics_source TEXT NOT NULL,
  external_lrc_path TEXT, external_lrc_hash TEXT, external_lrc_mtime_ns INTEGER, source_file_path TEXT NOT NULL,
  offset_ms INTEGER, duration_ms INTEGER, logical_track_id TEXT NOT NULL, artwork_path TEXT, play_count INTEGER NOT NULL DEFAULT 0,
  rating INTEGER, last_played_ms INTEGER, UNIQUE(root_id, track_id), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE TABLE lyrics(song_id INTEGER NOT NULL, kind TEXT NOT NULL, line_index INTEGER NOT NULL, timestamp_ms INTEGER NOT NULL, text TEXT NOT NULL,
  PRIMARY KEY(song_id, kind, line_index), FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE);
CREATE TABLE errors(root_id INTEGER NOT NULL, error_index INTEGER NOT NULL, code TEXT NOT NULL, message TEXT NOT NULL, detail TEXT NOT NULL, path TEXT,
  PRIMARY KEY(root_id, error_index), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE INDEX idx_songs_root_file ON songs(root_id, file_path);
CREATE INDEX idx_lyrics_song_kind ON lyrics(song_id, kind);
INSERT INTO schema_meta(key, value) VALUES('schema_version', '2');
PRAGMA user_version=2;
)sql");
}

void seedRoot(sqlite3* db) {
  Statement insert{db, "INSERT INTO roots(id, path, directory_hash, updated_at_ms) VALUES(1, ?1, 'tree-hash', 7000);"};
  insert.bind(1, std::string{kRootPath});
  insert.stepDone();
}

void bindOptionalInt64(Statement& statement, const int index, const std::optional<std::int64_t> value) {
  value.has_value() ? statement.bind(index, *value) : statement.bindNull(index);
}

void seedSong(sqlite3* db, const std::string& trackId, const std::string& filePath, const std::int64_t fileSize,
              const std::int64_t fileMtimeNs, const std::uint64_t playCount,
              const std::optional<std::int64_t> lastPlayedMs) {
  Statement insert{db, R"sql(
INSERT INTO songs(root_id, track_id, file_path, title, artist, album, album_artist, genre, track_number, disc_number, year,
  sample_rate, bit_depth, channels, file_size_bytes, file_mtime_ns, content_hash, lyrics_source, external_lrc_path,
  external_lrc_hash, external_lrc_mtime_ns, source_file_path, offset_ms, duration_ms, logical_track_id, artwork_path,
  play_count, rating, last_played_ms)
VALUES(1, ?1, ?2, 'Shared Song', 'Artist', 'Album', 'Album Artist', 'Genre', 1, 1, 2026, 48000, 24, 2,
  ?3, ?4, 'content-hash', 'external', 'song.lrc', 'lrc-hash', 456, ?2, NULL, 180000, ?1, 'cover.png', ?5, 4, ?6);
)sql"};
  insert.bind(1, trackId);
  insert.bind(2, filePath);
  insert.bind(3, fileSize);
  insert.bind(4, fileMtimeNs);
  insert.bind(5, static_cast<std::int64_t>(playCount));
  bindOptionalInt64(insert, 6, lastPlayedMs);
  insert.stepDone();
}

void seedV2Cache(const std::filesystem::path& dbPath) {
  TestDb db{dbPath};
  execSql(db.get(), "PRAGMA foreign_keys=ON;");
  createV2Schema(db.get());
  seedRoot(db.get());
  seedSong(db.get(), "track-a", "/music/disc-a.flac", 4096, 1000, 3U, 1111);
  seedSong(db.get(), "track-b", "/music/disc-b.flac", static_cast<std::int64_t>(kFileSize), kFileMtimeNs, 7U, 2222);
}

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] CachedLocation movedLocation(const std::string& contentId) {
  return {.locationId = computeLocationId(kMovedPath, kFileSize, nsToFileTime(kFileMtimeNs)),
          .contentId = contentId,
          .rootPath = kRootPath,
          .filePath = kMovedPath,
          .fileSizeBytes = kFileSize,
          .fileMtimeNs = kFileMtimeNs,
          .sourceFilePath = kMovedPath,
          .cueTrackOffset = std::nullopt,
          .artworkPath = std::filesystem::path{"cover.png"},
          .lyricsSource = LyricsSource::ExternalLrc,
          .externalLrcPath = std::filesystem::path{"song.lrc"},
          .externalLrcMtimeNs = 456,
          .discoveredAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{8000}},
          .scannedAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{9000}}};
}

}

TEST_CASE("sqlite cache phase 1 migrates v2 and preserves stats when file location changes") {
  test::TempScannerRoot temp{"scanner-phase1-integration"};
  seedV2Cache(temp.dbPath());

  auto cache = openCache(temp.dbPath());
  const auto contentId = computeContentId(kDuration, "Shared Song", "Artist");
  const auto oldLocationId = computeLocationId("/music/disc-b.flac", kFileSize, nsToFileTime(kFileMtimeNs));

  REQUIRE(cache.schemaVersion() == 3);
  CHECK(std::filesystem::exists(backupPath(temp.dbPath())));
  REQUIRE(cache.loadContent(contentId).has_value());
  CHECK(cache.loadContent(contentId)->userStats.playCount == 7U);
  CHECK(cache.loadContent(contentId)->userStats.rating == 4U);
  CHECK(cache.loadContent(contentId)->userStats.lastPlayed == std::chrono::system_clock::time_point{std::chrono::milliseconds{2222}});
  REQUIRE(cache.loadLocation(oldLocationId).has_value());
  CHECK(cache.loadLocationsByRoot(kRootPath).size() == 2U);

  const auto moved = movedLocation(contentId);
  REQUIRE(moved.locationId != oldLocationId);
  cache.upsertLocation(moved);
  cache.pruneDeletedLocations(kRootPath, {moved.locationId});

  const auto movedLoaded = cache.loadLocation(moved.locationId);
  const auto contentLoaded = cache.loadContent(contentId);
  REQUIRE(movedLoaded.has_value());
  REQUIRE(contentLoaded.has_value());
  CHECK_FALSE(cache.loadLocation(oldLocationId).has_value());
  CHECK(movedLoaded->contentId == contentId);
  CHECK(movedLoaded->filePath == std::filesystem::path{kMovedPath});
  CHECK(contentLoaded->userStats.playCount == 7U);
  CHECK(contentLoaded->userStats.rating == 4U);
  CHECK(contentLoaded->userStats.lastPlayed == std::chrono::system_clock::time_point{std::chrono::milliseconds{2222}});
  CHECK(cache.loadLyrics(moved.locationId, "external").empty());
  cache.replaceLyrics(moved.locationId, "external", {{std::chrono::milliseconds{10}, "line"}});
  CHECK(cache.loadLyrics(moved.locationId, "external").size() == 1U);
}

}
