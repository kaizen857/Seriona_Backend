#include "file_scanner_service_internal.h"
#include "scanner_internal_types.h"
#include "file_scanner_orchestrator_test_access.h"

#include "spdlog/spdlog.h"
#include "logging/logging.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/hash_utils.h"
#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"
#include "seriona/scanner/playlist_tree_builder.h"
#include "seriona/scanner/song_identity.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"
#include "seriona/scanner/worker_pool.h"

#include "wtr/watcher.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

namespace seriona::scanner {
namespace {

static PreallocationObserver g_preallocationObserver = nullptr;
static TestCueSheetProvider g_testCueSheetProvider = nullptr;
static LrcParseObserver g_lrcParseObserver = nullptr;
static TestLyricsSidecarHashProvider g_testLyricsSidecarHashProvider = nullptr;
static IncrementalPlanObserver g_incrementalPlanObserver = nullptr;
static WorkerTaskObserver g_workerTaskObserver = nullptr;
static PublishedSongObserver g_publishedSongObserver = nullptr;
static CacheWriteObserver g_cacheWriteObserver = nullptr;

[[nodiscard]] FileHashResult hashLyricsSidecarWithTestSeam(const std::filesystem::path& path,
                                                           const HashOptions& options) {
  if (g_testLyricsSidecarHashProvider) {
    return g_testLyricsSidecarHashProvider(path, options);
  }
  return hashLyricsSidecar(path, options);
}

[[nodiscard]] LrcParseResult parseLrcFileWithTestSeam(const std::filesystem::path& path) {
  if (g_lrcParseObserver) {
    g_lrcParseObserver(path);
  }
  return parseLrcFile(path);
}

[[nodiscard]] std::filesystem::path resolvePortableDataRoot() {
#ifdef __linux__
  std::array<char, 4096> buffer{};
  const auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len > 0) {
    buffer[static_cast<std::size_t>(len)] = '\0';
    return std::filesystem::path{buffer.data()}.parent_path() / "SerionaData";
  }
#endif
  return std::filesystem::current_path() / "SerionaData";
}

[[nodiscard]] std::filesystem::path defaultDatabasePath() {
  return resolvePortableDataRoot() / "library.sqlite";
}

[[nodiscard]] std::filesystem::path defaultCoverExportDir() {
  return resolvePortableDataRoot() / "artwork";
}

[[nodiscard]] std::string pathKey(const std::filesystem::path& path) { return path.lexically_normal().generic_string(); }

[[nodiscard]] std::filesystem::path rootPathFor(const ScannerRoot& root) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(root.path, error);
  if (error) {
    canonical = root.path.lexically_normal();
  }
  return canonical;
}

struct ScanModeDecision {
  ScanMode mode{ScanMode::Full};
  std::optional<std::string> directoryTreeHash;
};

struct EffectiveScannerConfig {
  ScannerConfig scanner;
  std::size_t workerCount{1};
  std::ptrdiff_t tagReaderSlots{1};
};

enum class ExternalLyricsCacheAction {
  None,
  UpdateExternal,
  RemoveExternal,
  Cancelled,
};

[[nodiscard]] bool shouldRetainLocationForOrigin(ScanItemOrigin origin) {
  switch (origin) {
    case ScanItemOrigin::CacheHit:
    case ScanItemOrigin::CueTrackCacheHit:
    case ScanItemOrigin::RescannedChanged:
    case ScanItemOrigin::ScannedNew:
    case ScanItemOrigin::ScannedFull:
    case ScanItemOrigin::CueTrackRescannedChanged:
    case ScanItemOrigin::CueTrackScannedNew:
      return true;
    case ScanItemOrigin::VirtualContainer:
      return false;
  }
  return false;
}

[[nodiscard]] bool shouldWriteSongForOrigin(ScanItemOrigin origin) {
  switch (origin) {
    case ScanItemOrigin::RescannedChanged:
    case ScanItemOrigin::ScannedNew:
    case ScanItemOrigin::ScannedFull:
      return true;
    case ScanItemOrigin::CacheHit:
    case ScanItemOrigin::CueTrackCacheHit:
    case ScanItemOrigin::CueTrackRescannedChanged:
    case ScanItemOrigin::CueTrackScannedNew:
    case ScanItemOrigin::VirtualContainer:
      return false;
  }
  return false;
}

[[nodiscard]] bool shouldWriteCueTrackForOrigin(ScanItemOrigin origin) {
  switch (origin) {
    case ScanItemOrigin::CueTrackRescannedChanged:
    case ScanItemOrigin::CueTrackScannedNew:
      return true;
    case ScanItemOrigin::CacheHit:
    case ScanItemOrigin::CueTrackCacheHit:
    case ScanItemOrigin::RescannedChanged:
    case ScanItemOrigin::ScannedNew:
    case ScanItemOrigin::ScannedFull:
    case ScanItemOrigin::VirtualContainer:
      return false;
  }
  return false;
}

[[nodiscard]] bool shouldPublishFileScanned(ScanItemOrigin origin) {
  switch (origin) {
    case ScanItemOrigin::ScannedFull:
    case ScanItemOrigin::ScannedNew:
    case ScanItemOrigin::RescannedChanged:
    case ScanItemOrigin::CueTrackScannedNew:
    case ScanItemOrigin::CueTrackRescannedChanged:
      return true;
    case ScanItemOrigin::CacheHit:
    case ScanItemOrigin::CueTrackCacheHit:
    case ScanItemOrigin::VirtualContainer:
      return false;
  }
  return false;
}

[[nodiscard]] std::optional<std::size_t> parsePositiveSizeEnv(std::string_view name) {
  const auto* raw = std::getenv(std::string{name}.c_str());
  if (raw == nullptr || std::string_view{raw}.empty()) {
    return std::nullopt;
  }
  std::size_t value = 0;
  const auto text = std::string_view{raw};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end || value == 0U) {
    spdlog::warn("ignoring invalid {}={} ; expected a positive integer", name, text);
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] bool scannerConcurrencyDisabledByEnv() {
  const auto* raw = std::getenv("SERIONA_SCANNER_DISABLE_CONCURRENCY");
  if (raw == nullptr || std::string_view{raw}.empty()) {
    return false;
  }
  const auto value = std::string_view{raw};
  if (value == "1") {
    return true;
  }
  if (value == "0") {
    return false;
  }
  spdlog::warn("ignoring invalid SERIONA_SCANNER_DISABLE_CONCURRENCY={} ; expected 0 or 1", value);
  return false;
}

[[nodiscard]] EffectiveScannerConfig effectiveScannerConfig(const ScannerConfig& config) {
  auto workerCount = config.workerCount == 0U ? getOptimalWorkerCount() : config.workerCount;
  auto tagReaderSlots = config.tagReaderConcurrency == 0 ? getOptimalTagReaderLimit(workerCount) : config.tagReaderConcurrency;
  if (const auto envWorkers = parsePositiveSizeEnv("SERIONA_SCANNER_WORKERS"); envWorkers.has_value()) {
    workerCount = *envWorkers;
  }
  if (const auto envTagReaders = parsePositiveSizeEnv("SERIONA_SCANNER_TAGREADER_CONCURRENCY"); envTagReaders.has_value()) {
    tagReaderSlots = static_cast<std::ptrdiff_t>(*envTagReaders);
  }
  if (scannerConcurrencyDisabledByEnv()) {
    workerCount = 1U;
    tagReaderSlots = 1;
  }
  return {.scanner = config, .workerCount = workerCount, .tagReaderSlots = tagReaderSlots};
}

[[nodiscard]] std::filesystem::path scanRootDatabasePath(const std::filesystem::path& databasePath) {
  return std::filesystem::path{databasePath.generic_string() + ".scan-roots.sqlite"};
}

[[nodiscard]] cache::CachedScanRoot scanRootRecord(const std::filesystem::path& rootPath,
                                                   const ScanModeDecision& decision,
                                                   const std::uint64_t totalFiles,
                                                   const std::chrono::milliseconds scanDuration) {
  return {.rootPath = rootPath,
          .directoryTreeHash = decision.directoryTreeHash.value_or({}),
          .totalFiles = totalFiles,
          .lastScanMode = decision.mode,
          .lastScanDuration = scanDuration,
          .lastScanAt = std::chrono::system_clock::now()};
}

[[nodiscard]] ScanModeDecision decideScanMode(const ScannerRoot& root,
                                             const ScanMode requestedMode,
                                             const std::filesystem::path& databasePath) {
  const auto rootPath = rootPathFor(root);
  const auto directoryTreeHash = computeDirectoryTreeHash(rootPath);
  if (!directoryTreeHash.hash.has_value() || requestedMode == ScanMode::Full) {
    return {.mode = ScanMode::Full, .directoryTreeHash = directoryTreeHash.hash};
  }
  try {
    const cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath)}};
    const auto cachedRoot = cache.loadScanRoot(rootPath);
    if (cachedRoot.has_value() && cachedRoot->directoryTreeHash == *directoryTreeHash.hash) {
      return {.mode = ScanMode::Incremental, .directoryTreeHash = directoryTreeHash.hash};
    }
  } catch (const std::exception& error) {
    spdlog::warn("scan mode decision fell back to full: {}", error.what());
  }
  return {.mode = ScanMode::Full, .directoryTreeHash = directoryTreeHash.hash};
}

[[nodiscard]] std::filesystem::path relativePathFor(const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code error;
  auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty() || relative == ".") {
    return path.filename();
  }
  return relative.lexically_normal();
}

[[nodiscard]] ScannerError scannerErrorFrom(const PathClassificationError& error) {
  return {.code = error.code, .message = error.message, .detail = error.detail, .path = error.path};
}

[[nodiscard]] ScannerError scannerErrorFrom(const HashError& error) { return error.scannerError; }

[[nodiscard]] ScannerError scannerErrorFrom(const LrcParseError& error) {
  return {.code = ScannerErrorCode::MetadataReadFailed,
          .message = "failed to parse external lyrics",
          .detail = error.message,
          .path = error.path};
}

[[nodiscard]] std::optional<std::filesystem::file_time_type> fileMtime(const std::filesystem::path& path) {
  std::error_code error;
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return mtime;
}

[[nodiscard]] cache::CachedSong cachedSongFrom(MappedTagMetadata mapped) {
  cache::CachedSong song{};
  song.metadata = std::move(mapped.metadata);
  song.embeddedLyrics = std::move(mapped.embeddedLyrics);
  song.externalLyrics = std::move(mapped.externalLyrics);
  return song;
}

