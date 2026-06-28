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
          .artworkPath = std::nullopt,
          .lyricsSource = LyricsSource::None,
          .externalLrcPath = std::nullopt,
          .externalLrcMtimeNs = std::nullopt,
          .discoveredAt = {},
          .scannedAt = {}};
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

}
}
