#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/song_identity.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <map>

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

TEST_CASE("SQLiteCache: round trip keeps identical metadata lyrics and thumbnail with empty artwork") {
  test::TempScannerRoot temp{"cache-round-trip-thumbnail"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.title = "Round Trip Song";
  meta.artist = "Round Trip Artist";
  meta.duration = std::chrono::milliseconds{240000};
  cache.upsertContent("content-round-trip", meta);

  const auto filePath = temp.path() / "music" / "song.flac";
  const auto locationId = computeLocationId(filePath, 4096, std::nullopt);
  const auto thumbnail = temp.path() / "covers" / "thumbnails" / "ab" / "thumb.png";

  cache.upsertLocation(CachedLocation{
    .locationId = locationId,
    .contentId = "content-round-trip",
    .rootPath = temp.path(),
    .filePath = filePath,
    .fileSizeBytes = 4096,
    .fileMtimeNs = 123456789,
    .sourceFilePath = filePath,
    .cueTrackOffset = std::nullopt,
    .artworkPath = std::nullopt,
    .thumbnailPath = thumbnail,
    .lyricsSource = LyricsSource::EmbeddedTag
  });
  cache.replaceLyrics(locationId,
                      "embedded",
                      {LyricLine{.timestamp = std::chrono::milliseconds{1000}, .text = "first line"},
                       LyricLine{.timestamp = std::chrono::milliseconds{2000}, .text = "second line"}});

  const auto content = cache.loadContent("content-round-trip");
  REQUIRE(content.has_value());
  CHECK(content->metadata.title == "Round Trip Song");
  CHECK(content->metadata.artist == "Round Trip Artist");
  CHECK(content->metadata.duration == std::chrono::milliseconds{240000});

  const auto loaded = cache.loadLocation(locationId);
  REQUIRE(loaded.has_value());
  CHECK(loaded->thumbnailPath == thumbnail);
  CHECK_FALSE(loaded->artworkPath.has_value());
  CHECK(loaded->lyricsSource == LyricsSource::EmbeddedTag);

  const auto lyrics = cache.loadLyrics(locationId, "embedded");
  REQUIRE(lyrics.size() == 2U);
  CHECK(lyrics[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(lyrics[0].text == "first line");
  CHECK(lyrics[1].timestamp == std::chrono::milliseconds{2000});
  CHECK(lyrics[1].text == "second line");
}

TEST_CASE("SQLiteCache: deleteLocationsByPathPrefix removes subtree and keeps sibling-prefix paths") {
  test::TempScannerRoot temp{"cache-delete-by-prefix"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-a-1", "c-a-2", "c-ab-3", "c-music-4"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto musicDir = temp.path() / "Music";
  const auto aDir = musicDir / "A";
  const auto abDir = musicDir / "AB";

  auto upsertSong = [&](const std::string& locationId, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = locationId,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .sourceFilePath = file,
        .externalLrcMtimeNs = std::nullopt,
        .externalLrcHash = std::nullopt});
  };

  upsertSong("loc-a-1", "c-a-1", aDir / "song1.flac");
  upsertSong("loc-a-2", "c-a-2", aDir / "song2.mp3");
  upsertSong("loc-ab-3", "c-ab-3", abDir / "song3.flac");
  upsertSong("loc-music-4", "c-music-4", musicDir / "song4.ogg");

  const auto deleted = cache.deleteLocationsByPathPrefix(temp.path().generic_string(), aDir.generic_string());

  CHECK(deleted == 2);
  CHECK_FALSE(cache.loadLocation("loc-a-1").has_value());
  CHECK_FALSE(cache.loadLocation("loc-a-2").has_value());
  CHECK(cache.loadLocation("loc-ab-3").has_value());
  CHECK(cache.loadLocation("loc-music-4").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  REQUIRE(locations.size() == 2);
  CHECK(locations[0].filePath == abDir / "song3.flac");
  CHECK(locations[1].filePath == musicDir / "song4.ogg");
}

TEST_CASE("SQLiteCache: deleteLocationsByPathPrefix removes CUE locations and source-referenced tracks") {
  test::TempScannerRoot temp{"cache-delete-by-prefix-cue"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{30000};
  for (const auto& id : {"c-cue-0", "c-cue-1", "c-cue-2"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto albumDir = temp.path() / "Album";
  const auto cueFile = albumDir / "album.cue";
  const auto flacFile = albumDir / "album.flac";
  const auto offset = std::chrono::milliseconds{30000};

  cache.upsertLocation(CachedLocation{
      .locationId = "cue-track-0",
      .contentId = "c-cue-0",
      .rootPath = temp.path(),
      .filePath = cueFile,
      .fileSizeBytes = 2048,
      .sourceFilePath = flacFile,
      .cueTrackOffset = offset,
      .cueTrackIndex = 0U,
      .cueTrackDuration = std::chrono::milliseconds{30000},
      .externalLrcMtimeNs = std::nullopt,
      .externalLrcHash = std::nullopt});
  cache.upsertLocation(CachedLocation{
      .locationId = "cue-track-1",
      .contentId = "c-cue-1",
      .rootPath = temp.path(),
      .filePath = cueFile,
      .fileSizeBytes = 2048,
      .sourceFilePath = flacFile,
      .cueTrackOffset = offset,
      .cueTrackIndex = 1U,
      .cueTrackDuration = std::chrono::milliseconds{30000},
      .externalLrcMtimeNs = std::nullopt,
      .externalLrcHash = std::nullopt});
  cache.upsertLocation(CachedLocation{
      .locationId = "loc-source-only",
      .contentId = "c-cue-2",
      .rootPath = temp.path(),
      .filePath = temp.path() / "Other" / "song.mp3",
      .fileSizeBytes = 1024,
      .sourceFilePath = albumDir / "shared.flac",
      .externalLrcMtimeNs = std::nullopt,
      .externalLrcHash = std::nullopt});

  const auto deleted = cache.deleteLocationsByPathPrefix(temp.path().generic_string(), albumDir.generic_string());

  CHECK(deleted == 3);
  CHECK_FALSE(cache.loadLocation("cue-track-0").has_value());
  CHECK_FALSE(cache.loadLocation("cue-track-1").has_value());
  CHECK_FALSE(cache.loadLocation("loc-source-only").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  CHECK(locations.empty());
}

TEST_CASE("SQLiteCache: deleteLocationsByPathPrefixNoTransaction deletes within caller transaction") {
  test::TempScannerRoot temp{"cache-delete-by-prefix-notx"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-x-1", "c-y-1", "c-keep"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto xDir = temp.path() / "X";
  const auto yDir = temp.path() / "Y";

  auto upsertSong = [&](const std::string& locationId, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = locationId,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .sourceFilePath = file,
        .externalLrcMtimeNs = std::nullopt,
        .externalLrcHash = std::nullopt});
  };

  upsertSong("loc-x-1", "c-x-1", xDir / "song.flac");
  upsertSong("loc-y-1", "c-y-1", yDir / "song.flac");
  upsertSong("loc-keep", "c-keep", temp.path() / "keep.flac");

  auto transaction = cache.beginWriter();
  const auto deletedX = cache.deleteLocationsByPathPrefixNoTransaction(temp.path().generic_string(), xDir.generic_string());
  const auto deletedY = cache.deleteLocationsByPathPrefixNoTransaction(temp.path().generic_string(), yDir.generic_string());
  transaction.commit();

  CHECK(deletedX == 1);
  CHECK(deletedY == 1);
  CHECK_FALSE(cache.loadLocation("loc-x-1").has_value());
  CHECK_FALSE(cache.loadLocation("loc-y-1").has_value());
  CHECK(cache.loadLocation("loc-keep").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  REQUIRE(locations.size() == 1);
  CHECK(locations[0].filePath == temp.path() / "keep.flac");
}

TEST_CASE("SQLiteCache: deleteLocationsByPathPrefix treats _ literally and protects sibling directories") {
  test::TempScannerRoot temp{"cache-delete-prefix-underscore"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-album", "c-alias"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto albumDir = temp.path() / "Music" / "My_Album";
  const auto aliasDir = temp.path() / "Music" / "My-Album";

  auto upsertSong = [&](const std::string& locationId, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = locationId,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .sourceFilePath = file,
        .externalLrcMtimeNs = std::nullopt,
        .externalLrcHash = std::nullopt});
  };

  upsertSong("loc-album", "c-album", albumDir / "song.flac");
  upsertSong("loc-alias", "c-alias", aliasDir / "song2.flac");

  const auto deleted = cache.deleteLocationsByPathPrefix(temp.path().generic_string(), albumDir.generic_string());

  CHECK(deleted == 1);
  CHECK_FALSE(cache.loadLocation("loc-album").has_value());
  CHECK(cache.loadLocation("loc-alias").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  REQUIRE(locations.size() == 1);
  CHECK(locations[0].filePath == aliasDir / "song2.flac");
}

TEST_CASE("SQLiteCache: deleteLocationsByPathPrefix is case-sensitive and never matches lowercased paths") {
  test::TempScannerRoot temp{"cache-delete-prefix-case"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-upper", "c-lower"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto upperDir = temp.path() / "Music" / "A";
  const auto lowerDir = temp.path() / "music" / "a";

  auto upsertSong = [&](const std::string& locationId, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = locationId,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .sourceFilePath = file,
        .externalLrcMtimeNs = std::nullopt,
        .externalLrcHash = std::nullopt});
  };

  upsertSong("loc-upper", "c-upper", upperDir / "song.flac");
  upsertSong("loc-lower", "c-lower", lowerDir / "song2.flac");

  const auto deleted = cache.deleteLocationsByPathPrefix(temp.path().generic_string(), upperDir.generic_string());

  CHECK(deleted == 1);
  CHECK_FALSE(cache.loadLocation("loc-upper").has_value());
  CHECK(cache.loadLocation("loc-lower").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  REQUIRE(locations.size() == 1);
  CHECK(locations[0].filePath == lowerDir / "song2.flac");
}

TEST_CASE("SQLiteCache: replaceLocationsBySubtree never deletes sibling rows (underscore and case)") {
  test::TempScannerRoot temp{"cache-replace-by-subtree-siblings"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-album", "c-alias", "c-upper", "c-lower"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto musicDir = temp.path() / "Music";
  const auto albumDir = musicDir / "My_Album";
  const auto aliasDir = musicDir / "My-Album";
  const auto upperDir = musicDir / "A";
  const auto lowerDir = musicDir / "a";
  const auto newDir = musicDir / "Moved";

  constexpr std::int64_t kMtimeNs = 555;
  const auto mtime = std::filesystem::file_time_type{std::chrono::nanoseconds{kMtimeNs}};

  auto insert = [&](const std::string& id, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = id,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .fileMtimeNs = kMtimeNs,
        .sourceFilePath = file});
  };
  insert("loc-album", "c-album", albumDir / "song.flac");
  insert("loc-alias", "c-alias", aliasDir / "song2.flac");
  insert("loc-upper", "c-upper", upperDir / "song3.flac");
  insert("loc-lower", "c-lower", lowerDir / "song4.flac");

  const auto renamed = cache.replaceLocationsBySubtree(temp.path().generic_string(), albumDir.generic_string(), newDir.generic_string());

  CHECK(renamed == 1);
  CHECK_FALSE(cache.loadLocation("loc-album").has_value());
  const auto moved = cache.loadLocation(computeLocationId(newDir / "song.flac", 1024, mtime));
  REQUIRE(moved.has_value());
  CHECK(moved->filePath == newDir / "song.flac");
  CHECK(cache.loadLocation("loc-alias").has_value());
  CHECK(cache.loadLocation("loc-upper").has_value());
  CHECK(cache.loadLocation("loc-lower").has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  REQUIRE(locations.size() == 4);
}

TEST_CASE("SQLiteCache: replaceLocationsBySubtree rewrites paths, recomputes ids, migrates lyrics, keeps artwork") {
  test::TempScannerRoot temp{"cache-replace-by-subtree"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-s1", "c-s2", "c-cue", "c-source", "c-ab", "c-keep"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto musicDir = temp.path() / "Music";
  const auto aDir = musicDir / "A";
  const auto dDir = musicDir / "D";
  const auto abDir = musicDir / "AB";
  const auto otherFile = temp.path() / "Other" / "song.mp3";

  constexpr std::int64_t kMtimeNs = 123456789;
  const auto mtime = std::filesystem::file_time_type{std::chrono::nanoseconds{kMtimeNs}};
  const auto offset = std::chrono::milliseconds{30000};

  const auto song1Old = aDir / "song1.flac";
  const auto song2Old = aDir / "song2.mp3";
  const auto cueOld = aDir / "album.cue";
  const auto sourceOld = aDir / "album.flac";
  const auto sharedOld = aDir / "shared.flac";
  const auto abOld = abDir / "song3.flac";
  const auto keepOld = temp.path() / "keep.flac";

  const auto song1OldId = computeLocationId(song1Old, 1024, mtime);
  const auto song2OldId = computeLocationId(song2Old, 2048, mtime);
  const auto cueOldId = computeLocationId(cueOld, 4096, mtime, offset, 0U);
  const auto sourceOldId = computeLocationId(otherFile, 1024, mtime);
  const auto abOldId = computeLocationId(abOld, 1024, mtime);
  const auto keepOldId = computeLocationId(keepOld, 1024, mtime);

  cache.upsertLocation(CachedLocation{
      .locationId = song1OldId,
      .contentId = "c-s1",
      .rootPath = temp.path(),
      .filePath = song1Old,
      .fileSizeBytes = 1024,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = song1Old,
      .artworkPath = temp.path() / "artwork" / "full.png",
      .thumbnailPath = temp.path() / "artwork" / "thumbnails" / "thumb.png"});
  cache.upsertLocation(CachedLocation{
      .locationId = song2OldId,
      .contentId = "c-s2",
      .rootPath = temp.path(),
      .filePath = song2Old,
      .fileSizeBytes = 2048,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = song2Old,
      .lyricsSource = LyricsSource::EmbeddedTag});
  cache.replaceLyrics(song2OldId,
                      "embedded",
                      {LyricLine{.timestamp = std::chrono::milliseconds{1000}, .text = "s2 first"},
                       LyricLine{.timestamp = std::chrono::milliseconds{2000}, .text = "s2 second"}});
  cache.replaceLyrics(song2OldId,
                      "external",
                      {LyricLine{.timestamp = std::chrono::milliseconds{500}, .text = "s2 ext"}});
  cache.upsertLocation(CachedLocation{
      .locationId = cueOldId,
      .contentId = "c-cue",
      .rootPath = temp.path(),
      .filePath = cueOld,
      .fileSizeBytes = 4096,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = sourceOld,
      .cueTrackOffset = offset,
      .cueTrackIndex = 0U,
      .cueTrackDuration = std::chrono::milliseconds{30000},
      .cueFileSizeBytes = 4096,
      .cueFileMtimeNs = kMtimeNs,
      .sourceFileSizeBytes = 8192,
      .sourceFileMtimeNs = kMtimeNs,
      .lyricsSource = LyricsSource::ExternalLrc});
  cache.replaceLyrics(cueOldId,
                      "external",
                      {LyricLine{.timestamp = std::chrono::milliseconds{1000}, .text = "cue line"}});
  cache.upsertLocation(CachedLocation{
      .locationId = sourceOldId,
      .contentId = "c-source",
      .rootPath = temp.path(),
      .filePath = otherFile,
      .fileSizeBytes = 1024,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = sharedOld,
      .artworkPath = temp.path() / "artwork" / "source.png"});
  cache.upsertLocation(CachedLocation{
      .locationId = abOldId,
      .contentId = "c-ab",
      .rootPath = temp.path(),
      .filePath = abOld,
      .fileSizeBytes = 1024,
      .sourceFilePath = abOld});
  cache.upsertLocation(CachedLocation{
      .locationId = keepOldId,
      .contentId = "c-keep",
      .rootPath = temp.path(),
      .filePath = keepOld,
      .fileSizeBytes = 1024,
      .sourceFilePath = keepOld});

  const auto renamed = cache.replaceLocationsBySubtree(temp.path().generic_string(), aDir.generic_string(), dDir.generic_string());
  CHECK(renamed == 4);

  const auto song1New = dDir / "song1.flac";
  const auto song2New = dDir / "song2.mp3";
  const auto cueNew = dDir / "album.cue";
  const auto sourceNew = dDir / "album.flac";
  const auto sharedNew = dDir / "shared.flac";
  const auto song1NewId = computeLocationId(song1New, 1024, mtime);
  const auto song2NewId = computeLocationId(song2New, 2048, mtime);
  const auto cueNewId = computeLocationId(cueNew, 4096, mtime, offset, 0U);

  CHECK_FALSE(cache.loadLocation(song1OldId).has_value());
  CHECK_FALSE(cache.loadLocation(song2OldId).has_value());
  CHECK_FALSE(cache.loadLocation(cueOldId).has_value());

  const auto song1 = cache.loadLocation(song1NewId);
  REQUIRE(song1.has_value());
  CHECK(song1->filePath == song1New);
  CHECK(song1->sourceFilePath == song1New);
  CHECK(song1->artworkPath == temp.path() / "artwork" / "full.png");
  CHECK(song1->thumbnailPath == temp.path() / "artwork" / "thumbnails" / "thumb.png");

  const auto song2 = cache.loadLocation(song2NewId);
  REQUIRE(song2.has_value());
  CHECK(song2->filePath == song2New);
  const auto song2Embedded = cache.loadLyrics(song2NewId, "embedded");
  REQUIRE(song2Embedded.size() == 2U);
  CHECK(song2Embedded[0].text == "s2 first");
  CHECK(song2Embedded[1].text == "s2 second");
  const auto song2External = cache.loadLyrics(song2NewId, "external");
  REQUIRE(song2External.size() == 1U);
  CHECK(song2External[0].text == "s2 ext");

  const auto cue = cache.loadLocation(cueNewId);
  REQUIRE(cue.has_value());
  CHECK(cue->filePath == cueNew);
  CHECK(cue->sourceFilePath == sourceNew);
  REQUIRE(cue->cueTrackOffset.has_value());
  CHECK(cue->cueTrackOffset->count() == 30000);
  REQUIRE(cue->cueTrackIndex.has_value());
  CHECK(*cue->cueTrackIndex == 0U);
  const auto cueExternal = cache.loadLyrics(cueNewId, "external");
  REQUIRE(cueExternal.size() == 1U);
  CHECK(cueExternal[0].text == "cue line");

  const auto cross = cache.loadLocation(sourceOldId);
  REQUIRE(cross.has_value());
  CHECK(cross->filePath == otherFile);
  CHECK(cross->sourceFilePath == sharedNew);
  CHECK(cross->artworkPath == temp.path() / "artwork" / "source.png");

  CHECK(cache.loadLocation(abOldId).has_value());
  CHECK(cache.loadLocation(keepOldId).has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  CHECK(locations.size() == 6);
}

TEST_CASE("SQLiteCache: replaceLocationsBySubtree respects prefix boundary and never rewrites siblings") {
  test::TempScannerRoot temp{"cache-replace-by-subtree-boundary"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{100000};
  for (const auto& id : {"c-a", "c-ab", "c-music2"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto musicDir = temp.path() / "Music";
  const auto aDir = musicDir / "A";
  const auto abDir = musicDir / "AB";
  const auto music2Dir = musicDir / "Music2";
  const auto newDir = musicDir / "D";

  constexpr std::int64_t kMtimeNs = 555;
  const auto mtime = std::filesystem::file_time_type{std::chrono::nanoseconds{kMtimeNs}};

  const auto aFile = aDir / "song.flac";
  const auto abFile = abDir / "song.flac";
  const auto m2File = music2Dir / "song.flac";
  const auto aId = computeLocationId(aFile, 1024, mtime);
  const auto abId = computeLocationId(abFile, 1024, mtime);
  const auto m2Id = computeLocationId(m2File, 1024, mtime);

  auto insert = [&](const std::string& id, const std::string& contentId, const std::filesystem::path& file) {
    cache.upsertLocation(CachedLocation{
        .locationId = id,
        .contentId = contentId,
        .rootPath = temp.path(),
        .filePath = file,
        .fileSizeBytes = 1024,
        .fileMtimeNs = kMtimeNs,
        .sourceFilePath = file});
  };
  insert(aId, "c-a", aFile);
  insert(abId, "c-ab", abFile);
  insert(m2Id, "c-music2", m2File);

  const auto renamed = cache.replaceLocationsBySubtree(temp.path().generic_string(), aDir.generic_string(), newDir.generic_string());
  CHECK(renamed == 1);

  CHECK_FALSE(cache.loadLocation(aId).has_value());
  const auto moved = cache.loadLocation(computeLocationId(newDir / "song.flac", 1024, mtime));
  REQUIRE(moved.has_value());
  CHECK(moved->filePath == newDir / "song.flac");
  CHECK(cache.loadLocation(abId).has_value());
  CHECK(cache.loadLocation(m2Id).has_value());
}

TEST_CASE("SQLiteCache: replaceLocationsBySubtree row set equals re-insert at new paths (diff equivalence)") {
  test::TempScannerRoot temp{"cache-replace-by-subtree-diff"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{120000};
  for (const auto& id : {"c-1", "c-2", "c-3", "c-4"}) {
    meta.title = id;
    cache.upsertContent(id, meta);
  }

  const auto aDir = temp.path() / "Music" / "A";
  const auto dDir = temp.path() / "Music" / "D";
  constexpr std::int64_t kMtimeNs = 987654321;
  const auto mtime = std::filesystem::file_time_type{std::chrono::nanoseconds{kMtimeNs}};
  const auto offset = std::chrono::milliseconds{30000};

  const auto songOld = aDir / "song.flac";
  const auto cueOld = aDir / "album.cue";
  const auto sourceOld = aDir / "album.flac";
  const auto crossFile = temp.path() / "Other" / "shared.mp3";
  const auto sharedOld = aDir / "shared.flac";

  cache.upsertLocation(CachedLocation{
      .locationId = computeLocationId(songOld, 1024, mtime),
      .contentId = "c-1",
      .rootPath = temp.path(),
      .filePath = songOld,
      .fileSizeBytes = 1024,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = songOld,
      .artworkPath = temp.path() / "artwork" / "s1.png",
      .thumbnailPath = temp.path() / "artwork" / "thumbnails" / "s1.png"});
  cache.upsertLocation(CachedLocation{
      .locationId = computeLocationId(cueOld, 4096, mtime, offset, 0U),
      .contentId = "c-2",
      .rootPath = temp.path(),
      .filePath = cueOld,
      .fileSizeBytes = 4096,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = sourceOld,
      .cueTrackOffset = offset,
      .cueTrackIndex = 0U,
      .cueTrackDuration = std::chrono::milliseconds{30000},
      .cueFileSizeBytes = 4096,
      .cueFileMtimeNs = kMtimeNs,
      .sourceFileSizeBytes = 8192,
      .sourceFileMtimeNs = kMtimeNs});
  cache.upsertLocation(CachedLocation{
      .locationId = computeLocationId(cueOld, 4096, mtime, offset, 1U),
      .contentId = "c-3",
      .rootPath = temp.path(),
      .filePath = cueOld,
      .fileSizeBytes = 4096,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = sourceOld,
      .cueTrackOffset = offset,
      .cueTrackIndex = 1U,
      .cueTrackDuration = std::chrono::milliseconds{35000},
      .cueFileSizeBytes = 4096,
      .cueFileMtimeNs = kMtimeNs,
      .sourceFileSizeBytes = 8192,
      .sourceFileMtimeNs = kMtimeNs});
  cache.upsertLocation(CachedLocation{
      .locationId = computeLocationId(crossFile, 1024, mtime),
      .contentId = "c-4",
      .rootPath = temp.path(),
      .filePath = crossFile,
      .fileSizeBytes = 1024,
      .fileMtimeNs = kMtimeNs,
      .sourceFilePath = sharedOld});

  const auto renamed = cache.replaceLocationsBySubtree(temp.path().generic_string(), aDir.generic_string(), dDir.generic_string());
  CHECK(renamed == 4);

  const auto songNew = dDir / "song.flac";
  const auto cueNew = dDir / "album.cue";
  const auto sourceNew = dDir / "album.flac";
  const auto sharedNew = dDir / "shared.flac";

  const auto expected = std::vector<CachedLocation>{
      CachedLocation{
          .locationId = computeLocationId(songNew, 1024, mtime),
          .contentId = "c-1",
          .rootPath = temp.path(),
          .filePath = songNew,
          .fileSizeBytes = 1024,
          .fileMtimeNs = kMtimeNs,
          .sourceFilePath = songNew,
          .artworkPath = temp.path() / "artwork" / "s1.png",
          .thumbnailPath = temp.path() / "artwork" / "thumbnails" / "s1.png"},
      CachedLocation{
          .locationId = computeLocationId(cueNew, 4096, mtime, offset, 0U),
          .contentId = "c-2",
          .rootPath = temp.path(),
          .filePath = cueNew,
          .fileSizeBytes = 4096,
          .fileMtimeNs = kMtimeNs,
          .sourceFilePath = sourceNew,
          .cueTrackOffset = offset,
          .cueTrackIndex = 0U,
          .cueTrackDuration = std::chrono::milliseconds{30000},
          .cueFileSizeBytes = 4096,
          .cueFileMtimeNs = kMtimeNs,
          .sourceFileSizeBytes = 8192,
          .sourceFileMtimeNs = kMtimeNs},
      CachedLocation{
          .locationId = computeLocationId(cueNew, 4096, mtime, offset, 1U),
          .contentId = "c-3",
          .rootPath = temp.path(),
          .filePath = cueNew,
          .fileSizeBytes = 4096,
          .fileMtimeNs = kMtimeNs,
          .sourceFilePath = sourceNew,
          .cueTrackOffset = offset,
          .cueTrackIndex = 1U,
          .cueTrackDuration = std::chrono::milliseconds{35000},
          .cueFileSizeBytes = 4096,
          .cueFileMtimeNs = kMtimeNs,
          .sourceFileSizeBytes = 8192,
          .sourceFileMtimeNs = kMtimeNs},
      CachedLocation{
          .locationId = computeLocationId(crossFile, 1024, mtime),
          .contentId = "c-4",
          .rootPath = temp.path(),
          .filePath = crossFile,
          .fileSizeBytes = 1024,
          .fileMtimeNs = kMtimeNs,
          .sourceFilePath = sharedNew},
  };

  const auto loaded = cache.loadLocationsByRoot(temp.path());
  REQUIRE(loaded.size() == expected.size());

  auto sameLocation = [](const CachedLocation& a, const CachedLocation& b) {
    return a.contentId == b.contentId && a.rootPath == b.rootPath &&
           a.filePath == b.filePath && a.fileSizeBytes == b.fileSizeBytes && a.fileMtimeNs == b.fileMtimeNs &&
           a.sourceFilePath == b.sourceFilePath && a.cueTrackOffset == b.cueTrackOffset &&
           a.cueTrackIndex == b.cueTrackIndex && a.cueTrackDuration == b.cueTrackDuration &&
           a.cueFileSizeBytes == b.cueFileSizeBytes && a.cueFileMtimeNs == b.cueFileMtimeNs &&
           a.sourceFileSizeBytes == b.sourceFileSizeBytes && a.sourceFileMtimeNs == b.sourceFileMtimeNs &&
           a.artworkPath == b.artworkPath && a.thumbnailPath == b.thumbnailPath &&
           a.lyricsSource == b.lyricsSource && a.externalLrcPath == b.externalLrcPath &&
           a.externalLrcMtimeNs == b.externalLrcMtimeNs && a.externalLrcHash == b.externalLrcHash;
  };

  std::map<std::string, CachedLocation> loadedById;
  for (const auto& location : loaded) {
    loadedById.emplace(location.locationId, location);
  }
  for (const auto& expectedLocation : expected) {
    const auto it = loadedById.find(expectedLocation.locationId);
    REQUIRE_MESSAGE(it != loadedById.end(),
                    "missing row with id " << expectedLocation.locationId << " for path "
                                           << expectedLocation.filePath.generic_string());
    CHECK_MESSAGE(sameLocation(it->second, expectedLocation),
                  "row differs for path " << expectedLocation.filePath.generic_string());
  }
}

TEST_CASE("SQLiteCache: replaceLocationsBySubtree is a no-op for unmatched and identical prefixes") {
  test::TempScannerRoot temp{"cache-replace-by-subtree-noop"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};
  cache.updateScanRoot(CachedScanRoot{.rootPath = temp.path()});

  SongMetadata meta;
  meta.duration = std::chrono::milliseconds{90000};
  cache.upsertContent("c-keep", meta);

  const auto file = temp.path() / "keep.flac";
  const auto id = computeLocationId(file, 1024, std::nullopt);
  cache.upsertLocation(CachedLocation{
      .locationId = id,
      .contentId = "c-keep",
      .rootPath = temp.path(),
      .filePath = file,
      .fileSizeBytes = 1024,
      .sourceFilePath = file});

  const auto unmatched = cache.replaceLocationsBySubtree(temp.path().generic_string(),
                                                         (temp.path() / "nonexistent").generic_string(),
                                                         (temp.path() / "elsewhere").generic_string());
  CHECK(unmatched == 0);
  CHECK(cache.loadLocation(id).has_value());

  const auto identical = cache.replaceLocationsBySubtree(temp.path().generic_string(),
                                                         (temp.path() / "keep.flac").generic_string(),
                                                         (temp.path() / "keep.flac").generic_string());
  CHECK(identical == 0);
  CHECK(cache.loadLocation(id).has_value());

  const auto locations = cache.loadLocationsByRoot(temp.path());
  CHECK(locations.size() == 1);
  CHECK(locations[0].filePath == file);
}

}  // namespace
}  // namespace seriona::scanner::cache
