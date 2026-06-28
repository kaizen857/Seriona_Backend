#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include "seriona/scanner/song_identity.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace seriona::scanner::cache {
namespace {

[[nodiscard]] sqlite3* asDb(void* db) noexcept { return static_cast<sqlite3*>(db); }

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

void closeConnection(void*& db) noexcept {
  if (db != nullptr) {
    sqlite3_close(asDb(db));
    db = nullptr;
  }
}

[[nodiscard]] std::runtime_error sqliteError(sqlite3* db, const std::string& action) {
  return std::runtime_error(action + ": " + sqlite3_errmsg(db));
}

void rollbackNoThrow(sqlite3* db) noexcept {
  char* message = nullptr;
  static_cast<void>(sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &message));
  sqlite3_free(message);
}

class Statement final {
public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw sqliteError(db_, "prepare statement");
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(const int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
      throw sqliteError(db_, "bind text");
    }
  }
  void bind(const int index, const std::int64_t value) { if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) { throw sqliteError(db_, "bind int64"); } }
  void bindNull(const int index) { if (sqlite3_bind_null(statement_, index) != SQLITE_OK) { throw sqliteError(db_, "bind null"); } }
  void bindOptionalInt64(const int index, const std::optional<std::int64_t> value) { value.has_value() ? bind(index, *value) : bindNull(index); }
  void bindOptionalPath(const int index, const std::optional<std::filesystem::path>& value) { value.has_value() ? bind(index, pathText(*value)) : bindNull(index); }
  void stepDone() { if (sqlite3_step(statement_) != SQLITE_DONE) { throw sqliteError(db_, "step done"); } }
  [[nodiscard]] bool stepRow() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) { return true; }
    if (result == SQLITE_DONE) { return false; }
    throw sqliteError(db_, "step row");
  }
  [[nodiscard]] int columnType(const int index) const { return sqlite3_column_type(statement_, index); }
  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }
  [[nodiscard]] std::string textColumn(const int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
  }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

[[nodiscard]] std::optional<std::int64_t> optionalInt64(Statement& row, const int index) {
  return row.columnType(index) == SQLITE_INTEGER ? std::optional{row.int64Column(index)} : std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> optionalPath(Statement& row, const int index) {
  return row.columnType(index) == SQLITE_TEXT ? std::optional<std::filesystem::path>{row.textColumn(index)} : std::nullopt;
}

[[nodiscard]] std::string v3LyricsSource(const std::string& source) {
  if (source == "embedded") { return "embedded_tag"; }
  if (source == "external") { return "external_lrc"; }
  return "none";
}

}

std::filesystem::path SQLiteCacheV3::backupPath() const {
  auto backup = databasePath_;
  backup += ".bak";
  return backup;
}

void SQLiteCacheV3::createMigrationBackup() {
  closeConnection(db_);
  std::filesystem::copy_file(databasePath_, backupPath(), std::filesystem::copy_options::overwrite_existing);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(pathText(databasePath_).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    const std::string message = db == nullptr ? "failed to reopen scanner cache v3" : sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }
  db_ = db;
  configureConnection(asDb(db_), busyTimeout_);
}

void SQLiteCacheV3::restoreMigrationBackup() {
  closeConnection(db_);
  std::filesystem::copy_file(backupPath(), databasePath_, std::filesystem::copy_options::overwrite_existing);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(pathText(databasePath_).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    const std::string message = db == nullptr ? "failed to reopen restored scanner cache v3" : sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }
  db_ = db;
  configureConnection(asDb(db_), busyTimeout_);
}

void SQLiteCacheV3::rollbackToBackup() {
  std::scoped_lock lock{writerMutex_};
  restoreMigrationBackup();
}

