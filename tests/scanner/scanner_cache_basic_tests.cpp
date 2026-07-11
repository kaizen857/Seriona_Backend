#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCache: initializes with schema version 3 and WAL journal mode") {
  test::TempScannerRoot temp{"scanner-cache-init"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CHECK(cache.schemaVersion() == 3);
  CHECK(cache.journalMode() == "wal");
  
  CHECK(std::filesystem::exists(temp.dbPath()));
}

TEST_CASE("SQLiteCache: rejects directory path as invalid database file") {
  test::TempScannerRoot temp{"scanner-cache-invalid-path"};

  CHECK_THROWS_AS(([&temp] {
    SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.path()}};
    static_cast<void>(cache);
  }()),
                  std::runtime_error);
}

TEST_CASE("SQLiteCache: upsertContent stores and retrieves data") {
  test::TempScannerRoot temp{"scanner-cache-upsert"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  SongMetadata metadata;
  metadata.title = "Test Song";
  metadata.artist = "Test Artist";
  metadata.duration = std::chrono::milliseconds{180000};
  
  // upsertContent manages its own transaction internally
  cache.upsertContent("content-1", metadata);

  auto loaded = cache.loadContent("content-1");
  REQUIRE(loaded.has_value());
  CHECK(loaded->metadata.title == "Test Song");
  CHECK(loaded->metadata.artist == "Test Artist");
}

TEST_CASE("SQLiteCache: multiple upserts work correctly") {
  test::TempScannerRoot temp{"scanner-cache-multi-upsert"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  SongMetadata meta1, meta2;
  meta1.title = "Song 1";
  meta1.duration = std::chrono::milliseconds{150000};
  meta2.title = "Song 2";
  meta2.duration = std::chrono::milliseconds{200000};
  
  // Each upsert manages its own transaction
  cache.upsertContent("content-1", meta1);
  cache.upsertContent("content-2", meta2);

  auto content1 = cache.loadContent("content-1");
  auto content2 = cache.loadContent("content-2");
  
  REQUIRE(content1.has_value());
  REQUIRE(content2.has_value());
  CHECK(content1->metadata.title == "Song 1");
  CHECK(content2->metadata.title == "Song 2");
}

TEST_CASE("SQLiteCache: handles empty database gracefully") {
  test::TempScannerRoot temp{"scanner-cache-empty"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CHECK_FALSE(cache.loadContent("non-existent").has_value());
  CHECK_FALSE(cache.loadLocation("non-existent").has_value());
  CHECK_FALSE(cache.loadScanRoot(temp.path()).has_value());
}

TEST_CASE("SQLiteCache: upsert updates existing content") {
  test::TempScannerRoot temp{"scanner-cache-update"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  SongMetadata meta1;
  meta1.title = "Old Title";
  meta1.duration = std::chrono::milliseconds{100000};
  cache.upsertContent("content-1", meta1);

  SongMetadata meta2;
  meta2.title = "New Title";
  meta2.duration = std::chrono::milliseconds{120000};
  cache.upsertContent("content-1", meta2);

  auto loaded = cache.loadContent("content-1");
  REQUIRE(loaded.has_value());
  CHECK(loaded->metadata.title == "New Title");
}

TEST_CASE("SQLiteCache: beginWriter transaction can be committed") {
  test::TempScannerRoot temp{"scanner-cache-writer"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  // When using beginWriter, don't call upsert methods that create their own transactions
  {
    auto writer = cache.beginWriter();
    // Just test that transaction works
    writer.commit();
  }

  // Verify cache is still usable
  CHECK_FALSE(cache.loadContent("test").has_value());
}

TEST_CASE("SQLiteCache: prepared statements reused across multiple queries") {
  test::TempScannerRoot temp{"scanner-cache-prepared-stmts"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CachedScanRoot root;
  root.rootPath = temp.path();
  root.directoryTreeHash = "test-hash";
  root.totalFiles = 3;
  root.lastScanMode = ScanMode::Full;
  root.lastScanDuration = std::chrono::milliseconds{1000};
  root.lastScanAt = std::chrono::system_clock::now();
  cache.updateScanRoot(root);

  SongMetadata meta1;
  meta1.title = "Song 1";
  meta1.artist = "Artist 1";
  meta1.album = "Album 1";
  meta1.duration = std::chrono::milliseconds{180000};
  cache.upsertContent("content-1", meta1);

  SongMetadata meta2;
  meta2.title = "Song 2";
  meta2.artist = "Artist 2";
  meta2.album = "Album 2";
  meta2.duration = std::chrono::milliseconds{200000};
  cache.upsertContent("content-2", meta2);

  SongMetadata meta3;
  meta3.title = "Song 3";
  meta3.artist = "Artist 3";
  meta3.album = "Album 3";
  meta3.duration = std::chrono::milliseconds{220000};
  cache.upsertContent("content-3", meta3);

  CachedLocation loc1;
  loc1.locationId = "loc-1";
  loc1.contentId = "content-1";
  loc1.rootPath = temp.path();
  loc1.filePath = temp.path() / "song1.flac";
  loc1.sourceFilePath = loc1.filePath;
  loc1.fileSizeBytes = 1000;
  loc1.fileMtimeNs = 123456789;
  loc1.lyricsSource = LyricsSource::None;
  loc1.discoveredAt = std::chrono::system_clock::now();
  loc1.scannedAt = std::chrono::system_clock::now();
  cache.upsertLocation(loc1);

  CachedLocation loc2 = loc1;
  loc2.locationId = "loc-2";
  loc2.contentId = "content-2";
  loc2.filePath = temp.path() / "song2.flac";
  loc2.sourceFilePath = loc2.filePath;
  cache.upsertLocation(loc2);

  CachedLocation loc3 = loc1;
  loc3.locationId = "loc-3";
  loc3.contentId = "content-3";
  loc3.filePath = temp.path() / "song3.flac";
  loc3.sourceFilePath = loc3.filePath;
  cache.upsertLocation(loc3);

  for (int i = 0; i < 100; ++i) {
    auto loaded1 = cache.loadContent("content-1");
    REQUIRE(loaded1.has_value());
    CHECK(loaded1->metadata.title == "Song 1");

    auto loaded2 = cache.loadContent("content-2");
    REQUIRE(loaded2.has_value());
    CHECK(loaded2->metadata.title == "Song 2");

    auto loaded3 = cache.loadContent("content-3");
    REQUIRE(loaded3.has_value());
    CHECK(loaded3->metadata.title == "Song 3");

    auto loadedLoc1 = cache.loadLocation("loc-1");
    REQUIRE(loadedLoc1.has_value());
    CHECK(loadedLoc1->contentId == "content-1");

    auto loadedLoc2 = cache.loadLocation("loc-2");
    REQUIRE(loadedLoc2.has_value());
    CHECK(loadedLoc2->contentId == "content-2");

    auto loadedLoc3 = cache.loadLocation("loc-3");
    REQUIRE(loadedLoc3.has_value());
    CHECK(loadedLoc3->contentId == "content-3");

    auto nonExistent = cache.loadContent("non-existent");
    CHECK_FALSE(nonExistent.has_value());

    auto nonExistentLoc = cache.loadLocation("non-existent-loc");
    CHECK_FALSE(nonExistentLoc.has_value());
  }
}

}
}
