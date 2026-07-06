#include <doctest.h>

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <sqlite3.h>
#include <string>
#include <vector>
#include <utility>

namespace {

[[nodiscard]] std::filesystem::path schemaV3Path() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "src/scanner/cache/schema_v3.sql";
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input{path};
  REQUIRE(input.is_open());
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> sqliteObjects(sqlite3* db) {
  std::vector<std::pair<std::string, std::string>> objects;
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, "SELECT name, type FROM sqlite_master WHERE type IN ('table', 'index') ORDER BY type, name;", -1,
                              &statement, nullptr) == SQLITE_OK);
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* nameText = sqlite3_column_text(statement, 0);
    const auto* typeText = sqlite3_column_text(statement, 1);
    objects.emplace_back(reinterpret_cast<const char*>(nameText), reinterpret_cast<const char*>(typeText));
  }
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return objects;
}

[[nodiscard]] bool hasObject(const std::vector<std::pair<std::string, std::string>>& objects,
                             const std::string& name,
                             const std::string& type) {
  return std::find(objects.begin(), objects.end(), std::pair{name, type}) != objects.end();
}

[[nodiscard]] int scalarInt(sqlite3* db, const char* sql) {
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const auto value = sqlite3_column_int(statement, 0);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return value;
}

[[nodiscard]] std::vector<std::string> sqliteColumnNames(sqlite3* db, const char* tableName) {
  std::vector<std::string> columns;
  const std::string sql = "PRAGMA table_info('" + std::string{tableName} + "');";
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* nameText = sqlite3_column_text(statement, 1);
    columns.emplace_back(reinterpret_cast<const char*>(nameText));
  }
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return columns;
}

[[nodiscard]] bool hasColumn(const std::vector<std::string>& columns, const std::string& name) {
  return std::find(columns.begin(), columns.end(), name) != columns.end();
}

}

TEST_CASE("sqlite scanner cache schema v3 artifact validates against a temp database") {
  const auto tempDatabasePath = std::filesystem::temp_directory_path() / "seriona-scanner-schema-v3-tests.sqlite";
  std::error_code removeError;
  std::filesystem::remove(tempDatabasePath, removeError);

  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(tempDatabasePath.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);

  std::error_code readError;
  const auto schemaPath = schemaV3Path();
  REQUIRE(std::filesystem::exists(schemaPath, readError));
  const auto sql = readTextFile(schemaPath);

  REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "PRAGMA user_version;", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(scalarInt(db, "PRAGMA user_version;") == 3);

  const auto objects = sqliteObjects(db);

  CHECK(hasObject(objects, "content", "table"));
  CHECK(hasObject(objects, "locations", "table"));
  CHECK(hasObject(objects, "lyrics", "table"));
  CHECK(hasObject(objects, "scan_roots", "table"));
  CHECK(hasObject(objects, "scan_errors", "table"));
  CHECK(hasObject(objects, "idx_content_album", "index"));
  CHECK(hasObject(objects, "idx_content_artist", "index"));
  CHECK(hasObject(objects, "idx_locations_content", "index"));
  CHECK(hasObject(objects, "idx_locations_root", "index"));
  CHECK(hasObject(objects, "idx_locations_path", "index"));
  CHECK(hasObject(objects, "idx_lyrics_location", "index"));
  CHECK(hasObject(objects, "idx_errors_root", "index"));

  CHECK(hasColumn(sqliteColumnNames(db, "content"), "content_id"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "title"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "artist"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "album"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "album_artist"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "genre"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "track_number"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "disc_number"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "year"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "duration_ms"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "sample_rate"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "bit_depth"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "channels"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "play_count"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "rating"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "last_played_ms"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "created_at_ms"));
  CHECK(hasColumn(sqliteColumnNames(db, "content"), "updated_at_ms"));

  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "location_id"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "content_id"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "root_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "file_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "file_size_bytes"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "file_mtime_ns"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "source_file_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "cue_track_offset_ms"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "artwork_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "thumbnail_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "lyrics_source"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "external_lrc_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "external_lrc_mtime_ns"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "discovered_at_ms"));
  CHECK(hasColumn(sqliteColumnNames(db, "locations"), "scanned_at_ms"));

  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "id"));
  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "root_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "file_path"));
  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "error_code"));
  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "error_message"));
  CHECK(hasColumn(sqliteColumnNames(db, "scan_errors"), "occurred_at_ms"));

  CHECK(sqlite3_exec(db, "INSERT INTO content(content_id, title, artist, album, album_artist, genre, track_number, disc_number, year, duration_ms, sample_rate, bit_depth, channels, play_count, rating, last_played_ms, created_at_ms, updated_at_ms) VALUES('content-1', 'Song', 'Artist', 'Album', 'Album Artist', 'Genre', 1, 1, 2026, 180000, 48000, 24, 2, 0, NULL, NULL, 1, 1);", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO scan_roots(root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms) VALUES('/music', 'hash', 1, 'Full', 123, 456);", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO locations(location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, source_file_path, cue_track_offset_ms, artwork_path, thumbnail_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, discovered_at_ms, scanned_at_ms) VALUES('location-1', 'content-1', '/music', '/music/track.flac', 4096, 1000, '/music/track.flac', 5, '/music/cover.jpg', '/music/thumb.jpg', 'embedded', '/music/track.lrc', 2000, 10, 11);", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO lyrics(location_id, kind, line_index, timestamp_ms, text) VALUES('location-1', 'embedded', 0, 10, 'line');", nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO scan_errors(root_path, file_path, error_code, error_message, occurred_at_ms) VALUES('/music', '/music/bad.flac', 'metadata_read_failed', 'bad tag', 99);", nullptr, nullptr, nullptr) == SQLITE_OK);

  CHECK(sqlite3_exec(db, "INSERT INTO scan_roots(root_path, directory_tree_hash, total_files, last_scan_mode, last_scan_duration_ms, last_scan_at_ms) VALUES('/music', 'dup', 2, 'Incremental', 456, 789);", nullptr, nullptr, nullptr) != SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO locations(location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, source_file_path, cue_track_offset_ms, artwork_path, thumbnail_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, discovered_at_ms, scanned_at_ms) VALUES('location-2', 'content-1', '/music', '/music/track.flac', 4096, 1000, '/music/track.flac', 5, '/music/cover.jpg', '/music/thumb.jpg', 'embedded', '/music/track.lrc', 2000, 10, 11);", nullptr, nullptr, nullptr) != SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO locations(location_id, content_id, root_path, file_path, file_size_bytes, file_mtime_ns, source_file_path, cue_track_offset_ms, artwork_path, thumbnail_path, lyrics_source, external_lrc_path, external_lrc_mtime_ns, discovered_at_ms, scanned_at_ms) VALUES('location-3', NULL, '/music', '/music/other.flac', 4096, 1000, '/music/other.flac', NULL, NULL, NULL, 'embedded', NULL, NULL, 10, 11);", nullptr, nullptr, nullptr) != SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO lyrics(location_id, kind, line_index, timestamp_ms, text) VALUES('location-1', 'embedded', 0, 20, 'duplicate');", nullptr, nullptr, nullptr) != SQLITE_OK);
  CHECK(sqlite3_exec(db, "INSERT INTO scan_errors(root_path, file_path, error_code, error_message, occurred_at_ms) VALUES('/music', '/music/missing.flac', 'metadata_read_failed', 'bad tag', 99);", nullptr, nullptr, nullptr) == SQLITE_OK);


  REQUIRE(sqlite3_close(db) == SQLITE_OK);
  std::filesystem::remove(tempDatabasePath, removeError);
}
