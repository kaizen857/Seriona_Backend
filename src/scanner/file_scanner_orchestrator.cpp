#include "file_scanner_service_internal.h"
#include "scanner_internal_types.h"
#include "file_scanner_orchestrator_test_access.h"

#include "spdlog/spdlog.h"
#include "logging/logging.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
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

[[nodiscard]] std::vector<RawTagMetadata> readCueSheetWithTestSeam(const std::filesystem::path& cuePath,
                                                                   const std::filesystem::path& coverExportDir) {
  if (g_testCueSheetProvider) {
    const auto testTracks = g_testCueSheetProvider(cuePath);
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
  return readCueSheet(cuePath, coverExportDir);
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
  return std::filesystem::path{databasePath.generic_string() + ".scan-roots-v3.sqlite"};
}

[[nodiscard]] cache::CachedScanRootV3 scanRootRecord(const std::filesystem::path& rootPath,
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
    const cache::SQLiteCacheV3 cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath)}};
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
};

struct IncrementalExecutionPlan {
  std::unordered_set<std::string> unchangedPaths;
  std::unordered_set<std::string> workerPaths;
};

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

using CachedLocationPathIndex = std::unordered_map<std::string, std::reference_wrapper<const cache::CachedLocation>>;

[[nodiscard]] CachedLocationPathIndex buildCachedLocationPathIndex(const std::filesystem::path& rootPath,
                                                                   const std::vector<cache::CachedLocation>& locations) {
  CachedLocationPathIndex index;
  index.reserve(locations.size());
  for (const auto& location : locations) {
    if (pathKey(location.rootPath) != pathKey(rootPath)) {
      continue;
    }
    index.try_emplace(pathKey(location.filePath), std::cref(location));
  }
  return index;
}

[[nodiscard]] IncrementalScanPlan planIncrementalScan(const std::filesystem::path& rootPath,
                                                      const std::vector<ClassifiedPath>& fileSystemEntries,
                                                      const std::vector<cache::CachedLocation>& cachedLocations,
                                                      bool treeHashMatches = false) {
  const auto cachedLocationsByPath = buildCachedLocationPathIndex(rootPath, cachedLocations);
  std::unordered_set<std::string> observedFilePaths;
  observedFilePaths.reserve(fileSystemEntries.size());

  IncrementalScanPlan plan;
  for (const auto& entry : fileSystemEntries) {
    if (!isPlanAudioCandidate(entry)) {
      continue;
    }
    const auto normalizedPathKey = pathKey(entry.path);
    observedFilePaths.insert(normalizedPathKey);
    const auto cachedLocation = cachedLocationsByPath.find(normalizedPathKey);
    if (cachedLocation == cachedLocationsByPath.end()) {
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
                                                               const std::filesystem::path& databasePath,
                                                               const std::vector<cache::CachedLocation>& cachedLocations,
                                                               bool treeHashMatches) {
  cache::SQLiteCacheV3 cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath)}};
  const auto plan = planIncrementalScan(rootPath, entries, cachedLocations, treeHashMatches);
  const auto cachedLocationsByPath = buildCachedLocationPathIndex(rootPath, cachedLocations);
  std::vector<std::string> retainedLocationIds;
  retainedLocationIds.reserve(plan.unchanged.size() + plan.changed.size() + plan.added.size());
  IncrementalExecutionPlan executionPlan;
  executionPlan.unchangedPaths.reserve(plan.unchanged.size());
  executionPlan.workerPaths.reserve(plan.changed.size() + plan.added.size());
  for (const auto& entry : plan.unchanged) {
    executionPlan.unchangedPaths.insert(pathKey(entry.path));
    const auto cachedIt = cachedLocationsByPath.find(pathKey(entry.path));
    if (cachedIt != cachedLocationsByPath.end()) {
      retainedLocationIds.push_back(cachedIt->second.get().locationId);
    }
  }
  for (const auto& entry : plan.changed) {
    executionPlan.workerPaths.insert(pathKey(entry.path));
    const auto size = fileSizeBytes(entry.path);
    if (size.has_value()) {
      retainedLocationIds.push_back(computeLocationId(entry.path, *size, fileMtime(entry.path)));
    }
  }
  for (const auto& entry : plan.added) {
    executionPlan.workerPaths.insert(pathKey(entry.path));
    const auto size = fileSizeBytes(entry.path);
    if (size.has_value()) {
      retainedLocationIds.push_back(computeLocationId(entry.path, *size, fileMtime(entry.path)));
    }
  }
  cache.pruneDeletedLocations(rootPath, retainedLocationIds);
  return executionPlan;
}

