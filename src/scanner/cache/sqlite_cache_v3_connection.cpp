#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace seriona::scanner::cache {
namespace {

constexpr int kSchemaVersion = 3;

[[nodiscard]] sqlite3* asDb(void* db) noexcept { return static_cast<sqlite3*>(db); }

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

[[nodiscard]] std::runtime_error sqliteError(sqlite3* db, const std::string& action) {
  return std::runtime_error(action + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) { char* message = nullptr; if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) { std::string detail = message == nullptr ? sqlite3_errmsg(db) : message; sqlite3_free(message); throw std::runtime_error(detail); } }

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

  [[nodiscard]] bool stepRow() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    throw sqliteError(db_, "step row");
  }

  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }

  void bind(const int index, const std::string& value) { if (sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) { throw sqliteError(db_, "bind text"); } }
  void bind(const int index, const std::int64_t value) { if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) { throw sqliteError(db_, "bind int64"); } }
  void bindNull(const int index) { if (sqlite3_bind_null(statement_, index) != SQLITE_OK) { throw sqliteError(db_, "bind null"); } }
  void bindOptionalPath(const int index, const std::optional<std::filesystem::path>& value) { value.has_value() ? bind(index, pathText(*value)) : bindNull(index); }
  void stepDone() { if (sqlite3_step(statement_) != SQLITE_DONE) { throw sqliteError(db_, "step done"); } }
  [[nodiscard]] int columnType(const int index) const { return sqlite3_column_type(statement_, index); }

  [[nodiscard]] std::string textColumn(const int index) const { const auto* text = sqlite3_column_text(statement_, index); return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text); }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) { return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count(); }
[[nodiscard]] std::chrono::system_clock::time_point msToSystemTime(const std::int64_t value) { return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}}; }
[[nodiscard]] std::string scanModeText(const ScanMode mode) { return mode == ScanMode::Full ? "full" : "incremental"; }
[[nodiscard]] ScanMode parseScanMode(const std::string& value) { if (value == "full") { return ScanMode::Full; } if (value == "incremental") { return ScanMode::Incremental; } throw std::runtime_error("unknown cached scan mode"); }
[[nodiscard]] std::string errorCodeText(const ScannerErrorCode code) {
  switch (code) { case ScannerErrorCode::RootUnavailable: return "root_unavailable"; case ScannerErrorCode::PermissionDenied: return "permission_denied"; case ScannerErrorCode::UnsupportedFile: return "unsupported_file"; case ScannerErrorCode::MetadataReadFailed: return "metadata_read_failed"; case ScannerErrorCode::CacheUnavailable: return "cache_unavailable"; case ScannerErrorCode::Cancelled: return "cancelled"; }
  throw std::runtime_error("unknown scanner error code");
}
[[nodiscard]] ScannerErrorCode parseErrorCode(const std::string& value) {
  if (value == "root_unavailable") { return ScannerErrorCode::RootUnavailable; } if (value == "permission_denied") { return ScannerErrorCode::PermissionDenied; } if (value == "unsupported_file") { return ScannerErrorCode::UnsupportedFile; }
  if (value == "metadata_read_failed") { return ScannerErrorCode::MetadataReadFailed; } if (value == "cache_unavailable") { return ScannerErrorCode::CacheUnavailable; } if (value == "cancelled") { return ScannerErrorCode::Cancelled; }
  throw std::runtime_error("unknown cached scanner error code");
}

}

