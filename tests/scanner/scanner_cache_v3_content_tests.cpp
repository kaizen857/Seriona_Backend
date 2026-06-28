#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>

namespace seriona::scanner::cache {
namespace {

[[nodiscard]] SongMetadata contentFixture(std::string contentId, std::string title, std::chrono::milliseconds duration) {
  SongMetadata metadata{};
  metadata.trackId = std::move(contentId);
  metadata.title = std::move(title);
  metadata.artist = "Artist";
  metadata.album = "Album";
  metadata.albumArtist = "Album Artist";
  metadata.genre = "Genre";
  metadata.trackNumber = 7U;
  metadata.discNumber = 1U;
  metadata.year = 2026U;
  metadata.duration = duration;
  metadata.sampleRate = 48000U;
  metadata.bitDepth = 24U;
  metadata.channels = 2U;
  return metadata;
}

[[nodiscard]] SQLiteCacheV3 openCache(const std::filesystem::path& dbPath) {
  return SQLiteCacheV3{ScannerCacheConfig{.databasePath = dbPath, .busyTimeout = std::chrono::milliseconds{25}}};
}

}

TEST_CASE("sqlite cache v3 upserts content and loads stored metadata") {
  test::TempScannerRoot temp{"scanner-cache-v3-content-insert"};
  auto cache = openCache(temp.dbPath());
  const auto contentId = std::string{"content-1"};
  cache.upsertContent(contentId, contentFixture(contentId, "Song", std::chrono::milliseconds{180000}));

  const auto loaded = cache.loadContent(contentId);

  REQUIRE(loaded.has_value());
  CHECK(loaded->metadata.trackId == contentId);
  CHECK(loaded->metadata.title == "Song");
  CHECK(loaded->metadata.artist == "Artist");
  CHECK(loaded->metadata.albumArtist == "Album Artist");
  CHECK(loaded->metadata.trackNumber == 7U);
  CHECK(loaded->metadata.year == 2026U);
  CHECK(loaded->metadata.duration == std::chrono::milliseconds{180000});
  CHECK(loaded->metadata.sampleRate == 48000U);
  CHECK(loaded->metadata.bitDepth == 24U);
  CHECK(loaded->metadata.channels == 2U);
  CHECK(loaded->userStats.playCount == 0U);
  CHECK_FALSE(loaded->userStats.rating.has_value());
  CHECK_FALSE(loaded->userStats.lastPlayed.has_value());
}

TEST_CASE("sqlite cache v3 updates content metadata without clearing user stats") {
  test::TempScannerRoot temp{"scanner-cache-v3-content-update"};
  auto cache = openCache(temp.dbPath());
  const auto contentId = std::string{"content-2"};
  cache.upsertContent(contentId, contentFixture(contentId, "Song", std::chrono::milliseconds{180000}));
  const auto lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{123456}};
  cache.updateUserStats(contentId, {.playCount = 9U, .rating = 4U, .lastPlayed = lastPlayed});

  auto updatedMetadata = contentFixture(contentId, "Song refreshed", std::chrono::milliseconds{181000});
  updatedMetadata.year = 2027U;
  cache.upsertContent(contentId, updatedMetadata);
  const auto loaded = cache.loadContent(contentId);

  REQUIRE(loaded.has_value());
  CHECK(loaded->metadata.title == "Song refreshed");
  CHECK(loaded->metadata.duration == std::chrono::milliseconds{181000});
  CHECK(loaded->metadata.year == 2027U);
  CHECK(loaded->userStats.playCount == 9U);
  CHECK(loaded->userStats.rating == 4U);
  CHECK(loaded->userStats.lastPlayed == lastPlayed);
}

TEST_CASE("sqlite cache v3 returns no content when the row is missing") {
  test::TempScannerRoot temp{"scanner-cache-v3-content-miss"};
  auto cache = openCache(temp.dbPath());

  const auto loaded = cache.loadContent("missing-content");

  CHECK_FALSE(loaded.has_value());
}

TEST_CASE("sqlite cache v3 updates user stats independently of content metadata") {
  test::TempScannerRoot temp{"scanner-cache-v3-content-stats"};
  auto cache = openCache(temp.dbPath());
  const auto contentId = std::string{"content-3"};
  cache.upsertContent(contentId, contentFixture(contentId, "Song", std::chrono::milliseconds{180000}));
  const auto lastPlayed = std::chrono::system_clock::time_point{std::chrono::milliseconds{246810}};

  cache.updateUserStats(contentId, {.playCount = 12U, .rating = 5U, .lastPlayed = lastPlayed});
  const auto loaded = cache.loadContent(contentId);

  REQUIRE(loaded.has_value());
  CHECK(loaded->metadata.title == "Song");
  CHECK(loaded->userStats.playCount == 12U);
  CHECK(loaded->userStats.rating == 5U);
  CHECK(loaded->userStats.lastPlayed == lastPlayed);
}

TEST_CASE("sqlite cache v3 rejects content rows without duration") {
  test::TempScannerRoot temp{"scanner-cache-v3-content-invalid"};
  auto cache = openCache(temp.dbPath());
  const auto contentId = std::string{"content-4"};
  auto metadata = contentFixture(contentId, "Song", std::chrono::milliseconds{180000});
  metadata.duration = std::nullopt;

  CHECK_THROWS_AS(cache.upsertContent(contentId, metadata), std::runtime_error);
}

}
