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

  // 传与 artwork/thumbnail 不相交的 coverExportDir，使写侧相对化不触发：
  // 本用例聚焦 cachedLocationFromSong 的字段保真链；相对化契约见下方
  // "cover cache paths are stored relative / resolved back" 用例（真实 orchestrator
  // 以自身 coverExportDir_ 注入）。
  const auto location = cachedLocationFromSong(song, temp.path(), filePath, temp.path() / "unused-export-dir");

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

  // 同上：传不相交的 coverExportDir，聚焦 applyCachedLocation 保真链本身。
  const auto location = cachedLocationFromSong(song, temp.path(), filePath, temp.path() / "unused-export-dir");

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

TEST_CASE("cover cache paths are stored relative to cover export dir when under it") {
  test::TempScannerRoot temp{"incremental-plan-cover-relative"};
  const auto exportDir = temp.path() / "SerionaData" / "artwork";

  // 位于 coverExportDir 树下的绝对路径 → 相对（预期经 lexically_normal 统一分隔符）
  const auto underThumb = exportDir / "thumbnails" / "90" / "a.png";
  const auto thumbResult = coverRelativePath(std::optional{underThumb}, exportDir);
  REQUIRE(thumbResult.has_value());
  CHECK(thumbResult->lexically_normal() ==
        std::filesystem::path{"thumbnails/90/a.png"}.lexically_normal());

  const auto underArtwork = exportDir / "artwork" / "90" / "b.png";
  const auto artworkResult = coverRelativePath(std::optional{underArtwork}, exportDir);
  REQUIRE(artworkResult.has_value());
  CHECK(artworkResult->lexically_normal() ==
        std::filesystem::path{"artwork/90/b.png"}.lexically_normal());

  // 树外绝对路径 → 原样保留（异常数据防御）
  const auto outside = temp.path() / "elsewhere" / "c.png";
  CHECK(coverRelativePath(std::optional{outside}, exportDir) == outside);

  // 已相对 / nullopt → 原样
  CHECK(coverRelativePath(std::optional<std::filesystem::path>{"rel/d.png"}, exportDir) ==
        std::filesystem::path{"rel/d.png"});
  CHECK_FALSE(coverRelativePath(std::nullopt, exportDir).has_value());
}

TEST_CASE("cached cover paths resolve back to absolute on read") {
  test::TempScannerRoot temp{"incremental-plan-cover-resolve"};
  const auto oldRoot = temp.path() / "old-app" / "SerionaData" / "artwork";
  const auto newRoot = temp.path() / "new-app" / "SerionaData" / "artwork";

  // 新格式相对路径 → 拼当前 coverExportDir
  CHECK(resolveCoverPath(std::optional<std::filesystem::path>{"thumbnails/ab/x.png"}, newRoot) ==
        (newRoot / "thumbnails" / "ab" / "x.png"));

  // 旧绝对路径（应用整体移动，前缀失效）→ 按结构锚重建到新目录
  const auto staleThumb = oldRoot / "thumbnails" / "90" / "a.png";
  const auto rebuiltThumb = resolveCoverPath(std::optional{staleThumb}, newRoot);
  REQUIRE(rebuiltThumb.has_value());
  CHECK(*rebuiltThumb == newRoot / "thumbnails" / "90" / "a.png");

  // artwork 大图旧绝对路径（含 "artwork/artwork/…" 两段）→ 锚取内部段
  const auto staleArtwork = oldRoot / "artwork" / "90" / "b.png";
  const auto rebuiltArtwork = resolveCoverPath(std::optional{staleArtwork}, newRoot);
  REQUIRE(rebuiltArtwork.has_value());
  CHECK(*rebuiltArtwork == newRoot / "artwork" / "90" / "b.png");

  // 仍位于当前 coverExportDir 下的旧绝对路径 → 原样可用
  const auto stillValid = newRoot / "thumbnails" / "11" / "c.png";
  CHECK(resolveCoverPath(std::optional{stillValid}, newRoot) == stillValid);

  // 无结构锚（不在 coverExportDir 下且无 artwork/thumbnails 段）→ 原样
  const auto noAnchor = temp.path() / "misc" / "cover.png";
  CHECK(resolveCoverPath(std::optional{noAnchor}, newRoot) == noAnchor);

  // nullopt → 原样
  CHECK_FALSE(resolveCoverPath(std::nullopt, newRoot).has_value());
}

TEST_CASE("cached location stores relative artwork and resolves back under real export dir") {
  test::TempScannerRoot temp{"incremental-plan-cover-roundtrip"};
  const auto exportDir = temp.path() / "SerionaData" / "artwork";
  const auto filePath = test::writeAudioFixture(temp.path(), "song.flac");

  cache::CachedSong song{};
  song.metadata.contentHash = "content-id";
  song.metadata.fileSizeBytes = fileSizeBytes(filePath);
  song.metadata.fileMtime = fileMtime(filePath);
  song.metadata.sourceFilePath = filePath;
  song.metadata.artworkPath = exportDir / "artwork" / "ab" / "full.png";
  song.metadata.thumbnailPath = exportDir / "thumbnails" / "ab" / "thumb.png";

  // 写侧：真实 coverExportDir 注入 → DB 落盘形态为相对路径
  const auto location = cachedLocationFromSong(song, temp.path(), filePath, exportDir);
  REQUIRE(location.artworkPath.has_value());
  REQUIRE(location.thumbnailPath.has_value());
  CHECK(location.artworkPath->is_relative());
  CHECK(location.thumbnailPath->is_relative());

  // 读侧：同一 coverExportDir 下 resolve 回绝对路径（等价于移动后目录名未变的场景）
  const auto resolvedArtwork = resolveCoverPath(location.artworkPath, exportDir);
  const auto resolvedThumbnail = resolveCoverPath(location.thumbnailPath, exportDir);
  REQUIRE(resolvedArtwork.has_value());
  REQUIRE(resolvedThumbnail.has_value());
  CHECK(*resolvedArtwork == exportDir / "artwork" / "ab" / "full.png");
  CHECK(*resolvedThumbnail == exportDir / "thumbnails" / "ab" / "thumb.png");

  // 读侧：目录整体移动后（新 coverExportDir），相对路径直接拼接到新目录
  const auto movedDir = temp.path() / "moved-app" / "SerionaData" / "artwork";
  const auto movedThumbnail = resolveCoverPath(location.thumbnailPath, movedDir);
  REQUIRE(movedThumbnail.has_value());
  CHECK(*movedThumbnail == movedDir / "thumbnails" / "ab" / "thumb.png");
}

}
}