[[nodiscard]] cache::CachedLocation cachedLocationFromSong(const cache::CachedSong& song,
                                                           const std::filesystem::path& rootPath,
                                                           const std::filesystem::path& filePath) {
  const auto filesystemMtime = fileMtime(filePath);
  const auto stableMtime = filesystemMtime.has_value() ? filesystemMtime : song.metadata.fileMtime;
  const auto mtime = fileTimeNanoseconds(stableMtime).value_or(0);
  const auto fileSize = song.metadata.fileSizeBytes.value_or(0);
  return {.locationId = computeLocationId(filePath, fileSize, stableMtime),
          .contentId = song.metadata.contentHash,
          .rootPath = rootPath,
          .filePath = filePath,
          .fileSizeBytes = song.metadata.fileSizeBytes.value_or(0),
          .fileMtimeNs = mtime,
          .sourceFilePath = song.metadata.sourceFilePath.empty() ? filePath : song.metadata.sourceFilePath,
          .cueTrackOffset = song.metadata.offset,
          .artworkPath = song.metadata.artworkPath,
          .thumbnailPath = song.metadata.thumbnailPath,
          .lyricsSource = song.metadata.effectiveLyricsSource,
          .externalLrcPath = song.metadata.externalLyricsPath,
          .externalLrcMtimeNs = fileTimeNanoseconds(song.metadata.externalLyricsMtime),
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
  song.metadata.trackId = filePath.generic_string();
  song.metadata.logicalTrackId = filePath.generic_string();
  selectEffectiveLyrics(song);
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
  if (event.pathKind == WatchPathKind::Watcher && watcherMessageRequestsRootReconciliation(event.path)) {
    state->pendingWatcherMessages.push_back(event.path.generic_string());
  }
  for (const auto& associated : event.associated) {
    if (associated.pathKind == WatchPathKind::Watcher && watcherMessageRequestsRootReconciliation(associated.path)) {
      state->pendingWatcherMessages.push_back(associated.path.generic_string());
    }
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

    cache::SQLiteCacheV3 cache{cache::ScannerCacheConfig{.databasePath = databasePath_}};
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
      recordScanRootDecision(rootPathFor(root), decision, rootResult.songs, rootScanDuration);
      allErrors.insert(allErrors.end(), rootResult.errors.begin(), rootResult.errors.end());
      allSongs.insert(allSongs.end(), rootResult.songs.begin(), rootResult.songs.end());
      for (const auto& error : rootResult.errors) {
        publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
      }
      for (const auto& publishedSong : rootResult.songs) {
        publishEvent(sink, ScannerEventType::FileScanned, ++eventVersion_, publishedSong.song.metadata);
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
    };

    std::vector<PublishedSong> songs;
    std::vector<ScannerError> errors;
  };

  struct AudioReconcileTask {
    std::filesystem::path path;
    std::filesystem::path treeRelativePath;
    std::size_t discoveryIndex{0};
    std::optional<std::string> contentHash;
    std::optional<cache::CachedSong> cachedSong;
  };

  struct WorkerSongStore {
    std::mutex mutex;
    std::map<std::string, cache::CachedSong> songsByPath;

    void put(const std::filesystem::path& path, cache::CachedSong song) {
      std::scoped_lock lock{mutex};
      songsByPath[pathKey(path)] = std::move(song);
    }

    [[nodiscard]] std::optional<cache::CachedSong> take(const std::filesystem::path& path) {
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

  [[nodiscard]] RootResult reconcileRoot(const ScannerRoot& root, const ScanModeDecision& decision, const EffectiveScannerConfig& config,
                                         cache::SQLiteCacheV3& cache,
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
    cache::SQLiteCacheV3 v3cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};

    std::vector<ClassifiedPath> entries;
    std::vector<cache::CachedLocation> cachedLocations;
    bool treeHashMatches = false;
    
    // Load v3 cache locations for incremental planning
    if (decision.mode == ScanMode::Incremental && decision.directoryTreeHash.has_value()) {
      try {
        const auto cachedScanRoot = v3cache.loadScanRoot(rootPath);
        if (cachedScanRoot.has_value()) {
          cachedLocations = v3cache.loadLocationsByRoot(rootPath);
          if (decision.directoryTreeHash.has_value() && 
              cachedScanRoot->directoryTreeHash == *decision.directoryTreeHash) {
            treeHashMatches = true;
          }
        }
      } catch (const std::exception& error) {
        spdlog::warn("reconcileRoot: failed to load v3 cache for incremental planning: {}", error.what());
      }
    }
    
    entries = discoverScannerPaths(ScannerRoot{.path = rootPath, .recursive = root.recursive}, pathConfig);
    phase1End = std::chrono::steady_clock::now();

    const auto incrementalPlan = decision.mode == ScanMode::Incremental
                                     ? std::optional<IncrementalExecutionPlan>{incrementalExecutionPlan(rootPath, entries, databasePath_, cachedLocations, treeHashMatches)}
                                     : std::nullopt;
    
    std::size_t nodeCount = 0;
    std::unordered_set<std::string> failedCuePaths;
    for (const auto& entry : entries) {
      if (entry.kind == PathEntryKind::AudioCandidate || entry.kind == PathEntryKind::SingleFileRoot) {
        ++nodeCount;
      } else if (entry.kind == PathEntryKind::CueSheet) {
        try {
          const auto tracks = readCueSheetWithTestSeam(entry.path, coverExportDir_);
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
          indexedSongs[cueContainerIndex].isVirtualFolder = true;
          continue;
        }
        
        const auto cueContainerIndex = discoveryIndex++;
        ++discovered;
        
        indexedSongs[cueContainerIndex].discoveryIndex = cueContainerIndex;
        indexedSongs[cueContainerIndex].treeRelativePath = relativePathFor(rootPath, entry.path);
        indexedSongs[cueContainerIndex].nodeType = NodeType::CueContainer;
        indexedSongs[cueContainerIndex].isVirtualFolder = true;
        
        const auto tracks = readCueSheetWithTestSeam(entry.path, coverExportDir_);
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
        // V3: Try to load from cache by locationId
        const auto fileSize = fileSizeBytes(entry.path);
        const auto fileMtimeValue = fileMtime(entry.path);
        const auto locationId = fileSize.has_value() ? computeLocationId(entry.path, *fileSize, fileMtimeValue) : std::string{};
        const auto cachedLocation = locationId.empty() ? std::optional<cache::CachedLocation>{} : v3cache.loadLocation(locationId);
        if (cachedLocation.has_value()) {
          const auto cachedSong = v3cache.loadContent(cachedLocation->contentId);
          if (cachedSong.has_value()) {
            ++skipped;
            spdlog::info("Cache hit for nodeIndex={}, filePath={}", currentDiscoveryIndex, entry.path.generic_string());
            auto hydratedSong = *cachedSong;
            hydratedSong.embeddedLyrics = v3cache.loadLyrics(cachedLocation->locationId, "embedded");
            hydratedSong.externalLyrics = v3cache.loadLyrics(cachedLocation->locationId, "external");
            applyCachedLocation(hydratedSong, *cachedLocation, entry.path);
            indexedSongs[currentDiscoveryIndex] = IndexedPublishedSong{currentDiscoveryIndex, std::move(hydratedSong), relativePathFor(rootPath, entry.path)};
            indexedSongs[currentDiscoveryIndex].filled.store(true);
            shouldProcessViaWorker = false;
          }
        }
      }
      if (!shouldProcessViaWorker) {
        continue;
      }
      const auto fileSize = fileSizeBytes(entry.path);
      const auto fileMtimeValue = fileMtime(entry.path);
      const auto locationId = fileSize.has_value() ? computeLocationId(entry.path, *fileSize, fileMtimeValue) : std::string{};
      const auto cachedLocation = fileSize.has_value() ? v3cache.loadLocation(locationId) : std::optional<cache::CachedLocation>{};
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
                                       .cachedLocation = cachedLocation,
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

    auto workerSongs = std::make_shared<WorkerSongStore>();
    ScannerWorkerPool workerPool{ScannerWorkerPool::Config{.workerCount = config.workerCount,
                                                           .tagReaderSlots = config.tagReaderSlots,
                                                           .tagReader = [this, workerSongs, &v3cache, &audioTasks, &audioTaskIndexByPath, &indexedSongs, &rootPath](const WorkerTask& task) {
                                                             auto metadata = readWorkerSong(task, v3cache, audioTasks, audioTaskIndexByPath, workerSongs);
                                                             if (task.nodeIndex < indexedSongs.size()) {
                                                               auto song = workerSongs->take(task.filePath);
                                                               if (song.has_value()) {
                                                                 indexedSongs[task.nodeIndex].discoveryIndex = task.nodeIndex;
                                                                 indexedSongs[task.nodeIndex].song = std::move(*song);
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
          indexedSongs[nodeIdx].song = std::move(*song);
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
        result.songs.push_back({.song = std::move(indexedSong.song), .treeRelativePath = std::move(indexedSong.treeRelativePath)});
        ++filledCount;
      } else if (indexedSong.filled.load()) {
        result.songs.push_back({.song = std::move(indexedSong.song), .treeRelativePath = std::move(indexedSong.treeRelativePath)});
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

    for (auto& publishedSong : result.songs) {
      reconcileLyrics(publishedSong.song, config.scanner, result.errors, skipped);
    }
    
    const auto directoryHash = hashDirectoryMerkle(rootPath);
    phase4End = std::chrono::steady_clock::now();
    
    // V3 Cache: No need for saveRoot, data is already saved via upsertContent/upsertLocation
    phase5End = std::chrono::steady_clock::now();

    const auto phase1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase1End - phaseStart).count();
    const auto phase2Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase2End - phase1End).count();
    const auto phase3Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase3End - phase2End).count();
    const auto phase4Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase4End - phase3End).count();
    const auto phase5Ms = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phase4End).count();
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(phase5End - phaseStart).count();

    spdlog::info("reconcileRoot phase timing for {}: total={}ms | discovery={}ms | task-prep={}ms | worker-wait={}ms | final-hash={}ms | cache-save={}ms",
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
      cache::SQLiteCacheV3 cache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath_)}};
      cache.updateScanRoot(scanRootRecord(rootPath, decision, songs.size(), scanDuration));
      std::vector<std::string> retainedLocationIds;
      retainedLocationIds.reserve(songs.size());
      for (const auto& publishedSong : songs) {
        cache.upsertContent(publishedSong.song.metadata.contentHash, publishedSong.song.metadata);
        const auto location = cachedLocationFromSong(publishedSong.song, rootPath, publishedSong.song.metadata.filePath);
        retainedLocationIds.push_back(location.locationId);
        cache.upsertLocation(location);
        if (!publishedSong.song.embeddedLyrics.empty()) {
          cache.replaceLyrics(location.locationId, "embedded", publishedSong.song.embeddedLyrics);
        }
        if (!publishedSong.song.externalLyrics.empty()) {
          cache.replaceLyrics(location.locationId, "external", publishedSong.song.externalLyrics);
        }
      }
      cache.pruneDeletedLocations(rootPath, retainedLocationIds);
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
                                            const cache::SQLiteCacheV3& v3cache,
                                            const std::vector<AudioReconcileTask>& audioTasks,
                                            const std::unordered_map<std::string, std::size_t>& audioTaskIndexByPath,
                                            const std::shared_ptr<WorkerSongStore>& workerSongs) {
    spdlog::trace("readWorkerSong called: nodeIndex={}, filePath={}, hasCachedLocation={}", 
                 task.nodeIndex, task.filePath.generic_string(), task.cachedLocation.has_value());
    const auto audioTask = audioTaskByPath(audioTasks, audioTaskIndexByPath, task.filePath);
    if (audioTask == nullptr) {
      throw std::runtime_error{"missing scanner worker task context"};
    }

    if (task.cachedLocation.has_value() && task.locationId == task.cachedLocation->locationId) {
      spdlog::trace("readWorkerSong cache-hit path: trying to load location and content for locationId={}", task.locationId);
      const auto cachedLocation = v3cache.loadLocation(task.locationId);
      if (cachedLocation.has_value()) {
        const auto cachedSong = v3cache.loadContent(cachedLocation->contentId);
        if (cachedSong.has_value()) {
          spdlog::trace("readWorkerSong cache-hit path: successfully loaded content, will call workerSongs->put()");
        } else {
          spdlog::warn("readWorkerSong cache-hit path: loadContent returned nullopt for contentId={}", cachedLocation->contentId);
        }
        if (cachedSong.has_value()) {
          auto song = *cachedSong;
          song.embeddedLyrics = v3cache.loadLyrics(cachedLocation->locationId, "embedded");
          song.externalLyrics = v3cache.loadLyrics(cachedLocation->locationId, "external");
          applyCachedLocation(song, *cachedLocation, task.filePath);
          auto metadata = song.metadata;
          workerSongs->put(task.filePath, std::move(song));
          return metadata;
        }
      }
    }

    auto raw = metadataReader_->read(task.filePath, coverExportDir_);
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
    workerSongs->put(task.filePath, std::move(song));
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

  void reconcileLyrics(cache::CachedSong& song, const ScannerConfig& config,
                      std::vector<ScannerError>& errors, std::uint64_t& skipped) {
    const auto sidecar = expectedLyricsSidecarPath(song.metadata.filePath);
    if (!config.readExternalLyrics || !std::filesystem::is_regular_file(sidecar)) {
      song.externalLyrics.clear();
      selectEffectiveLyrics(song);
      return;
    }
    const auto lrcHash = hashLyricsSidecar(sidecar, HashOptions{.cancellationRequested = &cancellationRequested_});
    for (const auto& error : lrcHash.errors) {
      errors.push_back(scannerErrorFrom(error));
    }
    const auto relativeSidecar = relativePathFor(song.metadata.filePath.parent_path(), sidecar);
    
    // V3: Skip cache check for external lyrics, always reparse
    const auto parsed = parseLrcFile(sidecar);
    for (const auto& error : parsed.errors) {
      errors.push_back(scannerErrorFrom(error));
    }
    if (!parsed.errors.empty()) {
      song.externalLyrics.clear();
      selectEffectiveLyrics(song);
      return;
    }
    song.externalLyrics = parsed.lines;
    song.metadata.externalLyricsPath = relativeSidecar;
    song.metadata.externalLyricsHash = lrcHash.hash;
    song.metadata.externalLyricsMtime = fileMtime(sidecar);
    selectEffectiveLyrics(song);
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

void setTestCueSheetProvider(TestCueSheetProvider provider) {
  g_testCueSheetProvider = std::move(provider);
}

void clearTestCueSheetProvider() {
  g_testCueSheetProvider = nullptr;
}

}
