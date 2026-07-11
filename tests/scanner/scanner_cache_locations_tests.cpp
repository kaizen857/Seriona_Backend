#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/song_identity.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCache: stores and retrieves location without CUE offset") {
  test::TempScannerRoot temp{"cache-location-basic"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

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
    .cueTrackOffset = std::nullopt,
    .artworkPath = temp.path() / "artwork" / "full.png",
    .thumbnailPath = temp.path() / "artwork" / "thumbnails" / "thumb.png"
  };

  cache.upsertLocation(location);

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-1");
  CHECK(loaded->filePath == filePath);
  CHECK(loaded->fileSizeBytes == 1024);
  CHECK_FALSE(loaded->cueTrackOffset.has_value());
  CHECK(loaded->artworkPath == temp.path() / "artwork" / "full.png");
  CHECK(loaded->thumbnailPath == temp.path() / "artwork" / "thumbnails" / "thumb.png");
}

TEST_CASE("SQLiteCache: stores and retrieves CUE track location with offset") {
  test::TempScannerRoot temp{"cache-location-cue"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.title = "Track 1";
  meta.duration = std::chrono::milliseconds{30000};
  cache.upsertContent("content-track-1", meta);

  const auto cueFile = temp.path() / "album.cue";
  const auto flacFile = temp.path() / "album.flac";
  const auto offset = std::chrono::milliseconds{30000};
	  const auto locationId = computeLocationId(cueFile, 2048, std::nullopt, offset, 0U);

  CachedLocation location{
    .locationId = locationId,
    .contentId = "content-track-1",
    .rootPath = temp.path(),
    .filePath = cueFile,
    .fileSizeBytes = 2048,
    .fileMtimeNs = 987654321,
    .sourceFilePath = flacFile,
    .cueTrackOffset = offset,
    .cueTrackIndex = 0U,
    .cueTrackDuration = std::chrono::milliseconds{30000},
    .cueFileSizeBytes = 2048,
    .cueFileMtimeNs = 987654321,
    .sourceFileSizeBytes = 4096,
    .sourceFileMtimeNs = 123456789,
    .lyricsSource = LyricsSource::ExternalLrc,
    .externalLrcPath = temp.path() / "album.lrc",
    .externalLrcMtimeNs = 777777777,
    .externalLrcHash = "external-lrc-hash"
  };

  cache.upsertLocation(location);

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-track-1");
  CHECK(loaded->filePath == cueFile);
  CHECK(loaded->sourceFilePath == flacFile);
  REQUIRE(loaded->cueTrackOffset.has_value());
  CHECK(loaded->cueTrackOffset->count() == 30000);
  REQUIRE(loaded->cueTrackIndex.has_value());
  CHECK(*loaded->cueTrackIndex == 0U);
  REQUIRE(loaded->cueTrackDuration.has_value());
  CHECK(loaded->cueTrackDuration->count() == 30000);
  REQUIRE(loaded->cueFileSizeBytes.has_value());
  CHECK(*loaded->cueFileSizeBytes == 2048);
  REQUIRE(loaded->cueFileMtimeNs.has_value());
  CHECK(*loaded->cueFileMtimeNs == 987654321);
  REQUIRE(loaded->sourceFileSizeBytes.has_value());
  CHECK(*loaded->sourceFileSizeBytes == 4096);
  REQUIRE(loaded->sourceFileMtimeNs.has_value());
  CHECK(*loaded->sourceFileMtimeNs == 123456789);
  CHECK(loaded->lyricsSource == LyricsSource::ExternalLrc);
  CHECK(loaded->externalLrcPath == temp.path() / "album.lrc");
  CHECK(loaded->externalLrcMtimeNs == 777777777);
  CHECK(loaded->externalLrcHash == "external-lrc-hash");
}

TEST_CASE("SQLiteCache: keeps distinct CUE track indexes and replaces duplicate CUE identity") {
  test::TempScannerRoot temp{"cache-location-cue-identity"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta1, meta2, meta3;
  meta1.title = "Track 1";
  meta1.duration = std::chrono::milliseconds{30000};
  meta2.title = "Track 2";
  meta2.duration = std::chrono::milliseconds{35000};
  meta3.title = "Track 2 Replacement";
  meta3.duration = std::chrono::milliseconds{36000};
  cache.upsertContent("content-track-1", meta1);
  cache.upsertContent("content-track-2", meta2);
  cache.upsertContent("content-track-2-replacement", meta3);

  const auto cueFile = temp.path() / "album.cue";
  const auto flacFile = temp.path() / "album.flac";
  const auto offset = std::chrono::milliseconds{30000};

  cache.upsertLocation(CachedLocation{
    .locationId = "cue-track-0",
    .contentId = "content-track-1",
    .rootPath = temp.path(),
    .filePath = cueFile,
    .fileSizeBytes = 2048,
    .fileMtimeNs = 111,
    .sourceFilePath = flacFile,
    .cueTrackOffset = offset,
    .cueTrackIndex = 0U,
    .cueTrackDuration = std::chrono::milliseconds{30000}
  });
  cache.upsertLocation(CachedLocation{
    .locationId = "cue-track-1",
    .contentId = "content-track-2",
    .rootPath = temp.path(),
    .filePath = cueFile,
    .fileSizeBytes = 2048,
    .fileMtimeNs = 111,
    .sourceFilePath = flacFile,
    .cueTrackOffset = offset,
    .cueTrackIndex = 1U,
    .cueTrackDuration = std::chrono::milliseconds{35000}
  });
  cache.upsertLocation(CachedLocation{
    .locationId = "cue-track-1-replacement",
    .contentId = "content-track-2-replacement",
    .rootPath = temp.path(),
    .filePath = cueFile,
    .fileSizeBytes = 2048,
    .fileMtimeNs = 111,
    .sourceFilePath = flacFile,
    .cueTrackOffset = offset,
    .cueTrackIndex = 1U,
    .cueTrackDuration = std::chrono::milliseconds{36000}
  });

  CHECK(cache.loadLocation("cue-track-0").has_value());
  CHECK_FALSE(cache.loadLocation("cue-track-1").has_value());
  auto replacement = cache.loadLocation("cue-track-1-replacement");
  REQUIRE(replacement.has_value());
  CHECK(replacement->contentId == "content-track-2-replacement");
  CHECK(replacement->cueTrackIndex == 1U);
  REQUIRE(replacement->cueTrackDuration.has_value());
  CHECK(replacement->cueTrackDuration->count() == 36000);

  const auto locations = cache.loadLocationsByRoot(temp.path());
  CHECK(locations.size() == 2);
}

TEST_CASE("SQLiteCache: loads locations by root path") {
  test::TempScannerRoot temp{"cache-location-by-root"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

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
    .sourceFilePath = file1,
    .artworkPath = temp.path() / "artwork" / "song1.png",
    .thumbnailPath = temp.path() / "artwork" / "thumbnails" / "song1.png"
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
  REQUIRE(locations.size() == 2);
  CHECK(locations[0].thumbnailPath == temp.path() / "artwork" / "thumbnails" / "song1.png");
}

TEST_CASE("SQLiteCache: updates existing location") {
  test::TempScannerRoot temp{"cache-location-update"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

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
    .sourceFilePath = filePath,
    .thumbnailPath = temp.path() / "old-thumb.png"
  });

  cache.upsertLocation(CachedLocation{
    .locationId = locationId,
    .contentId = "content-new",
    .rootPath = temp.path(),
    .filePath = filePath,
    .fileSizeBytes = 1500,
    .sourceFilePath = filePath,
    .thumbnailPath = temp.path() / "new-thumb.png"
  });

  auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->contentId == "content-new");
  CHECK(loaded->thumbnailPath == temp.path() / "new-thumb.png");
}

}  // namespace
}  // namespace seriona::scanner::cache