[[nodiscard]] std::optional<std::int64_t> fileTimeNanoseconds(std::optional<std::filesystem::file_time_type> fileTime) {
  if (!fileTime.has_value()) {
    return std::nullopt;
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(fileTime->time_since_epoch()).count();
}

struct IncrementalScanPlan {
  std::vector<cache::CachedLocation> deleted;
  std::vector<ClassifiedPath> unchanged;
  std::vector<ClassifiedPath> added;
  std::vector<ClassifiedPath> changed;
  std::vector<ClassifiedPath> cueUnchanged;
  std::vector<ClassifiedPath> cueAdded;
  std::vector<ClassifiedPath> cueChanged;
};

struct IncrementalExecutionPlan {
  std::unordered_set<std::string> unchangedPaths;
  std::unordered_set<std::string> workerPaths;
  std::unordered_map<std::string, ScanItemOrigin> workerOriginsByPath;
  std::unordered_map<std::string, ScanItemOrigin> cueReaderOriginsByPath;
  std::vector<std::string> retainedLocationIds;
  std::unordered_map<std::string, std::vector<cache::CachedLocation>> cueLocationsByCuePath;
  std::unordered_map<std::string, std::vector<std::string>> cueRetainedLocationIdsByCuePath;
  std::vector<cache::LyricsCacheUpdate> lyricsOnlyUpdates;
};

struct CachedCueTrack {
  cache::CachedLocation location;
  cache::CachedSong song;
  std::uint32_t trackIndex{0};
};

using CachedCueTracksByPath = std::unordered_map<std::string, std::vector<CachedCueTrack>>;

[[nodiscard]] IncrementalPlanSnapshot incrementalPlanSnapshotFrom(const IncrementalExecutionPlan& plan) {
  return {.retainedLocationIds = plan.retainedLocationIds, .lyricsOnlyUpdates = plan.lyricsOnlyUpdates};
}

[[nodiscard]] ScanItemOrigin workerOriginForPath(const std::unordered_map<std::string, ScanItemOrigin>& originsByPath,
                                                const std::filesystem::path& path) {
  const auto origin = originsByPath.find(pathKey(path));
  return origin == originsByPath.end() ? ScanItemOrigin::ScannedFull : origin->second;
}

[[nodiscard]] ScanItemOrigin cueReaderOriginForPath(const std::optional<IncrementalExecutionPlan>& plan,
                                                   const std::filesystem::path& cuePath) {
  if (!plan.has_value()) {
    return ScanItemOrigin::ScannedFull;
  }
  const auto origin = plan->cueReaderOriginsByPath.find(pathKey(cuePath));
  return origin == plan->cueReaderOriginsByPath.end() ? ScanItemOrigin::CueTrackScannedNew : origin->second;
}

[[nodiscard]] std::optional<std::uint64_t> fileSizeBytes(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return std::nullopt;
  }
  return size;
}

[[nodiscard]] bool isPlanAudioCandidate(const ClassifiedPath& entry) {
  return entry.kind == PathEntryKind::AudioCandidate || entry.kind == PathEntryKind::SingleFileRoot;
}

using CachedLocationRefs = std::vector<std::reference_wrapper<const cache::CachedLocation>>;

struct CachedLocationPathIndex {
  std::unordered_map<std::string, std::reference_wrapper<const cache::CachedLocation>> audioLocationByPath;
  std::unordered_map<std::string, CachedLocationRefs> cueLocationsByCuePath;
};

[[nodiscard]] bool isCueCachedLocation(const cache::CachedLocation& location) {
  const auto hasDistinctSource = !location.sourceFilePath.empty() && pathKey(location.sourceFilePath) != pathKey(location.filePath);
  return location.cueTrackOffset.has_value() || location.cueTrackIndex.has_value() || hasDistinctSource;
}

[[nodiscard]] CachedLocationPathIndex buildCachedLocationPathIndex(const std::filesystem::path& rootPath,
                                                                   const std::vector<cache::CachedLocation>& locations) {
  CachedLocationPathIndex index;
  index.audioLocationByPath.reserve(locations.size());
  index.cueLocationsByCuePath.reserve(locations.size());
  for (const auto& location : locations) {
    if (pathKey(location.rootPath) != pathKey(rootPath)) {
      continue;
    }
    const auto locationPathKey = pathKey(location.filePath);
    if (isCueCachedLocation(location)) {
      index.cueLocationsByCuePath[locationPathKey].push_back(std::cref(location));
      continue;
    }
    index.audioLocationByPath.try_emplace(locationPathKey, std::cref(location));
  }
  return index;
}

[[nodiscard]] bool cueLocationMatchesCueFile(const cache::CachedLocation& location,
                                             const std::filesystem::path& cuePath,
                                             std::optional<std::uint64_t> cueFileSize,
                                             std::optional<std::int64_t> cueFileMtimeNs) {
  if (!cueFileSize.has_value() || !cueFileMtimeNs.has_value()) {
    return false;
  }
  const auto cachedCueSize = location.cueFileSizeBytes.value_or(location.fileSizeBytes);
  const auto cachedCueMtime = location.cueFileMtimeNs.value_or(location.fileMtimeNs);
  return pathKey(location.filePath) == pathKey(cuePath) && cachedCueSize == *cueFileSize && cachedCueMtime == *cueFileMtimeNs;
}