void SQLiteCacheV3::migrateSchemaV2ToV3() {
  exec(asDb(db_), "BEGIN IMMEDIATE;");
  try {
    exec(asDb(db_), "ALTER TABLE lyrics RENAME TO lyrics_v2;");
    exec(asDb(db_), schemaV3Sql().c_str());
    exec(asDb(db_), R"sql(
INSERT INTO scan_roots(root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms)
SELECT roots.path, roots.directory_hash, COUNT(songs.id), 'incremental', 0, roots.updated_at_ms
FROM roots LEFT JOIN songs ON songs.root_id=roots.id GROUP BY roots.id;
)sql");

    Statement select{asDb(db_), R"sql(
SELECT roots.path, songs.file_path, songs.title, songs.artist, songs.album, songs.album_artist, songs.genre,
  songs.track_number, songs.disc_number, songs.year, songs.sample_rate, songs.bit_depth, songs.channels,
  songs.file_size_bytes, songs.file_mtime_ns, songs.lyrics_source, songs.external_lrc_path, songs.external_lrc_mtime_ns,
  songs.source_file_path, songs.offset_ms, songs.duration_ms, songs.artwork_path, songs.play_count, songs.rating,
  songs.last_played_ms FROM songs INNER JOIN roots ON roots.id=songs.root_id ORDER BY songs.id;
)sql"};
    while (select.stepRow()) {
      const auto filePath = select.textColumn(1);
      const auto duration = std::chrono::milliseconds{select.int64Column(20)};
      const auto fileSize = static_cast<std::uint64_t>(select.int64Column(13));
      const auto mtimeNs = select.int64Column(14);
      const auto contentId = ::seriona::scanner::computeContentId(duration, select.textColumn(2), select.textColumn(3));
      const auto locationId = ::seriona::scanner::computeLocationId(filePath, fileSize, std::filesystem::file_time_type{std::chrono::nanoseconds{mtimeNs}});

      Statement content{asDb(db_), R"sql(
INSERT INTO content(content_id, title, artist, album, album_artist, genre, track_number, disc_number, year, duration_ms,
  sample_rate, bit_depth, channels, play_count, rating, last_played_ms, created_at_ms, updated_at_ms)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?17)
ON CONFLICT(content_id) DO UPDATE SET play_count=max(content.play_count, excluded.play_count),
  rating=CASE WHEN excluded.rating IS NULL THEN content.rating WHEN content.rating IS NULL THEN excluded.rating ELSE max(content.rating, excluded.rating) END,
  last_played_ms=CASE WHEN excluded.last_played_ms IS NULL THEN content.last_played_ms WHEN content.last_played_ms IS NULL THEN excluded.last_played_ms ELSE max(content.last_played_ms, excluded.last_played_ms) END,
  updated_at_ms=max(content.updated_at_ms, excluded.updated_at_ms);
)sql"};
      content.bind(1, contentId); content.bind(2, select.textColumn(2)); content.bind(3, select.textColumn(3)); content.bind(4, select.textColumn(4));
      content.bind(5, select.textColumn(5)); content.bind(6, select.textColumn(6)); content.bindOptionalInt64(7, optionalInt64(select, 7)); content.bindOptionalInt64(8, optionalInt64(select, 8));
      content.bindOptionalInt64(9, optionalInt64(select, 9)); content.bind(10, duration.count()); content.bindOptionalInt64(11, optionalInt64(select, 10));
      content.bindOptionalInt64(12, optionalInt64(select, 11)); content.bindOptionalInt64(13, optionalInt64(select, 12)); content.bind(14, select.int64Column(22));
      content.bindOptionalInt64(15, optionalInt64(select, 23)); content.bindOptionalInt64(16, optionalInt64(select, 24)); content.bind(17, optionalInt64(select, 24).value_or(0)); content.stepDone();

      Statement location{asDb(db_), R"sql(
INSERT INTO locations(location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, source_file_path,
  cue_track_offset_ms, artwork_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, discovered_at_ms, scanned_at_ms)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, 0, ?13);
)sql"};
      location.bind(1, locationId); location.bind(2, contentId); location.bind(3, select.textColumn(0)); location.bind(4, filePath);
      location.bind(5, static_cast<std::int64_t>(fileSize)); location.bind(6, mtimeNs); location.bind(7, select.textColumn(18)); location.bindOptionalInt64(8, optionalInt64(select, 19));
      location.bindOptionalPath(9, optionalPath(select, 21)); location.bind(10, v3LyricsSource(select.textColumn(15))); location.bindOptionalPath(11, optionalPath(select, 16));
      location.bindOptionalInt64(12, optionalInt64(select, 17)); location.bind(13, optionalInt64(select, 24).value_or(0)); location.stepDone();
    }

    exec(asDb(db_), "DROP TABLE lyrics_v2; DROP TABLE songs; DROP TABLE directories; DROP TABLE errors; DROP TABLE roots; DROP TABLE schema_meta; PRAGMA user_version=3; COMMIT;");
  } catch (...) {
    rollbackNoThrow(asDb(db_));
    throw;
  }
}

}
