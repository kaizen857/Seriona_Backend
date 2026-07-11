#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <iostream>

namespace seriona::scanner::cache {
namespace {

TEST_CASE("SQLiteCache: performance benchmark with prepared statements") {
  test::TempScannerRoot temp{"scanner-cache-perf"};
  SQLiteCache cache{ScannerCacheConfig{.databasePath = temp.dbPath()}};

  CachedScanRoot root;
  root.rootPath = temp.path();
  root.directoryTreeHash = "perf-test-hash";
  root.totalFiles = 1000;
  root.lastScanMode = ScanMode::Full;
  root.lastScanDuration = std::chrono::milliseconds{5000};
  root.lastScanAt = std::chrono::system_clock::now();
  cache.updateScanRoot(root);

  constexpr int numEntries = 1000;
  
  for (int i = 0; i < numEntries; ++i) {
    SongMetadata meta;
    meta.title = "Song " + std::to_string(i);
    meta.artist = "Artist " + std::to_string(i % 100);
    meta.album = "Album " + std::to_string(i % 50);
    meta.duration = std::chrono::milliseconds{180000 + i * 1000};
    cache.upsertContent("content-" + std::to_string(i), meta);

    CachedLocation loc;
    loc.locationId = "loc-" + std::to_string(i);
    loc.contentId = "content-" + std::to_string(i);
    loc.rootPath = temp.path();
    loc.filePath = temp.path() / ("song" + std::to_string(i) + ".flac");
    loc.sourceFilePath = loc.filePath;
    loc.fileSizeBytes = 1000000 + i * 1000;
    loc.fileMtimeNs = 123456789 + i;
    loc.lyricsSource = LyricsSource::None;
    loc.discoveredAt = std::chrono::system_clock::now();
    loc.scannedAt = std::chrono::system_clock::now();
    cache.upsertLocation(loc);
  }

  constexpr int numQueries = 5000;
  
  auto startContent = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < numQueries; ++i) {
    auto loaded = cache.loadContent("content-" + std::to_string(i % numEntries));
    REQUIRE(loaded.has_value());
  }
  auto endContent = std::chrono::high_resolution_clock::now();
  auto contentDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endContent - startContent);

  auto startLocation = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < numQueries; ++i) {
    auto loaded = cache.loadLocation("loc-" + std::to_string(i % numEntries));
    REQUIRE(loaded.has_value());
  }
  auto endLocation = std::chrono::high_resolution_clock::now();
  auto locationDuration = std::chrono::duration_cast<std::chrono::milliseconds>(endLocation - startLocation);

  std::cout << "\n=== SQLiteCache Performance (with prepared statements) ===\n";
  std::cout << "Database entries: " << numEntries << "\n";
  std::cout << "Total queries: " << numQueries << " per query type\n";
  std::cout << "loadContent: " << contentDuration.count() << " ms (" 
            << (contentDuration.count() * 1000.0 / numQueries) << " μs/query)\n";
  std::cout << "loadLocation: " << locationDuration.count() << " ms (" 
            << (locationDuration.count() * 1000.0 / numQueries) << " μs/query)\n";
  std::cout << "Total: " << (contentDuration + locationDuration).count() << " ms\n";
  std::cout << "============================================================\n\n";

  CHECK(contentDuration.count() < 500);
  CHECK(locationDuration.count() < 500);
}

}
}
