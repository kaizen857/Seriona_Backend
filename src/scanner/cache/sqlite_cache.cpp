#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/song_identity.h"

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

[[nodiscard]] sqlite3* asDb(void* db) noexcept { return static_cast<sqlite3*>(db); }

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) { return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count(); }

[[nodiscard]] std::chrono::system_clock::time_point msToSystemTime(const std::int64_t value) { return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}}; }

[[nodiscard]] std::string pathText(const std::filesystem::path& path) { return path.generic_string(); }

[[nodiscard]] std::string rewritePathPrefix(const std::string& path,
                                            const std::string& oldPrefix,
                                            const std::string& newPrefix) {
  if (path == oldPrefix) {
    return newPrefix;
  }
  const auto oldDir = oldPrefix + "/";
  if (path.rfind(oldDir, 0) == 0) {
    return newPrefix + path.substr(oldPrefix.size());
  }
  return path;
}

[[nodiscard]] std::string lyricsSourceText(const LyricsSource source) {
  switch (source) {
  case LyricsSource::None:
    return "none";
  case LyricsSource::EmbeddedTag:
    return "embedded_tag";
  case LyricsSource::ExternalLrc:
    return "external_lrc";
  }
  throw std::runtime_error("unknown lyrics source");
}

[[nodiscard]] LyricsSource parseLyricsSource(const std::string& value) {
  if (value == "none") {
    return LyricsSource::None;
  }
  if (value == "embedded_tag") {
    return LyricsSource::EmbeddedTag;
  }
  if (value == "external_lrc") {
    return LyricsSource::ExternalLrc;
  }
  throw std::runtime_error("unknown cached lyrics source");
}

[[nodiscard]] std::runtime_error sqliteError(sqlite3* db, const std::string& action) {
  return std::runtime_error(action + ": " + sqlite3_errmsg(db));
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

  void bindOptional(const int index, const std::optional<std::uint32_t> value) { value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index); }
  void bindOptional(const int index, const std::optional<std::uint16_t> value) { value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index); }
  void bindOptionalSystemTime(const int index, const std::optional<std::chrono::system_clock::time_point> value) { value.has_value() ? bind(index, systemTimeToMs(*value)) : bindNull(index); }
  void bindOptionalPath(const int index, const std::optional<std::filesystem::path>& value) { value.has_value() ? bind(index, pathText(*value)) : bindNull(index); }
  void bindOptionalInt64(const int index, const std::optional<std::int64_t> value) { value.has_value() ? bind(index, *value) : bindNull(index); }
  void bindOptionalUint64(const int index, const std::optional<std::uint64_t> value) { value.has_value() ? bind(index, static_cast<std::int64_t>(*value)) : bindNull(index); }
  void bindOptionalMilliseconds(const int index, const std::optional<std::chrono::milliseconds> value) { value.has_value() ? bind(index, value->count()) : bindNull(index); }
  void bindOptionalString(const int index, const std::optional<std::string>& value) { value.has_value() ? bind(index, *value) : bindNull(index); }

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

