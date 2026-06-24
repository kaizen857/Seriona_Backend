#include "seriona/scanner/cache/sqlite_scanner_cache.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::scanner::cache {
namespace {

constexpr int kSchemaVersion = 1;

[[nodiscard]] sqlite3* asDb(void* db) noexcept { return static_cast<sqlite3*>(db); }

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

[[nodiscard]] std::uintmax_t fileBytes(const std::filesystem::path& path) {
  std::error_code error;
  const auto bytes = std::filesystem::file_size(path, error);
  return error ? 0U : bytes;
}

[[nodiscard]] std::filesystem::path walPathFor(const std::filesystem::path& databasePath) {
  return std::filesystem::path{databasePath.generic_string() + "-wal"};
}

[[nodiscard]] std::int64_t fileTimeToNs(const std::filesystem::file_time_type time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] std::filesystem::file_time_type nsToFileTime(const std::int64_t value) {
  return std::filesystem::file_time_type{std::chrono::nanoseconds{value}};
}

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point msToSystemTime(const std::int64_t value) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
}

[[nodiscard]] std::string lyricsSourceText(const LyricsSource source) {
  switch (source) {
    case LyricsSource::None:
      return "none";
    case LyricsSource::EmbeddedTag:
      return "embedded";
    case LyricsSource::ExternalLrc:
      return "external";
  }
  return "none";
}

[[nodiscard]] LyricsSource lyricsSourceFromText(const std::string_view text) {
  if (text == "embedded") {
    return LyricsSource::EmbeddedTag;
  }
  if (text == "external") {
    return LyricsSource::ExternalLrc;
  }
  return LyricsSource::None;
}

[[nodiscard]] std::string errorCodeText(const ScannerErrorCode code) {
  switch (code) {
    case ScannerErrorCode::RootUnavailable:
      return "root_unavailable";
    case ScannerErrorCode::PermissionDenied:
      return "permission_denied";
    case ScannerErrorCode::UnsupportedFile:
      return "unsupported_file";
    case ScannerErrorCode::MetadataReadFailed:
      return "metadata_read_failed";
    case ScannerErrorCode::CacheUnavailable:
      return "cache_unavailable";
    case ScannerErrorCode::Cancelled:
      return "cancelled";
  }
  return "metadata_read_failed";
}

[[nodiscard]] ScannerErrorCode errorCodeFromText(const std::string_view text) {
  if (text == "root_unavailable") {
    return ScannerErrorCode::RootUnavailable;
  }
  if (text == "permission_denied") {
    return ScannerErrorCode::PermissionDenied;
  }
  if (text == "unsupported_file") {
    return ScannerErrorCode::UnsupportedFile;
  }
  if (text == "cache_unavailable") {
    return ScannerErrorCode::CacheUnavailable;
  }
  if (text == "cancelled") {
    return ScannerErrorCode::Cancelled;
  }
  return ScannerErrorCode::MetadataReadFailed;
}

[[nodiscard]] std::runtime_error sqliteError(sqlite3* db, const std::string& action) {
  return std::runtime_error(action + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
    sqlite3_free(message);
    throw std::runtime_error(detail);
  }
}

void rollbackNoThrow(sqlite3* db) noexcept {
  char* message = nullptr;
  static_cast<void>(sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &message));
  sqlite3_free(message);
}

