#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
#include "seriona/scanner/song_identity.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCacheV3: stores and retrieves location without CUE offset") {
  test::TempScannerRoot temp{"cache-v3-location-basic"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  SongMetadata meta;
  meta.title = "Test Song";
  meta.duration = std::chrono::milliseconds{180000};
  cache.upsertContent("content-1", meta);

  const auto filePath = temp.path() / "music" / "song.flac";
  const auto locationId = computeLocationId(filePath, 1024, std::nullopt);

  CachedLocation location{
    .locationId = locationId,
    .contentId = "content-1",
    .rootPath = temp.path(),
    .filePath = filePath,
    .fileSizeBytes = 1024,
    .fileMtimeNs = 123456789,
    .sourceFilePath = filePath,
    .cueTrackOffset = std::nullopt
  };

  cache.upsertLocation(location);

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-1");
  CHECK(loaded->filePath == filePath);
  CHECK(loaded->fileSizeBytes == 1024);
  CHECK_FALSE(loaded->cueTrackOffset.has_value());
}

TEST_CASE("SQLiteCacheV3: stores and retrieves CUE track location with offset") {
  test::TempScannerRoot temp{"cache-v3-location-cue"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  SongMetadata meta;
  meta.title = "Track 1";
  meta.duration = std::chrono::milliseconds{30000};
  cache.upsertContent("content-track-1", meta);

  const auto cueFile = temp.path() / "album.cue";
  const auto flacFile = temp.path() / "album.flac";
  const auto offset = std::chrono::milliseconds{30000};
  const auto locationId = computeLocationId(cueFile, 2048, std::nullopt, offset);

  CachedLocation location{
    .locationId = locationId,
    .contentId = "content-track-1",
    .rootPath = temp.path(),
    .filePath = cueFile,
    .fileSizeBytes = 2048,
    .fileMtimeNs = 987654321,
    .sourceFilePath = flacFile,
    .cueTrackOffset = offset
  };

  cache.upsertLocation(location);

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-track-1");
  CHECK(loaded->filePath == cueFile);
  CHECK(loaded->sourceFilePath == flacFile);
  REQUIRE(loaded->cueTrackOffset.has_value());
  CHECK(loaded->cueTrackOffset->count() == 30000);
}

TEST_CASE("SQLiteCacheV3: loads locations by root path") {
  test::TempScannerRoot temp{"cache-v3-location-by-root"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  SongMetadata meta1, meta2;
  meta1.title = "Song 1";
  meta1.duration = std::chrono::milliseconds{100000};
  meta2.title = "Song 2";
  meta2.duration = std::chrono::milliseconds{120000};
  cache.upsertContent("c1", meta1);
  cache.upsertContent("c2", meta2);

  const auto file1 = temp.path() / "song1.mp3";
  const auto file2 = temp.path() / "song2.flac";

  const auto id1 = computeLocationId(file1, 1000, std::nullopt);
  const auto id2 = computeLocationId(file2, 2000, std::nullopt);

  cache.upsertLocation(CachedLocation{
    .locationId = id1, 
    .contentId = "c1", 
    .rootPath = temp.path(), 
    .filePath = file1, 
    .fileSizeBytes = 1000, 
    .sourceFilePath = file1
  });
  cache.upsertLocation(CachedLocation{
    .locationId = id2, 
    .contentId = "c2", 
    .rootPath = temp.path(), 
    .filePath = file2, 
    .fileSizeBytes = 2000, 
    .sourceFilePath = file2
  });

  auto locations = cache.loadLocationsByRoot(temp.path());
  CHECK(locations.size() == 2);
}

TEST_CASE("SQLiteCacheV3: updates existing location") {
  test::TempScannerRoot temp{"cache-v3-location-update"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRootV3{.rootPath = temp.path()});

  SongMetadata meta1, meta2;
  meta1.title = "Old Content";
  meta1.duration = std::chrono::milliseconds{100000};
  meta2.title = "New Content";
  meta2.duration = std::chrono::milliseconds{120000};
  cache.upsertContent("content-old", meta1);
  cache.upsertContent("content-new", meta2);

  const auto filePath = temp.path() / "song.mp3";
  const auto locationId = computeLocationId(filePath, 1500, std::nullopt);

  cache.upsertLocation(CachedLocation{
    .locationId = locationId,
    .contentId = "content-old",
    .rootPath = temp.path(),
    .filePath = filePath,
    .fileSizeBytes = 1500,
    .sourceFilePath = filePath
  });

  cache.upsertLocation(CachedLocation{
    .locationId = locationId,
    .contentId = "content-new",
    .rootPath = temp.path(),
    .filePath = filePath,
    .fileSizeBytes = 1500,
    .sourceFilePath = filePath
  });

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-new");
}

}  // namespace
}  // namespace seriona::scanner::cache