[[nodiscard]] std::optional<std::uint32_t> cueTrackIndexFromLogicalTrackId(const std::filesystem::path& cuePath,
                                                                            std::string_view logicalTrackId) {
  const auto prefix = cuePath.generic_string() + "#track";
  if (!logicalTrackId.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto suffix = logicalTrackId.substr(prefix.size());
  if (suffix.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto* first = suffix.data();
  const auto* last = suffix.data() + suffix.size();
  const auto [parsed, error] = std::from_chars(first, last, value);
  if (error != std::errc{} || parsed != last || value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool cueSourceAudioMatchesCachedLocation(const cache::CachedLocation& location) {
  if (location.sourceFilePath.empty() || pathKey(location.sourceFilePath) == pathKey(location.filePath) ||
      !location.sourceFileSizeBytes.has_value() || !location.sourceFileMtimeNs.has_value()) {
    return false;
  }
  const auto currentSourceSize = fileSizeBytes(location.sourceFilePath);
  const auto currentSourceMtimeNs = fileTimeNanoseconds(fileMtime(location.sourceFilePath));
  return currentSourceSize.has_value() && currentSourceMtimeNs.has_value() &&
         *currentSourceSize == *location.sourceFileSizeBytes && *currentSourceMtimeNs == *location.sourceFileMtimeNs;
}

[[nodiscard]] bool cueLocationMatchesCacheHitIdentity(const cache::CachedLocation& location,
                                                       const std::filesystem::path& cuePath,
                                                       std::optional<std::uint64_t> cueFileSize,
                                                       std::optional<std::filesystem::file_time_type> cueFileMtime,
                                                       std::optional<std::int64_t> cueFileMtimeNs) {
  if (!location.cueTrackIndex.has_value() || !location.cueTrackOffset.has_value() || !cueFileSize.has_value() ||
      !cueFileMtimeNs.has_value()) {
    return false;
  }
  if (!cueLocationMatchesCueFile(location, cuePath, cueFileSize, cueFileMtimeNs)) {
    return false;
  }
  if (!cueSourceAudioMatchesCachedLocation(location)) {
    return false;
  }
  const auto expectedLocationId = computeLocationId(cuePath, *cueFileSize, cueFileMtime, location.cueTrackOffset, location.cueTrackIndex);
  return location.locationId == expectedLocationId;
}

[[nodiscard]] IncrementalScanPlan planIncrementalScan(const std::filesystem::path& rootPath,
                                                      const std::vector<ClassifiedPath>& fileSystemEntries,
                                                      const std::vector<cache::CachedLocation>& cachedLocations,
                                                      [[maybe_unused]] bool treeHashMatches = false) {
  const auto cachedLocationsByPath = buildCachedLocationPathIndex(rootPath, cachedLocations);
  std::unordered_set<std::string> observedFilePaths;
  observedFilePaths.reserve(fileSystemEntries.size());

  IncrementalScanPlan plan;
  for (const auto& entry : fileSystemEntries) {
    if (!isPlanAudioCandidate(entry) && entry.kind != PathEntryKind::CueSheet) {
      continue;
    }
    const auto normalizedPathKey = pathKey(entry.path);
    observedFilePaths.insert(normalizedPathKey);

    if (entry.kind == PathEntryKind::CueSheet) {
      const auto cachedCueLocations = cachedLocationsByPath.cueLocationsByCuePath.find(normalizedPathKey);
      if (cachedCueLocations == cachedLocationsByPath.cueLocationsByCuePath.end() || cachedCueLocations->second.empty()) {
        plan.cueAdded.push_back(entry);
        continue;
      }
      const auto cueFileSize = fileSizeBytes(entry.path);
      const auto cueFileMtimeNs = fileTimeNanoseconds(fileMtime(entry.path));
      const auto cueMatches = std::ranges::all_of(cachedCueLocations->second, [&](const auto& locationRef) {
        return cueLocationMatchesCueFile(locationRef.get(), entry.path, cueFileSize, cueFileMtimeNs);
      });
      if (cueMatches) {
        plan.cueUnchanged.push_back(entry);
      } else {
        plan.cueChanged.push_back(entry);
      }
      continue;
    }

    const auto cachedLocation = cachedLocationsByPath.audioLocationByPath.find(normalizedPathKey);
    if (cachedLocation == cachedLocationsByPath.audioLocationByPath.end()) {
      plan.added.push_back(entry);
      continue;
    }
    // Always verify locationId to detect content changes, even when treeHashMatches.
    // Directory tree hash only includes paths/types, not file size/mtime/content.
    const auto currentFileSize = fileSizeBytes(entry.path);
    if (!currentFileSize.has_value()) {
      continue;
    }
    const auto currentLocationId = computeLocationId(entry.path, *currentFileSize, fileMtime(entry.path));
    if (currentLocationId != cachedLocation->second.get().locationId) {
      plan.changed.push_back(entry);
      continue;
    }
    plan.unchanged.push_back(entry);
  }

  for (const auto& location : cachedLocations) {
    if (pathKey(location.rootPath) == pathKey(rootPath) && !observedFilePaths.contains(pathKey(location.filePath))) {
      plan.deleted.push_back(location);
    }
  }
  return plan;
}

[[nodiscard]] IncrementalExecutionPlan incrementalExecutionPlan(const std::filesystem::path& rootPath,
                                                               const std::vector<ClassifiedPath>& entries,
                                                               const std::vector<cache::CachedLocation>& cachedLocations,
                                                               bool treeHashMatches) {
  const auto plan = planIncrementalScan(rootPath, entries, cachedLocations, treeHashMatches);
  const auto cachedLocationsByPath = buildCachedLocationPathIndex(rootPath, cachedLocations);
  IncrementalExecutionPlan executionPlan;
  executionPlan.unchangedPaths.reserve(plan.unchanged.size());
  executionPlan.workerPaths.reserve(plan.changed.size() + plan.added.size());
  executionPlan.workerOriginsByPath.reserve(plan.changed.size() + plan.added.size());
  executionPlan.cueReaderOriginsByPath.reserve(plan.cueUnchanged.size() + plan.cueChanged.size() + plan.cueAdded.size());
  executionPlan.cueLocationsByCuePath.reserve(plan.cueUnchanged.size());
  executionPlan.cueRetainedLocationIdsByCuePath.reserve(plan.cueUnchanged.size());
  for (const auto& entry : plan.unchanged) {
    executionPlan.unchangedPaths.insert(pathKey(entry.path));
  }
  for (const auto& entry : plan.changed) {
    const auto entryKey = pathKey(entry.path);
    executionPlan.workerPaths.insert(entryKey);
    executionPlan.workerOriginsByPath.emplace(entryKey, ScanItemOrigin::RescannedChanged);
  }
  for (const auto& entry : plan.added) {
    const auto entryKey = pathKey(entry.path);
    executionPlan.workerPaths.insert(entryKey);
    executionPlan.workerOriginsByPath.emplace(entryKey, ScanItemOrigin::ScannedNew);
  }
  for (const auto& entry : plan.cueAdded) {
    executionPlan.cueReaderOriginsByPath.emplace(pathKey(entry.path), ScanItemOrigin::CueTrackScannedNew);
  }
  for (const auto& entry : plan.cueChanged) {
    executionPlan.cueReaderOriginsByPath.emplace(pathKey(entry.path), ScanItemOrigin::CueTrackRescannedChanged);
  }
  for (const auto& entry : plan.cueUnchanged) {
    const auto entryKey = pathKey(entry.path);
    executionPlan.cueReaderOriginsByPath.emplace(entryKey, ScanItemOrigin::CueTrackRescannedChanged);
    const auto cueLocations = cachedLocationsByPath.cueLocationsByCuePath.find(entryKey);
    if (cueLocations == cachedLocationsByPath.cueLocationsByCuePath.end()) {
      continue;
    }
    auto& plannedCueLocations = executionPlan.cueLocationsByCuePath[entryKey];
    auto& retainedCueLocationIds = executionPlan.cueRetainedLocationIdsByCuePath[entryKey];
    plannedCueLocations.reserve(cueLocations->second.size());
    retainedCueLocationIds.reserve(cueLocations->second.size());
    for (const auto& locationRef : cueLocations->second) {
      const auto& location = locationRef.get();
      plannedCueLocations.push_back(location);
      retainedCueLocationIds.push_back(location.locationId);
    }
  }
  return executionPlan;
}

[[nodiscard]] std::unordered_set<std::string> cachedCueSourceAudioPathKeys(const std::optional<IncrementalExecutionPlan>& plan) {
  std::unordered_set<std::string> sourcePathKeys;
  if (!plan.has_value()) {
    return sourcePathKeys;
  }
  for (const auto& [cuePathKey, cueLocations] : plan->cueLocationsByCuePath) {
    for (const auto& location : cueLocations) {
      if (!location.sourceFilePath.empty() && pathKey(location.sourceFilePath) != cuePathKey) {
        sourcePathKeys.insert(pathKey(location.sourceFilePath));
      }
    }
  }
  return sourcePathKeys;
}

void publishIncrementalPlanSnapshot(const std::optional<IncrementalExecutionPlan>& plan) {
  if (g_incrementalPlanObserver == nullptr || !plan.has_value()) {
    return;
  }
  g_incrementalPlanObserver(incrementalPlanSnapshotFrom(*plan));
}

void publishCacheWriteSnapshot(const cache::ScanRootCacheWrite& write) {
  if (g_cacheWriteObserver == nullptr) {
    return;
  }
  g_cacheWriteObserver(write);
}

void publishWorkerTaskSnapshot(const std::vector<WorkerTask>& tasks,
                               const std::unordered_map<std::string, ScanItemOrigin>& originsByPath) {
  if (g_workerTaskObserver == nullptr) {
    return;
  }
  std::vector<WorkerTaskSnapshot> snapshots;
  snapshots.reserve(tasks.size());
  for (const auto& task : tasks) {
    snapshots.push_back(WorkerTaskSnapshot{.filePath = task.filePath,
                                           .origin = workerOriginForPath(originsByPath, task.filePath),
                                           .hasCachedLocation = task.cachedLocation.has_value(),
                                           .nodeIndex = task.nodeIndex});
  }
  g_workerTaskObserver(snapshots);
}

[[nodiscard]] cache::CachedLocation cachedLocationFromSong(const cache::CachedSong& song,
                                                           const std::filesystem::path& rootPath,
                                                           const std::filesystem::path& filePath) {
  const auto filesystemMtime = fileMtime(filePath);
  const auto stableMtime = filesystemMtime.has_value() ? filesystemMtime : song.metadata.fileMtime;
  const auto mtime = fileTimeNanoseconds(stableMtime).value_or(0);
  const auto filesystemFileSize = fileSizeBytes(filePath);
  const auto fileSize = filesystemFileSize.value_or(song.metadata.fileSizeBytes.value_or(0));
  const auto cueTrackOffset = !song.metadata.sourceFilePath.empty() && pathKey(song.metadata.sourceFilePath) != pathKey(filePath)
                              ? song.metadata.offset
                              : std::optional<std::chrono::milliseconds>{};
  const auto sourceFilePath = song.metadata.sourceFilePath.empty() ? filePath : song.metadata.sourceFilePath;
  const auto isCueTrack = cueTrackOffset.has_value();
  const auto cueFileSize = isCueTrack ? filesystemFileSize : std::optional<std::uint64_t>{};
  const auto sourceFileSize = isCueTrack ? fileSizeBytes(sourceFilePath) : std::optional<std::uint64_t>{};
  const auto sourceFileMtime = isCueTrack ? fileTimeNanoseconds(fileMtime(sourceFilePath)) : std::optional<std::int64_t>{};
  const auto cueTrackIndex = isCueTrack ? cueTrackIndexFromLogicalTrackId(filePath, song.metadata.logicalTrackId)
                                       : std::optional<std::uint32_t>{};
  return {.locationId = computeLocationId(filePath, fileSize, stableMtime, cueTrackOffset, cueTrackIndex),
          .contentId = song.metadata.contentHash,
          .rootPath = rootPath,
          .filePath = filePath,
          .fileSizeBytes = fileSize,
          .fileMtimeNs = mtime,
          .sourceFilePath = sourceFilePath,
          .cueTrackOffset = cueTrackOffset,
          .cueTrackIndex = cueTrackIndex,
          .cueTrackDuration = isCueTrack ? song.metadata.duration : std::optional<std::chrono::milliseconds>{},
          .cueFileSizeBytes = cueFileSize,
          .cueFileMtimeNs = isCueTrack ? std::optional<std::int64_t>{mtime} : std::optional<std::int64_t>{},
          .sourceFileSizeBytes = sourceFileSize,
          .sourceFileMtimeNs = sourceFileMtime,
          .artworkPath = song.metadata.artworkPath,
          .thumbnailPath = song.metadata.thumbnailPath,
          .lyricsSource = song.metadata.effectiveLyricsSource,
          .externalLrcPath = song.metadata.externalLyricsPath,
          .externalLrcMtimeNs = fileTimeNanoseconds(song.metadata.externalLyricsMtime),
          .externalLrcHash = song.metadata.externalLyricsHash,
          .discoveredAt = {},
          .scannedAt = {}};
}

[[nodiscard]] ScannerError scannerErrorFromWorker(const ScannerError& error) {
  return {.code = error.code,
          .message = "TagReader metadata read failed",
          .detail = error.detail,
          .path = error.path};
}

void selectEffectiveLyrics(cache::CachedSong& song) {
  if (!song.externalLyrics.empty()) {
    song.metadata.effectiveLyricsSource = LyricsSource::ExternalLrc;
    song.metadata.effectiveLyrics = song.externalLyrics;
    return;
  }
  song.metadata.externalLyricsPath = std::nullopt;
  song.metadata.externalLyricsHash = std::nullopt;
  song.metadata.externalLyricsMtime = std::nullopt;
  if (!song.embeddedLyrics.empty()) {
    song.metadata.effectiveLyricsSource = LyricsSource::EmbeddedTag;
    song.metadata.effectiveLyrics = song.embeddedLyrics;
    return;
  }
  song.metadata.effectiveLyricsSource = LyricsSource::None;
  song.metadata.effectiveLyrics.clear();
}

void applyCachedLocation(cache::CachedSong& song,
                         const cache::CachedLocation& location,
                         const std::filesystem::path& filePath) {
  song.metadata.filePath = filePath;
  song.metadata.sourceFilePath = location.sourceFilePath.empty() ? filePath : location.sourceFilePath;
  song.metadata.fileSizeBytes = location.fileSizeBytes;
  song.metadata.fileMtime = std::filesystem::file_time_type{std::chrono::nanoseconds{location.fileMtimeNs}};
  song.metadata.contentHash = location.contentId;
  song.metadata.effectiveLyricsSource = location.lyricsSource;
  song.metadata.offset = location.cueTrackOffset;
  song.metadata.artworkPath = location.artworkPath;
  song.metadata.thumbnailPath = location.thumbnailPath;
  song.metadata.externalLyricsPath = location.externalLrcPath;
  song.metadata.externalLyricsMtime = location.externalLrcMtimeNs.has_value()
                                          ? std::optional<std::filesystem::file_time_type>{
                                                std::filesystem::file_time_type{std::chrono::nanoseconds{
                                                    *location.externalLrcMtimeNs}}}
                                          : std::nullopt;
  song.metadata.externalLyricsHash = location.externalLrcHash;
  song.metadata.trackId = filePath.generic_string();
  song.metadata.logicalTrackId = filePath.generic_string();
  selectEffectiveLyrics(song);
}

void hydrateCachedCueTrack(cache::CachedSong& song,
                           const cache::CachedLocation& location,
                           const std::filesystem::path& cuePath,
                           std::uint32_t trackIndex) {
  song.metadata.filePath = cuePath;
  song.metadata.sourceFilePath = location.sourceFilePath;
  song.metadata.fileSizeBytes = location.fileSizeBytes;
  song.metadata.fileMtime = std::filesystem::file_time_type{std::chrono::nanoseconds{location.fileMtimeNs}};
  song.metadata.contentHash = location.contentId;
  song.metadata.offset = location.cueTrackOffset;
  if (location.cueTrackDuration.has_value()) {
    song.metadata.duration = location.cueTrackDuration;
  }
  song.metadata.artworkPath = location.artworkPath;
  song.metadata.thumbnailPath = location.thumbnailPath;
  song.metadata.externalLyricsPath = location.externalLrcPath;
  song.metadata.externalLyricsMtime = location.externalLrcMtimeNs.has_value()
                                          ? std::optional<std::filesystem::file_time_type>{
                                                std::filesystem::file_time_type{std::chrono::nanoseconds{
                                                    *location.externalLrcMtimeNs}}}
                                          : std::nullopt;
  song.metadata.externalLyricsHash = location.externalLrcHash;
  const auto trackIdentity = cuePath.generic_string() + "#track" + std::to_string(trackIndex);
  song.metadata.logicalTrackId = trackIdentity;
  song.metadata.trackId = trackIdentity;
  selectEffectiveLyrics(song);
}

[[nodiscard]] std::optional<std::vector<CachedCueTrack>> loadCachedCueTracksForHit(
    const std::filesystem::path& cuePath,
    const std::vector<cache::CachedLocation>& cueLocations,
    const cache::SQLiteCache& cache) {
  if (cueLocations.empty()) {
    return std::nullopt;
  }
  const auto cueFileSize = fileSizeBytes(cuePath);
  const auto cueFileMtime = fileMtime(cuePath);
  const auto cueFileMtimeNs = fileTimeNanoseconds(cueFileMtime);
  std::unordered_set<std::uint32_t> seenTrackIndexes;
  std::vector<CachedCueTrack> tracks;
  tracks.reserve(cueLocations.size());
  for (const auto& location : cueLocations) {
    if (!cueLocationMatchesCacheHitIdentity(location, cuePath, cueFileSize, cueFileMtime, cueFileMtimeNs)) {
      return std::nullopt;
    }
    if (!seenTrackIndexes.insert(*location.cueTrackIndex).second) {
      return std::nullopt;
    }
    auto cachedSong = cache.loadContent(location.contentId);
    if (!cachedSong.has_value()) {
      return std::nullopt;
    }
    cachedSong->embeddedLyrics = cache.loadLyrics(location.locationId, "embedded");
    cachedSong->externalLyrics = cache.loadLyrics(location.locationId, "external");
    hydrateCachedCueTrack(*cachedSong, location, cuePath, *location.cueTrackIndex);
    tracks.push_back(CachedCueTrack{.location = location,
                                    .song = std::move(*cachedSong),
                                    .trackIndex = *location.cueTrackIndex});
  }
  std::ranges::sort(tracks, {}, &CachedCueTrack::trackIndex);
  for (std::size_t index = 0; index < tracks.size(); ++index) {
    if (tracks[index].trackIndex != index) {
      return std::nullopt;
    }
  }
  return tracks;
}

[[nodiscard]] CachedCueTracksByPath buildCachedCueTracksByCuePath(const std::optional<IncrementalExecutionPlan>& plan,
                                                                  const cache::SQLiteCache& cache) {
  CachedCueTracksByPath tracksByCuePath;
  if (!plan.has_value()) {
    return tracksByCuePath;
  }
  tracksByCuePath.reserve(plan->cueLocationsByCuePath.size());
  for (const auto& [cuePathKey, cueLocations] : plan->cueLocationsByCuePath) {
    if (cueLocations.empty()) {
      continue;
    }
    const auto cachedTracks = loadCachedCueTracksForHit(cueLocations.front().filePath, cueLocations, cache);
    if (cachedTracks.has_value()) {
      tracksByCuePath.emplace(cuePathKey, *cachedTracks);
    }
  }
  return tracksByCuePath;
}

void publishEvent(const ScannerEventSink& sink, ScannerEventType type, std::uint64_t version, ScannerEventPayload payload) {
  if (!sink) {
    return;
  }
  sink(ScannerEvent{.type = type, .monotonicVersion = version, .timestamp = std::chrono::steady_clock::now(), .payload = std::move(payload)});
}

[[nodiscard]] WatchEffectKind watchEffectFrom(enum wtr::event::effect_type effect) {
  switch (effect) {
  case wtr::event::effect_type::create:
    return WatchEffectKind::Created;
  case wtr::event::effect_type::modify:
    return WatchEffectKind::Modified;
  case wtr::event::effect_type::destroy:
    return WatchEffectKind::Destroyed;
  case wtr::event::effect_type::rename:
    return WatchEffectKind::Renamed;
  case wtr::event::effect_type::owner:
    return WatchEffectKind::OwnerChanged;
  case wtr::event::effect_type::other:
    return WatchEffectKind::Other;
  }
  return WatchEffectKind::Other;
}

[[nodiscard]] WatchPathKind watchPathKindFrom(enum wtr::event::path_type pathType) {
  switch (pathType) {
  case wtr::event::path_type::file:
  case wtr::event::path_type::hard_link:
  case wtr::event::path_type::sym_link:
    return WatchPathKind::File;
  case wtr::event::path_type::dir:
    return WatchPathKind::Directory;
  case wtr::event::path_type::watcher:
    return WatchPathKind::Watcher;
  case wtr::event::path_type::other:
    return WatchPathKind::Other;
  }
  return WatchPathKind::Other;
}

[[nodiscard]] WatchEvent watchEventFrom(const wtr::event& event) {
  WatchEvent mapped{.path = event.path_name,
                    .pathKind = watchPathKindFrom(event.path_type),
                    .effectKind = watchEffectFrom(event.effect_type),
                    .associated = {}};
  if (event.associated) {
    mapped.associated.push_back(watchEventFrom(*event.associated));
  }
  return mapped;
}

[[nodiscard]] bool watcherMessageRequestsRootReconciliation(const std::filesystem::path& messagePath) {
  const auto message = messagePath.generic_string();
  return message.starts_with("e/") || message.starts_with("w_") || message.contains("overflow") ||
         message.contains("warning") || message.contains("error");
}

[[nodiscard]] bool pathChangeRequestsScan(const WatchEvent& event) {
  switch (event.pathKind) {
  case WatchPathKind::File:
  case WatchPathKind::Directory:
    break;
  case WatchPathKind::Watcher:
  case WatchPathKind::Other:
    return false;
  }

  switch (event.effectKind) {
  case WatchEffectKind::Created:
  case WatchEffectKind::Modified:
  case WatchEffectKind::Destroyed:
  case WatchEffectKind::Renamed:
  case WatchEffectKind::OwnerChanged:
    return true;
  case WatchEffectKind::Other:
    return false;
  }
  return false;
}

void collectActionableWatcherEvent(const WatchEvent& event, std::vector<std::string>& messages, bool& actionable) {
  if (event.pathKind == WatchPathKind::Watcher) {
    if (watcherMessageRequestsRootReconciliation(event.path)) {
      messages.push_back(event.path.generic_string());
      actionable = true;
    }
  } else if (pathChangeRequestsScan(event)) {
    actionable = true;
  }

  for (const auto& associated : event.associated) {
    collectActionableWatcherEvent(associated, messages, actionable);
  }
}

class WtrFolderWatcher final : public FolderWatcher {
public:
  WtrFolderWatcher(const std::filesystem::path& root, WatchEventCallback callback)
      : watcher_(std::make_unique<wtr::watch>(root, [callback = std::move(callback)](const wtr::event& event) {
          callback(watchEventFrom(event));
        })) {}

  ~WtrFolderWatcher() override { close(); }

  void close() noexcept override {
    if (watcher_) {
      watcher_->close();
      watcher_.reset();
    }
  }

private:
  std::unique_ptr<wtr::watch> watcher_;
};

class WtrFolderWatcherFactory final : public FolderWatcherFactory {
public:
  [[nodiscard]] std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                     WatchEventCallback callback) override {
    return std::make_unique<WtrFolderWatcher>(root, std::move(callback));
  }
};

struct WatchRuntimeState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ScannerRoot> watchedRoots;
  std::vector<std::string> pendingWatcherMessages;
  bool stopping{true};
  std::uint64_t dirtyGeneration{0};
};

void enqueueWatcherEvent(const std::shared_ptr<WatchRuntimeState>& state, const WatchEvent& event) {
  std::scoped_lock lock{state->mutex};
  if (state->stopping) {
    return;
  }
  bool actionable = false;
  collectActionableWatcherEvent(event, state->pendingWatcherMessages, actionable);
  if (!actionable) {
    return;
  }
  ++state->dirtyGeneration;
  state->changed.notify_one();
}

class OrchestratedFileScannerService final : public FileScannerService {
public:
  explicit OrchestratedFileScannerService(FileScannerServiceDependencies dependencies)
      : metadataReader_(std::move(dependencies.metadataReader)), databasePath_(std::move(dependencies.databasePath)),
        coverExportDir_(std::move(dependencies.coverExportDir)), watcherFactory_(std::move(dependencies.watcherFactory)),
        watcherDebounce_(dependencies.watcherDebounce) {
    if (!metadataReader_) {
      metadataReader_ = std::make_shared<ProductionTagMetadataReader>();
    }
    if (!watcherFactory_) {
      watcherFactory_ = std::make_shared<WtrFolderWatcherFactory>();
    }
    if (databasePath_.empty()) {
      databasePath_ = defaultDatabasePath();
    }
    if (coverExportDir_.empty()) {
      coverExportDir_ = defaultCoverExportDir();
    }
    spdlog::info("scanner configured: database={} artwork={}", databasePath_.generic_string(),
                 coverExportDir_.generic_string());
    
    const auto logDir = databasePath_.parent_path() / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    if (!ec) {
      const auto tagReaderLogPath = logDir / "tagreader-errors.log";
      tagReaderErrorLogger_ = logging::createDedicatedLogger(
          "tagreader_errors", tagReaderLogPath, spdlog::level::warn);
      if (tagReaderErrorLogger_) {
        spdlog::info("TagReader error logging enabled: {}", tagReaderLogPath.generic_string());
      }
    }
  }

  ~OrchestratedFileScannerService() override {
    stopWatching();
    stopScanWorker();
  }

  void setEventSink(ScannerEventSink sink) override {
    std::scoped_lock lock{mutex_};
    sink_ = std::move(sink);
  }

  void configure(const ScannerConfig& config) override {
    std::scoped_lock lock{mutex_};
    config_ = config;
  }

  void scan(const std::vector<ScannerRoot>& roots, ScanMode mode) override {
    bool submitted = false;
    {
      std::lock_guard lock{scanQueueMutex_};
      if (!scanWorkerStopping_ && scanQueue_.size() < 16U) {
        scanQueue_.push_back(ScanRequest{.roots = roots, .mode = mode});
        submitted = true;
      }
    }
    scanQueueChanged_.notify_one();
    if (!submitted) {
      spdlog::error("scanner scan queue is full (capacity 16)");
      ScannerEventSink sink;
      {
        std::scoped_lock lock{mutex_};
        sink = sink_;
      }
      publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, ScannerError{.code = ScannerErrorCode::CacheUnavailable,
                                                                                     .message = "scanner scan queue is full",
                                                                                     .detail = {},
                                                                                     .path = std::nullopt});
    }
  }

  void runScan(const std::vector<ScannerRoot>& roots, ScanMode mode) {
    std::lock_guard scanLock{scanMutex_};
    ScannerConfig config;
    ScannerEventSink sink;
    {
      std::scoped_lock lock{mutex_};
      config = config_;
      sink = sink_;
    }
    const auto effectiveConfig = effectiveScannerConfig(config);
    const auto scanVersion = ++eventVersion_;
    const auto scanStartTime = std::chrono::steady_clock::now();
    spdlog::info("scan started: {} roots", roots.size());
    publishEvent(sink, ScannerEventType::ScanStarted, scanVersion, ScanProgress{});
    if (cancellationRequested_.exchange(false)) {
      publishCancelled(sink, scanVersion);
      return;
    }

    cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = databasePath_}};
    std::vector<RootResult::PublishedSong> allSongs;
    std::vector<ScannerError> allErrors;
    std::uint64_t discovered = 0;
    std::uint64_t skipped = 0;
    std::uint64_t scanned = 0;
    std::uint64_t totalTagReaderTimeMs = 0;

    const auto phaseEnumStart = std::chrono::steady_clock::now();
    for (const auto& root : roots) {
      if (cancellationRequested_.load()) {
        publishCancelled(sink, scanVersion);
        return;
      }
      spdlog::debug("scanning root: {}", root.path.generic_string());
      const auto requestedMode = (!effectiveConfig.scanner.enableIncrementalScan || effectiveConfig.scanner.forceFull) ? ScanMode::Full : mode;
      const auto decision = decideScanMode(root, requestedMode, databasePath_);
      spdlog::debug("scan mode decision for {}: {}", root.path.generic_string(),
                    decision.mode == ScanMode::Full ? "full" : "incremental");
      const auto rootScanStartTime = std::chrono::steady_clock::now();
      auto rootResult = reconcileRoot(root, decision, effectiveConfig, cache, discovered, skipped, scanned, totalTagReaderTimeMs);
      const auto rootScanDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rootScanStartTime);
      if (rootResult.cancelled) {
        publishCancelled(sink, scanVersion);
        return;
      }
      recordScanRootDecision(rootPathFor(root), decision, rootResult.songs, rootScanDuration);
      allErrors.insert(allErrors.end(), rootResult.errors.begin(), rootResult.errors.end());
      allSongs.insert(allSongs.end(), rootResult.songs.begin(), rootResult.songs.end());
      for (const auto& error : rootResult.errors) {
        publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
      }
	      for (const auto& publishedSong : rootResult.songs) {
	        if (shouldPublishFileScanned(publishedSong.origin)) {
	          publishEvent(sink, ScannerEventType::FileScanned, ++eventVersion_, publishedSong.song.metadata);
	        }
	      }
    }

    const auto phaseEnumEnd = std::chrono::steady_clock::now();
    const auto phaseEnumTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(phaseEnumEnd - phaseEnumStart).count();

    const auto phaseAggregationStart = std::chrono::steady_clock::now();
    PlaylistTreeBuilder builder{"Library"};
    const auto cueSourcePaths = cueReferencedAudioPaths(allSongs);
    for (const auto& publishedSong : allSongs) {
      if (hiddenByCueSourceVisibility(publishedSong, cueSourcePaths)) {
        continue;
      }
      builder.addSong({.relativePath = publishedSong.treeRelativePath, .metadata = publishedSong.song.metadata});
    }
    auto published = builder.publish();
    {
      std::scoped_lock lock{mutex_};
      snapshot_ = published;
    }
    const auto phaseAggregationEnd = std::chrono::steady_clock::now();
    const auto phaseAggregationTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(phaseAggregationEnd - phaseAggregationStart).count();

    const auto scanEndTime = std::chrono::steady_clock::now();
    const auto totalScanTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(scanEndTime - scanStartTime).count();
    
    ScanProgress progress{};
    progress.filesDiscovered = discovered;
    progress.filesScanned = scanned;
    progress.filesSkipped = skipped;
    progress.errors = allErrors.size();

    spdlog::info("scan complete: {} discovered, {} scanned, {} skipped, {} errors", discovered, scanned, skipped, allErrors.size());
    
    if (!allErrors.empty()) {
      spdlog::warn("\n========== Scan Errors ({} total) ==========", allErrors.size());
      
      std::unordered_map<ScannerErrorCode, std::size_t> errorCodeCounts;
      std::size_t tagReaderErrorCount = 0;
      
      for (const auto& error : allErrors) {
        ++errorCodeCounts[error.code];
        if (error.message.find("TagReader") != std::string::npos) {
          ++tagReaderErrorCount;
          if (tagReaderErrorLogger_) {
            const auto pathStr = error.path ? error.path->generic_string() : "(no path)";
            if (error.detail.empty()) {
              tagReaderErrorLogger_->warn("{}: {}", error.message, pathStr);
            } else {
              tagReaderErrorLogger_->warn("{}: {} (detail: {})", error.message, pathStr, error.detail);
            }
          }
        }
      }
      
      spdlog::warn("Error breakdown by type:");
      for (const auto& [code, count] : errorCodeCounts) {
        const char* codeName = "Unknown";
        switch (code) {
          case ScannerErrorCode::RootUnavailable: codeName = "RootUnavailable"; break;
          case ScannerErrorCode::PermissionDenied: codeName = "PermissionDenied"; break;
          case ScannerErrorCode::UnsupportedFile: codeName = "UnsupportedFile"; break;
          case ScannerErrorCode::MetadataReadFailed: codeName = "MetadataReadFailed"; break;
          case ScannerErrorCode::CacheUnavailable: codeName = "CacheUnavailable"; break;
          case ScannerErrorCode::Cancelled: codeName = "Cancelled"; break;
        }
        spdlog::warn("  - {}: {} errors", codeName, count);
      }
      
      if (tagReaderErrorCount > 0) {
        spdlog::warn("  - TagReader errors logged to: tagreader-errors.log ({} errors)", tagReaderErrorCount);
      }
      
      constexpr std::size_t kMaxDetailedErrors = 10;
      const auto detailedErrorCount = std::min(allErrors.size(), kMaxDetailedErrors);
      spdlog::warn("\nFirst {} error(s) with details:", detailedErrorCount);
      for (std::size_t i = 0; i < detailedErrorCount; ++i) {
        const auto& error = allErrors[i];
        const auto pathStr = error.path ? error.path->generic_string() : "(no path)";
        if (error.detail.empty()) {
          spdlog::warn("  [{}] {}: {}", i + 1, error.message, pathStr);
        } else {
          spdlog::warn("  [{}] {}: {} (detail: {})", i + 1, error.message, pathStr, error.detail);
        }
      }
      
      if (allErrors.size() > kMaxDetailedErrors) {
        spdlog::warn("  ... and {} more errors (not shown)", allErrors.size() - kMaxDetailedErrors);
      }
      spdlog::warn("===============================================\n");
    }
    
    spdlog::info("\n========== Performance Analysis Report ==========");
    spdlog::info("Total Wall Time  : {} ms", totalScanTimeMs);
    spdlog::info("Processed Files  : {}", scanned);
    spdlog::info("-----------------------------------------------");
    spdlog::info("[Phase 1] Dir Scan + File Processing: {} ms", phaseEnumTimeMs);
    spdlog::info("[Phase 2] Aggregation               : {} ms", phaseAggregationTimeMs);
    spdlog::info("-----------------------------------------------");
    spdlog::info(">> Cumulative Worker CPU Time (Sum of all threads):");
    spdlog::info("   - TagReader Parse: {} ms", totalTagReaderTimeMs);
    spdlog::info(">> Per-File Average:");
    if (scanned > 0) {
      spdlog::info("   - Avg TagReader  : {:.1f} ms", static_cast<double>(totalTagReaderTimeMs) / scanned);
    }
    spdlog::info("===============================================");

    publishEvent(sink, ScannerEventType::ProgressUpdated, ++eventVersion_, progress);
    publishEvent(sink, ScannerEventType::PlaylistSnapshotUpdated, ++eventVersion_, published);
    publishEvent(sink, ScannerEventType::ScanCompleted, ++eventVersion_, published);
  }

  void startWatching(const std::vector<ScannerRoot>& roots) override {
    stopWatching();
    std::vector<ScannerRoot> normalizedRoots;
    normalizedRoots.reserve(roots.size());
    for (const auto& root : roots) {
      normalizedRoots.push_back(ScannerRoot{.path = rootPathFor(root), .recursive = root.recursive});
    }
    auto state = std::make_shared<WatchRuntimeState>();
    {
      std::scoped_lock lock{state->mutex};
      state->watchedRoots = normalizedRoots;
      state->stopping = false;
    }

    std::vector<std::unique_ptr<FolderWatcher>> watchers;
    watchers.reserve(normalizedRoots.size());
    try {
      for (const auto& root : normalizedRoots) {
        watchers.push_back(watcherFactory_->watch(root.path, [state](const WatchEvent& event) { enqueueWatcherEvent(state, event); }));
      }
    } catch (...) {
      {
        std::scoped_lock lock{state->mutex};
        state->stopping = true;
      }
      state->changed.notify_all();
      for (auto& watcher : watchers) {
        if (watcher) {
          watcher->close();
        }
      }
      throw;
    }
    {
      std::scoped_lock lock{watcherMutex_};
      watcherState_ = state;
      watchers_ = std::move(watchers);
    }
    debounceThread_ = std::thread([this, state] { debounceLoop(state); });
  }

  void stopWatching() override {
    std::vector<std::unique_ptr<FolderWatcher>> watchers;
    std::shared_ptr<WatchRuntimeState> state;
    {
      std::scoped_lock lock{watcherMutex_};
      state = std::move(watcherState_);
      watchers = std::move(watchers_);
    }
    if (state) {
      {
        std::scoped_lock lock{state->mutex};
        state->stopping = true;
        state->pendingWatcherMessages.clear();
      }
      state->changed.notify_all();
    }
    for (auto& watcher : watchers) {
      if (watcher) {
        watcher->close();
      }
    }
    if (debounceThread_.joinable()) {
      debounceThread_.join();
    }
  }

  void stop() override { cancellationRequested_.store(true); }

  [[nodiscard]] PlaylistTreeSnapshot snapshot() const override {
    std::scoped_lock lock{mutex_};
    return snapshot_;
  }