class Statement {
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
  void bind(const int index, const std::filesystem::path& value) { bind(index, pathText(value)); }
  void bind(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw sqliteError(db_, "bind int64");
    }
  }
  void bindNull(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw sqliteError(db_, "bind null");
    }
  }
  void bindOptional(const int index, const std::optional<std::uint32_t> value) {
    value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index);
  }
  void bindOptional(const int index, const std::optional<std::uint16_t> value) {
    value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index);
  }
  void bindOptional(const int index, const std::optional<std::uint64_t> value) {
    value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index);
  }
  void bindOptional(const int index, const std::optional<std::string>& value) {
    value.has_value() ? bind(index, *value) : bindNull(index);
  }
  void bindOptionalPath(const int index, const std::optional<std::filesystem::path>& value) {
    value.has_value() ? bind(index, *value) : bindNull(index);
  }
  void bindOptionalFileTime(const int index, const std::optional<std::filesystem::file_time_type> value) {
    value.has_value() ? bind(index, fileTimeToNs(*value)) : bindNull(index);
  }
  void bindOptionalDuration(const int index, const std::optional<std::chrono::milliseconds> value) {
    value.has_value() ? bind(index, value->count()) : bindNull(index);
  }
  void bindOptionalSystemTime(const int index, const std::optional<std::chrono::system_clock::time_point> value) {
    value.has_value() ? bind(index, systemTimeToMs(*value)) : bindNull(index);
  }
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
  void stepDone() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw sqliteError(db_, "step done");
    }
  }
  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }
  [[nodiscard]] std::string textColumn(const int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
  }
  [[nodiscard]] std::optional<std::uint32_t> uint32Column(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL
               ? std::nullopt
               : std::optional<std::uint32_t>{static_cast<std::uint32_t>(sqlite3_column_int64(statement_, index))};
  }
  [[nodiscard]] std::optional<std::uint16_t> uint16Column(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL
               ? std::nullopt
               : std::optional<std::uint16_t>{static_cast<std::uint16_t>(sqlite3_column_int64(statement_, index))};
  }
  [[nodiscard]] std::optional<std::uint64_t> uint64Column(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL
               ? std::nullopt
               : std::optional<std::uint64_t>{static_cast<std::uint64_t>(sqlite3_column_int64(statement_, index))};
  }
  [[nodiscard]] std::optional<std::string> optionalTextColumn(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL ? std::nullopt : std::optional<std::string>{textColumn(index)};
  }
  [[nodiscard]] std::optional<std::filesystem::path> optionalPathColumn(const int index) const {
    const auto value = optionalTextColumn(index);
    return value.has_value() ? std::optional<std::filesystem::path>{std::filesystem::path{*value}} : std::nullopt;
  }
  [[nodiscard]] std::optional<std::filesystem::file_time_type> optionalFileTimeColumn(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL ? std::nullopt : std::optional{nsToFileTime(int64Column(index))};
  }
  [[nodiscard]] std::optional<std::chrono::milliseconds> optionalDurationColumn(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL ? std::nullopt : std::optional{std::chrono::milliseconds{int64Column(index)}};
  }
  [[nodiscard]] std::optional<std::chrono::system_clock::time_point> optionalSystemTimeColumn(const int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL ? std::nullopt : std::optional{msToSystemTime(int64Column(index))};
  }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

[[nodiscard]] int scalarInt(sqlite3* db, const char* sql) {
  Statement statement{db, sql};
  return statement.stepRow() ? static_cast<int>(statement.int64Column(0)) : 0;
}

[[nodiscard]] std::string scalarText(sqlite3* db, const char* sql) {
  Statement statement{db, sql};
  return statement.stepRow() ? statement.textColumn(0) : std::string{};
}

void configureConnection(sqlite3* db, const std::chrono::milliseconds busyTimeout) {
  if (sqlite3_busy_timeout(db, static_cast<int>(busyTimeout.count())) != SQLITE_OK) {
    throw sqliteError(db, "set busy timeout");
  }
  exec(db, "PRAGMA journal_mode=WAL;");
  exec(db, "PRAGMA foreign_keys=ON;");
}