[[nodiscard]] CachedLocation readLocation(Statement& row) {
  CachedLocation location{};
  location.locationId = row.textColumn(0);
  location.contentId = row.textColumn(1);
  location.rootPath = row.textColumn(2);
  location.filePath = row.textColumn(3);
  location.fileSizeBytes = static_cast<std::uint64_t>(row.int64Column(4));
  location.fileMtimeNs = row.int64Column(5);
  location.sourceFilePath = row.textColumn(6);
  if (row.columnType(7) == SQLITE_INTEGER) { location.cueTrackOffset = std::chrono::milliseconds{row.int64Column(7)}; }
  if (row.columnType(8) == SQLITE_INTEGER) { location.cueTrackIndex = static_cast<std::uint32_t>(row.int64Column(8)); }
  if (row.columnType(9) == SQLITE_INTEGER) { location.cueTrackDuration = std::chrono::milliseconds{row.int64Column(9)}; }
  if (row.columnType(10) == SQLITE_INTEGER) { location.cueFileSizeBytes = static_cast<std::uint64_t>(row.int64Column(10)); }
  if (row.columnType(11) == SQLITE_INTEGER) { location.cueFileMtimeNs = row.int64Column(11); }
  if (row.columnType(12) == SQLITE_INTEGER) { location.sourceFileSizeBytes = static_cast<std::uint64_t>(row.int64Column(12)); }
  if (row.columnType(13) == SQLITE_INTEGER) { location.sourceFileMtimeNs = row.int64Column(13); }
  if (row.columnType(14) == SQLITE_TEXT) { location.artworkPath = row.textColumn(14); }
  if (row.columnType(15) == SQLITE_TEXT) { location.thumbnailPath = row.textColumn(15); }
  location.lyricsSource = parseLyricsSource(row.textColumn(16));
  if (row.columnType(17) == SQLITE_TEXT) { location.externalLrcPath = row.textColumn(17); }
  if (row.columnType(18) == SQLITE_INTEGER) { location.externalLrcMtimeNs = row.int64Column(18); }
  if (row.columnType(19) == SQLITE_TEXT) { location.externalLrcHash = row.textColumn(19); }
  location.discoveredAt = msToSystemTime(row.int64Column(20));
  location.scannedAt = msToSystemTime(row.int64Column(21));
  return location;
}

}

int SQLiteCache::schemaVersion() const { return readUserVersion(); }

std::string SQLiteCache::journalMode() const { return readJournalMode(); }

void SQLiteCache::upsertContent(const std::string& contentId, const SongMetadata& metadata) {
  auto transaction = beginWriter();
  upsertContentNoTransaction(contentId, metadata);
  transaction.commit();
}

void SQLiteCache::recordScanRootCacheWrite(const ScanRootCacheWrite& write) {
  auto transaction = beginWriter();
  updateScanRootNoTransaction(write.root);

  for (const auto& songWrite : write.changedSongs) {
    upsertContentNoTransaction(songWrite.location.contentId, songWrite.song.metadata);
    upsertLocationNoTransaction(songWrite.location);
    replaceLyricsNoTransaction(songWrite.location.locationId, "embedded", songWrite.song.embeddedLyrics);
    replaceLyricsNoTransaction(songWrite.location.locationId, "external", songWrite.song.externalLyrics);
  }

  for (const auto& songWrite : write.changedCueTracks) {
    upsertContentNoTransaction(songWrite.location.contentId, songWrite.song.metadata);
    upsertLocationNoTransaction(songWrite.location);
    replaceLyricsNoTransaction(songWrite.location.locationId, "embedded", songWrite.song.embeddedLyrics);
    replaceLyricsNoTransaction(songWrite.location.locationId, "external", songWrite.song.externalLyrics);
  }

  for (const auto& update : write.lyricsUpdates) {
    applyLyricsCacheUpdateNoTransaction(update);
  }

  pruneDeletedLocationsNoTransaction(write.root.rootPath, write.retainedLocationIds);
  transaction.commit();
}

void SQLiteCache::upsertContentNoTransaction(const std::string& contentId, const SongMetadata& metadata) {
  if (!metadata.duration.has_value()) {
    throw std::runtime_error("content duration is required");
  }

  const auto now = std::chrono::system_clock::now();
  Statement upsert{asDb(db_), R"sql(
INSERT INTO content(content_id, title, artist, album, album_artist, genre, track_number, disc_number, year, duration_ms,
sample_rate, bit_depth, channels, play_count, rating, last_played_ms, created_at_ms, updated_at_ms)
VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18)
ON CONFLICT(content_id) DO UPDATE SET title=excluded.title, artist=excluded.artist, album=excluded.album,
album_artist=excluded.album_artist, genre=excluded.genre, track_number=excluded.track_number, disc_number=excluded.disc_number,
year=excluded.year, duration_ms=excluded.duration_ms, sample_rate=excluded.sample_rate, bit_depth=excluded.bit_depth,
channels=excluded.channels, play_count=content.play_count, rating=content.rating, last_played_ms=content.last_played_ms,
created_at_ms=content.created_at_ms, updated_at_ms=excluded.updated_at_ms;
)sql"};
  upsert.bind(1, contentId);
  upsert.bind(2, metadata.title);
  upsert.bind(3, metadata.artist);
  upsert.bind(4, metadata.album);
  upsert.bind(5, metadata.albumArtist);
  upsert.bind(6, metadata.genre);
  upsert.bindOptional(7, metadata.trackNumber);
  upsert.bindOptional(8, metadata.discNumber);
  upsert.bindOptional(9, metadata.year);
  upsert.bind(10, static_cast<std::int64_t>(metadata.duration->count()));
  upsert.bindOptional(11, metadata.sampleRate);
  upsert.bindOptional(12, metadata.bitDepth);
  upsert.bindOptional(13, metadata.channels);
  upsert.bind(14, 0LL);
  upsert.bindNull(15);
  upsert.bindNull(16);
  upsert.bind(17, systemTimeToMs(now));
  upsert.bind(18, systemTimeToMs(now));
  upsert.stepDone();
}

