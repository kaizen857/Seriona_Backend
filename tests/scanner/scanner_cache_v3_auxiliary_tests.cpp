#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace seriona::scanner::cache {
namespace {

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

[[nodiscard]] SongMetadata contentFixture(std::string contentId) {
  SongMetadata metadata{};
  metadata.trackId = std::move(contentId);
  metadata.title = "Song";
  metadata.artist = "Artist";
  metadata.album = "Album";
  metadata.albumArtist = "Album Artist";
  metadata.genre = "Genre";
  metadata.duration = std::chrono::milliseconds{180000};
  return metadata;
}

[[nodiscard]] CachedScanRootV3 rootFixture(std::string rootPath) {
  return {.rootPath = std::move(rootPath),
          .directoryTreeHash = "hash-a",
          .totalFiles = 3,
          .lastScanMode = ScanMode::Full,
          .lastScanDuration = std::chrono::milliseconds{42},
          .lastScanAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{7000}}};
}

[[nodiscard]] CachedLocation locationFixture(std::string locationId, std::string contentId, std::string rootPath) {
  return {.locationId = std::move(locationId),
          .contentId = std::move(contentId),
          .rootPath = std::move(rootPath),
          .filePath = "/music/song.flac",
          .fileSizeBytes = 4096U,
          .fileMtimeNs = 123456789,
          .sourceFilePath = "disc.flac",
          .cueTrackOffset = std::nullopt,
          .artworkPath = std::nullopt,
          .lyricsSource = LyricsSource::EmbeddedTag,
          .externalLrcPath = std::nullopt,
          .externalLrcMtimeNs = std::nullopt,
          .discoveredAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{1000}},
          .scannedAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{2000}}};
}

void seedLocation(SQLiteCacheV3& cache, const std::string& rootPath, const std::string& contentId,
                  const std::string& locationId) {
  cache.updateScanRoot(rootFixture(rootPath));
  cache.upsertContent(contentId, contentFixture(contentId));
  cache.upsertLocation(locationFixture(locationId, contentId, rootPath));
}

}

TEST_CASE("sqlite cache v3 replaces and loads lyrics by kind") {
  test::TempScannerRoot temp{"scanner-cache-v3-lyrics"};
  auto cache = openCache(temp.dbPath());
  seedLocation(cache, "/music", "content-1", "location-1");
  cache.replaceLyrics("location-1", "embedded", {{std::chrono::milliseconds{10}, "old"}});
  cache.replaceLyrics("location-1", "external", {{std::chrono::milliseconds{20}, "external"}});

  cache.replaceLyrics("location-1", "embedded", {{std::chrono::milliseconds{30}, "first"},
                                                    {std::chrono::milliseconds{40}, "second"}});
  const auto embedded = cache.loadLyrics("location-1", "embedded");
  const auto external = cache.loadLyrics("location-1", "external");

  REQUIRE(embedded.size() == 2U);
  CHECK(embedded[0].timestamp == std::chrono::milliseconds{30});
  CHECK(embedded[1].text == "second");
  REQUIRE(external.size() == 1U);
  CHECK(external[0].text == "external");
}

TEST_CASE("sqlite cache v3 rejects lyrics for invalid location foreign key") {
  test::TempScannerRoot temp{"scanner-cache-v3-lyrics-invalid-location"};
  auto cache = openCache(temp.dbPath());

  CHECK_THROWS_AS(cache.replaceLyrics("missing", "embedded", {{std::chrono::milliseconds{1}, "line"}}), std::runtime_error);
}

TEST_CASE("sqlite cache v3 updates and loads scan root state") {
  test::TempScannerRoot temp{"scanner-cache-v3-root"};
  auto cache = openCache(temp.dbPath());
  auto root = rootFixture("/music");
  cache.updateScanRoot(root);
  root.directoryTreeHash = "hash-b";
  root.totalFiles = 8;
  root.lastScanMode = ScanMode::Incremental;
  cache.updateScanRoot(root);

  const auto loaded = cache.loadScanRoot("/music");

  REQUIRE(loaded.has_value());
  CHECK(loaded->directoryTreeHash == "hash-b");
  CHECK(loaded->totalFiles == 8U);
  CHECK(loaded->lastScanMode == ScanMode::Incremental);
  CHECK(loaded->lastScanDuration == std::chrono::milliseconds{42});
}

TEST_CASE("sqlite cache v3 saves loads and clears scan errors") {
  test::TempScannerRoot temp{"scanner-cache-v3-errors"};
  auto cache = openCache(temp.dbPath());
  cache.updateScanRoot(rootFixture("/music"));
  cache.saveErrors("/music", {{.rootPath = "/music",
                               .filePath = std::filesystem::path{"/music/a.flac"},
                               .errorCode = ScannerErrorCode::MetadataReadFailed,
                               .errorMessage = "bad tag",
                               .occurredAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{9000}}}});
  cache.saveErrors("/music", {{.rootPath = "/music",
                               .filePath = std::nullopt,
                               .errorCode = ScannerErrorCode::PermissionDenied,
                               .errorMessage = "denied",
                               .occurredAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{10000}}}});

  const auto loaded = cache.loadErrors("/music");
  REQUIRE(loaded.size() == 1U);
  CHECK_FALSE(loaded[0].filePath.has_value());
  CHECK(loaded[0].errorCode == ScannerErrorCode::PermissionDenied);
  CHECK(loaded[0].errorMessage == "denied");

  cache.clearErrors("/music");
  CHECK(cache.loadErrors("/music").empty());
}

TEST_CASE("sqlite cache v3 rejects scan errors for invalid root foreign key") {
  test::TempScannerRoot temp{"scanner-cache-v3-errors-invalid-root"};
  auto cache = openCache(temp.dbPath());

  CHECK_THROWS_AS(cache.saveErrors("/missing", {{.rootPath = "/missing",
                                                 .filePath = std::nullopt,
                                                 .errorCode = ScannerErrorCode::RootUnavailable,
                                                 .errorMessage = "missing"}}), std::runtime_error);
}

}
