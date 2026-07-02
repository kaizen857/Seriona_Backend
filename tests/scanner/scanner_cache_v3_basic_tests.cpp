#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCacheV3: initializes with schema version 3 and WAL journal mode") {
  test::TempScannerRoot temp{"scanner-cache-v3-init"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CHECK(cache.schemaVersion() == 3);
  CHECK(cache.journalMode() == "wal");
  
  CHECK(std::filesystem::exists(temp.dbPath()));
}

TEST_CASE("SQLiteCacheV3: rejects directory path as invalid database file") {
  test::TempScannerRoot temp{"scanner-cache-v3-invalid-path"};

  CHECK_THROWS_AS(([&temp] {
    SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.path()}};
    static_cast<void>(cache);
  }()),
                  std::runtime_error);
}

TEST_CASE("SQLiteCacheV3: upsertContent stores and retrieves data") {
  test::TempScannerRoot temp{"scanner-cache-v3-upsert"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

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

TEST_CASE("SQLiteCacheV3: multiple upserts work correctly") {
  test::TempScannerRoot temp{"scanner-cache-v3-multi-upsert"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

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

TEST_CASE("SQLiteCacheV3: handles empty database gracefully") {
  test::TempScannerRoot temp{"scanner-cache-v3-empty"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CHECK_FALSE(cache.loadContent("non-existent").has_value());
  CHECK_FALSE(cache.loadLocation("non-existent").has_value());
  CHECK_FALSE(cache.loadScanRoot(temp.path()).has_value());
}

TEST_CASE("SQLiteCacheV3: upsert updates existing content") {
  test::TempScannerRoot temp{"scanner-cache-v3-update"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

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

TEST_CASE("SQLiteCacheV3: beginWriter transaction can be committed") {
  test::TempScannerRoot temp{"scanner-cache-v3-writer"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  // When using beginWriter, don't call upsert methods that create their own transactions
  {
    auto writer = cache.beginWriter();
    // Just test that transaction works
    writer.commit();
  }

  // Verify cache is still usable
  CHECK_FALSE(cache.loadContent("test").has_value());
}

}
}