std::optional<CachedSong> SQLiteCache::loadContent(const std::string& contentId) const {
  std::lock_guard<std::mutex> lock(readerMutex_);
  
  auto* stmt = static_cast<sqlite3_stmt*>(contentStmt_);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  
  if (sqlite3_bind_text(stmt, 1, contentId.c_str(), static_cast<int>(contentId.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw sqliteError(asDb(db_), "bind content_id");
  }
  
  const auto result = sqlite3_step(stmt);
  if (result == SQLITE_DONE) {
    return std::nullopt;
  }
  if (result != SQLITE_ROW) {
    throw sqliteError(asDb(db_), "step content query");
  }

  CachedSong song{};
  song.metadata.trackId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  song.metadata.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  song.metadata.artist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  song.metadata.album = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  song.metadata.albumArtist = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  song.metadata.genre = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  if (sqlite3_column_type(stmt, 6) == SQLITE_INTEGER) {
    song.metadata.trackNumber = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 6));
  }
  if (sqlite3_column_type(stmt, 7) == SQLITE_INTEGER) {
    song.metadata.discNumber = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 7));
  }
  if (sqlite3_column_type(stmt, 8) == SQLITE_INTEGER) {
    song.metadata.year = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 8));
  }
  song.metadata.duration = std::chrono::milliseconds{sqlite3_column_int64(stmt, 9)};
  if (sqlite3_column_type(stmt, 10) == SQLITE_INTEGER) {
    song.metadata.sampleRate = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 10));
  }
  if (sqlite3_column_type(stmt, 11) == SQLITE_INTEGER) {
    song.metadata.bitDepth = static_cast<std::uint16_t>(sqlite3_column_int64(stmt, 11));
  }
  if (sqlite3_column_type(stmt, 12) == SQLITE_INTEGER) {
    song.metadata.channels = static_cast<std::uint16_t>(sqlite3_column_int64(stmt, 12));
  }
  
  return song;
}

void SQLiteCache::updateUserStats(const std::string& contentId, const CachedUserStats& userStats) {
  auto transaction = beginWriter();
  Statement update{asDb(db_), "UPDATE content SET play_count=?1, rating=?2, last_played_ms=?3 WHERE content_id=?4;"};
  update.bind(1, static_cast<std::int64_t>(userStats.playCount));
  if (userStats.rating != 0) {
    update.bind(2, static_cast<std::int64_t>(userStats.rating));
  } else {
    update.bindNull(2);
  }
  update.bindOptionalSystemTime(3, userStats.lastPlayed);
  update.bind(4, contentId);
  update.stepDone();
  transaction.commit();
}

void SQLiteCache::upsertLocation(const CachedLocation& location) {
  auto transaction = beginWriter();
  upsertLocationNoTransaction(location);
  transaction.commit();
}

