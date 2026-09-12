#include "file_scanner_service_internal.h"
#include "scanner_internal_types.h"
#include "file_scanner_orchestrator_test_access.h"
#include "path_utf8.h"

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

#include <efsw/efsw.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <iterator>
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
static WatcherEventQueueObserver g_watcherEventQueueObserver = nullptr;
static TreeBuilderObserver g_treeBuilderObserver = nullptr;

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

[[nodiscard]] std::string pathKey(const std::filesystem::path& path) { return pathToUtf8(path.lexically_normal()); }

[[nodiscard]] std::filesystem::file_time_type fileTimeFromNanoseconds(std::int64_t value) {
  return std::filesystem::file_time_type{
      std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds{value})};
}

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
  // 内部 Reconcile（设计 §7 / Oracle B4）：全根 stat + locationId 比对 + 仅变化文件重读；
  // 永不因 hash 缺失/不一致升级 Full，hash 缺失或缓存不可读时中止本次对账（保留既有索引）。
  bool reconcile{false};
};

// T1 scoped 扫描成本门（设计 §13.1 B5）：scope 前缀下的缓存 location 行数超过阈值时改用
// Reconcile（只重读变化文件），而不是 scoped Full 语义重读整个 scope。
// 阈值 = max(500, 根文件数 / 10)：小目录事件保持 T1；大 scope（祖先兜底落到大子树）用
// Reconcile 的标签读取成本更低。
constexpr std::size_t kScopedScanLocationFloor = 500;
constexpr std::size_t kScopedScanRootFractionDivisor = 10;

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
  auto path = databasePath;
  path += ".scan-roots.sqlite";
  return path;
}

[[nodiscard]] cache::CachedScanRoot scanRootRecord(const std::filesystem::path& rootPath,
                                                   const ScanModeDecision& decision,
                                                   const std::uint64_t totalFiles,
                                                   const std::chrono::milliseconds scanDuration) {
  return {.rootPath = rootPath,
          .directoryTreeHash = decision.directoryTreeHash.value_or(std::string{}),
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
  const auto prefix = pathToUtf8(cuePath) + "#track";
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

  IncrementalScanPlan plan;
  for (const auto& entry : fileSystemEntries) {
    if (!isPlanAudioCandidate(entry) && entry.kind != PathEntryKind::CueSheet) {
      continue;
    }
    const auto normalizedPathKey = pathKey(entry.path);

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

// ---------------------------------------------------------------------------
// 封面/缩略图缓存路径的持久化形态：SQLite 只存相对 coverExportDir 的路径，
// 内存/快照保持绝对路径。此前 DB 存绝对路径，应用目录整体移动后全部失效且
// 目录树哈希未变（增量扫描永不触发重导出），缩略图永久丢失；相对存储可移植。
// 旧库绝对路径由 resolveCoverPath 在读取时兼容。
// ---------------------------------------------------------------------------

// 写侧：绝对路径若位于 coverExportDir 树下 → 相对；否则（异常数据）原样保留。
[[nodiscard]] std::optional<std::filesystem::path> coverRelativePath(
    const std::optional<std::filesystem::path>& path,
    const std::filesystem::path& coverExportDir) {
  if (!path.has_value() || path->empty() || path->is_relative()) {
    return path;
  }
  const auto relative = path->lexically_normal().lexically_relative(coverExportDir.lexically_normal());
  if (relative.empty() || relative.is_absolute() ||
      (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path{".."})) {
    return path;
  }
  return relative;
}

// 读侧逆变换：相对路径 → coverExportDir 拼接；仍位于当前 coverExportDir 下的
// 旧绝对路径直接可用；已失效（目录移动）的旧绝对路径按其结构锚重建：
// 取最后一个 "/artwork/" 或 "/thumbnails/" 之后的内容作为 coverExportDir 的
// 相对尾段。thumbnail 绝对路径含 "/artwork/thumbnails/"（取更靠后的 thumbnails
// 锚），artwork 大图绝对路径含两个 "/artwork/"（rfind 命中内部段），均成立；
// 锚缺失则原样返回（维持旧行为，不更糟）。
[[nodiscard]] std::optional<std::filesystem::path> resolveCoverPath(
    const std::optional<std::filesystem::path>& path,
    const std::filesystem::path& coverExportDir) {
  if (!path.has_value() || path->empty()) {
    return path;
  }
  const auto normalized = path->lexically_normal();
  if (normalized.is_relative()) {
    if (normalized.begin() != normalized.end() &&
        *normalized.begin() == std::filesystem::path{".."}) {
      return path;
    }
    return coverExportDir / normalized;
  }
  // 文本通道走 pathToUtf8（Windows 窄 string 按代码页转换，非 ASCII 路径会
  // 抛异常/乱码，禁止直接 generic_string()）。generic_u8string 恒为 UTF-8 且
  // 分隔符统一为 '/'，逐字节比较安全（UTF-8 多字节续字节不含 0x5C/0x2F）。
  const auto text = pathToUtf8(normalized);
  const auto dirText = pathToUtf8(coverExportDir.lexically_normal());
  // 仍位于当前 coverExportDir 树下（前缀 + 分隔符边界）→ 原样可用。
  const auto underDir = text.size() > dirText.size() &&
                        text.compare(0, dirText.size(), dirText) == 0 &&
                        text[dirText.size()] == '/';
  if (underDir) {
    return path;
  }
  auto anchor = std::string::npos;
  for (const char* marker : {"/artwork/", "/thumbnails/"}) {
    const auto pos = text.rfind(marker);
    if (pos != std::string::npos && (anchor == std::string::npos || pos > anchor)) {
      anchor = pos;
    }
  }
  if (anchor == std::string::npos) {
    return path;
  }
  const auto tail = pathFromUtf8(text.substr(anchor + 1));
  if (tail.empty() || tail.is_absolute() ||
      (tail.begin() != tail.end() && *tail.begin() == std::filesystem::path{".."})) {
    return path;
  }
  return coverExportDir / tail;
}

[[nodiscard]] cache::CachedLocation cachedLocationFromSong(const cache::CachedSong& song,
                                                           const std::filesystem::path& rootPath,
                                                           const std::filesystem::path& filePath,
                                                           const std::filesystem::path& coverExportDir) {
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
          .artworkPath = coverRelativePath(song.metadata.artworkPath, coverExportDir),
          .thumbnailPath = coverRelativePath(song.metadata.thumbnailPath, coverExportDir),
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
  song.metadata.fileMtime = fileTimeFromNanoseconds(location.fileMtimeNs);
  song.metadata.contentHash = location.contentId;
  song.metadata.effectiveLyricsSource = location.lyricsSource;
  song.metadata.offset = location.cueTrackOffset;
  song.metadata.artworkPath = location.artworkPath;
  song.metadata.thumbnailPath = location.thumbnailPath;
  song.metadata.externalLyricsPath = location.externalLrcPath;
  song.metadata.externalLyricsMtime = location.externalLrcMtimeNs.has_value()
                                          ? std::optional<std::filesystem::file_time_type>{
                                                fileTimeFromNanoseconds(*location.externalLrcMtimeNs)}
                                          : std::nullopt;
  song.metadata.externalLyricsHash = location.externalLrcHash;
  song.metadata.trackId = pathToUtf8(filePath);
  song.metadata.logicalTrackId = pathToUtf8(filePath);
  selectEffectiveLyrics(song);
}

void hydrateCachedCueTrack(cache::CachedSong& song,
                           const cache::CachedLocation& location,
                           const std::filesystem::path& cuePath,
                           std::uint32_t trackIndex) {
  song.metadata.filePath = cuePath;
  song.metadata.sourceFilePath = location.sourceFilePath;
  song.metadata.fileSizeBytes = location.fileSizeBytes;
  song.metadata.fileMtime = fileTimeFromNanoseconds(location.fileMtimeNs);
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
                                                fileTimeFromNanoseconds(*location.externalLrcMtimeNs)}
                                          : std::nullopt;
  song.metadata.externalLyricsHash = location.externalLrcHash;
  const auto trackIdentity = pathToUtf8(cuePath) + "#track" + std::to_string(trackIndex);
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

[[nodiscard]] WatchEffectKind watchEffectFrom(efsw::Action action) {
  switch (action) {
  case efsw::Actions::Add:
    return WatchEffectKind::Created;
  case efsw::Actions::Delete:
    return WatchEffectKind::Destroyed;
  case efsw::Actions::Modified:
    return WatchEffectKind::Modified;
  case efsw::Actions::Moved:
    return WatchEffectKind::Renamed;
  }
  return WatchEffectKind::Other;
}

// efsw 事件不带文件/目录类型字段，按事件到达时的磁盘状态判定。路径已不存在（Delete，
// 以及 Moved 的旧路径）时判为 File：删除路径的扩展名门禁与"无法分类即回落"由分类器
// 按既有规则处理；目录 delete 若已无 stat 依据则走有界回落，与移出根语义一致。
[[nodiscard]] WatchPathKind watchPathKindFrom(const std::filesystem::path& absolutePath) {
  std::error_code error;
  const auto status = std::filesystem::status(absolutePath, error);
  if (!error && std::filesystem::is_directory(status)) {
    return WatchPathKind::Directory;
  }
  return WatchPathKind::File;
}

// efsw 的 dir 为事件所在目录（含尾斜杠），filename 为该目录下的条目名；两者拼接并
// 经 path_utf8 通道构造绝对路径（禁止裸 string()/generic_string()）。
[[nodiscard]] std::filesystem::path absolutePathFrom(const std::string& dir, const std::string& filename) {
  return (pathFromUtf8(dir) / pathFromUtf8(filename)).lexically_normal();
}

[[nodiscard]] WatchEvent watchEventFrom(const std::string& dir, const std::string& filename,
                                        efsw::Action action, const std::string& oldFilename) {
  const auto newPath = absolutePathFrom(dir, filename);
  if (action != efsw::Actions::Moved) {
    return WatchEvent{.path = newPath,
                      .pathKind = watchPathKindFrom(newPath),
                      .effectKind = watchEffectFrom(action),
                      .associated = {}};
  }
  // Moved 的 oldFilename 有两种形状：同目录 rename 为相对旧名；跨目录移动（根 watch 带
  // ReportCrossDirectoryMoves 选项）为绝对源路径。两种都落到内部 Renamed 对约定：
  // primary = 旧路径、associated = 新路径，分类器据此走 renameSubtree 精准更新。
  const auto reportedOld = pathFromUtf8(oldFilename);
  const auto oldPath =
      reportedOld.is_absolute() ? reportedOld.lexically_normal() : (pathFromUtf8(dir) / reportedOld).lexically_normal();
  WatchEvent renamed{.path = oldPath,
                     .pathKind = watchPathKindFrom(oldPath),
                     .effectKind = WatchEffectKind::Renamed,
                     .associated = {}};
  renamed.associated.push_back(WatchEvent{
      .path = newPath, .pathKind = watchPathKindFrom(newPath), .effectKind = WatchEffectKind::Renamed, .associated = {}});
  return renamed;
}

[[nodiscard]] bool watcherMessageRequestsRootReconciliation(const std::filesystem::path& messagePath) {
  const auto message = pathToUtf8(messagePath);
  // 监视消息由适配层合成，走 Watcher 通道（不携带文件路径语义）：
  //   - "e/sys/missed@<dir>"：efsw handleMissedFileActions（内核事件队列溢出）→ 必须根调和。
  // "s/" 前缀是 watch 生命周期消息的历史约定，必须显式排除：s/self/live@ 不得触发对账。
  // "e@" 是无路径后缀的独立完成标记，必须精确 == "e@" 而不能 contains("e@")——后者会误命中
  // s/self/live@（"live@" 含子串 "e@"）与 e/self/live@（同样含 "e@"）。
  if (message.starts_with("s/")) {
    return false;
  }
  return message.starts_with("w/") || message.starts_with("e/") || message == "e@" ||
         message.contains("overflow") || message.contains("warning") || message.contains("error");
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
    // 适配层不产出 Other；fake 注入与未来后端兜底场景仍按"无法分类的目录自移动"放行，
    // 由分类器细分，无法分类时回落全根重扫。
    return true;
  }
  return false;
}

void collectActionableWatcherEvent(const WatchEvent& event, std::vector<std::string>& messages, bool& actionable) {
  if (event.pathKind == WatchPathKind::Watcher) {
    if (watcherMessageRequestsRootReconciliation(event.path)) {
      messages.push_back(pathToUtf8(event.path));
      actionable = true;
    }
  } else if (pathChangeRequestsScan(event)) {
    actionable = true;
  }

  for (const auto& associated : event.associated) {
    collectActionableWatcherEvent(associated, messages, actionable);
  }
}

[[nodiscard]] bool watcherEventRequestsQueue(const WatchEvent& event) {
  if (pathChangeRequestsScan(event)) {
    return true;
  }
  for (const auto& associated : event.associated) {
    if (watcherEventRequestsQueue(associated)) {
      return true;
    }
  }
  return false;
}

// 内核事件队列溢出消息（Watcher 通道）：前缀须通过 watcherMessageRequestsRootReconciliation
// 的匹配规则（"e/" 开头），内容含 dir 便于诊断。
constexpr std::string_view kMissedEventsMessagePrefix = "e/sys/missed@";

class EfswFolderWatcher final : public FolderWatcher, private efsw::FileWatchListener {
public:
  EfswFolderWatcher(const std::filesystem::path& root, WatchEventCallback callback)
      : root_(root), callback_(std::move(callback)), watcher_(std::make_unique<efsw::FileWatcher>()) {
    std::vector<efsw::WatcherOption> options;
    // Gate 0 D4：启用跨目录移动单条 Moved 上报（oldFilename 为绝对源路径）；配对失败时
    // 上游回落到 Delete(源) + Add/Modified(目标)，适配层两种形状都映射。
    options.emplace_back(efsw::Options::ReportCrossDirectoryMoves, 1);

    std::error_code typeError;
    const auto rootStatus = std::filesystem::status(root_, typeError);
    const bool directoryRoot = !typeError && std::filesystem::is_directory(rootStatus);
    rootPathKind_ = directoryRoot ? WatchPathKind::Directory : WatchPathKind::File;
    // efsw addWatch 只接受目录（单文件根实测返回 -1 + FileNotFound）。非目录根改为监视其
    // 父目录（非递归）并在回调层按根文件名过滤，保持单文件根（CLI 单文件入口）可被监视。
    const auto watchTarget = directoryRoot ? root_ : root_.parent_path();
    fileRoot_ = !directoryRoot;
    if (fileRoot_) {
      rootFilenameUtf8_ = pathToUtf8(root_.filename());
    }

    watchId_ = watcher_->addWatch(pathToUtf8(watchTarget), this, directoryRoot, options);
    if (watchId_ < 0) {
      // efsw 1.7.2 的 Log::getLastErrorCode() 恒返回 NoError（只写文案、不更新错误码），
      // 失败判定必须以 addWatch 返回值为准（负值 = 错误枚举）。
      throw std::runtime_error("efsw addWatch failed for '" + pathToUtf8(watchTarget) +
                               "' (watchid=" + std::to_string(watchId_) + ")");
    }
    watcher_->watch();
    // efsw inotify 后端不投递 IN_MOVE_SELF/IN_DELETE_SELF：被监视根自身被移走/删除时没有
    // 任何回调，残留 watch 之后只在旧路径上产生幽灵事件。轮询根存在性并在消失时以自移动
    // 事件触发根调和（既有事件队列/去抖通道），保持"根移出 → 有界全根重扫"的既有语义。
    livenessThread_ = std::thread([this] { rootLivenessLoop(); });
  }

  ~EfswFolderWatcher() override { close(); }

  void close() noexcept override {
    livenessStopping_.store(true);
    if (livenessThread_.joinable()) {
      livenessThread_.join();
    }
    if (!watcher_) {
      return;  // 二次 close（含析构再次调用）：读线程已 join、watch 已摘除
    }
    if (watchId_ >= 0) {
      watcher_->removeWatch(watchId_);
      watchId_ = -1;
    }
    // watcher_.reset() 触发 ~FileWatcher → 后端析构（先等待派发中的 handleAction 完成，再
    // delete 读线程对象 → ~Thread join 读线程）。removeWatch 只能摘除 watch，无法撤销"已解析
    // 但尚未派发"的事件；只有 join 读线程后，才保证不会再有回调触碰本对象成员。这样 close()
    // 即"完整停止"（与被替换的旧监视器 close 语义一致），消除 callback_ 先于读线程 join 被析构的
    // use-after-free 竞态（P1，task-6-review.md §一）。
    watcher_.reset();
  }

private:
  static constexpr auto kRootLivenessProbeInterval = std::chrono::milliseconds{50};

  void handleFileAction(efsw::WatchID, const std::string& dir, const std::string& filename,
                        efsw::Action action, const std::string& oldFilename) override {
    if (fileRoot_ && filename != rootFilenameUtf8_) {
      return;  // 单文件根：父目录 watch 的兄弟条目事件一律过滤
    }
    callback_(watchEventFrom(dir, filename, action, oldFilename));
  }

  void handleMissedFileActions(efsw::WatchID, const std::string& dir) override {
    callback_(WatchEvent{.path = pathFromUtf8(std::string{kMissedEventsMessagePrefix} + dir),
                         .pathKind = WatchPathKind::Watcher,
                         .effectKind = WatchEffectKind::Other,
                         .associated = {}});
  }

  void rootLivenessLoop() {
    while (!livenessStopping_.load()) {
      std::this_thread::sleep_for(kRootLivenessProbeInterval);
      if (livenessStopping_.load()) {
        return;
      }
      std::error_code error;
      if (std::filesystem::exists(root_, error) || error) {
        continue;  // 仍存在或状态不可判定：继续监视
      }
      // 根已消失：以"目录/文件自移动、无法分类"的形状上报（分类器对根自身一律回落全根
      // 重扫，与移出根的既有兜底一致，且不占用消息通道）。只发一次。
      callback_(WatchEvent{.path = root_,
                           .pathKind = rootPathKind_,
                           .effectKind = WatchEffectKind::Other,
                           .associated = {}});
      return;
    }
  }

  std::filesystem::path root_;
  WatchEventCallback callback_;
  std::string rootFilenameUtf8_;
  WatchPathKind rootPathKind_{WatchPathKind::File};
  efsw::WatchID watchId_{-1};
  bool fileRoot_{false};
  std::atomic_bool livenessStopping_{false};
  std::thread livenessThread_;
  // 末位声明（最先析构）：构造期若在 watch() 启动读线程之后再抛错（构造体内仅 liveness 线程
  // 创建可能抛），对象未完整构造、close() 不会被调用，此时成员逆序析构由本成员先触发
  // ~FileWatcher join 读线程，早于 callback_ 等成员被销毁；正常路径由 close() 显式 reset
  // 完成同一保证（见 close 注释）。
  std::unique_ptr<efsw::FileWatcher> watcher_;
};

class EfswFolderWatcherFactory final : public FolderWatcherFactory {
public:
  [[nodiscard]] std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                     WatchEventCallback callback) override {
    return std::make_unique<EfswFolderWatcher>(root, std::move(callback));
  }
};

struct WatchRuntimeState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ScannerRoot> watchedRoots;
  std::vector<std::string> pendingWatcherMessages;
  std::vector<WatchEvent> pendingWatcherEvents;
  bool pendingFallbackRescan{false};
  bool stopping{true};
  std::uint64_t dirtyGeneration{0};
};

