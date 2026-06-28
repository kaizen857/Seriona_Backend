#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace seriona::scanner::cache {
namespace {

constexpr auto kRootPath = "/music";

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

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

  void stepDone() { REQUIRE(sqlite3_step(statement_) == SQLITE_DONE); }

  [[nodiscard]] bool stepRow() {
    const auto result = sqlite3_step(statement_);
    REQUIRE((result == SQLITE_ROW || result == SQLITE_DONE));
    return result == SQLITE_ROW;
  }

  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }

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
  rating INTEGER, last_played_ms, UNIQUE(root_id, track_id), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE TABLE lyrics(song_id INTEGER NOT NULL, kind TEXT NOT NULL, line_index INTEGER NOT NULL, timestamp_ms INTEGER NOT NULL, text TEXT NOT NULL,
  PRIMARY KEY(song_id, kind, line_index), FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE);
CREATE TABLE errors(root_id INTEGER NOT NULL, error_index INTEGER NOT NULL, code TEXT NOT NULL, message TEXT NOT NULL, detail TEXT NOT NULL, path TEXT,
  PRIMARY KEY(root_id, error_index), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
INSERT INTO schema_meta(key, value) VALUES('schema_version', '2');
PRAGMA user_version=2;
)sql");
}

void seedRoot(sqlite3* db) {
  Statement insert{db, "INSERT INTO roots(id, path, directory_hash, updated_at_ms) VALUES(1, ?1, 'tree-hash', 7000);"};
  insert.bind(1, std::string{kRootPath});
  insert.stepDone();
}

void seedSong(sqlite3* db, const std::string& trackId, const std::string& filePath) {
  Statement insert{db, R"sql(
INSERT INTO songs(root_id, track_id, file_path, title, artist, album, album_artist, genre, file_size_bytes,
  file_mtime_ns, content_hash, lyrics_source, source_file_path, duration_ms, logical_track_id)
VALUES(1, ?1, ?2, 'Song', 'Artist', 'Album', 'Album Artist', 'Genre', 4096, 1000, 'hash', 'none', ?2, 180000, ?1);
)sql"};
  insert.bind(1, trackId);
  insert.bind(2, filePath);
  insert.stepDone();
}

void seedV2Cache(const std::filesystem::path& dbPath, const bool duplicateLocation) {
  TestDb db{dbPath};
  execSql(db.get(), "PRAGMA foreign_keys=ON;");
  createV2Schema(db.get());
  seedRoot(db.get());
  seedSong(db.get(), "track-a", "/music/a.flac");
  seedSong(db.get(), "track-b", duplicateLocation ? "/music/a.flac" : "/music/b.flac");
}

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] std::int64_t scalarInt(const std::filesystem::path& dbPath, const char* sql) {
  TestDb db{dbPath};
  Statement statement{db.get(), sql};
  return statement.stepRow() ? statement.int64Column(0) : 0;
}

[[nodiscard]] bool tableExists(const std::filesystem::path& dbPath, const std::string& tableName) {
  TestDb db{dbPath};
  Statement statement{db.get(), "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;"};
  statement.bind(1, tableName);
  return statement.stepRow();
}

}

TEST_CASE("sqlite cache v3 creates backup before v2 migration") {
  test::TempScannerRoot temp{"scanner-backup-created"};
  seedV2Cache(temp.dbPath(), false);

  auto cache = openCache(temp.dbPath());

  CHECK(cache.schemaVersion() == 3);
  CHECK(std::filesystem::exists(backupPath(temp.dbPath())));
  CHECK(scalarInt(backupPath(temp.dbPath()), "PRAGMA user_version;") == 2);
}

TEST_CASE("sqlite cache v3 restores v2 database when migration fails") {
  test::TempScannerRoot temp{"scanner-backup-failed-migration"};
  seedV2Cache(temp.dbPath(), true);

  CHECK_THROWS_AS(([] (const std::filesystem::path& dbPath) { auto cache = openCache(dbPath); }(temp.dbPath())), std::runtime_error);

  CHECK(std::filesystem::exists(backupPath(temp.dbPath())));
  CHECK(scalarInt(temp.dbPath(), "PRAGMA user_version;") == 2);
  CHECK(tableExists(temp.dbPath(), "songs"));
  CHECK_FALSE(tableExists(temp.dbPath(), "content"));
}

TEST_CASE("sqlite cache v3 manual rollback restores backup") {
  test::TempScannerRoot temp{"scanner-backup-manual-rollback"};
  seedV2Cache(temp.dbPath(), false);
  auto cache = openCache(temp.dbPath());
  REQUIRE(cache.schemaVersion() == 3);

  cache.rollbackToBackup();

  CHECK(cache.schemaVersion() == 2);
  CHECK(tableExists(temp.dbPath(), "songs"));
  CHECK_FALSE(tableExists(temp.dbPath(), "content"));
}

TEST_CASE("sqlite cache v3 backup creation failure prevents migration start") {
  test::TempScannerRoot temp{"scanner-backup-copy-failure"};
  seedV2Cache(temp.dbPath(), false);
  std::filesystem::create_directory(backupPath(temp.dbPath()));

  CHECK_THROWS_AS(([] (const std::filesystem::path& dbPath) { auto cache = openCache(dbPath); }(temp.dbPath())), std::runtime_error);

  CHECK(scalarInt(temp.dbPath(), "PRAGMA user_version;") == 2);
  CHECK(tableExists(temp.dbPath(), "songs"));
  CHECK_FALSE(tableExists(temp.dbPath(), "content"));
}

}