void createSchema(sqlite3* db) {
  exec(db, R"sql(
CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS roots(
  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, directory_hash TEXT NOT NULL, updated_at_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS directories(
  root_id INTEGER NOT NULL, relative_path TEXT NOT NULL, directory_hash TEXT NOT NULL, mtime_ns INTEGER,
  PRIMARY KEY(root_id, relative_path), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS songs(
  id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL, track_id TEXT NOT NULL, file_path TEXT NOT NULL,
  title TEXT NOT NULL, artist TEXT NOT NULL, album TEXT NOT NULL, album_artist TEXT NOT NULL, genre TEXT NOT NULL,
  track_number INTEGER, disc_number INTEGER, year INTEGER, sample_rate INTEGER, bit_depth INTEGER, channels INTEGER,
  file_size_bytes INTEGER, file_mtime_ns INTEGER, content_hash TEXT NOT NULL, lyrics_source TEXT NOT NULL,
  external_lrc_path TEXT, external_lrc_hash TEXT, external_lrc_mtime_ns INTEGER, source_file_path TEXT NOT NULL,
  offset_ms INTEGER, duration_ms INTEGER, logical_track_id TEXT NOT NULL, play_count INTEGER NOT NULL DEFAULT 0,
  rating INTEGER, last_played_ms INTEGER, UNIQUE(root_id, track_id),
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS lyrics(
  song_id INTEGER NOT NULL, kind TEXT NOT NULL, line_index INTEGER NOT NULL, timestamp_ms INTEGER NOT NULL, text TEXT NOT NULL,
  PRIMARY KEY(song_id, kind, line_index), FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS errors(
  root_id INTEGER NOT NULL, error_index INTEGER NOT NULL, code TEXT NOT NULL, message TEXT NOT NULL, detail TEXT NOT NULL, path TEXT,
  PRIMARY KEY(root_id, error_index), FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS idx_songs_root_file ON songs(root_id, file_path);
CREATE INDEX IF NOT EXISTS idx_lyrics_song_kind ON lyrics(song_id, kind);
)sql");
  exec(db, "INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', '1');");
  exec(db, "PRAGMA user_version=1;");
}

void migrate(sqlite3* db) {
  const auto version = scalarInt(db, "PRAGMA user_version;");
  if (version == 0) {
    createSchema(db);
    return;
  }
  if (version != kSchemaVersion) {
    throw std::runtime_error("unsupported scanner cache schema version");
  }
  createSchema(db);
}

[[nodiscard]] std::int64_t ensureRoot(sqlite3* db, const CachedRoot& root) {
  Statement upsert{db, "INSERT INTO roots(path, directory_hash, updated_at_ms) VALUES(?1, ?2, ?3) "
                       "ON CONFLICT(path) DO UPDATE SET directory_hash=excluded.directory_hash, updated_at_ms=excluded.updated_at_ms;"};
  upsert.bind(1, root.rootPath);
  upsert.bind(2, root.directoryHash);
  upsert.bind(3, systemTimeToMs(std::chrono::system_clock::now()));
  upsert.stepDone();

  Statement select{db, "SELECT id FROM roots WHERE path=?1;"};
  select.bind(1, root.rootPath);
  if (!select.stepRow()) {
    throw std::runtime_error("scanner cache root upsert produced no row");
  }
  return select.int64Column(0);
}

[[nodiscard]] std::optional<CachedUserStats> existingStats(sqlite3* db, const std::int64_t rootId, const std::string& trackId) {
  Statement select{db, "SELECT play_count, rating, last_played_ms FROM songs WHERE root_id=?1 AND track_id=?2;"};
  select.bind(1, rootId);
  select.bind(2, trackId);
  if (!select.stepRow()) {
    return std::nullopt;
  }
  return CachedUserStats{.playCount = static_cast<std::uint64_t>(select.int64Column(0)),
                         .rating = select.uint32Column(1),
                         .lastPlayed = select.optionalSystemTimeColumn(2)};
}

void replaceDirectories(sqlite3* db, const std::int64_t rootId, const std::vector<CachedDirectory>& directories) {
  Statement remove{db, "DELETE FROM directories WHERE root_id=?1;"};
  remove.bind(1, rootId);
  remove.stepDone();
  for (const auto& directory : directories) {
    Statement insert{db, "INSERT INTO directories(root_id, relative_path, directory_hash, mtime_ns) VALUES(?1, ?2, ?3, ?4);"};
    insert.bind(1, rootId);
    insert.bind(2, directory.relativePath);
    insert.bind(3, directory.hash);
    insert.bindOptionalFileTime(4, directory.mtime);
    insert.stepDone();
  }
}

void replaceLyrics(sqlite3* db, const std::int64_t songId, const std::string_view kind, const std::vector<LyricLine>& lyrics) {
  Statement remove{db, "DELETE FROM lyrics WHERE song_id=?1 AND kind=?2;"};
  remove.bind(1, songId);
  remove.bind(2, std::string{kind});
  remove.stepDone();
  for (std::size_t index = 0; index < lyrics.size(); ++index) {
    Statement insert{db, "INSERT INTO lyrics(song_id, kind, line_index, timestamp_ms, text) VALUES(?1, ?2, ?3, ?4, ?5);"};
    insert.bind(1, songId);
    insert.bind(2, std::string{kind});
    insert.bind(3, static_cast<std::int64_t>(index));
    insert.bind(4, lyrics[index].timestamp.count());
    insert.bind(5, lyrics[index].text);
    insert.stepDone();
  }
}

[[nodiscard]] std::int64_t upsertSong(sqlite3* db, const std::int64_t rootId, const CachedSong& song) {
  const auto stats = existingStats(db, rootId, song.metadata.trackId).value_or(song.userStats);
  Statement upsert{db, R"sql(
INSERT INTO songs(root_id, track_id, file_path, title, artist, album, album_artist, genre, track_number, disc_number,
year, sample_rate, bit_depth, channels, file_size_bytes, file_mtime_ns, content_hash, lyrics_source, external_lrc_path,
external_lrc_hash, external_lrc_mtime_ns, source_file_path, offset_ms, duration_ms, logical_track_id, play_count, rating, last_played_ms)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, ?26, ?27, ?28)
ON CONFLICT(root_id, track_id) DO UPDATE SET file_path=excluded.file_path, title=excluded.title, artist=excluded.artist,
album=excluded.album, album_artist=excluded.album_artist, genre=excluded.genre, track_number=excluded.track_number,
disc_number=excluded.disc_number, year=excluded.year, sample_rate=excluded.sample_rate, bit_depth=excluded.bit_depth,
channels=excluded.channels, file_size_bytes=excluded.file_size_bytes, file_mtime_ns=excluded.file_mtime_ns,
content_hash=excluded.content_hash, lyrics_source=excluded.lyrics_source, external_lrc_path=excluded.external_lrc_path,
external_lrc_hash=excluded.external_lrc_hash, external_lrc_mtime_ns=excluded.external_lrc_mtime_ns,
source_file_path=excluded.source_file_path, offset_ms=excluded.offset_ms, duration_ms=excluded.duration_ms,
logical_track_id=excluded.logical_track_id, play_count=songs.play_count, rating=songs.rating, last_played_ms=songs.last_played_ms;
)sql"};
  const auto& metadata = song.metadata;
  upsert.bind(1, rootId);
  upsert.bind(2, metadata.trackId);
  upsert.bind(3, metadata.filePath);
  upsert.bind(4, metadata.title);
  upsert.bind(5, metadata.artist);
  upsert.bind(6, metadata.album);
  upsert.bind(7, metadata.albumArtist);
  upsert.bind(8, metadata.genre);
  upsert.bindOptional(9, metadata.trackNumber);
  upsert.bindOptional(10, metadata.discNumber);
  upsert.bindOptional(11, metadata.year);
  upsert.bindOptional(12, metadata.sampleRate);
  upsert.bindOptional(13, metadata.bitDepth);
  upsert.bindOptional(14, metadata.channels);
  upsert.bindOptional(15, metadata.fileSizeBytes);
  upsert.bindOptionalFileTime(16, metadata.fileMtime);
  upsert.bind(17, metadata.contentHash);
  upsert.bind(18, lyricsSourceText(metadata.effectiveLyricsSource));
  upsert.bindOptionalPath(19, metadata.externalLyricsPath);
  upsert.bindOptional(20, metadata.externalLyricsHash);
  upsert.bindOptionalFileTime(21, metadata.externalLyricsMtime);
  upsert.bind(22, metadata.sourceFilePath);
  upsert.bindOptionalDuration(23, metadata.offset);
  upsert.bindOptionalDuration(24, metadata.duration);
  upsert.bind(25, metadata.logicalTrackId);
  upsert.bind(26, static_cast<std::int64_t>(stats.playCount));
  upsert.bindOptional(27, stats.rating);
  upsert.bindOptionalSystemTime(28, stats.lastPlayed);
  upsert.stepDone();

  Statement select{db, "SELECT id FROM songs WHERE root_id=?1 AND track_id=?2;"};
  select.bind(1, rootId);
  select.bind(2, metadata.trackId);
  if (!select.stepRow()) {
    throw std::runtime_error("scanner cache song upsert produced no row");
  }
  return select.int64Column(0);
}

void replaceErrors(sqlite3* db, const std::int64_t rootId, const std::vector<ScannerError>& errors) {
  Statement remove{db, "DELETE FROM errors WHERE root_id=?1;"};
  remove.bind(1, rootId);
  remove.stepDone();
  for (std::size_t index = 0; index < errors.size(); ++index) {
    Statement insert{db, "INSERT INTO errors(root_id, error_index, code, message, detail, path) VALUES(?1, ?2, ?3, ?4, ?5, ?6);"};
    insert.bind(1, rootId);
    insert.bind(2, static_cast<std::int64_t>(index));
    insert.bind(3, errorCodeText(errors[index].code));
    insert.bind(4, errors[index].message);
    insert.bind(5, errors[index].detail);
    errors[index].path.has_value() ? insert.bind(6, *errors[index].path) : insert.bindNull(6);
    insert.stepDone();
  }
}

[[nodiscard]] std::vector<LyricLine> loadLyrics(sqlite3* db, const std::int64_t songId, const std::string_view kind) {
  Statement select{db, "SELECT timestamp_ms, text FROM lyrics WHERE song_id=?1 AND kind=?2 ORDER BY line_index ASC;"};
  select.bind(1, songId);
  select.bind(2, std::string{kind});
  std::vector<LyricLine> lines;
  while (select.stepRow()) {
    lines.push_back({.timestamp = std::chrono::milliseconds{select.int64Column(0)}, .text = select.textColumn(1)});
  }
  return lines;
}

void loadDirectories(sqlite3* db, const std::int64_t rootId, CachedRoot& root) {
  Statement select{db, "SELECT relative_path, directory_hash, mtime_ns FROM directories WHERE root_id=?1 ORDER BY relative_path;"};
  select.bind(1, rootId);
  while (select.stepRow()) {
    root.directories.push_back({.relativePath = std::filesystem::path{select.textColumn(0)},
                                .hash = select.textColumn(1),
                                .mtime = select.optionalFileTimeColumn(2)});
  }
}

void loadSongs(sqlite3* db, const std::int64_t rootId, CachedRoot& root) {
  Statement select{db, R"sql(
SELECT id, track_id, file_path, title, artist, album, album_artist, genre, track_number, disc_number, year, sample_rate,
bit_depth, channels, file_size_bytes, file_mtime_ns, content_hash, lyrics_source, external_lrc_path, external_lrc_hash,
external_lrc_mtime_ns, source_file_path, offset_ms, duration_ms, logical_track_id, play_count, rating, last_played_ms
FROM songs WHERE root_id=?1 ORDER BY file_path;
)sql"};
  select.bind(1, rootId);
  while (select.stepRow()) {
    CachedSong song{};
    const auto songId = select.int64Column(0);
    auto& metadata = song.metadata;
    metadata.trackId = select.textColumn(1);
    metadata.filePath = std::filesystem::path{select.textColumn(2)};
    metadata.title = select.textColumn(3);
    metadata.artist = select.textColumn(4);
    metadata.album = select.textColumn(5);
    metadata.albumArtist = select.textColumn(6);
    metadata.genre = select.textColumn(7);
    metadata.trackNumber = select.uint32Column(8);
    metadata.discNumber = select.uint32Column(9);
    metadata.year = select.uint32Column(10);
    metadata.sampleRate = select.uint32Column(11);
    metadata.bitDepth = select.uint16Column(12);
    metadata.channels = select.uint16Column(13);
    metadata.fileSizeBytes = select.uint64Column(14);
    metadata.fileMtime = select.optionalFileTimeColumn(15);
    metadata.contentHash = select.textColumn(16);
    metadata.effectiveLyricsSource = lyricsSourceFromText(select.textColumn(17));
    metadata.externalLyricsPath = select.optionalPathColumn(18);
    metadata.externalLyricsHash = select.optionalTextColumn(19);
    metadata.externalLyricsMtime = select.optionalFileTimeColumn(20);
    metadata.sourceFilePath = std::filesystem::path{select.textColumn(21)};
    metadata.offset = select.optionalDurationColumn(22);
    metadata.duration = select.optionalDurationColumn(23);
    metadata.logicalTrackId = select.textColumn(24);
    song.userStats.playCount = static_cast<std::uint64_t>(select.int64Column(25));
    song.userStats.rating = select.uint32Column(26);
    song.userStats.lastPlayed = select.optionalSystemTimeColumn(27);
    song.embeddedLyrics = loadLyrics(db, songId, "embedded");
    song.externalLyrics = loadLyrics(db, songId, "external");
    if (metadata.effectiveLyricsSource == LyricsSource::ExternalLrc) {
      metadata.effectiveLyrics = song.externalLyrics;
    } else if (metadata.effectiveLyricsSource == LyricsSource::EmbeddedTag) {
      metadata.effectiveLyrics = song.embeddedLyrics;
    }
    root.songs.push_back(std::move(song));
  }
}

void loadErrors(sqlite3* db, const std::int64_t rootId, CachedRoot& root) {
  Statement select{db, "SELECT code, message, detail, path FROM errors WHERE root_id=?1 ORDER BY error_index;"};
  select.bind(1, rootId);
  while (select.stepRow()) {
    root.errors.push_back({.code = errorCodeFromText(select.textColumn(0)),
                           .message = select.textColumn(1),
                           .detail = select.textColumn(2),
                           .path = select.optionalPathColumn(3)});
  }
}

} // namespace

SQLiteScannerCache::WriterTransaction::WriterTransaction(SQLiteScannerCache& cache)
    : cache_(&cache), lock_(cache.writerMutex_) {
  exec(asDb(cache_->db_), "BEGIN IMMEDIATE;");
  active_ = true;
}

SQLiteScannerCache::WriterTransaction::~WriterTransaction() {
  if (active_ && cache_ != nullptr) {
    rollbackNoThrow(asDb(cache_->db_));
  }
}

SQLiteScannerCache::WriterTransaction::WriterTransaction(WriterTransaction&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)), lock_(std::move(other.lock_)), active_(std::exchange(other.active_, false)) {}

SQLiteScannerCache::WriterTransaction& SQLiteScannerCache::WriterTransaction::operator=(WriterTransaction&& other) noexcept {
  if (this != &other) {
    cache_ = std::exchange(other.cache_, nullptr);
    lock_ = std::move(other.lock_);
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

void SQLiteScannerCache::WriterTransaction::commit() {
  if (!active_ || cache_ == nullptr) {
    return;
  }
  exec(asDb(cache_->db_), "COMMIT;");
  active_ = false;
}

SQLiteScannerCache::SQLiteScannerCache(ScannerCacheConfig config) {
  if (!config.databasePath.parent_path().empty()) {
    std::filesystem::create_directories(config.databasePath.parent_path());
  }
  databasePath_ = config.databasePath;
  maintenancePolicy_ = config.maintenancePolicy;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(pathText(config.databasePath).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    std::string message = db == nullptr ? "failed to open scanner cache" : sqlite3_errmsg(db);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }
  db_ = db;
  configureConnection(asDb(db_), config.busyTimeout);
  migrate(asDb(db_));
}

SQLiteScannerCache::~SQLiteScannerCache() { sqlite3_close(asDb(db_)); }

int SQLiteScannerCache::schemaVersion() const { return scalarInt(asDb(db_), "PRAGMA user_version;"); }

std::string SQLiteScannerCache::journalMode() const { return scalarText(asDb(db_), "PRAGMA journal_mode;"); }

std::optional<CachedRoot> SQLiteScannerCache::loadRoot(const std::filesystem::path& rootPath) const {
  Statement select{asDb(db_), "SELECT id, directory_hash FROM roots WHERE path=?1;"};
  select.bind(1, rootPath);
  if (!select.stepRow()) {
    return std::nullopt;
  }
  CachedRoot root{};
  root.rootPath = rootPath;
  root.directoryHash = select.textColumn(1);
  const auto rootId = select.int64Column(0);
  loadDirectories(asDb(db_), rootId, root);
  loadSongs(asDb(db_), rootId, root);
  loadErrors(asDb(db_), rootId, root);
  return root;
}

void SQLiteScannerCache::saveRoot(const CachedRoot& root) {
  auto transaction = beginWriter();
  const auto rootId = ensureRoot(asDb(db_), root);
  replaceDirectories(asDb(db_), rootId, root.directories);
  std::vector<std::string> retainedTrackIds;
  retainedTrackIds.reserve(root.songs.size());
  for (const auto& song : root.songs) {
    retainedTrackIds.push_back(song.metadata.trackId);
    const auto songId = upsertSong(asDb(db_), rootId, song);
    replaceLyrics(asDb(db_), songId, "embedded", song.embeddedLyrics);
    replaceLyrics(asDb(db_), songId, "external", song.externalLyrics);
  }
  Statement select{asDb(db_), "SELECT id, track_id FROM songs WHERE root_id=?1;"};
  select.bind(1, rootId);
  std::vector<std::int64_t> removedIds;
  while (select.stepRow()) {
    const auto trackId = select.textColumn(1);
    if (std::ranges::find(retainedTrackIds, trackId) == retainedTrackIds.end()) {
      removedIds.push_back(select.int64Column(0));
    }
  }
  for (const auto songId : removedIds) {
    Statement remove{asDb(db_), "DELETE FROM songs WHERE id=?1;"};
    remove.bind(1, songId);
    remove.stepDone();
  }
  replaceErrors(asDb(db_), rootId, root.errors);
  transaction.commit();
}

void SQLiteScannerCache::updateUserStats(const std::filesystem::path& rootPath, const std::string& trackId, CachedUserStats stats) {
  auto transaction = beginWriter();
  Statement update{asDb(db_), "UPDATE songs SET play_count=?1, rating=?2, last_played_ms=?3 WHERE root_id=(SELECT id FROM roots WHERE path=?4) AND track_id=?5;"};
  update.bind(1, static_cast<std::int64_t>(stats.playCount));
  update.bindOptional(2, stats.rating);
  update.bindOptionalSystemTime(3, stats.lastPlayed);
  update.bind(4, rootPath);
  update.bind(5, trackId);
  update.stepDone();
  transaction.commit();
}

void SQLiteScannerCache::pruneMissingSongs(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedTrackIds) {
  auto transaction = beginWriter();
  Statement select{asDb(db_), "SELECT id, track_id FROM songs WHERE root_id=(SELECT id FROM roots WHERE path=?1);"};
  select.bind(1, rootPath);
  std::vector<std::int64_t> removedIds;
  while (select.stepRow()) {
    const auto trackId = select.textColumn(1);
    if (std::ranges::find(retainedTrackIds, trackId) == retainedTrackIds.end()) {
      removedIds.push_back(select.int64Column(0));
    }
  }
  for (const auto songId : removedIds) {
    Statement remove{asDb(db_), "DELETE FROM songs WHERE id=?1;"};
    remove.bind(1, songId);
    remove.stepDone();
  }
  transaction.commit();
}

CacheMaintenanceDecision SQLiteScannerCache::maintenanceDecision() const {
  CacheMaintenanceDecision decision{};
  decision.databaseBytes = fileBytes(databasePath_);
  decision.walBytes = fileBytes(walPathFor(databasePath_));
  decision.cachedRoots = static_cast<std::uint32_t>(scalarInt(asDb(db_), "SELECT COUNT(*) FROM roots;"));
  decision.checkpointRecommended = decision.walBytes >= maintenancePolicy_.passiveCheckpointWalBytes;
  decision.cleanupRecommended = decision.databaseBytes >= maintenancePolicy_.softDatabaseBytes ||
                                decision.cachedRoots > maintenancePolicy_.maxCachedRoots;
  decision.vacuumRecommended = decision.databaseBytes >= maintenancePolicy_.hardDatabaseBytes;
  return decision;
}

CacheMaintenanceResult SQLiteScannerCache::maintainCache() {
  CacheMaintenanceResult result{};
  result.before = maintenanceDecision();
  if (result.before.checkpointRecommended) {
    result.checkpoint = checkpointPassive();
  }
  if (result.before.cleanupRecommended) {
    result.rootsRemoved = pruneOldestRoots(maintenancePolicy_.maxCachedRoots);
  }
  if (result.before.vacuumRecommended) {
    auto transaction = beginWriter();
    transaction.commit();
    exec(asDb(db_), "VACUUM;");
    result.vacuumed = true;
  }
  result.after = maintenanceDecision();
  return result;
}

CacheCheckpointResult SQLiteScannerCache::checkpointPassive() {
  CacheCheckpointResult result{};
  result.resultCode = sqlite3_wal_checkpoint_v2(asDb(db_), nullptr, SQLITE_CHECKPOINT_PASSIVE, &result.logFrames,
                                                &result.checkpointedFrames);
  return result;
}

SQLiteScannerCache::WriterTransaction SQLiteScannerCache::beginWriter() { return WriterTransaction{*this}; }

std::uint32_t SQLiteScannerCache::pruneOldestRoots(const std::uint32_t maxCachedRoots) {
  auto transaction = beginWriter();
  Statement count{asDb(db_), "SELECT COUNT(*) FROM roots;"};
  const auto rootCount = count.stepRow() ? static_cast<std::uint32_t>(count.int64Column(0)) : 0U;
  if (rootCount <= maxCachedRoots) {
    transaction.commit();
    return 0U;
  }
  const auto rootsToRemove = rootCount - maxCachedRoots;
  Statement select{asDb(db_), "SELECT id FROM roots ORDER BY updated_at_ms ASC, id ASC LIMIT ?1;"};
  select.bind(1, static_cast<std::int64_t>(rootsToRemove));
  std::vector<std::int64_t> rootIds;
  while (select.stepRow()) {
    rootIds.push_back(select.int64Column(0));
  }
  for (const auto rootId : rootIds) {
    Statement remove{asDb(db_), "DELETE FROM roots WHERE id=?1;"};
    remove.bind(1, rootId);
    remove.stepDone();
  }
  transaction.commit();
  return static_cast<std::uint32_t>(rootIds.size());
}

}