constexpr std::size_t kWatcherPendingEventLimit = 1024;

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
  bool queued = false;
  if (watcherEventRequestsQueue(event)) {
    if (state->pendingWatcherEvents.size() < kWatcherPendingEventLimit) {
      state->pendingWatcherEvents.push_back(event);
      queued = true;
    } else {
      state->pendingFallbackRescan = true;
    }
  }
  ++state->dirtyGeneration;
  state->changed.notify_one();
  if (g_watcherEventQueueObserver) {
    WatcherEventQueueSnapshot snapshot;
    if (queued) {
      snapshot.event = event;
    }
    snapshot.dirtyGeneration = state->dirtyGeneration;
    snapshot.eventQueueSize = state->pendingWatcherEvents.size();
    snapshot.messageQueueSize = state->pendingWatcherMessages.size();
    snapshot.fallbackRescan = state->pendingFallbackRescan;
    g_watcherEventQueueObserver(snapshot);
  }
}

class OrchestratedFileScannerService final : public FileScannerService {
public:
  explicit OrchestratedFileScannerService(FileScannerServiceDependencies dependencies)
      : metadataReader_(std::move(dependencies.metadataReader)), databasePath_(std::move(dependencies.databasePath)),
        coverExportDir_(std::move(dependencies.coverExportDir)), watcherFactory_(std::move(dependencies.watcherFactory)),
        folderThumbnailSeam_(std::move(dependencies.folderThumbnailSeam)),
        watcherDebounce_(dependencies.watcherDebounce), reconcileInterval_(dependencies.reconcileInterval) {
    if (!metadataReader_) {
      metadataReader_ = std::make_shared<ProductionTagMetadataReader>();
    }
    if (!watcherFactory_) {
      watcherFactory_ = std::make_shared<EfswFolderWatcherFactory>();
    }
    if (databasePath_.empty()) {
      databasePath_ = defaultDatabasePath();
    }
    if (coverExportDir_.empty()) {
      coverExportDir_ = defaultCoverExportDir();
    }
    if (!folderThumbnailSeam_) {
      folderThumbnailSeam_ = [this](const std::filesystem::path& folderPath) {
        return exportFolderCoverThumbnail(folderPath, coverExportDir_);
      };
    }
    spdlog::info("scanner configured: database={} artwork={}", pathToUtf8(databasePath_),
                 pathToUtf8(coverExportDir_));
    
    const auto logDir = databasePath_.parent_path() / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    if (!ec) {
      const auto tagReaderLogPath = logDir / "tagreader-errors.log";
      tagReaderErrorLogger_ = logging::createDedicatedLogger(
          "tagreader_errors", tagReaderLogPath, spdlog::level::warn);
      if (tagReaderErrorLogger_) {
        spdlog::info("TagReader error logging enabled: {}", pathToUtf8(tagReaderLogPath));
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

  void scan(const std::vector<ScannerRoot>& roots, ScanMode mode) override { enqueueScan(roots, mode, /*reconcile=*/false); }

  void runScan(const std::vector<ScannerRoot>& roots, ScanMode mode, bool reconcile = false) {
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
    spdlog::info("scan started: {} roots{}", roots.size(), reconcile ? " (reconcile)" : "");
    publishEvent(sink, ScannerEventType::ScanStarted, scanVersion, ScanProgress{});
    if (cancellationRequested_.exchange(false)) {
      publishCancelled(sink, scanVersion);
      return;
    }

    cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = databasePath_}};
    std::vector<ScannerError> allErrors;
    std::uint64_t discovered = 0;
    std::uint64_t skipped = 0;
    std::uint64_t scanned = 0;
    std::uint64_t completedFiles = 0;
    std::uint64_t totalTagReaderTimeMs = 0;

    const auto phaseEnumStart = std::chrono::steady_clock::now();
    for (const auto& root : roots) {
      if (cancellationRequested_.load()) {
        publishCancelled(sink, scanVersion);
        return;
      }
      spdlog::debug("scanning root: {}", pathToUtf8(root.path));
      // R4a：Reconcile 判定必须先于 config 折叠（enableIncrementalScan/forceFull 只作用于
      // 用户/常规扫描请求，不得把 Reconcile 折叠为 Full）。
      ScanModeDecision decision;
      if (reconcile) {
        const auto directoryTreeHash = computeDirectoryTreeHash(rootPathFor(root));
        decision = {.mode = ScanMode::Incremental, .directoryTreeHash = directoryTreeHash.hash, .reconcile = true};
      } else {
        const auto requestedMode = (!effectiveConfig.scanner.enableIncrementalScan || effectiveConfig.scanner.forceFull) ? ScanMode::Full : mode;
        decision = decideScanMode(root, requestedMode, databasePath_);
      }
      spdlog::debug("scan mode decision for {}: {}{}", pathToUtf8(root.path),
                    decision.mode == ScanMode::Full ? "full" : "incremental", decision.reconcile ? " (reconcile)" : "");
      const auto rootScanStartTime = std::chrono::steady_clock::now();
      auto rootResult = reconcileRoot(root, decision, effectiveConfig, cache, discovered, skipped, scanned, completedFiles, totalTagReaderTimeMs, sink);
      const auto rootScanDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rootScanStartTime);
      if (rootResult.cancelled) {
        publishCancelled(sink, scanVersion);
        return;
      }
      if (rootResult.aborted) {
        // Reconcile 中止（hash 缺失/缓存不可读）：保留既有索引与缓存，脏标记保持，等待重试。
        continue;
      }
      if (rootResult.rootUnavailable) {
        // unavailable 状态（B1）：根缺失/不可用时保留其既有条目与缓存，仅上报错误；
        // 绝不把该 root 从 allSongs_ 中抹掉。
        spdlog::warn("root unavailable, keeping previous index entries: {}", pathToUtf8(root.path));
        allErrors.insert(allErrors.end(), rootResult.errors.begin(), rootResult.errors.end());
        for (const auto& error : rootResult.errors) {
          publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
        }
        continue;
      }
      recordScanRootDecision(rootPathFor(root), decision, rootResult.songs, rootScanDuration);
      allErrors.insert(allErrors.end(), rootResult.errors.begin(), rootResult.errors.end());
      for (const auto& error : rootResult.errors) {
        publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
      }
	      for (const auto& publishedSong : rootResult.songs) {
	        if (shouldPublishFileScanned(publishedSong.origin)) {
	          publishEvent(sink, ScannerEventType::FileScanned, ++eventVersion_, publishedSong.song.metadata);
	        }
	      }
      mergeRootResult(rootPathFor(root), rootResult.songs);
      clearPendingReconcile(root.path);
    }

    const auto phaseEnumEnd = std::chrono::steady_clock::now();
    const auto phaseEnumTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(phaseEnumEnd - phaseEnumStart).count();

    const auto phaseAggregationStart = std::chrono::steady_clock::now();
    const auto published = rebuildTreeFromAllSongsAndPublish();
    const auto phaseAggregationEnd = std::chrono::steady_clock::now();
    const auto phaseAggregationTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(phaseAggregationEnd - phaseAggregationStart).count();

    const auto scanEndTime = std::chrono::steady_clock::now();
    const auto totalScanTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(scanEndTime - scanStartTime).count();
    
    ScanProgress progress{};
    progress.filesDiscovered = discovered;
    progress.filesScanned = completedFiles;
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
            const auto pathStr = error.path ? pathToUtf8(*error.path) : "(no path)";
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
        const auto pathStr = error.path ? pathToUtf8(*error.path) : "(no path)";
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
    publishSnapshotEvents(published, /*publishCompletion=*/true);
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
        state->pendingWatcherEvents.clear();
        state->pendingFallbackRescan = false;
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

  bool removeLocation(const std::filesystem::path& absolutePath) override {
    const auto target = absolutePath.lexically_normal();
    std::error_code ec;
    const bool exists = std::filesystem::exists(target, ec);
    if (ec) {
      spdlog::error("removeLocation failed probing '{}': {}", pathToUtf8(target), ec.message());
      return false;
    }
    if (!exists) {
      // 文件不存在：幂等成功（无需磁盘删除与缓存清理）。
      spdlog::info("removeLocation: target already absent, idempotent success: {}", pathToUtf8(target));
      return true;
    }

    std::shared_ptr<WatchRuntimeState> state;
    {
      std::scoped_lock lock{watcherMutex_};
      state = watcherState_;
    }
    std::vector<ScannerRoot> watchedRoots;
    if (state) {
      std::scoped_lock stateLock{state->mutex};
      watchedRoots = state->watchedRoots;
    }
    for (const auto& watched : watchedRoots) {
      if (pathKey(rootPathFor(watched)) == pathToUtf8(target)) {
        spdlog::warn("removeLocation refused: target is a scan root (use removeRoot for explicit root removal): {}",
                     pathToUtf8(target));
        return false;
      }
    }

    const bool isDirectory = std::filesystem::is_directory(target, ec);
    if (ec) {
      spdlog::error("removeLocation failed classifying '{}': {}", pathToUtf8(target), ec.message());
      return false;
    }
    if (isDirectory) {
      std::filesystem::remove_all(target, ec);
    } else {
      std::filesystem::remove(target, ec);
    }
    if (ec) {
      spdlog::error("removeLocation failed removing '{}': {}", pathToUtf8(target), ec.message());
      return false;
    }
    spdlog::info("removeLocation removed {} '{}'", isDirectory ? "folder" : "file", pathToUtf8(target));

    // 缓存与树更新串行于 scanMutex_（与 runScan/applyClassifierBatch 同一临界区）；
    // 未扫描过（无树）时磁盘已删，缓存由 watcher/周期对账兜底。
    std::lock_guard scanLock{scanMutex_};
    if (!treeBuilder_) {
      spdlog::debug("removeLocation: no tree seeded yet; cache update skipped");
      return true;
    }

    std::optional<std::filesystem::path> root;
    if (state) {
      std::scoped_lock stateLock{state->mutex};
      const auto normalized = pathToUtf8(target);
      std::size_t bestLength = 0;
      for (const auto& watched : watchedRoots) {
        const auto rootPath = rootPathFor(watched);
        const auto rootText = pathToUtf8(rootPath);
        if (normalized == rootText || normalized.rfind(rootText + "/", 0) == 0) {
          if (rootText.size() >= bestLength) {
            bestLength = rootText.size();
            root = rootPath;
          }
        }
      }
    }
    if (!root) {
      spdlog::warn("removeLocation: '{}' is outside all watched roots; disk removed, cache untouched",
                   pathToUtf8(target));
      return true;
    }

    const auto rel = relativePathFor(*root, target);
    treeBuilder_->removeSubtree(rel);
    const auto relText = pathToUtf8(rel);
    std::erase_if(allSongs_, [&](const RootResult::PublishedSong& entry) {
      const auto relative = pathToUtf8(entry.treeRelativePath);
      return relative == relText || relative.rfind(relText + "/", 0) == 0;
    });
    try {
      cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
      cache.deleteLocationsByPathPrefix(pathKey(*root), pathToUtf8(target));
    } catch (const std::exception& error) {
      spdlog::warn("removeLocation failed updating locations cache: {}", error.what());
    }
    refreshScanRootHash(*root);
    publishClassifierSnapshot();
    return true;
  }

  bool removeRoot(const std::filesystem::path& absolutePath) override {
    // 与缓存/监视根/脏标记同源的规范化键：调用方传入符号链接等非规范路径时同样可命中。
    const auto target = pathKey(rootPathFor(ScannerRoot{.path = absolutePath}));
    std::shared_ptr<WatchRuntimeState> state;
    {
      std::scoped_lock lock{watcherMutex_};
      state = watcherState_;
    }
    std::vector<ScannerRoot> watchedRoots;
    std::vector<ScannerRoot> remainingRoots;
    if (state) {
      std::scoped_lock stateLock{state->mutex};
      watchedRoots = state->watchedRoots;
      for (const auto& root : watchedRoots) {
        if (pathKey(rootPathFor(root)) != target) {
          remainingRoots.push_back(root);
        }
      }
    }
    bool knownRoot = std::any_of(watchedRoots.begin(), watchedRoots.end(), [&target](const ScannerRoot& root) {
      return pathKey(rootPathFor(root)) == target;
    });
    if (!knownRoot) {
      try {
        cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
        knownRoot = cache.loadScanRoot(pathFromUtf8(target)).has_value();
      } catch (const std::exception& error) {
        spdlog::warn("removeRoot failed probing cache for '{}': {}", target, error.what());
      }
    }
    if (!knownRoot) {
      spdlog::warn("removeRoot refused: '{}' is not a known scan root", target);
      return false;
    }

    // 擦除 allSongs_、清缓存、整树重建与发布必须在同一 scanMutex_ 作用域内：rebuild 会无锁
    // 遍历 allSongs_，与 runScan/applyClassifierBatch 并发读写是 UB。startWatching/stopWatching
    // 移到锁外（持锁 join debounce 线程会死锁）。
    {
      std::lock_guard scanLock{scanMutex_};
      {
        std::scoped_lock lock{mutex_};
        std::erase_if(allSongs_, [&](const RootResult::PublishedSong& entry) {
          return pathKey(entry.sourceRoot) == target;
        });
        pendingReconcileRoots_.erase(target);
      }
      try {
        cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
        // locations/scan_errors 经外键 ON DELETE CASCADE 随 scan_roots 行清理。
        cache.deleteScanRoot(pathFromUtf8(target));
      } catch (const std::exception& error) {
        spdlog::warn("removeRoot failed clearing cache for '{}': {}", target, error.what());
      }
      spdlog::info("removeRoot removed scan root '{}' from index and cache", target);
      if (treeBuilder_) {
        // 树必须从剩余 allSongs_ 整树重建：removeRoot 只删条目，成员 builder 仍含被移除根的节点，
        // 直接 publishClassifierSnapshot 会把已删根的歌曲继续发布出去。
        const auto published = rebuildTreeFromAllSongsAndPublish();
        publishSnapshotEvents(published, /*publishCompletion=*/true);
      }
    }
    if (state) {
      if (remainingRoots.empty()) {
        stopWatching();
      } else {
        startWatching(remainingRoots);
      }
    }
    return true;
  }

private:
  void enqueueScan(const std::vector<ScannerRoot>& roots, ScanMode mode, bool reconcile) {
    if (roots.empty()) {
      return;
    }
    bool submitted = false;
    {
      std::lock_guard lock{scanQueueMutex_};
      if (!scanWorkerStopping_ && scanQueue_.size() < 16U) {
        scanQueue_.push_back(ScanRequest{.roots = roots, .mode = mode, .reconcile = reconcile});
        submitted = true;
      }
    }
    scanQueueChanged_.notify_one();
    if (submitted) {
      return;
    }
    // R10：队列满不再静默丢请求——折叠为 per-root 脏标记，由周期探测无条件 Reconcile 收敛。
    spdlog::error("scanner scan queue is full (capacity 16); folding request into pending reconcile");
    for (const auto& root : roots) {
      markPendingReconcile(root.path);
    }
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

  void enqueueReconcile(const std::vector<ScannerRoot>& roots) { enqueueScan(roots, ScanMode::Incremental, /*reconcile=*/true); }

  // 脏标记键统一走 rootPathFor 规范化（与缓存/周期探测的 root 键同源），避免用户传入的
  // 非规范路径（符号链接）在 mark 与 isPending 之间产生键漂移。
  void markPendingReconcile(const std::filesystem::path& rootPath) {
    const auto key = pathKey(rootPathFor(ScannerRoot{.path = rootPath}));
    std::scoped_lock lock{mutex_};
    pendingReconcileRoots_.insert(key);
  }

  void clearPendingReconcile(const std::filesystem::path& rootPath) {
    const auto key = pathKey(rootPathFor(ScannerRoot{.path = rootPath}));
    std::scoped_lock lock{mutex_};
    pendingReconcileRoots_.erase(key);
  }

  [[nodiscard]] bool isPendingReconcile(const std::filesystem::path& rootPath) const {
    std::scoped_lock lock{mutex_};
    return pendingReconcileRoots_.contains(pathKey(rootPath));
  }

  struct RootResult {
	    struct PublishedSong {
	      cache::CachedSong song;
	      std::filesystem::path treeRelativePath;
	      // 该条目归属的扫描根（runScan 按 root 合并 allSongs_ 的键；根丢失时保留旧条目）。
	      std::filesystem::path sourceRoot;
	      ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
	      std::optional<std::string> locationId;
	      ExternalLyricsCacheAction externalLyricsCacheAction{ExternalLyricsCacheAction::None};
	    };

    std::vector<PublishedSong> songs;
    std::vector<ScannerError> errors;
    bool cancelled{false};
    // 根路径不可用（缺失/权限/stat 错误）：保留既有索引与缓存（unavailable 状态，设计 §6.3/B1）。
    bool rootUnavailable{false};
    // Reconcile 中止（hash 缺失或缓存不可读）：保留既有索引，绝不进入空计划全量重读（B4）。
    bool aborted{false};
  };

  // runScan 按 root 合并（B1）：只替换该 root 的既有条目，未成功扫描/缺失 root 的条目原样保留。
  void mergeRootResult(const std::filesystem::path& rootPath, std::vector<RootResult::PublishedSong>& songs) {
    const auto rootKey = pathKey(rootPath);
    std::scoped_lock lock{mutex_};
    std::erase_if(allSongs_, [&](const RootResult::PublishedSong& entry) {
      return !entry.sourceRoot.empty() && pathKey(entry.sourceRoot) == rootKey;
    });
    allSongs_.insert(allSongs_.end(), std::make_move_iterator(songs.begin()), std::make_move_iterator(songs.end()));
  }

  // 扫描收尾：为快照中全部非根 Directory 节点解析 node-level 缩略图（填 thumbnailPath）。
  // 相对路径由 displayName 父链重建（builder 保证 Directory displayName == relativePath 末段）；
  // 物理目录 = 歌曲物理路径反推的根目录 + 相对路径；根节点跳过（恒空）。
  // seam 的导出 I/O 在此发生，调用点在快照锁之外；失败由 resolver 隔离。
  // touchedPaths 非空时只解析"变更子树 + 其祖先链"（R13.1）：空 = 全库（全量扫描/整根移除）。
  void resolveFolderThumbnails(PlaylistTreeSnapshot& snapshot,
                               const std::vector<RootResult::PublishedSong>& songs,
                               const std::vector<std::filesystem::path>& touchedPaths = {}) {
    if (!snapshot.rootNodeId.has_value() || snapshot.nodes.empty()) {
      return;
    }
    std::vector<std::string> touchedKeys;
    touchedKeys.reserve(touchedPaths.size());
    for (const auto& touched : touchedPaths) {
      touchedKeys.push_back(pathToUtf8(touched.lexically_normal()));
    }
    const auto directoryAffected = [&](const std::string& directoryKey) {
      if (touchedKeys.empty()) {
        return true;
      }
      const auto underOrEqual = [](std::string_view prefix, std::string_view text) {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0 &&
               (text.size() == prefix.size() || text[prefix.size()] == '/');
      };
      return std::ranges::any_of(touchedKeys, [&](const std::string& touchedKey) {
        return underOrEqual(directoryKey, touchedKey) || underOrEqual(touchedKey, directoryKey);
      });
    };
    std::unordered_map<std::string, std::filesystem::path> physicalRootByRelPrefix;
    for (const auto& publishedSong : songs) {
      const auto& relative = publishedSong.treeRelativePath;
      const auto& physical = publishedSong.song.metadata.filePath;
      if (relative.empty() || physical.empty() || !physical.is_absolute()) {
        continue;
      }
      std::vector<std::string> components;
      for (const auto& component : relative) {
        components.push_back(pathToUtf8(component));
      }
      if (components.empty()) {
        continue;
      }
      auto root = physical;
      for (std::size_t index = 0; index < components.size(); ++index) {
        root = root.parent_path();
      }
      std::filesystem::path prefix;
      for (const auto& component : components) {
        prefix /= pathFromUtf8(component);
        physicalRootByRelPrefix.emplace(pathToUtf8(prefix.lexically_normal()), root);
      }
    }

    std::unordered_map<std::string, PlaylistNode*> nodeById;
    nodeById.reserve(snapshot.nodes.size());
    for (auto& node : snapshot.nodes) {
      nodeById.emplace(node.nodeId, &node);
    }

    std::unordered_set<std::string> seenDirectoryKeys;
    for (auto& node : snapshot.nodes) {
      if (node.kind != PlaylistNodeKind::Directory) {
        continue;
      }
      std::filesystem::path relativePath;
      {
        std::vector<std::string> names;
        const auto* cursor = &node;
        while (cursor->nodeId != *snapshot.rootNodeId) {
          names.push_back(cursor->displayName);
          if (!cursor->parentNodeId.has_value()) {
            break;
          }
          const auto parentIterator = nodeById.find(*cursor->parentNodeId);
          if (parentIterator == nodeById.end()) {
            break;
          }
          cursor = parentIterator->second;
        }
        for (auto iterator = names.rbegin(); iterator != names.rend(); ++iterator) {
          relativePath /= pathFromUtf8(*iterator);
        }
      }
      const auto relKey = pathToUtf8(relativePath.lexically_normal());
      seenDirectoryKeys.insert(relKey);
      if (!directoryAffected(relKey)) {
        // R13.1：未受本批影响的目录回填上次解析结果（builder 每次重建，节点本身不持有缩略图）。
        std::scoped_lock lock{folderThumbnailCacheMutex_};
        const auto cached = folderThumbnailCache_.find(relKey);
        if (cached != folderThumbnailCache_.end()) {
          node.thumbnailPath = cached->second;
        }
        continue;
      }
      const auto rootIterator = physicalRootByRelPrefix.find(relKey);
      if (rootIterator == physicalRootByRelPrefix.end()) {
        std::scoped_lock lock{folderThumbnailCacheMutex_};
        folderThumbnailCache_.erase(relKey);
        continue;
      }
      const auto physicalDirectory = rootIterator->second / relativePath;

      std::vector<FolderThumbnailCandidate> candidates;
      const std::function<void(const PlaylistNode&)> collectDescendants = [&](const PlaylistNode& parent) {
        for (const auto& childId : parent.childNodeIds) {
          const auto childIterator = nodeById.find(childId);
          if (childIterator == nodeById.end()) {
            continue;
          }
          const auto& child = *childIterator->second;
          if (child.kind == PlaylistNodeKind::Track) {
            if (!child.song.has_value() || child.song->filePath.empty()) {
              continue;
            }
            FolderThumbnailCandidate candidate;
            candidate.filePath = child.song->filePath;
            const auto trackDirectory = child.song->filePath.parent_path();
            const auto relativeDirectory = trackDirectory.lexically_relative(physicalDirectory);
            candidate.relativeDirectory = (relativeDirectory.empty() || relativeDirectory == std::filesystem::path{"."})
                                              ? std::string{}
                                              : pathToUtf8(relativeDirectory);
            candidate.thumbnailPath = child.song->thumbnailPath;
            candidates.push_back(std::move(candidate));
          } else if (child.kind == PlaylistNodeKind::Directory) {
            collectDescendants(child);
          }
        }
      };
      collectDescendants(node);

      const auto resolved = resolveFolderThumbnail(physicalDirectory, candidates, folderThumbnailSeam_);
      std::scoped_lock lock{folderThumbnailCacheMutex_};
      if (resolved.has_value()) {
        node.thumbnailPath = pathToUtf8(*resolved);
        folderThumbnailCache_[relKey] = *node.thumbnailPath;
      } else {
        // 解析为空（封面删除/后代缩略图消失）：同步清缓存，避免陈旧值复活。
        folderThumbnailCache_.erase(relKey);
      }
    }
    {
      std::scoped_lock lock{folderThumbnailCacheMutex_};
      std::erase_if(folderThumbnailCache_, [&](const auto& entry) { return !seenDirectoryKeys.contains(entry.first); });
    }
  }

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

  // 移除前缀的成对表示：relPrefix 用于 treeRelativePath（root 相对），absPrefix 用于
  // sourceFilePath/pathKey（绝对）。cue 条目路径是 root 相对、源音频是绝对，两者坐标系不同
  // （Oracle B3），必须分开匹配。
  struct RemovalPrefixPair {
    std::string relPrefix;
    std::string absPrefix;
  };

  struct CueCrossings {
    // cue 没了、源还在 → 源需重新可见（合成 upsert，created=true）。
    std::vector<std::filesystem::path> orphanedSources;
    // cue 还在、源没了 → 该 cue 需重展开（T1 scope = cue 父目录）。
    std::vector<std::filesystem::path> cueRefreshTargets;
  };

  // 单遍交叉收集器（设计 §3.2 / Oracle B3）：一次遍历 allSongs_，按成对前缀集判定两个漂移方向。
  // 共置 cue+源（用户形状）：fileUnder && sourceUnder → 两集合都不命中 → 纯前缀删除即可。
  [[nodiscard]] CueCrossings collectCueCrossings(const std::vector<RemovalPrefixPair>& removedPrefixes) const {
    CueCrossings crossings;
    if (removedPrefixes.empty()) {
      return crossings;
    }
    const auto underOrEqual = [](std::string_view prefix, std::string_view text) {
      return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0 &&
             (text.size() == prefix.size() || text[prefix.size()] == '/');
    };
    const auto underAnyRel = [&](std::string_view text) {
      return std::ranges::any_of(removedPrefixes, [&](const RemovalPrefixPair& prefix) {
        return underOrEqual(prefix.relPrefix, text);
      });
    };
    const auto underAnyAbs = [&](std::string_view text) {
      return std::ranges::any_of(removedPrefixes, [&](const RemovalPrefixPair& prefix) {
        return underOrEqual(prefix.absPrefix, text);
      });
    };
    std::unordered_set<std::string> seenOrphans;
    std::unordered_set<std::string> seenCueTargets;
    for (const auto& entry : allSongs_) {
      const auto relativeText = pathToUtf8(entry.treeRelativePath);
      const auto filePathText = pathToUtf8(entry.song.metadata.filePath);
      const bool isCueEntry = entry.treeRelativePath.extension() == ".cue" || filePathText.ends_with(".cue");
      const auto& source = entry.song.metadata.sourceFilePath;
      if (source.empty()) {
        continue;
      }
      const auto sourceKey = pathKey(source);
      const bool fileUnder = underAnyRel(relativeText);
      const bool sourceUnder = underAnyAbs(sourceKey);
      if (isCueEntry && fileUnder && !sourceUnder) {
        if (seenOrphans.insert(sourceKey).second) {
          crossings.orphanedSources.push_back(source);
        }
      } else if (sourceUnder && !fileUnder) {
        const auto cuePath = entry.song.metadata.filePath;
        if (seenCueTargets.insert(pathKey(cuePath)).second) {
          crossings.cueRefreshTargets.push_back(cuePath);
        }
      }
    }
    return crossings;
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
                                         std::uint64_t& completedFiles, std::uint64_t& totalTagReaderTimeMs,
                                         const ScannerEventSink& sink,
                                         const std::optional<std::filesystem::path>& walkScope = std::nullopt) {
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

    // Reconcile（B4）：hash 缺失（根缺失/hash 计算失败）时中止本次对账——绝不进入
    // "空计划 = 全部按新增"的全量标签重读；既有索引与缓存保持不动。
    if (decision.reconcile && !decision.directoryTreeHash.has_value()) {
      spdlog::info("root unavailable, reconcile skipped (keeping previous index): {}", pathToUtf8(rootPath));
      result.aborted = true;
      return result;
    }

    if (decision.mode == ScanMode::Incremental) {
      try {
        const auto cachedScanRoot = scanRootCache.loadScanRoot(rootPath);
        if (cachedScanRoot.has_value()) {
          cachedLocations = scanRootCache.loadLocationsByRoot(rootPath);
          for (auto& location : cachedLocations) {
            location.artworkPath = resolveCoverPath(location.artworkPath, coverExportDir_);
            location.thumbnailPath = resolveCoverPath(location.thumbnailPath, coverExportDir_);
          }
          if (decision.directoryTreeHash.has_value() &&
              cachedScanRoot->directoryTreeHash == *decision.directoryTreeHash) {
            treeHashMatches = true;
          }
        }
      } catch (const std::exception& error) {
        if (decision.reconcile) {
          // 缓存不可读同样中止：加载失败时 cachedLocations 为空会让计划把所有文件当新增。
          spdlog::warn("reconcile aborted for {}: cache unreadable: {}", pathToUtf8(rootPath), error.what());
          result.aborted = true;
          return result;
        }
        spdlog::warn("reconcileRoot: failed to load scanner cache for incremental planning: {}", error.what());
      }
    }
    
    // walk root 与 classify root 分离（任务 7）：scoped 对账只枚举 scope 子树，但相对路径、
    // 缓存键与发布身份仍以扫描根 rootPath 为基准，因此并入 allSongs_/locations 的形态与
    // 全根扫描完全一致。
    const auto walkPath = walkScope.has_value() ? walkScope->lexically_normal() : rootPath;
    entries = discoverScannerPaths(ScannerRoot{.path = walkPath, .recursive = root.recursive}, pathConfig);
    phase1End = std::chrono::steady_clock::now();

    // 根路径不可用（缺失/权限/stat 错误）：保留既有索引与缓存（unavailable 状态，设计 §6.3/B1），
    // 仅上报错误。scoped walk（walkScope 有值）不适用——scope 消失不等于根丢失。
    // discoverScannerPaths 恒返回至少一个条目（存在根=DirectoryRoot，缺失根=Missing），
    // 故只需判定首条目 kind。
    if (!walkScope.has_value() &&
        (entries.front().kind == PathEntryKind::Missing ||
         entries.front().kind == PathEntryKind::PermissionDenied || entries.front().kind == PathEntryKind::Error)) {
      result.rootUnavailable = true;
      if (!entries.empty()) {
        for (const auto& error : entries.front().errors) {
          result.errors.push_back(scannerErrorFrom(error));
        }
      }
      return result;
    }

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
          spdlog::warn("CUE sheet metadata read failed for {}: {}", pathToUtf8(entry.path), error.what());
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
            const auto contentHash = std::string{"cue:"} + pathToUtf8(entry.path) + "#" + std::to_string(trackIdx);
            auto mapped = mapRawTagMetadata(trackRaw, contentHash, std::nullopt, false);
            
            mapped.metadata.sourceFilePath = trackRaw.filePath;
            mapped.metadata.filePath = entry.path;
            mapped.metadata.offset = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(trackRaw.offset));
            mapped.metadata.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(trackRaw.duration));
            mapped.metadata.logicalTrackId = pathToUtf8(entry.path) + "#track" + std::to_string(trackIdx);
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
          auto cachedLocation = locationId.empty() ? std::optional<cache::CachedLocation>{} : scanRootCache.loadLocation(locationId);
          if (cachedLocation.has_value()) {
            cachedLocation->artworkPath = resolveCoverPath(cachedLocation->artworkPath, coverExportDir_);
            cachedLocation->thumbnailPath = resolveCoverPath(cachedLocation->thumbnailPath, coverExportDir_);
            const auto cachedSong = scanRootCache.loadContent(cachedLocation->contentId);
            if (cachedSong.has_value()) {
              ++skipped;
              spdlog::debug("Cache hit for nodeIndex={}, filePath={}", currentDiscoveryIndex, pathToUtf8(entry.path));
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
          spdlog::warn("failed to hydrate scanner cache hit for {}: {}", pathToUtf8(entry.path), error.what());
        }
      }
      if (!shouldProcessViaWorker) {
        continue;
      }
	      const auto fileSize = fileSizeBytes(entry.path);
	      const auto fileMtimeValue = fileMtime(entry.path);
	      const auto locationId = fileSize.has_value() ? computeLocationId(entry.path, *fileSize, fileMtimeValue) : std::string{};
	      spdlog::trace("Preparing audio task: nodeIndex={}, filePath={}", currentDiscoveryIndex, pathToUtf8(entry.path));
	      auto audioTask = prepareAudioTask(entry.path, rootPath, result.errors, skipped, scanned);
      if (!audioTask.has_value()) {
        spdlog::warn("prepareAudioTask returned nullopt for nodeIndex={}, filePath={}", currentDiscoveryIndex, pathToUtf8(entry.path));
        continue;
      }
      audioTask->discoveryIndex = currentDiscoveryIndex;
      audioTasks.push_back(*audioTask);
      
	      workerTasks.push_back(WorkerTask{.rootPath = rootPath,
	                                       .filePath = entry.path,
	                                       .locationId = locationId,
	                                       .cachedLocation = std::nullopt,
	                                       .nodeIndex = currentDiscoveryIndex});
      spdlog::trace("Queued worker task: nodeIndex={}, filePath={}", currentDiscoveryIndex, pathToUtf8(entry.path));
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
                                                                               pathToUtf8(task.filePath), task.nodeIndex);
                                                               }
                                                             } else {
                                                               spdlog::error("Worker callback: task.nodeIndex={} >= indexedSongs.size()={}", 
                                                                            task.nodeIndex, indexedSongs.size());
                                                             }
                                                             return metadata;
                                                           }}};
    const auto workerTaskCount = workerTasks.size();
    workerPool.submitBatch(std::move(workerTasks));
    const auto workerStarted = std::chrono::steady_clock::now();
    auto lastProgressPublish = workerStarted;
    const auto progressInterval = config.scanner.progressInterval;
    // 发现阶段已处理完的节点（缓存命中、内联 CUE 曲目、虚拟容器）在 worker 启动前即计入"已完成"。
    const auto inlineCompleted = discovered - skipped - workerTaskCount;
    auto workerResults = workerPool.waitAll([&](std::uint64_t completed) {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastProgressPublish < progressInterval) {
        return;
      }
      lastProgressPublish = now;
      ScanProgress progress{};
      progress.filesDiscovered = discovered;
      progress.filesScanned = inlineCompleted + completed;
      progress.filesSkipped = skipped;
      progress.errors = result.errors.size();
      publishEvent(sink, ScannerEventType::ProgressUpdated, ++eventVersion_, progress);
    });
    completedFiles = discovered - skipped;
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
                        nodeIdx, pathToUtf8(workResult.filePath));
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
        indexedSong.song.metadata.logicalTrackId = pathToUtf8(indexedSong.treeRelativePath);
        result.songs.push_back({.song = std::move(indexedSong.song),
                                .treeRelativePath = std::move(indexedSong.treeRelativePath),
                                .sourceRoot = rootPath,
                                .origin = indexedSong.origin,
                                .locationId = std::move(indexedSong.locationId)});
        ++filledCount;
      } else if (indexedSong.filled.load()) {
        result.songs.push_back({.song = std::move(indexedSong.song),
                                .treeRelativePath = std::move(indexedSong.treeRelativePath),
                                .sourceRoot = rootPath,
                                .origin = indexedSong.origin,
                                .locationId = std::move(indexedSong.locationId)});
        ++filledCount;
      } else {
        ++unfilledCount;
        spdlog::debug("indexedSong[{}] unfilled: nodeType={}, discoveryIndex={}, treeRelativePath={}", 
                     &indexedSong - &indexedSongs[0],
                     static_cast<int>(indexedSong.nodeType),
                     indexedSong.discoveryIndex,
                     pathToUtf8(indexedSong.treeRelativePath));
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
                 pathToUtf8(rootPath), totalMs, phase1Ms, phase2Ms, phase3Ms, phase4Ms, phase5Ms);

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
	        const auto location = cachedLocationFromSong(publishedSong.song, rootPath, publishedSong.song.metadata.filePath, coverExportDir_);
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
	                 task.nodeIndex, pathToUtf8(task.filePath), task.cachedLocation.has_value());
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
    song.metadata.trackId = pathToUtf8(task.filePath);
    song.metadata.logicalTrackId = pathToUtf8(task.filePath);
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

  struct ClassifierRename {
    std::filesystem::path root;
    std::filesystem::path oldAbs;
    std::filesystem::path newAbs;
    std::filesystem::path oldRel;
    std::filesystem::path newRel;
  };
  struct ClassifierRemove {
    std::filesystem::path root;
    std::filesystem::path abs;
    std::filesystem::path rel;
  };
  struct ClassifierUpsert {
    std::filesystem::path root;
    std::filesystem::path raw;
    std::filesystem::path abs;
    std::filesystem::path rel;
    bool created{false};
  };
  struct ClassifierDestroy {
    std::filesystem::path raw;
    WatchPathKind pathKind{WatchPathKind::Other};
  };

  struct ScopedScanTarget {
    ScannerRoot root;
    std::filesystem::path scopeAbs;
    // 封面谓词命中的 scope 必须重读该目录歌曲标签：封面增删不改动音频的 size/mtime，
    // 增量计划会判 unchanged 走缓存直灌，歌曲级 artworkPath/thumbnailPath 将保持陈旧
    // （与设计 §5 选 T1 的理由矛盾）。置位后该 scope 以 Full 语义重读（仅该目录）。
    bool forceTagReread{false};
  };

  [[nodiscard]] static bool pathWithinScope(const std::filesystem::path& path, const std::filesystem::path& scope) {
    const auto pathText = pathToUtf8(path.lexically_normal());
    const auto scopeText = pathToUtf8(scope.lexically_normal());
    return pathText == scopeText || pathText.rfind(scopeText + "/", 0) == 0;
  }

  [[nodiscard]] static std::optional<ScannerRoot> findRootForPath(const std::vector<ScannerRoot>& roots,
                                                                  const std::filesystem::path& path) {
    std::optional<ScannerRoot> best;
    std::size_t bestLength = 0;
    const auto normalized = pathToUtf8(path.lexically_normal());
    for (const auto& root : roots) {
      const auto rootPath = rootPathFor(root);
      const auto rootText = pathToUtf8(rootPath);
      if (normalized == rootText || normalized.rfind(rootText + "/", 0) == 0) {
        if (rootText.size() >= bestLength) {
          bestLength = rootText.size();
          best = ScannerRoot{.path = rootPath, .recursive = root.recursive};
        }
      }
    }
    return best;
  }

  // 事件是否由某个 scope 的子树枚举接管：普通事件按路径是否落在任一 scope 内判定；
  // Renamed 对仅当新旧两端都在 scope 内才交给枚举（跨边界 rename 仍走精准分类器，
  // 否则新落点的文件会丢失更新）。
  [[nodiscard]] static bool eventCoveredByScopes(const WatchEvent& event, const std::vector<ScopedScanTarget>& scopes) {
    const auto withinAny = [&scopes](const std::filesystem::path& path) {
      return std::ranges::any_of(scopes, [&path](const ScopedScanTarget& scope) {
        return pathWithinScope(path, scope.scopeAbs);
      });
    };
    if (event.effectKind == WatchEffectKind::Renamed) {
      if (!withinAny(event.path)) {
        return false;
      }
      for (const auto& associated : event.associated) {
        if (associated.effectKind == WatchEffectKind::Renamed && !withinAny(associated.path)) {
          return false;
        }
      }
      return true;
    }
    return withinAny(event.path);
  }

  // 从事件批提取最外层 Created/Modified + Directory 作为 scoped 目标：嵌套 scope 并入外层、
  // 重复路径去重；路径不在任何监视根内（或等于根自身）的事件不构成 scope。祖先路径恒短于
  // 后代，按文本长度升序即可保证外层先入列。
  [[nodiscard]] static std::vector<ScopedScanTarget> extractScopedScanTargets(const std::vector<ScannerRoot>& roots,
                                                                              const std::vector<WatchEvent>& batch) {
    std::vector<std::filesystem::path> candidates;
    const std::function<void(const WatchEvent&)> collect = [&candidates, &collect](const WatchEvent& event) {
      if ((event.effectKind == WatchEffectKind::Created || event.effectKind == WatchEffectKind::Modified) &&
          event.pathKind == WatchPathKind::Directory) {
        candidates.push_back(event.path.lexically_normal());
      }
      for (const auto& associated : event.associated) {
        collect(associated);
      }
    };
    for (const auto& event : batch) {
      collect(event);
    }
    if (candidates.empty()) {
      return {};
    }
    std::ranges::sort(candidates, {}, [](const std::filesystem::path& path) { return pathToUtf8(path).size(); });
    std::vector<ScopedScanTarget> scopes;
    for (const auto& candidate : candidates) {
      const auto root = findRootForPath(roots, candidate);
      if (!root.has_value() || pathKey(root->path) == pathKey(candidate)) {
        continue;
      }
      // 上方守卫已排除 scope==root（返回 false 的唯一情形），此处返回值恒为 true。
      static_cast<void>(mergeScopedScanTarget(scopes, *root, candidate));
    }
    return scopes;
  }

  // 合并 scoped 目标：同一 root 下被既有 scope 包含的候选跳过（并入时 OR 强制重读标志）；
  // 包含既有 scope 的候选替换之。返回 false 表示候选等于 root 自身（scope==root，调用方须升级为 Reconcile）。
  [[nodiscard]] static bool mergeScopedScanTarget(std::vector<ScopedScanTarget>& scopes,
                                                  const ScannerRoot& root,
                                                  const std::filesystem::path& scopeAbs,
                                                  bool forceTagReread = false) {
    const auto normalized = scopeAbs.lexically_normal();
    const auto rootKey = pathKey(root.path);
    if (pathKey(normalized) == rootKey) {
      return false;
    }
    for (auto& scope : scopes) {
      if (pathKey(scope.root.path) == rootKey && pathWithinScope(normalized, scope.scopeAbs)) {
        scope.forceTagReread = scope.forceTagReread || forceTagReread;
        return true;
      }
    }
    std::erase_if(scopes, [&](const ScopedScanTarget& scope) {
      return pathKey(scope.root.path) == rootKey && pathWithinScope(scope.scopeAbs, normalized) &&
             pathKey(scope.scopeAbs) != pathKey(normalized);
    });
    scopes.push_back(ScopedScanTarget{.root = root, .scopeAbs = normalized, .forceTagReread = forceTagReread});
    return true;
  }

  // T1 成本门（R5/B5）：scope 前缀下的缓存 location 行数超过 max(500, root 文件数/10) 时，
  // 改用 Reconcile（只重读变化文件）而不是 scoped Full 语义重读整个 scope。
  [[nodiscard]] bool scopeOverCostGate(const ScopedScanTarget& target) const {
    try {
      const auto rootPath = rootPathFor(target.root);
      cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
      const auto rootRecord = cache.loadScanRoot(rootPath);
      const auto rootFiles = rootRecord.has_value() ? rootRecord->totalFiles : 0;
      const auto threshold = std::max(kScopedScanLocationFloor, static_cast<std::size_t>(rootFiles / kScopedScanRootFractionDivisor));
      const auto scopeRel = relativePathFor(rootPath, target.scopeAbs);
      const auto scopeAbsKey = pathKey((rootPath / scopeRel).lexically_normal());
      const auto count = cache.countLocationsByPathPrefix(pathKey(rootPath), scopeAbsKey);
      if (static_cast<std::size_t>(count) > threshold) {
        spdlog::info("scoped reconcile cost gate: {} cached rows under {} exceeds threshold {}; switching to root reconcile",
                     count, pathToUtf8(scopeAbsKey), threshold);
        return true;
      }
    } catch (const std::exception& error) {
      spdlog::warn("scoped reconcile cost gate probe failed for {}: {}", pathToUtf8(target.scopeAbs), error.what());
    }
    return false;
  }

  // 单文件根判定：根路径自身就是一条已索引歌曲（root 的 filePath == 根路径）。
  [[nodiscard]] bool rootHasIndexedSelfEntry(const std::filesystem::path& rootPath) const {
    const auto rootKey = pathKey(rootPath);
    return std::ranges::any_of(allSongs_, [&](const RootResult::PublishedSong& entry) {
      return pathKey(entry.song.metadata.filePath) == rootKey;
    });
  }

  // 仅枚举 scope 子树（walk root），classify root 仍是扫描根，因此 treeRelativePath/缓存键
  // 可直接并入 allSongs_/locations。scope 内路径相对根是全新路径：计划置空即"整子树按新增
  // 处理"，不做根级增量计划比对。
  [[nodiscard]] RootResult runScopedScan(const ScopedScanTarget& target) {
    ScannerConfig config;
    ScannerEventSink sink;
    {
      std::scoped_lock lock{mutex_};
      config = config_;
      sink = sink_;
    }
    std::error_code error;
    // 非递归根：子树内容不在曲库内，scope 结果恒为空（仍经 mergeScopedResult 清理残留行）。
    // scope 在去抖窗口内消失（移入后又被移出/替换）同样返回空结果：merge 仍会清理该子树残留。
    if (!target.root.recursive || !std::filesystem::is_directory(target.scopeAbs, error) || error) {
      return {};
    }
    const auto effectiveConfig = effectiveScannerConfig(config);
    std::uint64_t discovered = 0;
    std::uint64_t skipped = 0;
    std::uint64_t scanned = 0;
    std::uint64_t completedFiles = 0;
    std::uint64_t totalTagReaderTimeMs = 0;
    // R13.2：已存在目录的 scope 用增量执行计划（locationId 命中即缓存直灌，标签重读 ≈ 0）；
    // scope 内全新路径不在缓存 → added → 全量读取（移入语义）。封面谓词命中的 scope
    // （forceTagReread）例外：以 Full 语义重读该目录，保证歌曲级 artwork 不陈旧。
    const ScanModeDecision scopedDecision{.mode = target.forceTagReread ? ScanMode::Full : ScanMode::Incremental,
                                          .directoryTreeHash = std::nullopt};
    cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
    return reconcileRoot(target.root, scopedDecision, effectiveConfig, cache, discovered, skipped, scanned,
                         completedFiles, totalTagReaderTimeMs, sink, target.scopeAbs);
  }

  // scoped 结果并入长期成员 allSongs_，并以既有精确写 API 收敛缓存：先按 scope 前缀清理
  // 树/缓存，再写入扫描结果（disk truth）。重复/嵌套 scope 合并后仍幂等，且能清理"移入后
  // 又被移出"留下的残留行。删除 + 写入 + scope 外 cue 补偿行恢复在同一事务内（R13.5）。
  void mergeScopedResult(const ScopedScanTarget& target, RootResult& result) {
    const auto rootPath = rootPathFor(target.root);
    const auto scopeRel = relativePathFor(rootPath, target.scopeAbs);
    const auto scopeRelText = pathToUtf8(scopeRel);
    std::erase_if(allSongs_, [&scopeRelText](const RootResult::PublishedSong& entry) {
      const auto relative = pathToUtf8(entry.treeRelativePath);
      return relative == scopeRelText || relative.rfind(scopeRelText + "/", 0) == 0;
    });
    const auto scopeAbs = (rootPath / scopeRel).lexically_normal();
    cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};

    // B2 补偿：deleteLocationsByPathPrefix 同时匹配 source_file_path，会删掉"source 在 scope 内、
    // cue 在 scope 外"的 cue 轨行；scope 内枚举无法重建它们。merge 前收集这些行，merge 后原样恢复，
    // 并把 scope 结果中同一 source 的普通行跳过（全量语义下被 cue 引用的源不写普通 location）。
    std::vector<cache::CacheWriteSong> externalCueRows;
    std::unordered_set<std::string> externalCueSourceKeys;
    for (const auto& location : cache.loadLocationsByRoot(pathKey(rootPath))) {
      if (!isCueCachedLocation(location) || pathWithinScope(location.filePath, scopeAbs) ||
          !pathWithinScope(location.sourceFilePath, scopeAbs)) {
        continue;
      }
      auto song = cache.loadContent(location.contentId);
      if (!song.has_value()) {
        continue;
      }
      song->embeddedLyrics = cache.loadLyrics(location.locationId, "embedded");
      song->externalLyrics = cache.loadLyrics(location.locationId, "external");
      externalCueSourceKeys.insert(pathKey(location.sourceFilePath));
      externalCueRows.push_back(cache::CacheWriteSong{.song = std::move(*song), .location = location});
    }

    std::vector<cache::CacheWriteSong> writes;
    for (auto& publishedSong : result.songs) {
      if (!shouldRetainLocationForOrigin(publishedSong.origin)) {
        allSongs_.push_back(std::move(publishedSong));
        continue;
      }
      const auto location = cachedLocationFromSong(publishedSong.song, rootPath, publishedSong.song.metadata.filePath,
                                                   coverExportDir_);
      publishedSong.locationId = location.locationId;
      if (!externalCueSourceKeys.contains(pathKey(publishedSong.song.metadata.filePath))) {
        writes.push_back(cache::CacheWriteSong{.song = publishedSong.song, .location = location});
      } else {
        // 该源仍被 scope 外 cue 引用：按全量语义不写普通 location 行；登记到抑制集合，
        // 批次收尾若它已不再被任何 cue 引用，则补孤儿 upsert（B2 家族收敛）。
        externalCueSuppressedSourceKeys_.insert(pathKey(publishedSong.song.metadata.filePath));
      }
      allSongs_.push_back(std::move(publishedSong));
    }
    cache.replaceLocationsByPathPrefixWithSongs(pathKey(rootPath), pathKey(scopeAbs), writes, externalCueRows);
  }

  [[nodiscard]] cache::CachedSong readClassifierSong(const std::filesystem::path& path) {
    auto raw = metadataReader_->read(thumbnailOnlyRequest(path, coverExportDir_));
    raw.filePath = path;
    auto mapped = mapRawTagMetadata(raw,
                                    computeContentId(std::chrono::duration_cast<std::chrono::milliseconds>(raw.duration),
                                                     raw.title,
                                                     raw.artist),
                                    std::nullopt,
                                    false);
    auto song = cachedSongFrom(std::move(mapped));
    song.metadata.filePath = path;
    song.metadata.sourceFilePath = path;
    if (song.metadata.artworkPath.has_value() && !song.metadata.artworkPath->empty() &&
        song.metadata.artworkPath->is_relative()) {
      song.metadata.artworkPath = std::filesystem::absolute(*song.metadata.artworkPath);
    }
    if (song.metadata.thumbnailPath.has_value() && !song.metadata.thumbnailPath->empty() &&
        song.metadata.thumbnailPath->is_relative()) {
      song.metadata.thumbnailPath = std::filesystem::absolute(*song.metadata.thumbnailPath);
    }
    song.metadata.trackId = pathToUtf8(path);
    song.metadata.logicalTrackId = pathToUtf8(path);
    return song;
  }

  void refreshScanRootHash(const std::filesystem::path& rootPath) {
    try {
      const auto hash = computeDirectoryTreeHash(rootPath);
      if (!hash.hash.has_value()) {
        return;
      }
      cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
      auto scanRoot = cache.loadScanRoot(rootPath);
      if (!scanRoot.has_value()) {
        return;
      }
      scanRoot->directoryTreeHash = *hash.hash;
      cache.updateScanRoot(*scanRoot);
    } catch (const std::exception& error) {
      spdlog::warn("failed to refresh scan-root directory-tree hash for {}: {}", pathToUtf8(rootPath), error.what());
    }
  }

  void rewriteAllSongsForRename(const ClassifierRename& rename) {
    const auto oldRelText = pathToUtf8(rename.oldRel);
    const auto newRelText = pathToUtf8(rename.newRel);
    const auto oldAbsText = pathKey(rename.oldAbs);
    const auto newAbsText = pathKey(rename.newAbs);
    const auto rewrite = [&](const std::string& text) -> std::string {
      if (text == oldRelText) {
        return newRelText;
      }
      if (text.rfind(oldRelText + "/", 0) == 0) {
        return newRelText + text.substr(oldRelText.size());
      }
      if (text == oldAbsText) {
        return newAbsText;
      }
      if (text.rfind(oldAbsText + "/", 0) == 0) {
        return newAbsText + text.substr(oldAbsText.size());
      }
      // CUE 轨 logicalTrackId/trackId = <absCue>#trackN：文件级 .cue rename 必须命中
      // '#' 后缀分支，否则树键/逻辑 ID 停留在旧 cue 路径（B7）。
      if (text.rfind(oldAbsText + "#", 0) == 0) {
        return newAbsText + text.substr(oldAbsText.size());
      }
      return text;
    };
    for (auto& entry : allSongs_) {
      const auto relative = pathToUtf8(entry.treeRelativePath);
      if (relative != oldRelText && relative.rfind(oldRelText + "/", 0) != 0) {
        continue;
      }
      entry.treeRelativePath = pathFromUtf8(rewrite(relative));
      auto& metadata = entry.song.metadata;
      if (!metadata.filePath.empty()) {
        metadata.filePath = pathFromUtf8(rewrite(pathToUtf8(metadata.filePath)));
      }
      if (!metadata.sourceFilePath.empty()) {
        metadata.sourceFilePath = pathFromUtf8(rewrite(pathToUtf8(metadata.sourceFilePath)));
      }
      metadata.logicalTrackId = rewrite(metadata.logicalTrackId);
      metadata.trackId = rewrite(metadata.trackId);
    }
  }

  // 从 allSongs_ 整树重建并原子接管成员（treeBuilder_/snapshot_）：runScan 全量聚合与 scoped
  // 子树合并两条路径共用（任务 7）。scoped 歌曲只并入 allSongs_、不逐条 upsert 既有成员
  // builder，发布前必须整树重建，保证快照与 allSongs_ 一致（补丁结果 == 全量重建）。
  [[nodiscard]] PlaylistTreeSnapshot rebuildTreeFromAllSongsAndPublish(const std::vector<std::filesystem::path>& touchedPaths = {}) {
    auto builder = std::make_unique<PlaylistTreeBuilder>("Library");
    const auto cueSourcePaths = cueReferencedAudioPaths(allSongs_);
    for (const auto& publishedSong : allSongs_) {
      if (hiddenByCueSourceVisibility(publishedSong, cueSourcePaths)) {
        continue;
      }
      builder->addSong({.relativePath = publishedSong.treeRelativePath, .metadata = publishedSong.song.metadata});
    }
    auto published = builder->publish();
    // 文件夹缩略图解析：publish 之后、快照锁之外 —— seam（TagReader 导出写缓存）的 I/O 不持锁；
    // 单文件夹失败在 resolver 内隔离（回退树内兜底/空），不阻断扫描、不新增 error 事件。
    resolveFolderThumbnails(published, allSongs_, touchedPaths);
    TreeBuilderSeededSnapshot treeBuilderReport;
    {
      std::scoped_lock lock{mutex_};
      // 波 3a：长生命周期 builder 成员（生命周期=watch 会话）。整树重建并原子替换成员
      // （不累积、不产生重复树）；精准更新路径（波 3b/3c/3d、4.1）继续复用成员。
      treeBuilder_ = std::move(builder);
      ++treeBuilderGeneration_;
      snapshot_ = published;
      treeBuilderReport.seeded = true;
      treeBuilderReport.generation = treeBuilderGeneration_;
      treeBuilderReport.songCount = treeBuilder_->stats().songCount;
    }
    if (g_treeBuilderObserver) {
      g_treeBuilderObserver(treeBuilderReport);
    }
    return published;
  }

  // 发布契约（唯一事实来源；任务 7 建立、任务 8 明文固化，审查建议见 task-7-review §二.5）：
  //   1) 全量/增量扫描（runScan）：ScanStarted → ProgressUpdated(≥0) → PlaylistSnapshotUpdated
  //      → ScanCompleted（任一阶段错误时另有 ScanError）——即"一次扫描"的完整生命周期。
  //   2) 文件级精准批次（applyClassifierBatch 无 scope → publishClassifierSnapshot）：
  //      PlaylistSnapshotUpdated + ScanCompleted。commit 24186ef 起的既有设计，既有测试
  //      （waitForScanCompletedCount）与前端 LibraryScanCompleted 通知均依赖，不得压制。
  //   3) scoped 子树对账（含"精准+scope"混合批次）：只发 PlaylistSnapshotUpdated（+ 错误时
  //      ScanError）；不发 ScanStarted，也不发 ScanCompleted——子树对账不是一次扫描，其完成
  //      只以最新快照表达。计划所述"不发 ScanStarted/ScanCompleted"仅指本路径。
  // 参数 publishCompletion 即第 2/1 类与第 3 类的分界：全量扫描与文件级精准批次为 true。
  void publishSnapshotEvents(const PlaylistTreeSnapshot& published, bool publishCompletion) {
    ScannerEventSink sink;
    {
      std::scoped_lock lock{mutex_};
      sink = sink_;
    }
    publishEvent(sink, ScannerEventType::PlaylistSnapshotUpdated, ++eventVersion_, published);
    if (publishCompletion) {
      publishEvent(sink, ScannerEventType::ScanCompleted, ++eventVersion_, published);
    }
  }

  void publishClassifierSnapshot(const std::vector<std::filesystem::path>& touchedPaths = {}) {
    if (!treeBuilder_) {
      return;
    }
    auto published = treeBuilder_->publish();
    resolveFolderThumbnails(published, allSongs_, touchedPaths);
    {
      std::scoped_lock lock{mutex_};
      snapshot_ = published;
    }
    publishSnapshotEvents(published, /*publishCompletion=*/true);
  }

  bool applyClassifierBatch(const std::vector<ScannerRoot>& roots, const std::vector<WatchEvent>& batch) {
    if (batch.empty()) {
      return true;
    }
    if (!treeBuilder_) {
      // 树未种子化（首扫尚在进行）：丢弃批次 + 置脏 + 请求 Reconcile（内容变更对树哈希不可见）。
      spdlog::debug("classifier batch dropped: tree not seeded; requesting reconcile");
      for (const auto& root : roots) {
        markPendingReconcile(root.path);
      }
      enqueueReconcile(roots);
      return true;
    }
    ScannerConfig config;
    {
      std::scoped_lock lock{mutex_};
      config = config_;
    }
    const auto effective = effectiveScannerConfig(config);
    std::vector<ScopedScanTarget> pendingScopes = extractScopedScanTargets(roots, batch);

    std::vector<ClassifierRename> renames;
    std::vector<ClassifierRemove> removes;
    std::vector<ClassifierUpsert> upserts;
    std::unordered_map<std::string, std::filesystem::path> renameOldToNew;
    std::unordered_map<std::string, ClassifierUpsert> upsertByKey;
    std::unordered_map<std::string, ClassifierDestroy> destroyByKey;
    std::unordered_map<std::string, std::filesystem::path> moveSelfByRaw;
    std::unordered_set<std::string> createdInBatch;
    std::vector<std::filesystem::path> lyricsTouched;
    std::unordered_set<std::string> lyricsTouchedKeys;
    std::vector<std::filesystem::path> shapeChangedRoots;
    std::unordered_set<std::string> shapeChangedRootKeys;
    std::vector<ScannerRoot> reconcileRequests;
    std::vector<std::filesystem::path> touchedPaths;

    const auto isRootSelf = [](const std::filesystem::path& path, const std::filesystem::path& root) {
      return pathKey(path) == pathKey(root);
    };
    const auto noteShapeChange = [&](const std::filesystem::path& rootPath) {
      if (shapeChangedRootKeys.insert(pathKey(rootPath)).second) {
        shapeChangedRoots.push_back(rootPath);
      }
    };
    const auto requestReconcile = [&](const std::filesystem::path& rootPath) {
      markPendingReconcile(rootPath);
      const auto known = std::ranges::find_if(roots, [&](const ScannerRoot& root) {
        return pathKey(rootPathFor(root)) == pathKey(rootPath);
      });
      if (known != roots.end()) {
        reconcileRequests.push_back(*known);
      } else {
        reconcileRequests.push_back(ScannerRoot{.path = rootPath, .recursive = true});
      }
    };
    const auto addScope = [&](const std::filesystem::path& rootPath, const std::filesystem::path& scopeAbs,
                              bool forceTagReread = false) {
      const ScannerRoot root{.path = rootPath, .recursive = true};
      if (!mergeScopedScanTarget(pendingScopes, root, scopeAbs, forceTagReread)) {
        requestReconcile(rootPath);
      }
    };
    const auto addLyricsTouched = [&](const std::filesystem::path& lrcPath) {
      if (lyricsTouchedKeys.insert(pathKey(lrcPath)).second) {
        lyricsTouched.push_back(lrcPath);
      }
    };

    const auto handlePrimaryEvent = [&](const WatchEvent& event) {
      switch (event.effectKind) {
        case WatchEffectKind::Created:
        case WatchEffectKind::Modified: {
          if (event.pathKind != WatchPathKind::File) {
            // Created/Modified Directory（移入）：T1 scope = 该目录（R1）；scope==root 时 Reconcile。
            const auto root = findRootForPath(roots, event.path);
            if (root.has_value()) {
              addScope(root->path, event.path);
              if (event.effectKind == WatchEffectKind::Created) {
                noteShapeChange(root->path);
              }
            }
            return;
          }
          if (isLyricsSidecarPath(event.path)) {
            addLyricsTouched(event.path);  // .lrc 排除出树哈希：不刷新 hash（R12）。
            return;
          }
          if (isCoverSidecar(event.path)) {
            // 封面谓词命中：T1 scope = 父目录（存在且 != root）；父目录 == root 时 no-op（R7）。
            // 强制重读该目录歌曲标签（R13.2）：歌曲级 artworkPath/thumbnailPath 是扫描期元数据，
            // 封面增删不改音频 size/mtime，缓存直灌不会刷新它。
            const auto root = findRootForPath(roots, event.path);
            if (root.has_value()) {
              const auto parent = event.path.parent_path();
              if (pathKey(parent) != pathKey(root->path)) {
                addScope(root->path, parent, /*forceTagReread=*/true);
              }
              if (event.effectKind == WatchEffectKind::Created) {
                noteShapeChange(root->path);
              }
            }
            return;
          }
          if (event.effectKind == WatchEffectKind::Created) {
            createdInBatch.insert(pathKey(event.path));
            if (std::filesystem::exists(event.path)) {
              const auto root = findRootForPath(roots, event.path);
              if (root.has_value()) {
                noteShapeChange(root->path);
              }
            }
          }
          upsertByKey[pathKey(event.path)] = ClassifierUpsert{.root = {},
                                                              .raw = event.path,
                                                              .abs = {},
                                                              .rel = {},
                                                              .created = event.effectKind == WatchEffectKind::Created};
          return;
        }
        case WatchEffectKind::Destroyed: {
          if (event.pathKind != WatchPathKind::File && event.pathKind != WatchPathKind::Directory) {
            return;
          }
          if (isLyricsSidecarPath(event.path)) {
            addLyricsTouched(event.path);
            return;
          }
          if (isCoverSidecar(event.path)) {
            const auto root = findRootForPath(roots, event.path);
            if (root.has_value()) {
              const auto parent = event.path.parent_path();
              if (pathKey(parent) != pathKey(root->path)) {
                addScope(root->path, parent, /*forceTagReread=*/true);
              }
              noteShapeChange(root->path);
            }
            return;
          }
          destroyByKey[pathKey(event.path)] = ClassifierDestroy{.raw = event.path, .pathKind = event.pathKind};
          return;
        }
        case WatchEffectKind::OwnerChanged:
          return;
        case WatchEffectKind::Other: {
          if (event.pathKind != WatchPathKind::Directory) {
            return;
          }
          moveSelfByRaw[pathKey(event.path)] = event.path;
          return;
        }
        case WatchEffectKind::Renamed:
          return;
      }
    };

    std::function<void(const WatchEvent&)> walk = [&](const WatchEvent& event) {
      if (event.effectKind == WatchEffectKind::Renamed) {
        for (const auto& associated : event.associated) {
          if (associated.effectKind != WatchEffectKind::Renamed) {
            continue;
          }
          const auto oldPath = event.path;
          const auto newPath = associated.path;
          const auto root = findRootForPath(roots, oldPath);
          if (!root.has_value()) {
            spdlog::debug("rename old path outside watched roots; ignored: {}", pathToUtf8(oldPath));
            return;
          }
          if (isRootSelf(oldPath, root->path)) {
            // 根自身 rename：永不破坏性、永不回落（R3c）。
            spdlog::info("root self renamed; keeping index: {}", pathToUtf8(oldPath));
            return;
          }
          const auto newRoot = findRootForPath(roots, newPath);
          if (!newRoot.has_value() || pathKey(newRoot->path) != pathKey(root->path)) {
            // 移出所有根或跨根移动：旧路径消失 → 旧根精准前缀删除；跨根时新落点按 upsert/scope 收编。
            const auto oldRel = relativePathFor(root->path, oldPath);
            const auto oldAbs = (root->path / oldRel).lexically_normal();
            removes.push_back(ClassifierRemove{.root = root->path, .abs = oldAbs, .rel = oldRel});
            noteShapeChange(root->path);
            if (newRoot.has_value()) {
              const auto newRel = relativePathFor(newRoot->path, newPath);
              const auto newAbs = (newRoot->path / newRel).lexically_normal();
              if (associated.pathKind == WatchPathKind::Directory) {
                addScope(newRoot->path, newAbs);
              } else {
                createdInBatch.insert(pathKey(newPath));
                upsertByKey[pathKey(newPath)] = ClassifierUpsert{.root = newRoot->path,
                                                                 .raw = newPath,
                                                                 .abs = newAbs,
                                                                 .rel = newRel,
                                                                 .created = true};
              }
              noteShapeChange(newRoot->path);
            }
            return;
          }
          const auto oldRel = relativePathFor(root->path, oldPath);
          const auto newRel = relativePathFor(root->path, newPath);
          const auto oldAbs = (root->path / oldRel).lexically_normal();
          const auto newAbs = (root->path / newRel).lexically_normal();
          renames.push_back(ClassifierRename{.root = root->path,
                                             .oldAbs = oldAbs,
                                             .newAbs = newAbs,
                                             .oldRel = oldRel,
                                             .newRel = newRel});
          renameOldToNew[pathKey(oldAbs)] = newAbs;
          upsertByKey.erase(pathKey(oldAbs));
          destroyByKey.erase(pathKey(oldAbs));
          moveSelfByRaw.erase(pathKey(oldAbs));
          noteShapeChange(root->path);
          return;
        }
        spdlog::debug("renamed event without associated path; ignored: {}", pathToUtf8(event.path));
        return;
      }
      handlePrimaryEvent(event);
      for (const auto& associated : event.associated) {
        walk(associated);
      }
    };

    for (const auto& event : batch) {
      if (eventCoveredByScopes(event, pendingScopes)) {
        continue;
      }
      walk(event);
    }

    // upsert 的判定统一以磁盘当前状态为准，而不是只看事件类型 —— macOS 的
    // FSEvents 与 Linux inotify 在这里差异很大：
    //   - 删除：FSEvents 会在 destroyed 之后再补一个 modified（文件已不存在）；
    //   - 移出被监视目录：FSEvents 只给一个 modified，完全没有 destroyed。
    // 只要 upsert 的路径已不存在，就不可能真的 upsert，按删除处理才是对的。
    for (auto iterator = upsertByKey.begin(); iterator != upsertByKey.end();) {
      std::error_code existsError;
      if (std::filesystem::exists(iterator->second.raw, existsError) && !existsError) {
        destroyByKey.erase(iterator->first);
        ++iterator;
        continue;
      }

      const auto key = iterator->first;
      const auto raw = iterator->second.raw;
      const auto createdHere = createdInBatch.contains(key);
      iterator = upsertByKey.erase(iterator);

      if (createdHere) {
        // 批内建了又没了（如 TagReader 的封面导出探针文件），净效果为零。
        destroyByKey.erase(key);
        continue;
      }

      destroyByKey.try_emplace(key, ClassifierDestroy{.raw = raw, .pathKind = WatchPathKind::File});
      const auto root = findRootForPath(roots, raw);
      if (root.has_value()) {
        noteShapeChange(root->path);  // 净效果为删除：磁盘形状变化
      }
    }

    for (const auto& [key, raw] : moveSelfByRaw) {
      if (renameOldToNew.contains(key)) {
        continue;
      }
      const auto root = findRootForPath(roots, raw);
      if (!root.has_value()) {
        continue;
      }
      if (isRootSelf(raw, root->path)) {
        spdlog::info("root self move reported; keeping index for recovery: {}", pathToUtf8(root->path));
        continue;
      }
      noteShapeChange(root->path);
      const auto rel = relativePathFor(root->path, raw);
      const auto abs = (root->path / rel).lexically_normal();
      std::error_code kindError;
      const bool stillExists = std::filesystem::exists(abs, kindError) && !kindError;
      if (stillExists) {
        // 路径仍存在（歧义：移出后被同名目录顶替）：按目录 scope 收敛，不回落。
        if (std::filesystem::is_directory(abs, kindError) && !kindError) {
          addScope(root->path, abs);
        } else {
          upsertByKey[pathKey(abs)] = ClassifierUpsert{.root = root->path,
                                                       .raw = abs,
                                                       .abs = abs,
                                                       .rel = rel,
                                                       .created = false};
        }
        continue;
      }
      removes.push_back(ClassifierRemove{.root = root->path, .abs = abs, .rel = rel});
    }

    for (const auto& [key, destroy] : destroyByKey) {
      const auto root = findRootForPath(roots, destroy.raw);
      if (!root.has_value()) {
        continue;
      }
      if (isRootSelf(destroy.raw, root->path)) {
        // 根自身事件：永不破坏性、永不回落（R3c）。单文件根（根自身即已索引歌曲）的删除
        // 是唯一例外——删除的是整个根，按精准前缀删除收敛。
        if (rootHasIndexedSelfEntry(root->path)) {
          const auto rel = relativePathFor(root->path, destroy.raw);
          const auto abs = (root->path / rel).lexically_normal();
          removes.push_back(ClassifierRemove{.root = root->path, .abs = abs, .rel = rel});
          noteShapeChange(root->path);
        } else {
          spdlog::info("root self destroyed; keeping index for recovery: {}", pathToUtf8(root->path));
        }
        continue;
      }
      const auto rel = relativePathFor(root->path, destroy.raw);
      const auto abs = (root->path / rel).lexically_normal();
      removes.push_back(ClassifierRemove{.root = root->path, .abs = abs, .rel = rel});
      noteShapeChange(root->path);
    }

    // 批内防御性去重：同一路径可能经不同事件形状（moveSelf 或重复 destroy）在同一批内
    // 进入 removes。跨批冗余无需处理（重复 remove 幂等：removeSubtree/
    // deleteLocationsByPathPrefix 重复执行结果一致，也不引入跨批去重状态）；本段只按
    // pathKey(abs) 消除同批内重复，保留首个。
    {
      std::unordered_set<std::string> seenRemoveKeys;
      seenRemoveKeys.reserve(removes.size());
      std::erase_if(removes, [&](const ClassifierRemove& remove) {
        return !seenRemoveKeys.insert(pathKey(remove.abs)).second;
      });
    }

    std::vector<RemovalPrefixPair> removalPrefixes;
    removalPrefixes.reserve(removes.size());
    for (const auto& remove : removes) {
      removalPrefixes.push_back({pathToUtf8(remove.rel), pathKey(remove.abs)});
    }

    for (auto& [key, op] : upsertByKey) {
      const auto root = findRootForPath(roots, op.raw);
      if (!root.has_value()) {
        continue;
      }
      if (isRootSelf(op.raw, root->path)) {
        // 根自身 upsert：仅单文件根（根自身是常规文件）按 basename upsert；目录根 no-op（R3c）。
        std::error_code kindError;
        if (!std::filesystem::is_regular_file(root->path, kindError) || kindError) {
          continue;
        }
      }
      const auto rel = relativePathFor(root->path, op.raw);
      const auto abs = (root->path / rel).lexically_normal();
      if (isCueSheetPath(abs)) {
        // .cue 新建/修改：T1 scope = cue 父目录（R1/§3.3）；父目录 == root → Reconcile。
        if (std::filesystem::exists(abs)) {
          addScope(root->path, abs.parent_path());
          noteShapeChange(root->path);
        }
        continue;
      }
      if (!isSupportedAudioExtension(abs, effective.scanner.allowedExtensions)) {
        continue;  // 无关文件（.txt 等）：严格 no-op（R13.3）。
      }
      op.root = root->path;
      op.abs = abs;
      op.rel = rel;
      upserts.push_back(op);
    }

    bool anyMutation = false;
    bool ranScopedScan = false;
    try {
      cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
      std::unordered_set<std::string> scannedRootKeys;
      const auto rootIsScanned = [&](const std::filesystem::path& rootPath) {
        const auto key = pathKey(rootPath);
        if (scannedRootKeys.contains(key)) {
          return true;
        }
        if (cache.loadScanRoot(rootPath).has_value()) {
          scannedRootKeys.insert(key);
          return true;
        }
        return false;
      };
      // 未扫描 root（scan_roots 无记录）→ 提交该 root 的 Reconcile（R11），不回落全根重扫。
      std::erase_if(renames, [&](const ClassifierRename& rename) {
        if (rootIsScanned(rename.root)) {
          return false;
        }
        spdlog::warn("precise rename requires a scanned root; requesting reconcile: {}", pathToUtf8(rename.root));
        requestReconcile(rename.root);
        return true;
      });
      std::erase_if(upserts, [&](const ClassifierUpsert& upsert) {
        if (rootIsScanned(upsert.root)) {
          return false;
        }
        spdlog::warn("precise upsert requires a scanned root; requesting reconcile: {}", pathToUtf8(upsert.root));
        requestReconcile(upsert.root);
        return true;
      });

      std::unordered_set<std::string> cueSourcePaths = cueReferencedAudioPaths(allSongs_);
      const auto hidden = [&](const RootResult::PublishedSong& entry) {
        return hiddenByCueSourceVisibility(entry, cueSourcePaths);
      };

      for (const auto& rename : renames) {
        if (!treeBuilder_->renameSubtree(rename.oldRel, rename.newRel)) {
          spdlog::warn("classifier rename could not be applied to tree; requesting reconcile: {} -> {}",
                       pathToUtf8(rename.oldRel), pathToUtf8(rename.newRel));
          requestReconcile(rename.root);
          continue;
        }
        rewriteAllSongsForRename(rename);
        // rename 已改写 allSongs_ 路径，CUE 源音频（sourceFilePath 集合）须按新路径重算，
        // 否则重 upsert 用改名前的旧路径判定隐藏，CUE 源音频被误插为幽灵可见曲目。
        cueSourcePaths = cueReferencedAudioPaths(allSongs_);
        cache.replaceLocationsBySubtree(pathKey(rename.root), pathKey(rename.oldAbs), pathKey(rename.newAbs));
        const auto newRelText = pathToUtf8(rename.newRel);
        for (const auto& entry : allSongs_) {
          const auto relative = pathToUtf8(entry.treeRelativePath);
          if (relative != newRelText && relative.rfind(newRelText + "/", 0) != 0) {
            continue;
          }
          if (hidden(entry)) {
            continue;
          }
          treeBuilder_->upsertSong({.relativePath = entry.treeRelativePath, .metadata = entry.song.metadata});
        }
        touchedPaths.push_back(rename.oldRel);
        touchedPaths.push_back(rename.newRel);
        anyMutation = true;
      }

      // 交叉收集必须在 removes 擦除 allSongs_ 之前（需要看到被删的 cue 条目）。
      const auto crossings = collectCueCrossings(removalPrefixes);

      for (const auto& remove : removes) {
        const bool treeChanged = treeBuilder_->removeSubtree(remove.rel);
        const auto cacheDeleted = cache.deleteLocationsByPathPrefix(pathKey(remove.root), pathKey(remove.abs));
        const auto beforeCount = allSongs_.size();
        const auto relText = pathToUtf8(remove.rel);
        std::erase_if(allSongs_, [&](const RootResult::PublishedSong& entry) {
          const auto relative = pathToUtf8(entry.treeRelativePath);
          return relative == relText || relative.rfind(relText + "/", 0) == 0;
        });
        anyMutation = anyMutation || treeChanged || cacheDeleted > 0 || allSongs_.size() != beforeCount;
        touchedPaths.push_back(remove.rel);
      }
      // removes 已从 allSongs_ 删除整棵子树条目（含原 CUE 源音频条目）：upserts 判定隐藏
      // 前必须重算 cue 源可见性集合。否则同批"移出含 cue 的目录 + upsert 一个原本被 CUE
      // 隐藏的源音频"会沿用删除前的陈旧集合，把新条目留在树外（快照/缓存分歧）。
      cueSourcePaths = cueReferencedAudioPaths(allSongs_);

      // 孤儿源重 upsert（B3 精确过滤）：未被剩余 cue 引用 ∧ 仍存在 ∧ 属 watched root；按 abs 去重。
      std::unordered_set<std::string> upsertAbsKeys;
      for (const auto& upsert : upserts) {
        upsertAbsKeys.insert(pathKey(upsert.abs));
      }
      for (const auto& orphan : crossings.orphanedSources) {
        const auto orphanKey = pathKey(orphan);
        if (cueSourcePaths.contains(orphanKey)) {
          continue;
        }
        if (!std::filesystem::exists(orphan)) {
          continue;
        }
        const auto orphanRoot = findRootForPath(roots, orphan);
        if (!orphanRoot.has_value()) {
          continue;
        }
        if (!rootIsScanned(orphanRoot->path)) {
          // 未扫描 root（无 scan_roots 行）无法写 locations（FK）→ 提交 Reconcile 首次索引，
          // 不靠 FK 异常兜底。
          requestReconcile(orphanRoot->path);
          continue;
        }
        if (upsertAbsKeys.contains(orphanKey)) {
          continue;
        }
        const auto orphanRel = relativePathFor(orphanRoot->path, orphan);
        upserts.push_back(ClassifierUpsert{.root = orphanRoot->path,
                                           .raw = orphan,
                                           .abs = orphan,
                                           .rel = orphanRel,
                                           .created = true});
        upsertAbsKeys.insert(orphanKey);
      }

      // cueRefresh：cue 仍在、源被移除 → T1 scope = cue 父目录（scope==root → Reconcile）。
      for (const auto& cueTarget : crossings.cueRefreshTargets) {
        const auto cueRoot = findRootForPath(roots, cueTarget);
        if (!cueRoot.has_value()) {
          continue;
        }
        const auto parent = cueTarget.parent_path();
        if (pathKey(parent) == pathKey(cueRoot->path)) {
          requestReconcile(cueRoot->path);
        } else {
          addScope(cueRoot->path, parent);
        }
      }

      std::vector<ScannerError> upsertLyricsErrors;
      // 精准 upsert 应用体：removes 派生孤儿与本批新增 upsert 共用；返回 false 表示取消
      // （调用方置脏标记后结束本批，绝不回落扫描）。
      const auto applyUpsert = [&](const ClassifierUpsert& upsert) -> bool {
        if (!std::filesystem::exists(upsert.abs)) {
          return true;
        }
        auto song = readClassifierSong(upsert.abs);
        // 必须在 cachedLocationFromSong 之前对账：location 行的 lyricsSource/externalLrc* 取自
        // song.metadata，顺序颠倒会让缓存行停留在"无外部歌词"的旧判定。
        const auto lyricsAction = reconcileLyrics(song, effective.scanner, upsertLyricsErrors);
        if (lyricsAction == ExternalLyricsCacheAction::Cancelled) {
          for (const auto& root : roots) {
            markPendingReconcile(root.path);
          }
          return false;
        }
        const auto location = cachedLocationFromSong(song, upsert.root, upsert.abs, coverExportDir_);
        RootResult::PublishedSong entry{.song = std::move(song),
                                        .treeRelativePath = upsert.rel,
                                        .sourceRoot = upsert.root,
                                        .origin = upsert.created ? ScanItemOrigin::ScannedNew : ScanItemOrigin::RescannedChanged,
                                        .locationId = location.locationId,
                                        .externalLyricsCacheAction = lyricsAction};
        const auto existing = std::ranges::find_if(allSongs_, [&](const RootResult::PublishedSong& candidate) {
          return candidate.treeRelativePath == upsert.rel && pathKey(candidate.sourceRoot) == pathKey(upsert.root);
        });
        if (existing != allSongs_.end()) {
          *existing = entry;
        } else {
          allSongs_.push_back(entry);
        }
        if (!hidden(entry)) {
          treeBuilder_->upsertSong({.relativePath = upsert.rel, .metadata = entry.song.metadata});
        }
        cache.upsertContent(location.contentId, entry.song.metadata);
        cache.upsertLocation(location);
        cache.replaceLyrics(location.locationId, "embedded", entry.song.embeddedLyrics);
        cache.replaceLyrics(location.locationId, "external", entry.song.externalLyrics);
        touchedPaths.push_back(upsert.rel);
        anyMutation = true;
        return true;
      };
      for (const auto& upsert : upserts) {
        if (!applyUpsert(upsert)) {
          return true;
        }
      }

      // .lrc 事件（增/改/删统一）：按 expectedLyricsSidecarPath 配对全部匹配条目并逐条对账；
      // 树侧统一 upsertSong（cue 轨按 logicalTrackId 定位，attachExternalLyrics 对 cue 轨无效）；
      // 缓存侧单事务 applyLyricsCacheUpdates（不得构造 ScanRootCacheWrite，空 retained 会清库）。
      if (!lyricsTouched.empty()) {
        std::vector<cache::LyricsCacheUpdate> lyricsUpdates;
        for (const auto& lrcPath : lyricsTouched) {
          const auto lrcKey = pathKey(lrcPath);
          for (auto& entry : allSongs_) {
            if (entry.song.metadata.filePath.empty() ||
                pathKey(expectedLyricsSidecarPath(entry.song.metadata.filePath)) != lrcKey) {
              continue;
            }
            const auto action = reconcileLyrics(entry.song, effective.scanner, upsertLyricsErrors);
            if (action == ExternalLyricsCacheAction::Cancelled) {
              for (const auto& root : roots) {
                markPendingReconcile(root.path);
              }
              return true;
            }
            touchedPaths.push_back(entry.treeRelativePath);
            anyMutation = true;
            if (hidden(entry)) {
              // 全量语义下被 cue 隐藏的源音频不入缓存，无可更新的 location 行。
              continue;
            }
            const auto location = cachedLocationFromSong(entry.song, entry.sourceRoot, entry.song.metadata.filePath,
                                                         coverExportDir_);
            entry.locationId = location.locationId;
            lyricsUpdates.push_back(cache::LyricsCacheUpdate{
                .locationId = location.locationId,
                .externalLrcPath = entry.song.metadata.externalLyricsPath,
                .externalLrcMtimeNs = fileTimeNanoseconds(entry.song.metadata.externalLyricsMtime),
                .externalLrcHash = entry.song.metadata.externalLyricsHash,
                .externalLyrics = entry.song.externalLyrics,
                .effectiveLyricsSource = entry.song.metadata.effectiveLyricsSource,
                .removeExternalLyrics = action == ExternalLyricsCacheAction::RemoveExternal});
            treeBuilder_->upsertSong({.relativePath = entry.treeRelativePath, .metadata = entry.song.metadata});
          }
        }
        if (!lyricsUpdates.empty()) {
          cache.applyLyricsCacheUpdates(lyricsUpdates);
        }
      }

      // scope 守卫 + 成本门（覆盖 walk 期间新增的 cueRefresh/封面/目录 scope）。
      std::vector<ScopedScanTarget> runnableScopes;
      for (const auto& scope : pendingScopes) {
        if (!rootIsScanned(scope.root.path)) {
          requestReconcile(scope.root.path);
          continue;
        }
        if (scopeOverCostGate(scope)) {
          requestReconcile(scope.root.path);
          continue;
        }
        runnableScopes.push_back(scope);
      }
      if (!runnableScopes.empty()) {
        ScannerEventSink scopeSink;
        {
          std::scoped_lock lock{mutex_};
          scopeSink = sink_;
        }
        for (auto& scope : runnableScopes) {
          // scope 只可能由形状变化事件（目录移入/封面/cue 增改）产生：合并后刷新根哈希。
          noteShapeChange(scope.root.path);
          auto scoped = runScopedScan(scope);
          if (scoped.cancelled) {
            spdlog::debug("scoped reconcile cancelled for {}", pathToUtf8(scope.scopeAbs));
            for (const auto& root : roots) {
              markPendingReconcile(root.path);
            }
            return true;
          }
          for (const auto& error : scoped.errors) {
            publishEvent(scopeSink, ScannerEventType::ScanError, ++eventVersion_, error);
          }
          mergeScopedResult(scope, scoped);
          touchedPaths.push_back(relativePathFor(rootPathFor(scope.root), scope.scopeAbs));
          ranScopedScan = true;
          anyMutation = true;
        }
      }

      // B2 家族收敛：曾被 scope 外 cue 抑制写入的源，在 cue 重解析/删除后可能已不再被引用。
      // 此时必须补孤儿 upsert（读标签 + 写 location + 树可见），否则快照可见而缓存无行，
      // 补丁 ≠ 全量重建。仍被引用的源继续保留在抑制集合中等待后续批次。
      if (!externalCueSuppressedSourceKeys_.empty()) {
        cueSourcePaths = cueReferencedAudioPaths(allSongs_);
        std::vector<std::string> resolvedSuppressedKeys;
        for (const auto& key : externalCueSuppressedSourceKeys_) {
          if (cueSourcePaths.contains(key)) {
            continue;
          }
          const auto source = pathFromUtf8(key);
          const auto sourceRoot = std::filesystem::exists(source) ? findRootForPath(roots, source) : std::nullopt;
          if (sourceRoot.has_value() && rootIsScanned(sourceRoot->path) && !upsertAbsKeys.contains(key)) {
            ClassifierUpsert orphan{.root = sourceRoot->path,
                                    .raw = source,
                                    .abs = source,
                                    .rel = relativePathFor(sourceRoot->path, source),
                                    .created = true};
            upsertAbsKeys.insert(key);
            if (!applyUpsert(orphan)) {
              return true;
            }
          }
          resolvedSuppressedKeys.push_back(key);
        }
        for (const auto& key : resolvedSuppressedKeys) {
          externalCueSuppressedSourceKeys_.erase(key);
        }
      }

      if (!upsertLyricsErrors.empty()) {
        ScannerEventSink lyricsErrorSink;
        {
          std::scoped_lock lock{mutex_};
          lyricsErrorSink = sink_;
        }
        for (const auto& error : upsertLyricsErrors) {
          publishEvent(lyricsErrorSink, ScannerEventType::ScanError, ++eventVersion_, error);
        }
      }

      for (const auto& rootPath : shapeChangedRoots) {
        refreshScanRootHash(rootPath);
      }
      if (anyMutation) {
        if (!ranScopedScan) {
          publishClassifierSnapshot(touchedPaths);
        } else {
          // scoped 子树对账：整树重建（scope 歌曲只并入 allSongs_）后发布。按 publishSnapshotEvents
          // 顶部的发布契约边界（第 3 类），本路径只发 PlaylistSnapshotUpdated，不发 ScanStarted/
          // ScanCompleted；纯文件级精准批次（上方 publishClassifierSnapshot）保留既有 ScanCompleted。
          const auto published = rebuildTreeFromAllSongsAndPublish(touchedPaths);
          publishSnapshotEvents(published, /*publishCompletion=*/false);
        }
      }
    } catch (const std::exception& error) {
      // R10：异常恢复 = 按批 root 提交 Reconcile（有界），绝不回落全根重扫。
      spdlog::warn("classifier precise update failed; requesting reconcile: {}", error.what());
      for (const auto& root : roots) {
        markPendingReconcile(root.path);
      }
      enqueueReconcile(roots);
      return true;
    }

    if (!reconcileRequests.empty()) {
      std::vector<ScannerRoot> uniqueRequests;
      std::unordered_set<std::string> seenRequests;
      for (const auto& root : reconcileRequests) {
        if (seenRequests.insert(pathKey(root.path)).second) {
          uniqueRequests.push_back(root);
        }
      }
      enqueueReconcile(uniqueRequests);
    }
    return true;
  }

  void reconcileRootsPeriodically(const std::vector<ScannerRoot>& roots) {
    if (roots.empty()) {
      return;
    }
    std::vector<ScannerRoot> rootsToReconcile;
    for (const auto& root : roots) {
      // R10：脏标记 root 无条件 Reconcile（纯内容修改对树哈希不可见）；否则维持 hash 门控。
      if (isPendingReconcile(rootPathFor(root))) {
        rootsToReconcile.push_back(root);
        continue;
      }
      const auto decision = decideScanMode(root, ScanMode::Incremental, databasePath_);
      if (decision.mode == ScanMode::Full) {
        rootsToReconcile.push_back(root);
      }
    }
    if (!rootsToReconcile.empty()) {
      enqueueScan(rootsToReconcile, ScanMode::Incremental, /*reconcile=*/true);
    }
  }

  void debounceLoop(const std::shared_ptr<WatchRuntimeState>& state) {
    std::uint64_t processedGeneration = 0;
    while (true) {
      std::vector<ScannerRoot> roots;
      std::vector<std::string> watcherMessages;
      std::vector<WatchEvent> watcherEvents;
      bool fallbackRescan = false;
      bool periodicProbe = false;
      {
        std::unique_lock lock{state->mutex};
        const auto wokeByEvent = state->changed.wait_for(lock, reconcileInterval_, [&state, processedGeneration] {
          return state->stopping || state->dirtyGeneration != processedGeneration;
        });
        if (state->stopping) {
          return;
        }
        if (!wokeByEvent) {
          roots = state->watchedRoots;
          periodicProbe = true;
        } else {
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
          watcherEvents = std::move(state->pendingWatcherEvents);
          state->pendingWatcherEvents.clear();
          fallbackRescan = state->pendingFallbackRescan;
          state->pendingFallbackRescan = false;
        }
      }
      if (periodicProbe) {
        reconcileRootsPeriodically(roots);
        continue;
      }
      {
        std::scoped_lock lock{mutex_};
        pendingClassifierEvents_.insert(pendingClassifierEvents_.end(),
                                        std::make_move_iterator(watcherEvents.begin()),
                                        std::make_move_iterator(watcherEvents.end()));
        pendingClassifierFallbackRescan_ = pendingClassifierFallbackRescan_ || fallbackRescan;
      }
      publishWatcherMessages(watcherMessages);

      std::vector<WatchEvent> classifierEvents;
      bool classifierFallback = false;
      {
        std::scoped_lock lock{mutex_};
        classifierEvents = std::move(pendingClassifierEvents_);
        pendingClassifierEvents_.clear();
        classifierFallback = pendingClassifierFallbackRescan_;
        pendingClassifierFallbackRescan_ = false;
      }

      // watcher 消息 / 队列溢出标记 = 事件可能丢失或不完整 → Reconcile（永不 Full，R4d）；
      // 无事件批次同样走 Reconcile（设计 §1 :3305 处置）。
      const bool eventsUnreliable = classifierFallback || !watcherMessages.empty();
      if (eventsUnreliable || classifierEvents.empty()) {
        enqueueScan(roots, ScanMode::Incremental, /*reconcile=*/true);
        continue;
      }

      bool handled = false;
      {
        std::lock_guard scanLock{scanMutex_};
        handled = applyClassifierBatch(roots, classifierEvents);
      }
      if (!handled) {
        // applyClassifierBatch 全覆盖后恒为 true；防御性保留：Reconcile，绝不回落 Full。
        enqueueScan(roots, ScanMode::Incremental, /*reconcile=*/true);
      }
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
    bool reconcile{false};
  };

  void reportScanFailure(std::string_view detail) noexcept {
    try {
      spdlog::error("scanner scan failed with an unhandled exception: {}", detail);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "failed to log scanner scan failure: %s\n", error.what());
    } catch (...) {
      std::fputs("failed to log scanner scan failure: unknown exception\n", stderr);
    }

    try {
      ScannerEventSink sink;
      {
        std::scoped_lock lock{mutex_};
        sink = sink_;
      }
      publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_,
                   ScannerError{.code = ScannerErrorCode::CacheUnavailable,
                                .message = "scanner scan failed with an unhandled exception",
                                .detail = std::string{detail},
                                .path = std::nullopt});
    } catch (const std::exception& error) {
      std::fprintf(stderr, "failed to publish scanner scan failure: %s\n", error.what());
    } catch (...) {
      std::fputs("failed to publish scanner scan failure: unknown exception\n", stderr);
    }
  }

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
      try {
        runScan(request.roots, request.mode, request.reconcile);
      } catch (const std::exception& error) {
        reportScanFailure(error.what());
      } catch (...) {
        reportScanFailure("unknown exception escaped runScan");
      }
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
  FolderThumbnailExportSeam folderThumbnailSeam_;
  std::chrono::milliseconds watcherDebounce_{50};
  std::chrono::milliseconds reconcileInterval_{60000};
  PlaylistTreeSnapshot snapshot_{};
  mutable std::mutex mutex_;
  std::mutex scanMutex_;
  std::mutex scanQueueMutex_;
  std::mutex watcherMutex_;
  std::condition_variable scanQueueChanged_;
  std::deque<ScanRequest> scanQueue_;
  std::vector<std::unique_ptr<FolderWatcher>> watchers_;
  std::shared_ptr<WatchRuntimeState> watcherState_;
  std::vector<WatchEvent> pendingClassifierEvents_;
  bool pendingClassifierFallbackRescan_{false};
  // per-root 收敛脏标记（R10）：丢弃批次/队列满时置位；周期探测对其无条件 Reconcile
  // （纯内容修改对目录树哈希不可见）。Reconcile 成功后清除。
  std::unordered_set<std::string> pendingReconcileRoots_;
  std::unique_ptr<PlaylistTreeBuilder> treeBuilder_;
  std::size_t treeBuilderGeneration_{0};
  std::vector<RootResult::PublishedSong> allSongs_;
  // 目录缩略图解析结果缓存（relKey → thumbnailPath，R13.1）：watcher 局部批次只重解析
  // touched 子树，未受影响目录回填上次解析结果——每次发布都换新 builder，节点不持有缩略图，
  // 不回填会让文件夹封面在任意事件后被清成 nullopt。scan worker 与 debounce 线程都可能调用，
  // 故用独立互斥锁保护（seam I/O 在锁外执行）。
  std::mutex folderThumbnailCacheMutex_;
  std::unordered_map<std::string, std::string> folderThumbnailCache_;
  // 被 scope 外 cue 抑制写入的源音频键（B2 家族）：mergeScopedResult 跳写时登记；批次收尾
  // 若该源已不再被任何 cue 引用，则补孤儿 upsert（否则快照可见/缓存无行，补丁 ≠ 全量重建）。
  // 仅在 debounce 线程（applyClassifierBatch）访问。
  std::unordered_set<std::string> externalCueSuppressedSourceKeys_;
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

void setWatcherEventQueueObserver(WatcherEventQueueObserver observer) {
  g_watcherEventQueueObserver = std::move(observer);
}

void clearWatcherEventQueueObserver() {
  g_watcherEventQueueObserver = nullptr;
}

void setTreeBuilderObserver(TreeBuilderObserver observer) {
  g_treeBuilderObserver = std::move(observer);
}

void clearTreeBuilderObserver() {
  g_treeBuilderObserver = nullptr;
}

}