void SQLiteCache::upsertLocationNoTransaction(const CachedLocation& location) {
  Statement removeReplacedPath{asDb(db_),
                               "DELETE FROM locations "
                               "WHERE root_path=?1 "
                               "AND file_path=?2 "
                               "AND COALESCE(cue_track_offset_ms, -1)=?3 "
                               "AND COALESCE(cue_track_index, -1)=?4 "
                               "AND location_id<>?5;"};
  removeReplacedPath.bind(1, pathText(location.rootPath));
  removeReplacedPath.bind(2, pathText(location.filePath));
  removeReplacedPath.bind(3, location.cueTrackOffset.has_value() ? location.cueTrackOffset->count() : -1);
  removeReplacedPath.bind(4, location.cueTrackIndex.has_value() ? static_cast<std::int64_t>(*location.cueTrackIndex) : -1);
  removeReplacedPath.bind(5, location.locationId);
  removeReplacedPath.stepDone();

  Statement upsert{
      asDb(db_),
      "INSERT INTO locations("
      "location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, "
      "source_file_path, cue_track_offset_ms, cue_track_index, cue_track_duration_ms, "
      "cue_file_size_bytes, cue_file_mtime_ns, source_file_size_bytes, source_file_mtime_ns, "
      "artwork_path, thumbnail_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, "
      "external_lrc_hash, discovered_at_ms, scanned_at_ms"
      ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22) "
      "ON CONFLICT(location_id) DO UPDATE SET "
      "content_id=excluded.content_id, "
      "root_path=excluded.root_path, "
      "file_path=excluded.file_path, "
      "file_size_bytes=excluded.file_size_bytes, "
      "file_mtime_ns=excluded.file_mtime_ns, "
      "source_file_path=excluded.source_file_path, "
      "cue_track_offset_ms=excluded.cue_track_offset_ms, "
      "cue_track_index=excluded.cue_track_index, "
      "cue_track_duration_ms=excluded.cue_track_duration_ms, "
      "cue_file_size_bytes=excluded.cue_file_size_bytes, "
      "cue_file_mtime_ns=excluded.cue_file_mtime_ns, "
      "source_file_size_bytes=excluded.source_file_size_bytes, "
      "source_file_mtime_ns=excluded.source_file_mtime_ns, "
      "artwork_path=excluded.artwork_path, "
      "thumbnail_path=excluded.thumbnail_path, "
      "lyrics_source=excluded.lyrics_source, "
      "external_lrc_path=excluded.external_lrc_path, "
      "external_lrc_mtime_ns=excluded.external_lrc_mtime_ns, "
      "external_lrc_hash=excluded.external_lrc_hash, "
      "discovered_at_ms=locations.discovered_at_ms, "
      "scanned_at_ms=excluded.scanned_at_ms;"};
  upsert.bind(1, location.locationId);
  upsert.bind(2, location.contentId);
  upsert.bind(3, pathText(location.rootPath));
  upsert.bind(4, pathText(location.filePath));
  upsert.bind(5, static_cast<std::int64_t>(location.fileSizeBytes));
  upsert.bind(6, location.fileMtimeNs);
  upsert.bind(7, pathText(location.sourceFilePath));
  upsert.bindOptionalMilliseconds(8, location.cueTrackOffset);
  upsert.bindOptional(9, location.cueTrackIndex);
  upsert.bindOptionalMilliseconds(10, location.cueTrackDuration);
  upsert.bindOptionalUint64(11, location.cueFileSizeBytes);
  upsert.bindOptionalInt64(12, location.cueFileMtimeNs);
  upsert.bindOptionalUint64(13, location.sourceFileSizeBytes);
  upsert.bindOptionalInt64(14, location.sourceFileMtimeNs);
  upsert.bindOptionalPath(15, location.artworkPath);
  upsert.bindOptionalPath(16, location.thumbnailPath);
  upsert.bind(17, lyricsSourceText(location.lyricsSource));
  upsert.bindOptionalPath(18, location.externalLrcPath);
  upsert.bindOptionalInt64(19, location.externalLrcMtimeNs);
  upsert.bindOptionalString(20, location.externalLrcHash);
  upsert.bind(21, systemTimeToMs(location.discoveredAt));
  upsert.bind(22, systemTimeToMs(location.scannedAt));
  upsert.stepDone();
}