private:
  struct RootResult {
	    struct PublishedSong {
	      cache::CachedSong song;
	      std::filesystem::path treeRelativePath;
	      ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
	      std::optional<std::string> locationId;
	      ExternalLyricsCacheAction externalLyricsCacheAction{ExternalLyricsCacheAction::None};
	    };

	    std::vector<PublishedSong> songs;
	    std::vector<ScannerError> errors;
	    bool cancelled{false};
	  };

  struct AudioReconcileTask {
    std::filesystem::path path;
    std::filesystem::path treeRelativePath;
    std::size_t discoveryIndex{0};
    std::optional<std::string> contentHash;
    std::optional<cache::CachedSong> cachedSong;
  };

  struct WorkerSongPublication {
    cache::CachedSong song;
    ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
    std::optional<std::string> locationId;
  };

  struct WorkerSongStore {
    std::mutex mutex;
    std::map<std::string, WorkerSongPublication> songsByPath;

    void put(const std::filesystem::path& path, WorkerSongPublication song) {
      std::scoped_lock lock{mutex};
      songsByPath[pathKey(path)] = std::move(song);
    }

    [[nodiscard]] std::optional<WorkerSongPublication> take(const std::filesystem::path& path) {
      std::scoped_lock lock{mutex};
      const auto iterator = songsByPath.find(pathKey(path));
      if (iterator == songsByPath.end()) {
        return std::nullopt;
      }
      auto song = std::move(iterator->second);
      songsByPath.erase(iterator);
      return song;
    }
  };

  [[nodiscard]] std::unordered_set<std::string> cueReferencedAudioPaths(const std::vector<RootResult::PublishedSong>& songs) const {
    std::unordered_set<std::string> referencedPaths;
    for (const auto& publishedSong : songs) {
      const auto& metadata = publishedSong.song.metadata;
      if (!metadata.sourceFilePath.empty() && !metadata.filePath.empty() && pathKey(metadata.sourceFilePath) != pathKey(metadata.filePath)) {
        referencedPaths.insert(pathKey(metadata.sourceFilePath));
      }
    }
    return referencedPaths;
  }

  [[nodiscard]] bool hiddenByCueSourceVisibility(const RootResult::PublishedSong& song,
                                                 const std::unordered_set<std::string>& cueSourcePaths) const {
    const auto& metadata = song.song.metadata;
    if (metadata.filePath.empty() || !cueSourcePaths.contains(pathKey(metadata.filePath))) {
      return false;
    }
    return metadata.sourceFilePath.empty() || pathKey(metadata.sourceFilePath) == pathKey(metadata.filePath);
  }

  void publishCancelled(const ScannerEventSink& sink, std::uint64_t scanVersion) {
    ScannerError error{};
    error.code = ScannerErrorCode::Cancelled;
    error.message = "scanner scan cancelled";
    publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
    publishEvent(sink, ScannerEventType::ScanStopped, scanVersion, error);
  }

  // Test seam first, then the TagMetadataReader gateway for production CUE reads.
  [[nodiscard]] std::vector<RawTagMetadata> readCueSheetWithTestSeam(const TagReadRequest& request) {
    if (g_testCueSheetProvider) {
      const auto testTracks = g_testCueSheetProvider(request.path);
      if (!testTracks.empty()) {
        std::vector<RawTagMetadata> results;
        results.reserve(testTracks.size());
        for (const auto& track : testTracks) {
          RawTagMetadata raw{};
          raw.filePath = track.audioFilePath;
          raw.offset = std::chrono::microseconds{track.offset};
          raw.duration = std::chrono::microseconds{track.duration};
          raw.title = track.title;
          raw.artist = track.artist;
          raw.album = track.album;
          raw.trackNumber = track.trackNumber;
          results.push_back(raw);
        }
        return results;
      }
    }
    return metadataReader_->readCueSheet(request);
  }

  [[nodiscard]] RootResult reconcileRoot(const ScannerRoot& root, const ScanModeDecision& decision, const EffectiveScannerConfig& config,
                         [[maybe_unused]] cache::SQLiteCache& cache,
                                         std::uint64_t& discovered, std::uint64_t& skipped, std::uint64_t& scanned,
                                         std::uint64_t& totalTagReaderTimeMs) {
    // Phase timing
    const auto phaseStart = std::chrono::steady_clock::now();
    auto phase1End = phaseStart;
    auto phase2End = phaseStart;
    auto phase3End = phaseStart;
    auto phase4End = phaseStart;
    auto phase5End = phaseStart;

    RootResult result;
    const auto rootPath = rootPathFor(root);
    const auto pathConfig = PathClassificationConfig{.allowedExtensions = config.scanner.allowedExtensions,
                                                     .followSymlinks = config.scanner.followSymlinks,
                                                     .readExternalLyrics = config.scanner.readExternalLyrics};
    cache::SQLiteCache scanRootCache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};

    std::vector<ClassifiedPath> entries;
    std::vector<cache::CachedLocation> cachedLocations;
    bool treeHashMatches = false;
    
    if (decision.mode == ScanMode::Incremental && decision.directoryTreeHash.has_value()) {
      try {
        const auto cachedScanRoot = scanRootCache.loadScanRoot(rootPath);
        if (cachedScanRoot.has_value()) {
          cachedLocations = scanRootCache.loadLocationsByRoot(rootPath);
          if (decision.directoryTreeHash.has_value() && 
              cachedScanRoot->directoryTreeHash == *decision.directoryTreeHash) {
            treeHashMatches = true;
          }
        }
      } catch (const std::exception& error) {
        spdlog::warn("reconcileRoot: failed to load scanner cache for incremental planning: {}", error.what());
      }
    }
    
    entries = discoverScannerPaths(ScannerRoot{.path = rootPath, .recursive = root.recursive}, pathConfig);
    phase1End = std::chrono::steady_clock::now();

	    auto incrementalPlan = decision.mode == ScanMode::Incremental
	                               ? std::optional<IncrementalExecutionPlan>{incrementalExecutionPlan(rootPath, entries, cachedLocations, treeHashMatches)}
	                               : std::nullopt;
	    auto cachedCueTracksByCuePath = buildCachedCueTracksByCuePath(incrementalPlan, scanRootCache);
	    const auto cachedCueSourcePathKeys = cachedCueSourceAudioPathKeys(incrementalPlan);
	    const auto isCachedCueSourceAudio = [&cachedCueSourcePathKeys](const ClassifiedPath& entry) {
	      return entry.kind == PathEntryKind::AudioCandidate && cachedCueSourcePathKeys.contains(pathKey(entry.path));
	    };
	    
	    std::size_t nodeCount = 0;
	    std::unordered_set<std::string> failedCuePaths;
	    for (const auto& entry : entries) {
	      if (isCachedCueSourceAudio(entry)) {
	        continue;
	      }
	      if (entry.kind == PathEntryKind::AudioCandidate || entry.kind == PathEntryKind::SingleFileRoot) {
	        ++nodeCount;
	      } else if (entry.kind == PathEntryKind::CueSheet) {
	        const auto cachedCueTracks = cachedCueTracksByCuePath.find(pathKey(entry.path));
	        if (cachedCueTracks != cachedCueTracksByCuePath.end()) {
	          nodeCount += 1 + cachedCueTracks->second.size();
	          continue;
	        }
        try {
          const auto tracks = readCueSheetWithTestSeam(thumbnailOnlyRequest(entry.path, coverExportDir_));
          nodeCount += 1 + tracks.size();
        } catch (const std::exception& error) {
          result.errors.push_back(ScannerError{
            .code = ScannerErrorCode::MetadataReadFailed,
            .message = "Failed to read CUE sheet metadata",
            .detail = error.what(),
            .path = entry.path
          });
          spdlog::warn("CUE sheet metadata read failed for {}: {}", entry.path.generic_string(), error.what());
          failedCuePaths.insert(pathKey(entry.path));
          ++nodeCount;
        }
      }
    }
    
    std::vector<IndexedPublishedSong> indexedSongs(nodeCount);
    std::vector<AudioReconcileTask> audioTasks;
    std::vector<WorkerTask> workerTasks;
    audioTasks.reserve(entries.size());
    workerTasks.reserve(entries.size());

	    auto discoveryIndex = std::size_t{0};
	    for (const auto& entry : entries) {
	      if (isCachedCueSourceAudio(entry)) {
	        continue;
	      }
	      for (const auto& error : entry.errors) {
	        result.errors.push_back(scannerErrorFrom(error));
	      }
      
      if (entry.kind == PathEntryKind::CueSheet) {
        if (failedCuePaths.contains(pathKey(entry.path))) {
          const auto cueContainerIndex = discoveryIndex++;
          ++discovered;
          indexedSongs[cueContainerIndex].discoveryIndex = cueContainerIndex;
          indexedSongs[cueContainerIndex].treeRelativePath = relativePathFor(rootPath, entry.path);
          indexedSongs[cueContainerIndex].nodeType = NodeType::CueContainer;
          indexedSongs[cueContainerIndex].origin = ScanItemOrigin::VirtualContainer;
          indexedSongs[cueContainerIndex].isVirtualFolder = true;
          continue;
        }
        
        const auto cueContainerIndex = discoveryIndex++;
        ++discovered;
        
        indexedSongs[cueContainerIndex].discoveryIndex = cueContainerIndex;
        indexedSongs[cueContainerIndex].treeRelativePath = relativePathFor(rootPath, entry.path);
        indexedSongs[cueContainerIndex].nodeType = NodeType::CueContainer;
	        indexedSongs[cueContainerIndex].origin = ScanItemOrigin::VirtualContainer;
	        indexedSongs[cueContainerIndex].isVirtualFolder = true;
	        const auto cachedCueTracks = cachedCueTracksByCuePath.find(pathKey(entry.path));
	        if (cachedCueTracks != cachedCueTracksByCuePath.end()) {
	          for (const auto& cachedTrack : cachedCueTracks->second) {
	            const auto trackNodeIndex = discoveryIndex++;
	            ++discovered;
	            ++skipped;

	            const auto offset = cachedTrack.location.cueTrackOffset.value_or(std::chrono::milliseconds{0});
	            const auto duration = cachedTrack.location.cueTrackDuration.value_or(
	                cachedTrack.song.metadata.duration.value_or(std::chrono::milliseconds{0}));
	            indexedSongs[trackNodeIndex].discoveryIndex = trackNodeIndex;
	            indexedSongs[trackNodeIndex].treeRelativePath = relativePathFor(rootPath, entry.path);
	            indexedSongs[trackNodeIndex].nodeType = NodeType::CueTrack;
	            indexedSongs[trackNodeIndex].cueInfo = CueInfo{
	              .cueFilePath = entry.path,
	              .audioFilePath = cachedTrack.location.sourceFilePath,
	              .offset = std::chrono::duration_cast<std::chrono::microseconds>(offset),
	              .duration = std::chrono::duration_cast<std::chrono::microseconds>(duration),
	              .trackIndex = static_cast<std::size_t>(cachedTrack.trackIndex)
	            };
	            indexedSongs[trackNodeIndex].song = cachedTrack.song;
	            indexedSongs[trackNodeIndex].origin = ScanItemOrigin::CueTrackCacheHit;
	            indexedSongs[trackNodeIndex].locationId = cachedTrack.location.locationId;
	            indexedSongs[trackNodeIndex].filled.store(true);
	            if (incrementalPlan.has_value()) {
	              incrementalPlan->retainedLocationIds.push_back(cachedTrack.location.locationId);
	            }
	          }
	          continue;
	        }
	        
	        const auto tracks = readCueSheetWithTestSeam(thumbnailOnlyRequest(entry.path, coverExportDir_));
	        const auto cueTrackOrigin = cueReaderOriginForPath(incrementalPlan, entry.path);
	        const auto cueFileSizeForLocation = fileSizeBytes(entry.path);
	        const auto cueFileMtimeForLocation = fileMtime(entry.path);
        for (std::size_t trackIdx = 0; trackIdx < tracks.size(); ++trackIdx) {
            const auto trackNodeIndex = discoveryIndex++;
            ++discovered;
            
            indexedSongs[trackNodeIndex].discoveryIndex = trackNodeIndex;
            indexedSongs[trackNodeIndex].treeRelativePath = relativePathFor(rootPath, entry.path);
            indexedSongs[trackNodeIndex].nodeType = NodeType::CueTrack;
            indexedSongs[trackNodeIndex].cueInfo = CueInfo{
              .cueFilePath = entry.path,
              .audioFilePath = tracks[trackIdx].filePath,
              .offset = std::chrono::microseconds(tracks[trackIdx].offset),
              .duration = std::chrono::microseconds(tracks[trackIdx].duration),
              .trackIndex = trackIdx
            };
            
            const auto& trackRaw = tracks[trackIdx];
            const auto contentHash = std::string{"cue:"} + entry.path.generic_string() + "#" + std::to_string(trackIdx);
            auto mapped = mapRawTagMetadata(trackRaw, contentHash, std::nullopt, false);
            
            mapped.metadata.sourceFilePath = trackRaw.filePath;
            mapped.metadata.filePath = entry.path;
            mapped.metadata.offset = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(trackRaw.offset));
            mapped.metadata.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(trackRaw.duration));
            mapped.metadata.logicalTrackId = entry.path.generic_string() + "#track" + std::to_string(trackIdx);
            mapped.metadata.trackId = mapped.metadata.logicalTrackId;
            
	            auto cachedSong = cachedSongFrom(std::move(mapped));
	            indexedSongs[trackNodeIndex].song = std::move(cachedSong);
	            indexedSongs[trackNodeIndex].origin = cueTrackOrigin;
	            if (cueFileSizeForLocation.has_value()) {
	              indexedSongs[trackNodeIndex].locationId = computeLocationId(entry.path,
	                                                                          *cueFileSizeForLocation,
	                                                                          cueFileMtimeForLocation,
	                                                                          indexedSongs[trackNodeIndex].song.metadata.offset,
	                                                                          static_cast<std::uint32_t>(trackIdx));
	            }
	            indexedSongs[trackNodeIndex].filled.store(true);
	          }
        continue;
      }
      
      if (entry.kind != PathEntryKind::AudioCandidate && entry.kind != PathEntryKind::SingleFileRoot) {
        continue;
      }
      const auto currentDiscoveryIndex = discoveryIndex++;
      ++discovered;
      const auto entryKey = pathKey(entry.path);
      bool shouldProcessViaWorker = true;
      if (incrementalPlan.has_value() && incrementalPlan->unchangedPaths.contains(entryKey)) {
        try {
          const auto fileSize = fileSizeBytes(entry.path);
          const auto fileMtimeValue = fileMtime(entry.path);
          const auto locationId = fileSize.has_value() ? computeLocationId(entry.path, *fileSize, fileMtimeValue) : std::string{};
          const auto cachedLocation = locationId.empty() ? std::optional<cache::CachedLocation>{} : scanRootCache.loadLocation(locationId);
          if (cachedLocation.has_value()) {
            const auto cachedSong = scanRootCache.loadContent(cachedLocation->contentId);
            if (cachedSong.has_value()) {
              ++skipped;
              spdlog::debug("Cache hit for nodeIndex={}, filePath={}", currentDiscoveryIndex, entry.path.generic_string());
              auto hydratedSong = *cachedSong;
              hydratedSong.embeddedLyrics = scanRootCache.loadLyrics(cachedLocation->locationId, "embedded");
              hydratedSong.externalLyrics = scanRootCache.loadLyrics(cachedLocation->locationId, "external");
              applyCachedLocation(hydratedSong, *cachedLocation, entry.path);
              indexedSongs[currentDiscoveryIndex] = IndexedPublishedSong{currentDiscoveryIndex, std::move(hydratedSong), relativePathFor(rootPath, entry.path)};
              indexedSongs[currentDiscoveryIndex].origin = ScanItemOrigin::CacheHit;
              indexedSongs[currentDiscoveryIndex].locationId = cachedLocation->locationId;
              indexedSongs[currentDiscoveryIndex].filled.store(true);
              incrementalPlan->retainedLocationIds.push_back(cachedLocation->locationId);
              shouldProcessViaWorker = false;
            }
          }
        } catch (const std::exception& error) {
          spdlog::warn("failed to hydrate scanner cache hit for {}: {}", entry.path.generic_string(), error.what());
        }
      }
      if (!shouldProcessViaWorker) {
        continue;
      }
	      const auto fileSize = fileSizeBytes(entry.path);
	      const auto fileMtimeValue = fileMtime(entry.path);
	      const auto locationId = fileSize.has_value() ? computeLocationId(entry.path, *fileSize, fileMtimeValue) : std::string{};
	      spdlog::trace("Preparing audio task: nodeIndex={}, filePath={}", currentDiscoveryIndex, entry.path.generic_string());
	      auto audioTask = prepareAudioTask(entry.path, rootPath, result.errors, skipped, scanned);
      if (!audioTask.has_value()) {
        spdlog::warn("prepareAudioTask returned nullopt for nodeIndex={}, filePath={}", currentDiscoveryIndex, entry.path.generic_string());
        continue;
      }
      audioTask->discoveryIndex = currentDiscoveryIndex;
      audioTasks.push_back(*audioTask);
      
	      workerTasks.push_back(WorkerTask{.rootPath = rootPath,
	                                       .filePath = entry.path,
	                                       .locationId = locationId,
	                                       .cachedLocation = std::nullopt,
	                                       .nodeIndex = currentDiscoveryIndex});
      spdlog::trace("Queued worker task: nodeIndex={}, filePath={}", currentDiscoveryIndex, entry.path.generic_string());
    }
    phase2End = std::chrono::steady_clock::now();

    std::unordered_map<std::string, std::size_t> audioTaskIndexByPath;
    audioTaskIndexByPath.reserve(audioTasks.size());
    for (std::size_t i = 0; i < audioTasks.size(); ++i) {
      audioTaskIndexByPath[pathKey(audioTasks[i].path)] = i;
    }

    std::vector<std::size_t> workerTaskNodeIndices;
    workerTaskNodeIndices.reserve(workerTasks.size());
	    for (const auto& task : workerTasks) {
	      workerTaskNodeIndices.push_back(task.nodeIndex);
	    }
	    const auto workerOriginsByPath = incrementalPlan.has_value()
	                                      ? incrementalPlan->workerOriginsByPath
	                                      : std::unordered_map<std::string, ScanItemOrigin>{};
	    publishWorkerTaskSnapshot(workerTasks, workerOriginsByPath);

	    auto workerSongs = std::make_shared<WorkerSongStore>();
	    ScannerWorkerPool workerPool{ScannerWorkerPool::Config{.workerCount = config.workerCount,
	                                                           .tagReaderSlots = config.tagReaderSlots,
	      .tagReader = [this, workerSongs, &audioTasks, &audioTaskIndexByPath, &indexedSongs, &rootPath, workerOriginsByPath](const WorkerTask& task) {
	        const auto workerOrigin = workerOriginForPath(workerOriginsByPath, task.filePath);
	        auto metadata = readWorkerSong(task, audioTasks, audioTaskIndexByPath, workerOrigin, workerSongs);
                                                             if (task.nodeIndex < indexedSongs.size()) {
                                                               auto song = workerSongs->take(task.filePath);
                                                               if (song.has_value()) {
                                                                 indexedSongs[task.nodeIndex].discoveryIndex = task.nodeIndex;
                                                                 indexedSongs[task.nodeIndex].song = std::move(song->song);
                                                                 indexedSongs[task.nodeIndex].origin = song->origin;
                                                                 indexedSongs[task.nodeIndex].locationId = std::move(song->locationId);
                                                                 indexedSongs[task.nodeIndex].treeRelativePath = relativePathFor(rootPath, task.filePath);
                                                                 indexedSongs[task.nodeIndex].filled.store(true);
                                                               } else {
                                                                 spdlog::debug("Worker callback: take() returned nullopt for task.filePath={}, nodeIndex={}",
                                                                               task.filePath.generic_string(), task.nodeIndex);
                                                               }
                                                             } else {
                                                               spdlog::error("Worker callback: task.nodeIndex={} >= indexedSongs.size()={}", 
                                                                            task.nodeIndex, indexedSongs.size());
                                                             }
                                                             return metadata;
                                                           }}};
    workerPool.submitBatch(std::move(workerTasks));
    auto workerResults = workerPool.waitAll();
    phase3End = std::chrono::steady_clock::now();
    const auto workerStats = workerPool.statsSnapshot();
    totalTagReaderTimeMs += std::chrono::duration_cast<std::chrono::milliseconds>(workerStats.tagReaderTime).count();
    
    for (std::size_t i = 0; i < workerResults.size() && i < workerTaskNodeIndices.size(); ++i) {
      const auto& workResult = workerResults[i];
      const auto nodeIdx = workerTaskNodeIndices[i];
      if (!workResult.error && workResult.metadata.has_value() && nodeIdx < indexedSongs.size()) {
        auto song = workerSongs->take(workResult.filePath);
        if (song.has_value()) {
          indexedSongs[nodeIdx].song = std::move(song->song);
          indexedSongs[nodeIdx].origin = song->origin;
          indexedSongs[nodeIdx].locationId = std::move(song->locationId);
          indexedSongs[nodeIdx].treeRelativePath = relativePathFor(rootPath, workResult.filePath);
          indexedSongs[nodeIdx].filled.store(true);
        } else {
          spdlog::debug("workerSongs->take() returned nullopt for cache-hit nodeIndex={}, filePath={}", 
                        nodeIdx, workResult.filePath.generic_string());
        }
      }
    }
    
    for (const auto& workerError : workerPool.errorsSnapshot()) {
      result.errors.push_back(scannerErrorFromWorker(workerError));
    }
    
    if (g_preallocationObserver) {
      g_preallocationObserver(indexedSongs);
    }
    std::size_t filledCount = 0;
    std::size_t unfilledCount = 0;
    for (auto& indexedSong : indexedSongs) {
      if (indexedSong.nodeType == NodeType::CueContainer) {
        indexedSong.song.metadata.filePath = indexedSong.treeRelativePath;
        indexedSong.song.metadata.logicalTrackId = indexedSong.treeRelativePath.generic_string();
        result.songs.push_back({.song = std::move(indexedSong.song),
                                .treeRelativePath = std::move(indexedSong.treeRelativePath),
                                .origin = indexedSong.origin,
                                .locationId = std::move(indexedSong.locationId)});
        ++filledCount;
      } else if (indexedSong.filled.load()) {
        result.songs.push_back({.song = std::move(indexedSong.song),
                                .treeRelativePath = std::move(indexedSong.treeRelativePath),
                                .origin = indexedSong.origin,
                                .locationId = std::move(indexedSong.locationId)});
        ++filledCount;
      } else {
        ++unfilledCount;
        spdlog::debug("indexedSong[{}] unfilled: nodeType={}, discoveryIndex={}, treeRelativePath={}", 
                     &indexedSong - &indexedSongs[0],
                     static_cast<int>(indexedSong.nodeType),
                     indexedSong.discoveryIndex,
                     indexedSong.treeRelativePath.generic_string());
      }
    }
	    if (unfilledCount > 0) {
	      spdlog::warn("reconcileRoot: {} filled, {} unfilled nodes after worker completion", filledCount, unfilledCount);
	    }

	    if (g_publishedSongObserver != nullptr) {
	      std::vector<PublishedSongSnapshot> snapshots;
	      snapshots.reserve(result.songs.size());
	      for (const auto& publishedSong : result.songs) {
	        snapshots.push_back(PublishedSongSnapshot{.filePath = publishedSong.song.metadata.filePath,
	                                                  .treeRelativePath = publishedSong.treeRelativePath,
	                                                  .origin = publishedSong.origin,
	                                                  .locationId = publishedSong.locationId});
	      }
	      g_publishedSongObserver(snapshots);
	    }

	    for (auto& publishedSong : result.songs) {
      publishedSong.externalLyricsCacheAction = reconcileLyrics(publishedSong.song, config.scanner, result.errors);
      if (publishedSong.externalLyricsCacheAction == ExternalLyricsCacheAction::Cancelled) {
        result.cancelled = true;
        publishIncrementalPlanSnapshot(incrementalPlan);
        return result;
      }
      if (incrementalPlan.has_value() && publishedSong.origin == ScanItemOrigin::CacheHit &&
          publishedSong.locationId.has_value() && publishedSong.externalLyricsCacheAction != ExternalLyricsCacheAction::None) {
        incrementalPlan->lyricsOnlyUpdates.push_back(
            cache::LyricsCacheUpdate{.locationId = *publishedSong.locationId,
                                     .externalLrcPath = publishedSong.song.metadata.externalLyricsPath,
                                     .externalLrcMtimeNs = fileTimeNanoseconds(publishedSong.song.metadata.externalLyricsMtime),
                                     .externalLrcHash = publishedSong.song.metadata.externalLyricsHash,
                                     .externalLyrics = publishedSong.song.externalLyrics,
                                     .removeExternalLyrics = publishedSong.externalLyricsCacheAction == ExternalLyricsCacheAction::RemoveExternal});
      }
    }
    publishIncrementalPlanSnapshot(incrementalPlan);
    
    phase4End = std::chrono::steady_clock::now();
    
    phase5End = std::chrono::steady_clock::now();

    const auto phase1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase1End - phaseStart).count();
    const auto phase2Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase2End - phase1End).count();
    const auto phase3Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase3End - phase2End).count();
    const auto phase4Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase4End - phase3End).count();
    const auto phase5Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phase4End).count();
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phaseStart).count();

    spdlog::info("reconcileRoot phase timing for {}: total={}ms | discovery={}ms | task-prep={}ms | worker-wait={}ms | finalize={}ms | cache-save={}ms",
                 rootPath.generic_string(), totalMs, phase1Ms, phase2Ms, phase3Ms, phase4Ms, phase5Ms);

    return result;
  }

	  void recordScanRootDecision(const std::filesystem::path& rootPath,
	                              const ScanModeDecision& decision,
	                              const std::vector<RootResult::PublishedSong>& songs,
	                              const std::chrono::milliseconds scanDuration) const {
	    if (!decision.directoryTreeHash.has_value()) {
	      return;
	    }
	    try {
	      cache::ScanRootCacheWrite write;
	      write.root = scanRootRecord(rootPath, decision, songs.size(), scanDuration);
	      write.retainedLocationIds.reserve(songs.size());
	      for (const auto& publishedSong : songs) {
	        if (!shouldRetainLocationForOrigin(publishedSong.origin)) {
	          continue;
	        }
	        if (!publishedSong.song.metadata.duration.has_value() || publishedSong.song.metadata.contentHash.empty()) {
	          continue;
	        }
	        const auto location = cachedLocationFromSong(publishedSong.song, rootPath, publishedSong.song.metadata.filePath);
	        write.retainedLocationIds.push_back(location.locationId);
	        if (publishedSong.origin == ScanItemOrigin::CacheHit &&
	            publishedSong.externalLyricsCacheAction != ExternalLyricsCacheAction::None) {
	          write.lyricsUpdates.push_back(cache::LyricsCacheUpdate{
	            .locationId = location.locationId,
	            .externalLrcPath = publishedSong.song.metadata.externalLyricsPath,
	            .externalLrcMtimeNs = fileTimeNanoseconds(publishedSong.song.metadata.externalLyricsMtime),
	            .externalLrcHash = publishedSong.song.metadata.externalLyricsHash,
	            .externalLyrics = publishedSong.song.externalLyrics,
	            .effectiveLyricsSource = publishedSong.song.metadata.effectiveLyricsSource,
	            .removeExternalLyrics = publishedSong.externalLyricsCacheAction == ExternalLyricsCacheAction::RemoveExternal});
	          continue;
	        }
	        if (shouldWriteSongForOrigin(publishedSong.origin)) {
	          write.changedSongs.push_back(cache::CacheWriteSong{.song = publishedSong.song, .location = location});
	        } else if (shouldWriteCueTrackForOrigin(publishedSong.origin)) {
	          write.changedCueTracks.push_back(cache::CacheWriteSong{.song = publishedSong.song, .location = location});
	        }
	      }
	      publishCacheWriteSnapshot(write);
	      cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
	      cache.recordScanRootCacheWrite(write);
	    } catch (const std::exception& error) {
	      spdlog::warn("failed to record scanner scan-root state: {}", error.what());
	    }
	  }

  [[nodiscard]] std::optional<AudioReconcileTask> prepareAudioTask(const std::filesystem::path& audioPath,
                                                                   const std::filesystem::path& rootPath,
                                                                   std::vector<ScannerError>&,
                                                                   std::uint64_t&,
                                                                   std::uint64_t& scanned) {
    ++scanned;
    return AudioReconcileTask{.path = audioPath,
                              .treeRelativePath = relativePathFor(rootPath, audioPath),
                              .discoveryIndex = 0,
                              .contentHash = std::nullopt,
                              .cachedSong = std::nullopt};
  }

	  [[nodiscard]] SongMetadata readWorkerSong(const WorkerTask& task,
	                                            const std::vector<AudioReconcileTask>& audioTasks,
	                                            const std::unordered_map<std::string, std::size_t>& audioTaskIndexByPath,
	                                            ScanItemOrigin workerOrigin,
	                                            const std::shared_ptr<WorkerSongStore>& workerSongs) {
	    spdlog::trace("readWorkerSong called: nodeIndex={}, filePath={}, hasCachedLocation={}", 
	                 task.nodeIndex, task.filePath.generic_string(), task.cachedLocation.has_value());
    const auto audioTask = audioTaskByPath(audioTasks, audioTaskIndexByPath, task.filePath);
    if (audioTask == nullptr) {
	      throw std::runtime_error{"missing scanner worker task context"};
	    }

	    auto raw = metadataReader_->read(thumbnailOnlyRequest(task.filePath, coverExportDir_));
    raw.filePath = task.filePath;
    auto mapped = mapRawTagMetadata(raw,
                                    computeContentId(std::chrono::duration_cast<std::chrono::milliseconds>(raw.duration),
                                                     raw.title,
                                                     raw.artist),
                                    std::nullopt,
                                    false);
    auto song = cachedSongFrom(std::move(mapped));
    song.metadata.filePath = task.filePath;
    song.metadata.sourceFilePath = task.filePath;
    if (song.metadata.artworkPath.has_value() && !song.metadata.artworkPath->empty() &&
        song.metadata.artworkPath->is_relative()) {
      song.metadata.artworkPath = std::filesystem::absolute(*song.metadata.artworkPath);
    }
    if (song.metadata.thumbnailPath.has_value() && !song.metadata.thumbnailPath->empty() &&
        song.metadata.thumbnailPath->is_relative()) {
      song.metadata.thumbnailPath = std::filesystem::absolute(*song.metadata.thumbnailPath);
    }
    song.metadata.trackId = task.filePath.generic_string();
    song.metadata.logicalTrackId = task.filePath.generic_string();
	    auto metadata = song.metadata;
	    workerSongs->put(task.filePath,
	                     WorkerSongPublication{.song = std::move(song),
	                                           .origin = workerOrigin,
	                                           .locationId = task.locationId.empty() ? std::optional<std::string>{}
	                                                                              : std::optional<std::string>{task.locationId}});
    return metadata;
  }

  [[nodiscard]] const AudioReconcileTask* audioTaskByPath(const std::vector<AudioReconcileTask>& audioTasks,
                                                         const std::unordered_map<std::string, std::size_t>& audioTaskIndexByPath,
                                                         const std::filesystem::path& path) const {
    const auto key = pathKey(path);
    const auto it = audioTaskIndexByPath.find(key);
    if (it == audioTaskIndexByPath.end()) {
      return nullptr;
    }
    return &audioTasks[it->second];
  }

  [[nodiscard]] std::optional<cache::CachedSong> cachedSongForWorkerResult(const WorkerResult& workerResult,
                                                                           const std::vector<AudioReconcileTask>& audioTasks,
                                                                           const std::unordered_map<std::string, std::size_t>& audioTaskIndexByPath) const {
    const auto audioTask = audioTaskByPath(audioTasks, audioTaskIndexByPath, workerResult.filePath);
    if (audioTask == nullptr || !audioTask->cachedSong.has_value()) {
      return std::nullopt;
    }
    return audioTask->cachedSong;
  }

	  ExternalLyricsCacheAction reconcileLyrics(cache::CachedSong& song, const ScannerConfig& config,
	                                            std::vector<ScannerError>& errors) {
	    const auto sidecar = expectedLyricsSidecarPath(song.metadata.filePath);
	    const auto hadExternalCache = !song.externalLyrics.empty() || song.metadata.externalLyricsPath.has_value() ||
	                                  song.metadata.externalLyricsHash.has_value();
	    auto clearExternalLyrics = [&song, hadExternalCache] {
	      song.externalLyrics.clear();
	      selectEffectiveLyrics(song);
	      return hadExternalCache ? ExternalLyricsCacheAction::RemoveExternal : ExternalLyricsCacheAction::None;
	    };

	    if (!config.readExternalLyrics) {
	      song.externalLyrics.clear();
	      selectEffectiveLyrics(song);
	      return ExternalLyricsCacheAction::None;
	    }
	    if (!std::filesystem::is_regular_file(sidecar)) {
	      return clearExternalLyrics();
	    }

	    const auto lrcHash = hashLyricsSidecarWithTestSeam(sidecar, HashOptions{.cancellationRequested = &cancellationRequested_});
	    const auto hashCancelled = std::ranges::any_of(lrcHash.errors, [](const HashError& error) {
	      return error.code == HashErrorCode::Cancelled;
	    });
	    if (hashCancelled) {
	      return ExternalLyricsCacheAction::Cancelled;
	    }
	    for (const auto& error : lrcHash.errors) {
	      errors.push_back(scannerErrorFrom(error));
	    }
	    if (!lrcHash.hash.has_value()) {
	      return clearExternalLyrics();
	    }

	    const auto relativeSidecar = relativePathFor(song.metadata.filePath.parent_path(), sidecar);
	    if (song.metadata.externalLyricsHash == lrcHash.hash && !song.externalLyrics.empty()) {
	      song.metadata.externalLyricsPath = relativeSidecar;
	      song.metadata.externalLyricsMtime = fileMtime(sidecar);
	      selectEffectiveLyrics(song);
	      return ExternalLyricsCacheAction::None;
	    }

	    const auto parsed = parseLrcFileWithTestSeam(sidecar);
	    for (const auto& error : parsed.errors) {
	      errors.push_back(scannerErrorFrom(error));
	    }
	    if (!parsed.errors.empty()) {
	      return clearExternalLyrics();
	    }

	    song.externalLyrics = parsed.lines;
	    if (song.externalLyrics.empty()) {
	      return clearExternalLyrics();
	    }
	    song.metadata.externalLyricsPath = relativeSidecar;
	    song.metadata.externalLyricsHash = lrcHash.hash;
	    song.metadata.externalLyricsMtime = fileMtime(sidecar);
	    selectEffectiveLyrics(song);
	    return ExternalLyricsCacheAction::UpdateExternal;
	  }

  void debounceLoop(const std::shared_ptr<WatchRuntimeState>& state) {
    std::uint64_t processedGeneration = 0;
    while (true) {
      std::vector<ScannerRoot> roots;
      std::vector<std::string> watcherMessages;
      {
        std::unique_lock lock{state->mutex};
        state->changed.wait(lock, [&state, processedGeneration] {
          return state->stopping || state->dirtyGeneration != processedGeneration;
        });
        if (state->stopping) {
          return;
        }
        auto observedGeneration = state->dirtyGeneration;
        state->changed.wait_for(lock, watcherDebounce_, [&state, observedGeneration] {
          return state->stopping || state->dirtyGeneration != observedGeneration;
        });
        if (state->stopping) {
          return;
        }
        if (state->dirtyGeneration != observedGeneration) {
          continue;
        }
        processedGeneration = observedGeneration;
        roots = state->watchedRoots;
        watcherMessages = std::move(state->pendingWatcherMessages);
        state->pendingWatcherMessages.clear();
      }
      publishWatcherMessages(watcherMessages);
      scan(roots, ScanMode::Incremental);
    }
  }

  void publishWatcherMessages(const std::vector<std::string>& messages) {
    ScannerEventSink sink;
    {
      std::scoped_lock lock{mutex_};
      sink = sink_;
    }
    for (const auto& message : messages) {
      publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_,
                   ScannerError{.code = ScannerErrorCode::CacheUnavailable,
                                .message = "watcher requested root reconciliation",
                                .detail = message,
                                .path = std::nullopt});
    }
  }

  struct ScanRequest {
    std::vector<ScannerRoot> roots;
    ScanMode mode{ScanMode::Incremental};
  };

  void scanWorkerLoop() {
    while (true) {
      ScanRequest request;
      {
        std::unique_lock lock{scanQueueMutex_};
        scanQueueChanged_.wait(lock, [this] { return scanWorkerStopping_ || !scanQueue_.empty(); });
        if (scanWorkerStopping_ && scanQueue_.empty()) {
          return;
        }
        request = std::move(scanQueue_.front());
        scanQueue_.pop_front();
      }
      runScan(request.roots, request.mode);
    }
  }

  void stopScanWorker() {
    {
      std::lock_guard lock{scanQueueMutex_};
      scanWorkerStopping_ = true;
      scanQueue_.clear();
    }
    cancellationRequested_.store(true);
    scanQueueChanged_.notify_all();
    if (scanWorker_.joinable()) {
      scanWorker_.join();
    }
  }

  ScannerEventSink sink_{};
  ScannerConfig config_{};
  std::shared_ptr<TagMetadataReader> metadataReader_;
  std::filesystem::path databasePath_;
  std::filesystem::path coverExportDir_;
  std::shared_ptr<FolderWatcherFactory> watcherFactory_;
  std::chrono::milliseconds watcherDebounce_{50};
  PlaylistTreeSnapshot snapshot_{};
  mutable std::mutex mutex_;
  std::mutex scanMutex_;
  std::mutex scanQueueMutex_;
  std::mutex watcherMutex_;
  std::condition_variable scanQueueChanged_;
  std::deque<ScanRequest> scanQueue_;
  std::vector<std::unique_ptr<FolderWatcher>> watchers_;
  std::shared_ptr<WatchRuntimeState> watcherState_;
  std::atomic_bool cancellationRequested_{false};
  std::atomic_uint64_t eventVersion_{0};
  bool scanWorkerStopping_{false};
  std::thread scanWorker_{[this] { scanWorkerLoop(); }};
  std::thread debounceThread_;
  std::shared_ptr<spdlog::logger> tagReaderErrorLogger_;
};

}

