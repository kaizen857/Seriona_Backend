#include "scanner_test_harness.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define SERIONA_SCANNER_ORCHESTRATOR_TESTING
#include "../../src/scanner/file_scanner_orchestrator.cpp"

namespace seriona::scanner {
namespace {

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] cache::CachedLocation cachedLocationFor(const std::filesystem::path& rootPath,
                                                      const std::filesystem::path& filePath) {
  const auto size = fileSizeBytes(filePath);
  REQUIRE(size.has_value());
  return {.locationId = computeLocationId(filePath, *size, fileMtime(filePath)),
          .contentId = filePath.filename().generic_string(),
          .rootPath = rootPath,
          .filePath = filePath,
          .fileSizeBytes = *size,
          .fileMtimeNs = fileTimeNanoseconds(fileMtime(filePath)).value_or(0),
          .sourceFilePath = filePath,
          .cueTrackOffset = std::nullopt,
          .cueTrackIndex = std::nullopt,
          .cueTrackDuration = std::nullopt,
          .cueFileSizeBytes = std::nullopt,
          .cueFileMtimeNs = std::nullopt,
          .sourceFileSizeBytes = std::nullopt,
          .sourceFileMtimeNs = std::nullopt,
          .artworkPath = std::nullopt,
          .thumbnailPath = std::nullopt,
          .lyricsSource = LyricsSource::None,
          .externalLrcPath = std::nullopt,
          .externalLrcMtimeNs = std::nullopt,
          .externalLrcHash = std::nullopt,
          .discoveredAt = {},
          .scannedAt = {}};
}

[[nodiscard]] cache::CachedLocation cachedCueLocationFor(const std::filesystem::path& rootPath,
                                                         const std::filesystem::path& cuePath,
                                                         const std::filesystem::path& sourcePath,
                                                         std::uint32_t trackIndex,
                                                         std::chrono::milliseconds offset) {
  const auto cueSize = fileSizeBytes(cuePath);
  const auto sourceSize = fileSizeBytes(sourcePath);
  REQUIRE(cueSize.has_value());
  REQUIRE(sourceSize.has_value());
	  return {.locationId = computeLocationId(cuePath, *cueSize, fileMtime(cuePath), offset, trackIndex),
          .contentId = cuePath.filename().generic_string() + "#" + std::to_string(trackIndex),
          .rootPath = rootPath,
          .filePath = cuePath,
          .fileSizeBytes = *cueSize,
          .fileMtimeNs = fileTimeNanoseconds(fileMtime(cuePath)).value_or(0),
          .sourceFilePath = sourcePath,
          .cueTrackOffset = offset,
          .cueTrackIndex = trackIndex,
          .cueTrackDuration = std::chrono::milliseconds{60'000},
          .cueFileSizeBytes = cueSize,
          .cueFileMtimeNs = fileTimeNanoseconds(fileMtime(cuePath)),
          .sourceFileSizeBytes = sourceSize,
          .sourceFileMtimeNs = fileTimeNanoseconds(fileMtime(sourcePath)),
          .artworkPath = std::nullopt,
          .thumbnailPath = std::nullopt,
          .lyricsSource = LyricsSource::None,
          .externalLrcPath = std::nullopt,
          .externalLrcMtimeNs = std::nullopt,
          .externalLrcHash = std::nullopt,
          .discoveredAt = {},
          .scannedAt = {}};
}

void seedScanRoot(cache::SQLiteCache& cache, const std::filesystem::path& rootPath) {
  cache.updateScanRoot(cache::CachedScanRoot{.rootPath = rootPath,
                                             .directoryTreeHash = "test-tree-hash",
                                             .totalFiles = 0,
                                             .lastScanMode = ScanMode::Incremental,
                                             .lastScanDuration = std::chrono::milliseconds{1},
                                             .lastScanAt = {}});
}

void seedCachedLocation(cache::SQLiteCache& cache, const cache::CachedLocation& location) {
  SongMetadata metadata{};
  metadata.contentHash = location.contentId;
  metadata.title = location.contentId;
  metadata.duration = std::chrono::milliseconds{60'000};
  cache.upsertContent(location.contentId, metadata);
  cache.upsertLocation(location);
}

[[nodiscard]] bool containsPath(const std::vector<ClassifiedPath>& entries, const std::filesystem::path& path) {
  return std::ranges::any_of(entries, [&path](const ClassifiedPath& entry) { return pathKey(entry.path) == pathKey(path); });
}

[[nodiscard]] bool containsDeletedPath(const std::vector<cache::CachedLocation>& locations,
                                       const std::filesystem::path& path) {
  return std::ranges::any_of(locations, [&path](const cache::CachedLocation& location) {
    return pathKey(location.filePath) == pathKey(path);
  });
}

[[nodiscard]] bool containsLocationId(const std::vector<std::string>& locationIds, const std::string& locationId) {
  return std::ranges::find(locationIds, locationId) != locationIds.end();
}

[[nodiscard]] bool locationsContainId(const std::vector<cache::CachedLocation>& locations, const std::string& locationId) {
  return std::ranges::any_of(locations, [&locationId](const cache::CachedLocation& location) {
    return location.locationId == locationId;
  });
}

TEST_CASE("incremental scan plan classifies unchanged added deleted and changed locations") {
  test::TempScannerRoot temp{"incremental-plan-mixed-root"};
  std::vector<std::filesystem::path> unchangedPaths;
  unchangedPaths.reserve(96U);
  for (int index = 0; index < 96; ++index) {
    unchangedPaths.push_back(test::writeAudioFixture(temp.path(), "unchanged-" + std::to_string(index) + ".flac"));
  }
  const auto changedPath = test::writeAudioFixture(temp.path(), "changed.flac");
  const auto deletedAPath = test::writeAudioFixture(temp.path(), "deleted-a.flac");
  const auto deletedBPath = test::writeAudioFixture(temp.path(), "deleted-b.flac");
  const auto rootPath = rootPathFor(ScannerRoot{.path = temp.path()});

  std::vector<cache::CachedLocation> cachedLocations;
  cachedLocations.reserve(99U);
  for (const auto& path : unchangedPaths) {
    cachedLocations.push_back(cachedLocationFor(rootPath, path));
  }
  cachedLocations.push_back(cachedLocationFor(rootPath, changedPath));
  cachedLocations.push_back(cachedLocationFor(rootPath, deletedAPath));
  cachedLocations.push_back(cachedLocationFor(rootPath, deletedBPath));

  std::filesystem::remove(deletedAPath);
  std::filesystem::remove(deletedBPath);
  writeText(changedPath, "changed-content");
  std::this_thread::sleep_for(std::chrono::milliseconds{5}); // mtime granularity guard
  const auto addedPath = test::writeAudioFixture(temp.path(), "added.flac");
  const auto entries = discoverScannerPaths(ScannerRoot{.path = rootPath}, PathClassificationConfig{});

  const auto plan = planIncrementalScan(rootPath, entries, cachedLocations);

  CHECK(plan.added.size() == 1U);
  CHECK(plan.deleted.size() == 2U);
  CHECK(plan.changed.size() == 1U);
  CHECK(containsPath(plan.added, addedPath));
  CHECK(containsPath(plan.changed, changedPath));
  CHECK(containsDeletedPath(plan.deleted, deletedAPath));
  CHECK(containsDeletedPath(plan.deleted, deletedBPath));
}

TEST_CASE("incremental scan plan treats absent cache as all current candidates added") {
  test::TempScannerRoot temp{"incremental-plan-empty-cache"};
  const auto firstPath = test::writeAudioFixture(temp.path(), "first.flac");
  const auto secondPath = test::writeAudioFixture(temp.path(), "second.flac");
  const auto rootPath = rootPathFor(ScannerRoot{.path = temp.path()});
  const auto entries = discoverScannerPaths(ScannerRoot{.path = rootPath}, PathClassificationConfig{});

  const auto plan = planIncrementalScan(rootPath, entries, {});

  CHECK(plan.added.size() == 2U);
  CHECK(plan.deleted.empty());
  CHECK(plan.changed.empty());
  CHECK(containsPath(plan.added, firstPath));
  CHECK(containsPath(plan.added, secondPath));
}

TEST_CASE("incremental execution plan tracks origins cue locations and does not prune while planning") {
  test::TempScannerRoot temp{"incremental-plan-origin-cue-retained"};
  const auto unchangedPath = test::writeAudioFixture(temp.path(), "unchanged.flac");
  const auto changedPath = test::writeAudioFixture(temp.path(), "changed.flac");
  const auto deletedPath = test::writeAudioFixture(temp.path(), "deleted.flac");
  const auto cuePath = temp.path() / "album.cue";
  const auto sourcePath = test::writeAudioFixture(temp.path(), "album.flac");
  writeText(cuePath, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n  TRACK 02 AUDIO\n");
  const auto rootPath = rootPathFor(ScannerRoot{.path = temp.path()});

  auto unchangedLocation = cachedLocationFor(rootPath, unchangedPath);
  auto changedLocation = cachedLocationFor(rootPath, changedPath);
  auto deletedLocation = cachedLocationFor(rootPath, deletedPath);
  auto cueTrackZero = cachedCueLocationFor(rootPath, cuePath, sourcePath, 0U, std::chrono::milliseconds{0});
  auto cueTrackOne = cachedCueLocationFor(rootPath, cuePath, sourcePath, 1U, std::chrono::milliseconds{60'000});

  cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(temp.path() / "library.sqlite")}};
  seedScanRoot(cache, rootPath);
  for (const auto& location : {unchangedLocation, changedLocation, deletedLocation, cueTrackZero, cueTrackOne}) {
    seedCachedLocation(cache, location);
  }

  std::filesystem::remove(deletedPath);
  writeText(changedPath, "changed-content-with-a-new-size");
  const auto addedPath = test::writeAudioFixture(temp.path(), "added.flac");
  const auto entries = discoverScannerPaths(ScannerRoot{.path = rootPath}, PathClassificationConfig{});
  const auto cachedLocations = cache.loadLocationsByRoot(rootPath);

  const auto executionPlan = incrementalExecutionPlan(rootPath, entries, cachedLocations, false);
  const auto locationsAfterPlanning = cache.loadLocationsByRoot(rootPath);

  CHECK(executionPlan.unchangedPaths.contains(pathKey(unchangedPath)));
  CHECK(executionPlan.workerPaths.contains(pathKey(changedPath)));
  CHECK(executionPlan.workerPaths.contains(pathKey(addedPath)));
  REQUIRE(executionPlan.workerOriginsByPath.contains(pathKey(changedPath)));
  REQUIRE(executionPlan.workerOriginsByPath.contains(pathKey(addedPath)));
  CHECK(executionPlan.workerOriginsByPath.at(pathKey(changedPath)) == ScanItemOrigin::RescannedChanged);
  CHECK(executionPlan.workerOriginsByPath.at(pathKey(addedPath)) == ScanItemOrigin::ScannedNew);

  REQUIRE(executionPlan.cueLocationsByCuePath.contains(pathKey(cuePath)));
  CHECK(executionPlan.cueLocationsByCuePath.at(pathKey(cuePath)).size() == 2U);
  REQUIRE(executionPlan.cueRetainedLocationIdsByCuePath.contains(pathKey(cuePath)));
  CHECK(containsLocationId(executionPlan.cueRetainedLocationIdsByCuePath.at(pathKey(cuePath)), cueTrackZero.locationId));
  CHECK(containsLocationId(executionPlan.cueRetainedLocationIdsByCuePath.at(pathKey(cuePath)), cueTrackOne.locationId));

  CHECK(executionPlan.retainedLocationIds.empty());
  CHECK(executionPlan.lyricsOnlyUpdates.empty());
  CHECK(locationsContainId(locationsAfterPlanning, deletedLocation.locationId));
}

TEST_CASE("incremental execution plan does not retain unchanged cache candidates before hydrate succeeds") {
  test::TempScannerRoot temp{"incremental-plan-retain-after-hydrate"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto rootPath = rootPathFor(ScannerRoot{.path = temp.path()});
  const auto cachedLocation = cachedLocationFor(rootPath, audioPath);
  const auto entries = discoverScannerPaths(ScannerRoot{.path = rootPath}, PathClassificationConfig{});

  const auto executionPlan = incrementalExecutionPlan(rootPath, entries, {cachedLocation}, true);

  CHECK(executionPlan.unchangedPaths.contains(pathKey(audioPath)));
  CHECK(executionPlan.retainedLocationIds.empty());
}

TEST_CASE("cached location from song preserves thumbnail path") {
  test::TempScannerRoot temp{"incremental-plan-thumbnail-location"};
  const auto filePath = test::writeAudioFixture(temp.path(), "song.flac");

  cache::CachedSong song{};
  song.metadata.contentHash = "content-id";
  song.metadata.fileSizeBytes = fileSizeBytes(filePath);
  song.metadata.fileMtime = fileMtime(filePath);
  song.metadata.sourceFilePath = filePath;
  song.metadata.artworkPath = temp.path() / "artwork" / "full.png";
  song.metadata.thumbnailPath = temp.path() / "artwork" / "thumbnails" / "thumb.png";

  const auto location = cachedLocationFromSong(song, temp.path(), filePath);

  CHECK(location.artworkPath == temp.path() / "artwork" / "full.png");
  CHECK(location.thumbnailPath == temp.path() / "artwork" / "thumbnails" / "thumb.png");
}

TEST_CASE("thumbnail path survives tree publish and cached location apply with artwork empty") {
  test::TempScannerRoot temp{"incremental-plan-thumbnail-apply"};
  const auto filePath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto thumbnail = temp.path() / "covers" / "thumbnails" / "ab" / "thumb.png";

  cache::CachedSong song{};
  song.metadata.contentHash = "content-id";
  song.metadata.fileSizeBytes = fileSizeBytes(filePath);
  song.metadata.fileMtime = fileMtime(filePath);
  song.metadata.sourceFilePath = filePath;
  song.metadata.artworkPath = std::nullopt;
  song.metadata.thumbnailPath = thumbnail;

  const auto location = cachedLocationFromSong(song, temp.path(), filePath);

  CHECK(location.contentId == "content-id");
  CHECK(location.sourceFilePath == filePath);
  CHECK(location.thumbnailPath == thumbnail);
  CHECK_FALSE(location.artworkPath.has_value());

  cache::CachedSong restored{};
  restored.metadata.title = "Restored Title";
  applyCachedLocation(restored, location, filePath);

  CHECK(restored.metadata.title == "Restored Title");
  CHECK(restored.metadata.contentHash == "content-id");
  CHECK(restored.metadata.filePath == filePath);
  CHECK(restored.metadata.sourceFilePath == filePath);
  CHECK(restored.metadata.thumbnailPath == thumbnail);
  CHECK_FALSE(restored.metadata.artworkPath.has_value());
}

}
}