std::optional<CachedLocation> SQLiteCache::loadLocation(const std::string& locationId) const {
  std::lock_guard<std::mutex> lock(readerMutex_);
  
  auto* stmt = static_cast<sqlite3_stmt*>(locationStmt_);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  
  if (sqlite3_bind_text(stmt, 1, locationId.c_str(), static_cast<int>(locationId.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    throw sqliteError(asDb(db_), "bind location_id");
  }
  
  const auto result = sqlite3_step(stmt);
  if (result == SQLITE_DONE) {
    return std::nullopt;
  }
  if (result != SQLITE_ROW) {
    throw sqliteError(asDb(db_), "step location query");
  }
  
  CachedLocation location{};
  location.locationId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  location.contentId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  location.rootPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  location.filePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  location.fileSizeBytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
  location.fileMtimeNs = sqlite3_column_int64(stmt, 5);
  location.sourceFilePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  if (sqlite3_column_type(stmt, 7) == SQLITE_INTEGER) {
    location.cueTrackOffset = std::chrono::milliseconds{sqlite3_column_int64(stmt, 7)};
  }
  if (sqlite3_column_type(stmt, 8) == SQLITE_INTEGER) {
    location.cueTrackIndex = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 8));
  }
  if (sqlite3_column_type(stmt, 9) == SQLITE_INTEGER) {
    location.cueTrackDuration = std::chrono::milliseconds{sqlite3_column_int64(stmt, 9)};
  }
  if (sqlite3_column_type(stmt, 10) == SQLITE_INTEGER) {
    location.cueFileSizeBytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 10));
  }
  if (sqlite3_column_type(stmt, 11) == SQLITE_INTEGER) {
    location.cueFileMtimeNs = sqlite3_column_int64(stmt, 11);
  }
  if (sqlite3_column_type(stmt, 12) == SQLITE_INTEGER) {
    location.sourceFileSizeBytes = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 12));
  }
  if (sqlite3_column_type(stmt, 13) == SQLITE_INTEGER) {
    location.sourceFileMtimeNs = sqlite3_column_int64(stmt, 13);
  }
  if (sqlite3_column_type(stmt, 14) == SQLITE_TEXT) {
    location.artworkPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
  }
  if (sqlite3_column_type(stmt, 15) == SQLITE_TEXT) {
    location.thumbnailPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 15));
  }
  location.lyricsSource = parseLyricsSource(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16)));
  if (sqlite3_column_type(stmt, 17) == SQLITE_TEXT) {
    location.externalLrcPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
  }
  if (sqlite3_column_type(stmt, 18) == SQLITE_INTEGER) {
    location.externalLrcMtimeNs = sqlite3_column_int64(stmt, 18);
  }
  if (sqlite3_column_type(stmt, 19) == SQLITE_TEXT) {
    location.externalLrcHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
  }
  location.discoveredAt = msToSystemTime(sqlite3_column_int64(stmt, 20));
  location.scannedAt = msToSystemTime(sqlite3_column_int64(stmt, 21));
  return location;
}

std::vector<CachedLocation> SQLiteCache::loadLocationsByRoot(const std::filesystem::path& rootPath) const {
  std::lock_guard<std::mutex> lock(readerMutex_);
  
  Statement select{
      asDb(db_),
      "SELECT location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, "
      "source_file_path, cue_track_offset_ms, cue_track_index, cue_track_duration_ms, "
      "cue_file_size_bytes, cue_file_mtime_ns, source_file_size_bytes, source_file_mtime_ns, "
      "artwork_path, thumbnail_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, "
      "external_lrc_hash, discovered_at_ms, scanned_at_ms "
      "FROM locations WHERE root_path=?1 ORDER BY file_path;"};
  select.bind(1, pathText(rootPath));
  std::vector<CachedLocation> locations;
  while (select.stepRow()) {
    locations.push_back(readLocation(select));
  }
  return locations;
}

void SQLiteCache::pruneDeletedLocations(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedLocationIds) {
  auto transaction = beginWriter();
  pruneDeletedLocationsNoTransaction(rootPath, retainedLocationIds);
  transaction.commit();
}