std::string SQLiteCacheV3::schemaV3Sql() {
  return R"sql(CREATE TABLE IF NOT EXISTS content(
  content_id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  artist TEXT NOT NULL,
  album TEXT NOT NULL,
  album_artist TEXT NOT NULL,
  genre TEXT NOT NULL,
  track_number INTEGER,
  disc_number INTEGER,
  year INTEGER,
  duration_ms INTEGER NOT NULL,
  sample_rate INTEGER,
  bit_depth INTEGER,
  channels INTEGER,
  play_count INTEGER NOT NULL DEFAULT 0,
  rating INTEGER,
  last_played_ms INTEGER,
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS scan_roots(
  root_path TEXT PRIMARY KEY,
  directory_tree_hash TEXT NOT NULL,
  total_files INTEGER NOT NULL,
  last_scan_mode TEXT NOT NULL,
  last_scan_duration_ms INTEGER NOT NULL,
  last_scan_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS locations(
  location_id TEXT PRIMARY KEY,
  content_id TEXT NOT NULL,
  root_path TEXT NOT NULL,
  file_path TEXT NOT NULL UNIQUE,
  file_size_bytes INTEGER NOT NULL,
  file_mtime_ns INTEGER NOT NULL,
  source_file_path TEXT NOT NULL,
  cue_track_offset_ms INTEGER,
  artwork_path TEXT,
  lyrics_source TEXT NOT NULL,
  external_lrc_path TEXT,
  external_lrc_mtime_ns INTEGER,
  discovered_at_ms INTEGER NOT NULL,
  scanned_at_ms INTEGER NOT NULL,
  FOREIGN KEY(content_id) REFERENCES content(content_id) ON DELETE CASCADE,
  FOREIGN KEY(root_path) REFERENCES scan_roots(root_path) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS lyrics(
  location_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  line_index INTEGER NOT NULL,
  timestamp_ms INTEGER NOT NULL,
  text TEXT NOT NULL,
  PRIMARY KEY(location_id, kind, line_index),
  FOREIGN KEY(location_id) REFERENCES locations(location_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS scan_errors(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  root_path TEXT NOT NULL,
  file_path TEXT,
  error_code TEXT NOT NULL,
  error_message TEXT NOT NULL,
  occurred_at_ms INTEGER NOT NULL,
  FOREIGN KEY(root_path) REFERENCES scan_roots(root_path) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_content_album ON content(album);
CREATE INDEX IF NOT EXISTS idx_content_artist ON content(artist);
CREATE INDEX IF NOT EXISTS idx_locations_content ON locations(content_id);
CREATE INDEX IF NOT EXISTS idx_locations_root ON locations(root_path);
CREATE INDEX IF NOT EXISTS idx_locations_path ON locations(file_path);
CREATE INDEX IF NOT EXISTS idx_lyrics_location ON lyrics(location_id);
CREATE INDEX IF NOT EXISTS idx_errors_root ON scan_errors(root_path);

PRAGMA user_version=3;)sql";
}

void SQLiteCacheV3::exec(sqlite3* db, const char* sql) { ::seriona::scanner::cache::exec(db, sql); }

void SQLiteCacheV3::configureConnection(sqlite3* db, const std::chrono::milliseconds busyTimeout) {
  if (sqlite3_busy_timeout(db, static_cast<int>(busyTimeout.count())) != SQLITE_OK) {
    throw sqliteError(db, "set busy timeout");
  }
  exec(db, "PRAGMA journal_mode=WAL;");
  exec(db, "PRAGMA foreign_keys=ON;");
}

SQLiteCacheV3::SQLiteCacheV3(ScannerCacheConfig config)
    : databasePath_(std::move(config.databasePath)), busyTimeout_(config.busyTimeout) {
  open();
}

SQLiteCacheV3::~SQLiteCacheV3() {
  if (db_ != nullptr) {
    sqlite3_close(asDb(db_));
  }
}

void SQLiteCacheV3::open() {
  if (!databasePath_.parent_path().empty()) {
    std::filesystem::create_directories(databasePath_.parent_path());
  }

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(pathText(databasePath_).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string message = db == nullptr ? "failed to open scanner cache v3" : sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }

  db_ = db;
  configureConnection(asDb(db_), busyTimeout_);

  const auto version = readUserVersion();
  if (version == 0) {
    initializeSchemaV3();
  } else if (version == 2) {
    createMigrationBackup();
    try { migrateSchemaV2ToV3(); } catch (...) { restoreMigrationBackup(); throw; }
  }

  if (readUserVersion() != kSchemaVersion) {
    throw std::runtime_error("unsupported scanner cache schema version");
  }
}

void SQLiteCacheV3::initializeSchemaV3() { exec(asDb(db_), schemaV3Sql().c_str()); }

int SQLiteCacheV3::readUserVersion() const {
  Statement statement{asDb(db_), "PRAGMA user_version;"};
  return statement.stepRow() ? static_cast<int>(statement.int64Column(0)) : 0;
}

std::string SQLiteCacheV3::readJournalMode() const {
  Statement statement{asDb(db_), "PRAGMA journal_mode;"};
  return statement.stepRow() ? statement.textColumn(0) : std::string{};
}

SQLiteCacheV3::WriterTransaction::WriterTransaction(SQLiteCacheV3& cache)
    : cache_(&cache), lock_(cache.writerMutex_) {
  exec(asDb(cache_->db_), "BEGIN IMMEDIATE;");
  active_ = true;
}

SQLiteCacheV3::WriterTransaction::~WriterTransaction() {
  if (!active_ || cache_ == nullptr) {
    return;
  }
  rollbackNoThrow(asDb(cache_->db_));
}

SQLiteCacheV3::WriterTransaction::WriterTransaction(WriterTransaction&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)), lock_(std::move(other.lock_)), active_(std::exchange(other.active_, false)) {}

SQLiteCacheV3::WriterTransaction& SQLiteCacheV3::WriterTransaction::operator=(WriterTransaction&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (active_ && cache_ != nullptr) {
    rollbackNoThrow(asDb(cache_->db_));
  }
  cache_ = std::exchange(other.cache_, nullptr);
  lock_ = std::move(other.lock_);
  active_ = std::exchange(other.active_, false);
  return *this;
}

void SQLiteCacheV3::WriterTransaction::commit() {
  if (!active_ || cache_ == nullptr) {
    return;
  }
  exec(asDb(cache_->db_), "COMMIT;");
  active_ = false;
}

SQLiteCacheV3::WriterTransaction SQLiteCacheV3::beginWriter() { return WriterTransaction{*this}; }

void SQLiteCacheV3::replaceLyrics(const std::string& locationId, const std::string& kind, const std::vector<LyricLine>& lyrics) {
  auto transaction = beginWriter(); Statement remove{asDb(db_), "DELETE FROM lyrics WHERE location_id=?1 AND kind=?2;"}; remove.bind(1, locationId); remove.bind(2, kind); remove.stepDone();
  for (std::size_t index = 0; index < lyrics.size(); ++index) { Statement insert{asDb(db_), "INSERT INTO lyrics(location_id, kind, line_index, timestamp_ms, text) VALUES(?1, ?2, ?3, ?4, ?5);"}; insert.bind(1, locationId); insert.bind(2, kind); insert.bind(3, static_cast<std::int64_t>(index)); insert.bind(4, lyrics[index].timestamp.count()); insert.bind(5, lyrics[index].text); insert.stepDone(); }
  transaction.commit();
}

std::vector<LyricLine> SQLiteCacheV3::loadLyrics(const std::string& locationId, const std::string& kind) const {
  Statement select{asDb(db_), "SELECT timestamp_ms, text FROM lyrics WHERE location_id=?1 AND kind=?2 ORDER BY line_index;"}; select.bind(1, locationId); select.bind(2, kind); std::vector<LyricLine> lyrics;
  while (select.stepRow()) { lyrics.push_back({.timestamp = std::chrono::milliseconds{select.int64Column(0)}, .text = select.textColumn(1)}); }
  return lyrics;
}

void SQLiteCacheV3::updateScanRoot(const CachedScanRootV3& root) {
  auto transaction = beginWriter(); Statement upsert{asDb(db_), "INSERT INTO scan_roots(root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms) VALUES(?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT(root_path) DO UPDATE SET directory_tree_hash=excluded.directory_tree_hash, total_files=excluded.total_files, last_scan_mode=excluded.last_scan_mode, last_scan_duration_ms=excluded.last_scan_duration_ms, last_scan_at_ms=excluded.last_scan_at_ms;"};
  upsert.bind(1, pathText(root.rootPath)); upsert.bind(2, root.directoryTreeHash); upsert.bind(3, static_cast<std::int64_t>(root.totalFiles)); upsert.bind(4, scanModeText(root.lastScanMode)); upsert.bind(5, root.lastScanDuration.count()); upsert.bind(6, systemTimeToMs(root.lastScanAt)); upsert.stepDone(); transaction.commit();
}

std::optional<CachedScanRootV3> SQLiteCacheV3::loadScanRoot(const std::filesystem::path& rootPath) const {
  Statement select{asDb(db_), "SELECT root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms FROM scan_roots WHERE root_path=?1;"}; select.bind(1, pathText(rootPath)); if (!select.stepRow()) { return std::nullopt; }
  return CachedScanRootV3{.rootPath = select.textColumn(0), .directoryTreeHash = select.textColumn(1), .totalFiles = static_cast<std::uint64_t>(select.int64Column(2)), .lastScanMode = parseScanMode(select.textColumn(3)), .lastScanDuration = std::chrono::milliseconds{select.int64Column(4)}, .lastScanAt = msToSystemTime(select.int64Column(5))};
}

void SQLiteCacheV3::saveErrors(const std::filesystem::path& rootPath, const std::vector<CachedScanErrorV3>& errors) {
  auto transaction = beginWriter(); Statement remove{asDb(db_), "DELETE FROM scan_errors WHERE root_path=?1;"}; remove.bind(1, pathText(rootPath)); remove.stepDone();
  for (const auto& error : errors) { Statement insert{asDb(db_), "INSERT INTO scan_errors(root_path, file_path, error_code, error_message, occurred_at_ms) VALUES(?1, ?2, ?3, ?4, ?5);"}; insert.bind(1, pathText(rootPath)); insert.bindOptionalPath(2, error.filePath); insert.bind(3, errorCodeText(error.errorCode)); insert.bind(4, error.errorMessage); insert.bind(5, systemTimeToMs(error.occurredAt)); insert.stepDone(); }
  transaction.commit();
}

std::vector<CachedScanErrorV3> SQLiteCacheV3::loadErrors(const std::filesystem::path& rootPath) const {
  Statement select{asDb(db_), "SELECT root_path, file_path, error_code, error_message, occurred_at_ms FROM scan_errors WHERE root_path=?1 ORDER BY id;"}; select.bind(1, pathText(rootPath)); std::vector<CachedScanErrorV3> errors;
  while (select.stepRow()) { errors.push_back({.rootPath = select.textColumn(0), .filePath = select.columnType(1) == SQLITE_TEXT ? std::optional<std::filesystem::path>{select.textColumn(1)} : std::nullopt, .errorCode = parseErrorCode(select.textColumn(2)), .errorMessage = select.textColumn(3), .occurredAt = msToSystemTime(select.int64Column(4))}); }
  return errors;
}

void SQLiteCacheV3::clearErrors(const std::filesystem::path& rootPath) {
  auto transaction = beginWriter(); Statement remove{asDb(db_), "DELETE FROM scan_errors WHERE root_path=?1;"}; remove.bind(1, pathText(rootPath)); remove.stepDone(); transaction.commit();
}

}
