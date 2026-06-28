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

namespace seriona::scanner::cache {
namespace {

constexpr auto kRootPath = "/music";

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

[[nodiscard]] std::filesystem::file_time_type nsToFileTime(const std::int64_t value) {
  return std::filesystem::file_time_type{std::chrono::nanoseconds{value}};
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

  [[nodiscard]] bool stepRow() {
    const auto result = sqlite3_step(statement_);
    REQUIRE((result == SQLITE_ROW || result == SQLITE_DONE));
    return result == SQLITE_ROW;
  }

  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }
  [[nodiscard]] std::string textColumn(const int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
  }

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
  seedSong(db.get(), "track-b", "/music/disc-b.flac", 8192, 2000, 7U, 2222);
}

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] std::int64_t scalarInt(sqlite3* db, const char* sql) {
  Statement statement{db, sql};
  return statement.stepRow() ? statement.int64Column(0) : 0;
}

[[nodiscard]] bool tableExists(sqlite3* db, const std::string& tableName) {
  Statement statement{db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;"};
  statement.bind(1, tableName);
  return statement.stepRow();
}

}

TEST_CASE("sqlite cache v3 auto migrates a v2 database") {
  test::TempScannerRoot temp{"scanner-migration-v2-basic"};
  seedV2Cache(temp.dbPath());

  auto cache = openCache(temp.dbPath());

  CHECK(cache.schemaVersion() == 3);
  CHECK(cache.loadScanRoot(kRootPath).has_value());
  CHECK(cache.loadLocationsByRoot(kRootPath).size() == 2U);
}

TEST_CASE("sqlite cache v3 migration preserves merged user stats") {
  test::TempScannerRoot temp{"scanner-migration-v2-stats"};
  seedV2Cache(temp.dbPath());

  auto cache = openCache(temp.dbPath());
  const auto contentId = computeContentId(std::chrono::milliseconds{180000}, "Shared Song", "Artist");
  const auto loaded = cache.loadContent(contentId);

  REQUIRE(loaded.has_value());
  CHECK(loaded->userStats.playCount == 7U);
  CHECK(loaded->userStats.rating == 4U);
  CHECK(loaded->userStats.lastPlayed == std::chrono::system_clock::time_point{std::chrono::milliseconds{2222}});
}

TEST_CASE("sqlite cache v3 migration collapses duplicate content into multiple locations") {
  test::TempScannerRoot temp{"scanner-migration-v2-duplicates"};
  seedV2Cache(temp.dbPath());

  auto cache = openCache(temp.dbPath());
  const auto contentId = computeContentId(std::chrono::milliseconds{180000}, "Shared Song", "Artist");
  const auto locations = cache.loadLocationsByRoot(kRootPath);

  REQUIRE(locations.size() == 2U);
  CHECK(cache.loadContent(contentId).has_value());
  CHECK(locations[0].contentId == contentId);
  CHECK(locations[1].contentId == contentId);
  CHECK(locations[0].locationId == computeLocationId(locations[0].filePath, locations[0].fileSizeBytes, nsToFileTime(locations[0].fileMtimeNs)));
}

TEST_CASE("sqlite cache v3 migration removes v2 tables after success") {
  test::TempScannerRoot temp{"scanner-migration-v2-cleanup"};
  seedV2Cache(temp.dbPath());

  auto cache = openCache(temp.dbPath());
  static_cast<void>(cache);
  TestDb db{temp.dbPath()};

  CHECK(scalarInt(db.get(), "PRAGMA user_version;") == 3);
  CHECK(tableExists(db.get(), "content"));
  CHECK_FALSE(tableExists(db.get(), "songs"));
  CHECK_FALSE(tableExists(db.get(), "roots"));
  CHECK_FALSE(tableExists(db.get(), "directories"));
  CHECK_FALSE(tableExists(db.get(), "errors"));
  CHECK_FALSE(tableExists(db.get(), "schema_meta"));
}

}