std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies) {
  return std::make_shared<OrchestratedFileScannerService>(std::move(dependencies));
}

void setPreallocationObserver(PreallocationObserver observer) {
  g_preallocationObserver = std::move(observer);
}

void clearPreallocationObserver() {
  g_preallocationObserver = nullptr;
}

void setWorkerTaskObserver(WorkerTaskObserver observer) {
  g_workerTaskObserver = std::move(observer);
}

void clearWorkerTaskObserver() {
  g_workerTaskObserver = nullptr;
}

void setPublishedSongObserver(PublishedSongObserver observer) {
  g_publishedSongObserver = std::move(observer);
}

void clearPublishedSongObserver() {
  g_publishedSongObserver = nullptr;
}

void setTestCueSheetProvider(TestCueSheetProvider provider) {
  g_testCueSheetProvider = std::move(provider);
}

void clearTestCueSheetProvider() {
  g_testCueSheetProvider = nullptr;
}

void setLrcParseObserver(LrcParseObserver observer) {
  g_lrcParseObserver = std::move(observer);
}

void clearLrcParseObserver() {
  g_lrcParseObserver = nullptr;
}

void setTestLyricsSidecarHashProvider(TestLyricsSidecarHashProvider provider) {
  g_testLyricsSidecarHashProvider = std::move(provider);
}

void clearTestLyricsSidecarHashProvider() {
  g_testLyricsSidecarHashProvider = nullptr;
}

void setIncrementalPlanObserver(IncrementalPlanObserver observer) {
  g_incrementalPlanObserver = std::move(observer);
}

void clearIncrementalPlanObserver() {
  g_incrementalPlanObserver = nullptr;
}

void setCacheWriteObserver(CacheWriteObserver observer) {
  g_cacheWriteObserver = std::move(observer);
}

void clearCacheWriteObserver() {
  g_cacheWriteObserver = nullptr;
}

}
