#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCacheV3: stores and retrieves scan root metadata") {
  test::TempScannerRoot temp{"cache-v3-scan-root"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CachedScanRootV3 root{
    .rootPath = temp.path(),
    .directoryTreeHash = "abc123hash",
    .totalFiles = 42,
    .lastScanMode = ScanMode::Full,
    .lastScanDuration = std::chrono::milliseconds{1500},
    .lastScanAt = std::chrono::system_clock::now()
  };

  cache.updateScanRoot(root);

  auto loaded = cache.loadScanRoot(temp.path());
  REQUIRE(loaded.has_value());
  CHECK(loaded->directoryTreeHash == "abc123hash");
  CHECK(loaded->totalFiles == 42);
  CHECK(loaded->lastScanMode == ScanMode::Full);
  CHECK(loaded->lastScanDuration.count() == 1500);
}

TEST_CASE("SQLiteCacheV3: updates existing scan root") {
  test::TempScannerRoot temp{"cache-v3-scan-root-update"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{
    .rootPath = temp.path(),
    .directoryTreeHash = "old-hash",
    .totalFiles = 10
  });

  cache.updateScanRoot(CachedScanRootV3{
    .rootPath = temp.path(),
    .directoryTreeHash = "new-hash",
    .totalFiles = 20
  });

  auto loaded = cache.loadScanRoot(temp.path());
  REQUIRE(loaded.has_value());
  CHECK(loaded->directoryTreeHash == "new-hash");
  CHECK(loaded->totalFiles == 20);
}

TEST_CASE("SQLiteCacheV3: stores and retrieves scan errors") {
  test::TempScannerRoot temp{"cache-v3-errors"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  std::vector<CachedScanErrorV3> errors = {
    {
      .rootPath = temp.path(),
      .filePath = temp.path() / "bad1.mp3",
      .errorCode = ScannerErrorCode::MetadataReadFailed,
      .errorMessage = "Error 1"
    },
    {
      .rootPath = temp.path(),
      .filePath = temp.path() / "bad2.flac",
      .errorCode = ScannerErrorCode::MetadataReadFailed,
      .errorMessage = "Error 2"
    }
  };

  cache.saveErrors(temp.path(), errors);

  auto loaded = cache.loadErrors(temp.path());
  REQUIRE(loaded.size() == 2);
  CHECK(loaded[0].errorMessage == "Error 1");
  CHECK(loaded[1].errorMessage == "Error 2");
}

TEST_CASE("SQLiteCacheV3: clears scan errors for root") {
  test::TempScannerRoot temp{"cache-v3-clear-errors"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  cache.saveErrors(temp.path(), {
    {
      .rootPath = temp.path(),
      .filePath = temp.path() / "file.mp3",
      .errorCode = ScannerErrorCode::MetadataReadFailed,
      .errorMessage = "Error"
    }
  });

  cache.clearErrors(temp.path());

  auto loaded = cache.loadErrors(temp.path());
  CHECK(loaded.empty());
}

TEST_CASE("SQLiteCacheV3: stores and updates user stats") {
  test::TempScannerRoot temp{"cache-v3-user-stats"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  const std::string contentId = "content-123";
  SongMetadata meta;
  meta.title = "Test";
  meta.duration = std::chrono::milliseconds{100000};
  cache.upsertContent(contentId, meta);

  CachedUserStats stats{
    .playCount = 42,
    .rating = 5,
    .lastPlayed = std::chrono::system_clock::now()
  };

  cache.updateUserStats(contentId, stats);

  auto content = cache.loadContent(contentId);
  REQUIRE(content.has_value());
  CHECK(content->metadata.title == "Test");
}

TEST_CASE("SQLiteCacheV3: prunes deleted locations for root") {
  test::TempScannerRoot temp{"cache-v3-prune"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  SongMetadata meta1, meta2;
  meta1.title = "Keep";
  meta1.duration = std::chrono::milliseconds{100000};
  meta2.title = "Delete";
  meta2.duration = std::chrono::milliseconds{120000};
  cache.upsertContent("c1", meta1);
  cache.upsertContent("c2", meta2);

  const auto file1 = temp.path() / "keep.mp3";
  const auto file2 = temp.path() / "delete.mp3";

  cache.upsertLocation(CachedLocation{
    .locationId = "loc-keep",
    .contentId = "c1",
    .rootPath = temp.path(),
    .filePath = file1,
    .sourceFilePath = file1
  });
  cache.upsertLocation(CachedLocation{
    .locationId = "loc-delete",
    .contentId = "c2",
    .rootPath = temp.path(),
    .filePath = file2,
    .sourceFilePath = file2
  });

  cache.pruneDeletedLocations(temp.path(), {"loc-keep"});

  CHECK(cache.loadLocation("loc-keep").has_value());
  CHECK_FALSE(cache.loadLocation("loc-delete").has_value());
}

TEST_CASE("SQLiteCacheV3: replaces lyrics for location") {
  test::TempScannerRoot temp{"cache-v3-replace-lyrics"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  // Create required entities for foreign keys
  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});
  
  SongMetadata meta;
  meta.title = "Song with lyrics";
  meta.duration = std::chrono::milliseconds{200000};
  cache.upsertContent("content-1", meta);

  const std::string locationId = "loc-1";
  cache.upsertLocation(CachedLocation{
    .locationId = locationId,
    .contentId = "content-1",
    .rootPath = temp.path(),
    .filePath = temp.path() / "song.mp3",
    .sourceFilePath = temp.path() / "song.mp3"
  });

  std::vector<LyricLine> lyrics1 = {
    {.timestamp = std::chrono::milliseconds{1000}, .text = "Line 1"}
  };
  std::vector<LyricLine> lyrics2 = {
    {.timestamp = std::chrono::milliseconds{2000}, .text = "Line 2"}
  };

  cache.replaceLyrics(locationId, "embedded", lyrics1);
  cache.replaceLyrics(locationId, "embedded", lyrics2);

  auto loaded = cache.loadLyrics(locationId, "embedded");
  REQUIRE(loaded.size() == 1);
  CHECK(loaded[0].text == "Line 2");
}

}
}
