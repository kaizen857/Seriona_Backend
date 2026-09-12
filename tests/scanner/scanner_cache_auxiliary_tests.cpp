#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCache: stores and retrieves scan root metadata") {
  test::TempScannerRoot temp{"cache-scan-root"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CachedScanRoot root{
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

TEST_CASE("SQLiteCache: updates existing scan root") {
  test::TempScannerRoot temp{"cache-scan-root-update"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{
    .rootPath = temp.path(),
    .directoryTreeHash = "old-hash",
    .totalFiles = 10
  });

  cache.updateScanRoot(CachedScanRoot{
    .rootPath = temp.path(),
    .directoryTreeHash = "new-hash",
    .totalFiles = 20
  });

  auto loaded = cache.loadScanRoot(temp.path());
  REQUIRE(loaded.has_value());
  CHECK(loaded->directoryTreeHash == "new-hash");
  CHECK(loaded->totalFiles == 20);
}

TEST_CASE("SQLiteCache: stores and retrieves scan errors") {
  test::TempScannerRoot temp{"cache-errors"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  std::vector<CachedScanError> errors = {
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

TEST_CASE("SQLiteCache: clears scan errors for root") {
  test::TempScannerRoot temp{"cache-clear-errors"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

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

TEST_CASE("SQLiteCache: stores and updates user stats") {
  test::TempScannerRoot temp{"cache-user-stats"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

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

TEST_CASE("SQLiteCache: prunes deleted locations for root") {
  test::TempScannerRoot temp{"cache-prune"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

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

TEST_CASE("SQLiteCache: replaces lyrics for location") {
  test::TempScannerRoot temp{"cache-replace-lyrics"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  // Create required entities for foreign keys
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});
  
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

TEST_CASE("SQLiteCache: applyLyricsCacheUpdates commits atomically and never prunes locations") {
  test::TempScannerRoot temp{"cache-apply-lyrics"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CachedScanRoot scanRoot{};
  scanRoot.rootPath = temp.path();
  cache.updateScanRoot(scanRoot);
  SongMetadata meta;
  meta.title = "Song";
  meta.duration = std::chrono::milliseconds{1000};
  cache.upsertContent("content-1", meta);
  cache.upsertContent("content-2", meta);

  CachedLocation first;
  first.locationId = "loc-1";
  first.contentId = "content-1";
  first.rootPath = temp.path();
  first.filePath = temp.path() / "first.flac";
  first.sourceFilePath = first.filePath;
  first.lyricsSource = LyricsSource::EmbeddedTag;
  cache.upsertLocation(first);

  CachedLocation second;
  second.locationId = "loc-2";
  second.contentId = "content-2";
  second.rootPath = temp.path();
  second.filePath = temp.path() / "second.flac";
  second.sourceFilePath = second.filePath;
  second.lyricsSource = LyricsSource::ExternalLrc;
  second.externalLrcPath = temp.path() / "old.lrc";
  second.externalLrcHash = "old-hash";
  cache.upsertLocation(second);
  cache.replaceLyrics("loc-2", "external", {{.timestamp = std::chrono::milliseconds{10}, .text = "old"}});

  // 批外行：不在任何更新批内，必须原样保留（锁定"不按批内 retained 剪枝/不清库"）。
  CachedLocation third;
  third.locationId = "loc-3";
  third.contentId = "content-2";
  third.rootPath = temp.path();
  third.filePath = temp.path() / "third.flac";
  third.sourceFilePath = third.filePath;
  third.lyricsSource = LyricsSource::EmbeddedTag;
  cache.upsertLocation(third);
  cache.replaceLyrics("loc-3", "embedded", {{.timestamp = std::chrono::milliseconds{30}, .text = "third embedded"}});

  // 单事务批量对账：两个 location 的歌词更新一起提交；与 ScanRootCacheWrite 的空 retained
  // prune 不同，本 API 绝不清空库中其它 location 行。
  cache.applyLyricsCacheUpdates({
      LyricsCacheUpdate{.locationId = "loc-1",
                        .externalLrcPath = temp.path() / "first.lrc",
                        .externalLrcMtimeNs = 111,
                        .externalLrcHash = "hash-1",
                        .externalLyrics = {{.timestamp = std::chrono::milliseconds{1}, .text = "first external"}},
                        .effectiveLyricsSource = LyricsSource::ExternalLrc,
                        .removeExternalLyrics = false},
      LyricsCacheUpdate{.locationId = "loc-2",
                        .externalLrcPath = std::nullopt,
                        .externalLrcMtimeNs = std::nullopt,
                        .externalLrcHash = std::nullopt,
                        .externalLyrics = {},
                        .effectiveLyricsSource = LyricsSource::EmbeddedTag,
                        .removeExternalLyrics = true},
  });

  const auto firstLyrics = cache.loadLyrics("loc-1", "external");
  REQUIRE(firstLyrics.size() == 1U);
  CHECK(firstLyrics[0].text == "first external");
  CHECK(cache.loadLyrics("loc-2", "external").empty());
  REQUIRE(cache.loadLocation("loc-1").has_value());
  REQUIRE(cache.loadLocation("loc-2").has_value());
  CHECK(cache.loadLocation("loc-1")->lyricsSource == LyricsSource::ExternalLrc);
  CHECK(cache.loadLocation("loc-2")->lyricsSource == LyricsSource::EmbeddedTag);
  CHECK(cache.loadLocationsByRoot(temp.path()).size() == 3U);
  const auto thirdLocation = cache.loadLocation("loc-3");
  REQUIRE(thirdLocation.has_value());
  CHECK(thirdLocation->lyricsSource == LyricsSource::EmbeddedTag);
  const auto thirdLyrics = cache.loadLyrics("loc-3", "embedded");
  REQUIRE(thirdLyrics.size() == 1U);
  CHECK(thirdLyrics[0].text == "third embedded");

  // 事务性：批内后一条因未知 location 触发歌词外键失败时，前一条必须一并回滚（不半提交）。
  CHECK_THROWS_AS(
      static_cast<void>(cache.applyLyricsCacheUpdates({
          LyricsCacheUpdate{.locationId = "loc-1",
                            .externalLrcPath = temp.path() / "first.lrc",
                            .externalLrcMtimeNs = std::nullopt,
                            .externalLrcHash = "hash-2",
                            .externalLyrics = {{.timestamp = std::chrono::milliseconds{2}, .text = "must roll back"}},
                            .effectiveLyricsSource = LyricsSource::ExternalLrc,
                            .removeExternalLyrics = false},
          LyricsCacheUpdate{.locationId = "loc-missing",
                            .externalLrcPath = std::nullopt,
                            .externalLrcMtimeNs = std::nullopt,
                            .externalLrcHash = std::nullopt,
                            .externalLyrics = {{.timestamp = std::chrono::milliseconds{3}, .text = "fk violation"}},
                            .effectiveLyricsSource = LyricsSource::ExternalLrc,
                            .removeExternalLyrics = false},
      })),
      std::runtime_error);
  const auto rolledBack = cache.loadLyrics("loc-1", "external");
  REQUIRE(rolledBack.size() == 1U);
  CHECK(rolledBack[0].text == "first external");
  CHECK(cache.loadLocationsByRoot(temp.path()).size() == 3U);
}

TEST_CASE("SQLiteCache: recordScanRootCacheWrite batches root songs lyrics and prune") {
  test::TempScannerRoot temp{"cache-batch-write"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path(), .directoryTreeHash = "old-hash", .totalFiles = 2});

  SongMetadata staleMeta;
  staleMeta.title = "Stale";
  staleMeta.duration = std::chrono::milliseconds{90000};
  cache.upsertContent("content-stale", staleMeta);
  CachedLocation staleLocation;
  staleLocation.locationId = "loc-stale";
  staleLocation.contentId = "content-stale";
  staleLocation.rootPath = temp.path();
  staleLocation.filePath = temp.path() / "stale.mp3";
  staleLocation.sourceFilePath = staleLocation.filePath;
  cache.upsertLocation(staleLocation);

  SongMetadata lyricsOnlyMeta;
  lyricsOnlyMeta.title = "Lyrics Only Seed";
  lyricsOnlyMeta.duration = std::chrono::milliseconds{100000};
  cache.upsertContent("content-lyrics", lyricsOnlyMeta);
  CachedLocation lyricsOnlyLocationSeed;
  lyricsOnlyLocationSeed.locationId = "loc-lyrics";
  lyricsOnlyLocationSeed.contentId = "content-lyrics";
  lyricsOnlyLocationSeed.rootPath = temp.path();
  lyricsOnlyLocationSeed.filePath = temp.path() / "lyrics-only.flac";
  lyricsOnlyLocationSeed.sourceFilePath = lyricsOnlyLocationSeed.filePath;
  lyricsOnlyLocationSeed.lyricsSource = LyricsSource::ExternalLrc;
  lyricsOnlyLocationSeed.externalLrcPath = temp.path() / "old.lrc";
  lyricsOnlyLocationSeed.externalLrcMtimeNs = 111;
  lyricsOnlyLocationSeed.externalLrcHash = "old-hash";
  cache.upsertLocation(lyricsOnlyLocationSeed);
  cache.replaceLyrics("loc-lyrics", "external", {{.timestamp = std::chrono::milliseconds{1000}, .text = "old external line"}});

  SongMetadata normalMeta;
  normalMeta.title = "Changed Normal";
  normalMeta.artist = "Batch Artist";
  normalMeta.duration = std::chrono::milliseconds{180000};

  SongMetadata cueMeta;
  cueMeta.title = "Changed CUE Track";
  cueMeta.duration = std::chrono::milliseconds{45000};

  const auto normalFile = temp.path() / "changed-normal.flac";
  const auto cueFile = temp.path() / "album.cue";
  const auto sourceFile = temp.path() / "album.flac";

  ScanRootCacheWrite write;
  write.root.rootPath = temp.path();
  write.root.directoryTreeHash = "batched-hash";
  write.root.totalFiles = 3;
  write.root.lastScanMode = ScanMode::Incremental;
  write.root.lastScanDuration = std::chrono::milliseconds{25};

  CacheWriteSong normalWrite;
  normalWrite.song.metadata = normalMeta;
  normalWrite.song.embeddedLyrics = {{.timestamp = std::chrono::milliseconds{500}, .text = "normal embedded"}};
  normalWrite.location.locationId = "loc-normal";
  normalWrite.location.contentId = "content-normal";
  normalWrite.location.rootPath = temp.path();
  normalWrite.location.filePath = normalFile;
  normalWrite.location.fileSizeBytes = 1024;
  normalWrite.location.fileMtimeNs = 222;
  normalWrite.location.sourceFilePath = normalFile;
  normalWrite.location.lyricsSource = LyricsSource::EmbeddedTag;
  write.changedSongs.push_back(normalWrite);

  CacheWriteSong cueWrite;
  cueWrite.song.metadata = cueMeta;
  cueWrite.song.externalLyrics = {{.timestamp = std::chrono::milliseconds{1500}, .text = "cue external"}};
  cueWrite.location.locationId = "loc-cue";
  cueWrite.location.contentId = "content-cue";
  cueWrite.location.rootPath = temp.path();
  cueWrite.location.filePath = cueFile;
  cueWrite.location.fileSizeBytes = 2048;
  cueWrite.location.fileMtimeNs = 333;
  cueWrite.location.sourceFilePath = sourceFile;
  cueWrite.location.cueTrackOffset = std::chrono::milliseconds{30000};
  cueWrite.location.cueTrackIndex = 1U;
  cueWrite.location.cueTrackDuration = std::chrono::milliseconds{45000};
  cueWrite.location.cueFileSizeBytes = 2048;
  cueWrite.location.cueFileMtimeNs = 333;
  cueWrite.location.sourceFileSizeBytes = 4096;
  cueWrite.location.sourceFileMtimeNs = 444;
  cueWrite.location.lyricsSource = LyricsSource::ExternalLrc;
  write.changedCueTracks.push_back(cueWrite);

  LyricsCacheUpdate lyricsUpdate;
  lyricsUpdate.locationId = "loc-lyrics";
  lyricsUpdate.externalLrcPath = temp.path() / "new.lrc";
  lyricsUpdate.externalLrcMtimeNs = 555;
  lyricsUpdate.externalLrcHash = "new-lyrics-hash";
  lyricsUpdate.externalLyrics = {{.timestamp = std::chrono::milliseconds{2500}, .text = "new external line"}};
  write.lyricsUpdates.push_back(lyricsUpdate);
  write.retainedLocationIds = {"loc-normal", "loc-cue", "loc-lyrics"};

  CHECK_NOTHROW(cache.recordScanRootCacheWrite(write));

  const auto loadedRoot = cache.loadScanRoot(temp.path());
  REQUIRE(loadedRoot.has_value());
  CHECK(loadedRoot->directoryTreeHash == "batched-hash");
  CHECK(loadedRoot->totalFiles == 3);

  const auto normalContent = cache.loadContent("content-normal");
  REQUIRE(normalContent.has_value());
  CHECK(normalContent->metadata.title == "Changed Normal");
  const auto normalLocation = cache.loadLocation("loc-normal");
  REQUIRE(normalLocation.has_value());
  CHECK(normalLocation->filePath == normalFile);
  const auto normalLyrics = cache.loadLyrics("loc-normal", "embedded");
  REQUIRE(normalLyrics.size() == 1);
  CHECK(normalLyrics[0].text == "normal embedded");

  const auto cueLocation = cache.loadLocation("loc-cue");
  REQUIRE(cueLocation.has_value());
  CHECK(cueLocation->filePath == cueFile);
  CHECK(cueLocation->sourceFilePath == sourceFile);
  REQUIRE(cueLocation->cueTrackIndex.has_value());
  CHECK(*cueLocation->cueTrackIndex == 1U);
  REQUIRE(cueLocation->cueTrackDuration.has_value());
  CHECK(cueLocation->cueTrackDuration->count() == 45000);
  const auto cueLyrics = cache.loadLyrics("loc-cue", "external");
  REQUIRE(cueLyrics.size() == 1);
  CHECK(cueLyrics[0].text == "cue external");

  const auto lyricsOnlyContent = cache.loadContent("content-lyrics");
  REQUIRE(lyricsOnlyContent.has_value());
  CHECK(lyricsOnlyContent->metadata.title == "Lyrics Only Seed");
  const auto lyricsOnlyLocation = cache.loadLocation("loc-lyrics");
  REQUIRE(lyricsOnlyLocation.has_value());
  CHECK(lyricsOnlyLocation->lyricsSource == LyricsSource::ExternalLrc);
  CHECK(lyricsOnlyLocation->externalLrcPath == temp.path() / "new.lrc");
  CHECK(lyricsOnlyLocation->externalLrcMtimeNs == 555);
  CHECK(lyricsOnlyLocation->externalLrcHash == "new-lyrics-hash");
  const auto lyricsOnlyRows = cache.loadLyrics("loc-lyrics", "external");
  REQUIRE(lyricsOnlyRows.size() == 1);
  CHECK(lyricsOnlyRows[0].text == "new external line");

  CHECK_FALSE(cache.loadLocation("loc-stale").has_value());
}

TEST_CASE("SQLiteCache: recordScanRootCacheWrite rolls back root content and location after later failure") {
  test::TempScannerRoot temp{"cache-batch-write-rollback"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  SongMetadata validMeta;
  validMeta.title = "Written Before Failure";
  validMeta.duration = std::chrono::milliseconds{123000};

  ScanRootCacheWrite write;
  write.root.rootPath = temp.path();
  write.root.directoryTreeHash = "later-failure-hash";
  write.root.totalFiles = 1;
  CacheWriteSong validWrite;
  validWrite.song.metadata = validMeta;
  validWrite.location.locationId = "loc-written";
  validWrite.location.contentId = "content-written";
  validWrite.location.rootPath = temp.path();
  validWrite.location.filePath = temp.path() / "written-before-failure.mp3";
  validWrite.location.fileSizeBytes = 4096;
  validWrite.location.fileMtimeNs = 777;
  validWrite.location.sourceFilePath = validWrite.location.filePath;
  write.changedSongs.push_back(validWrite);
  LyricsCacheUpdate failingLaterUpdate;
  failingLaterUpdate.locationId = "missing-lyrics-location";
  failingLaterUpdate.externalLyrics = {{.timestamp = std::chrono::milliseconds{1}, .text = "this insert fails"}};
  write.lyricsUpdates.push_back(failingLaterUpdate);
  write.retainedLocationIds = {"loc-written"};

  CHECK_THROWS_AS(cache.recordScanRootCacheWrite(write), std::runtime_error);
  CHECK_FALSE(cache.loadScanRoot(temp.path()).has_value());
  CHECK_FALSE(cache.loadContent("content-written").has_value());
  CHECK_FALSE(cache.loadLocation("loc-written").has_value());
}

}
}