void SQLiteCache::pruneDeletedLocationsNoTransaction(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedLocationIds) {
  Statement mark{asDb(db_), "CREATE TEMP TABLE IF NOT EXISTS retained_locations(location_id TEXT PRIMARY KEY);"};
  mark.stepDone();
  Statement clear{asDb(db_), "DELETE FROM retained_locations;"};
  clear.stepDone();
  for (const auto& locationId : retainedLocationIds) {
    Statement insert{asDb(db_), "INSERT OR IGNORE INTO retained_locations(location_id) VALUES(?1);"};
    insert.bind(1, locationId);
    insert.stepDone();
  }
  Statement prune{asDb(db_),
                  "DELETE FROM locations "
                  "WHERE root_path=?1 "
                  "AND location_id NOT IN (SELECT location_id FROM retained_locations);"};
  prune.bind(1, pathText(rootPath));
  prune.stepDone();
}

std::int64_t SQLiteCache::deleteLocationsByPathPrefix(const std::string& rootPath, const std::string& filePathPrefix) {
  auto transaction = beginWriter();
  const auto deleted = deleteLocationsByPathPrefixNoTransaction(rootPath, filePathPrefix);
  transaction.commit();
  return deleted;
}

std::int64_t SQLiteCache::deleteLocationsByPathPrefixNoTransaction(const std::string& rootPath, const std::string& filePathPrefix) {
  Statement remove{
      asDb(db_),
      "DELETE FROM locations "
      "WHERE root_path=?1 AND ("
      "file_path=?2 OR substr(file_path, 1, length(?3) + 1) = ?3 || '/' "
      "OR source_file_path=?4 OR substr(source_file_path, 1, length(?5) + 1) = ?5 || '/'"
      ");"};
  remove.bind(1, rootPath);
  remove.bind(2, filePathPrefix);
  remove.bind(3, filePathPrefix);
  remove.bind(4, filePathPrefix);
  remove.bind(5, filePathPrefix);
  remove.stepDone();
  return sqlite3_changes(asDb(db_));
}

std::int64_t SQLiteCache::replaceLocationsBySubtree(const std::string& rootPath, const std::string& oldPrefix, const std::string& newPrefix) {
  if (oldPrefix == newPrefix) {
    return 0;
  }

  struct RenamedRow {
    CachedLocation location;
    std::vector<LyricLine> embeddedLyrics;
    std::vector<LyricLine> externalLyrics;
  };

  auto transaction = beginWriter();

  std::vector<RenamedRow> renamed;
  const auto oldRows = loadLocationsByRoot(rootPath);
  renamed.reserve(oldRows.size());
  for (const auto& row : oldRows) {
    const auto rewrittenPath = rewritePathPrefix(pathText(row.filePath), oldPrefix, newPrefix);
    const auto rewrittenSource = rewritePathPrefix(pathText(row.sourceFilePath), oldPrefix, newPrefix);
    if (rewrittenPath == pathText(row.filePath) && rewrittenSource == pathText(row.sourceFilePath)) {
      continue;
    }
    auto moved = row;
    moved.filePath = rewrittenPath;
    moved.sourceFilePath = rewrittenSource;
    const auto mtime = std::filesystem::file_time_type{std::chrono::nanoseconds{row.fileMtimeNs}};
    moved.locationId = computeLocationId(moved.filePath, moved.fileSizeBytes, mtime, moved.cueTrackOffset, moved.cueTrackIndex);
    renamed.push_back(RenamedRow{
        .location = std::move(moved),
        .embeddedLyrics = loadLyrics(row.locationId, "embedded"),
        .externalLyrics = loadLyrics(row.locationId, "external"),
    });
  }

  deleteLocationsByPathPrefixNoTransaction(rootPath, oldPrefix);
  for (const auto& entry : renamed) {
    upsertLocationNoTransaction(entry.location);
  }
  for (const auto& entry : renamed) {
    replaceLyricsNoTransaction(entry.location.locationId, "embedded", entry.embeddedLyrics);
    replaceLyricsNoTransaction(entry.location.locationId, "external", entry.externalLyrics);
  }

  transaction.commit();
  return static_cast<std::int64_t>(renamed.size());
}

}
