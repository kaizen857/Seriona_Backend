#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_scanner_cache.h"
#include "seriona/scanner/hash_utils.h"
#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"
#include "seriona/scanner/playlist_tree_builder.h"

#include "wtr/watcher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ranges>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::filesystem::path defaultDatabasePath() {
  return std::filesystem::temp_directory_path() / "seriona" / "scanner-cache.sqlite";
}

[[nodiscard]] std::filesystem::path defaultCoverExportDir() {
  return std::filesystem::temp_directory_path() / "seriona" / "scanner-covers";
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

[[nodiscard]] std::optional<cache::CachedSong> cachedSongByPath(const cache::CachedRoot* root,
                                                                const std::filesystem::path& path) {
  if (root == nullptr) {
    return std::nullopt;
  }
  const auto key = pathKey(path);
  const auto iterator = std::ranges::find_if(root->songs, [&key](const cache::CachedSong& song) {
    return pathKey(song.metadata.filePath) == key;
  });
  if (iterator == root->songs.end()) {
    return std::nullopt;
  }
  return *iterator;
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

  void runScan(const std::vector<ScannerRoot>& roots, ScanMode) {
    std::lock_guard scanLock{scanMutex_};
    ScannerConfig config;
    ScannerEventSink sink;
    {
      std::scoped_lock lock{mutex_};
      config = config_;
      sink = sink_;
    }
    const auto scanVersion = ++eventVersion_;
    publishEvent(sink, ScannerEventType::ScanStarted, scanVersion, ScanProgress{});
    if (cancellationRequested_.exchange(false)) {
      publishCancelled(sink, scanVersion);
      return;
    }

    cache::SQLiteScannerCache cache{cache::ScannerCacheConfig{.databasePath = databasePath_}};
    std::vector<RootResult::PublishedSong> allSongs;
    std::vector<ScannerError> allErrors;
    std::uint64_t discovered = 0;
    std::uint64_t skipped = 0;
    std::uint64_t scanned = 0;

    for (const auto& root : roots) {
      if (cancellationRequested_.load()) {
        publishCancelled(sink, scanVersion);
        return;
      }
      auto rootResult = reconcileRoot(root, config, cache, discovered, skipped, scanned);
      allErrors.insert(allErrors.end(), rootResult.errors.begin(), rootResult.errors.end());
      allSongs.insert(allSongs.end(), rootResult.songs.begin(), rootResult.songs.end());
      for (const auto& error : rootResult.errors) {
        publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
      }
      for (const auto& publishedSong : rootResult.songs) {
        publishEvent(sink, ScannerEventType::FileScanned, ++eventVersion_, publishedSong.song.metadata);
      }
    }

    PlaylistTreeBuilder builder{"Library"};
    for (const auto& publishedSong : allSongs) {
      builder.addSong({.relativePath = publishedSong.treeRelativePath, .metadata = publishedSong.song.metadata});
    }
    auto published = builder.publish();
    {
      std::scoped_lock lock{mutex_};
      snapshot_ = published;
    }
    ScanProgress progress{};
    progress.filesDiscovered = discovered;
    progress.filesScanned = scanned;
    progress.filesSkipped = skipped;
    progress.errors = allErrors.size();
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

  void publishCancelled(const ScannerEventSink& sink, std::uint64_t scanVersion) {
    ScannerError error{};
    error.code = ScannerErrorCode::Cancelled;
    error.message = "scanner scan cancelled";
    publishEvent(sink, ScannerEventType::ScanError, ++eventVersion_, error);
    publishEvent(sink, ScannerEventType::ScanStopped, scanVersion, error);
  }

  [[nodiscard]] RootResult reconcileRoot(const ScannerRoot& root, const ScannerConfig& config, cache::SQLiteScannerCache& cache,
                                         std::uint64_t& discovered, std::uint64_t& skipped, std::uint64_t& scanned) {
    RootResult result;
    const auto rootPath = rootPathFor(root);
    const auto cachedRoot = cache.loadRoot(rootPath);
    const auto cachedRootPtr = cachedRoot.has_value() ? &*cachedRoot : nullptr;
    const auto pathConfig = PathClassificationConfig{.allowedExtensions = config.allowedExtensions,
                                                     .followSymlinks = config.followSymlinks,
                                                     .readExternalLyrics = config.readExternalLyrics};
    const auto entries = discoverScannerPaths(ScannerRoot{.path = rootPath, .recursive = root.recursive}, pathConfig);
    cache::CachedRoot updated{};
    updated.rootPath = rootPath;

    for (const auto& entry : entries) {
      for (const auto& error : entry.errors) {
        result.errors.push_back(scannerErrorFrom(error));
      }
      if (entry.kind != PathEntryKind::AudioCandidate && entry.kind != PathEntryKind::SingleFileRoot) {
        continue;
      }
      ++discovered;
      auto song = reconcileAudio(entry.path, cachedRootPtr, result.errors, skipped, scanned);
      if (song.has_value()) {
        result.songs.push_back({.song = std::move(*song), .treeRelativePath = relativePathFor(rootPath, entry.path)});
      }
    }

    for (auto& publishedSong : result.songs) {
      reconcileLyrics(publishedSong.song, config, cachedRootPtr, result.errors, skipped);
      updated.songs.push_back(publishedSong.song);
    }
    for (const auto& error : result.errors) {
      updated.errors.push_back(error);
    }
    const auto directoryHash = hashDirectoryMerkle(rootPath);
    if (directoryHash.hash.has_value()) {
      updated.directoryHash = *directoryHash.hash;
    }
    for (const auto& error : directoryHash.errors) {
      updated.errors.push_back(scannerErrorFrom(error));
    }
    cache.saveRoot(updated);
    return result;
  }

  [[nodiscard]] std::optional<cache::CachedSong> reconcileAudio(const std::filesystem::path& audioPath,
                                                               const cache::CachedRoot* cachedRoot,
                                                               std::vector<ScannerError>& errors,
                                                               std::uint64_t& skipped,
                                                               std::uint64_t& scanned) {
    const auto hash = hashFileContent(audioPath, HashOptions{.cancellationRequested = &cancellationRequested_});
    for (const auto& error : hash.errors) {
      errors.push_back(scannerErrorFrom(error));
    }
    const auto cachedSong = cachedSongByPath(cachedRoot, audioPath);
    if (hash.hash.has_value() && cachedSong.has_value() && cachedSong->metadata.contentHash == *hash.hash) {
      ++skipped;
      return cachedSong;
    }
    ++scanned;
    try {
      auto raw = metadataReader_->read(audioPath, coverExportDir_);
      raw.filePath = audioPath;
      auto mapped = mapRawTagMetadata(raw, hash.hash.value_or({}), cachedSong.transform([](const cache::CachedSong& song) {
                                      return song.userStats;
                                    }), false)
                        .cachedSong;
      mapped.metadata.filePath = audioPath;
      mapped.metadata.sourceFilePath = audioPath;
      mapped.metadata.trackId = audioPath.generic_string();
      mapped.metadata.logicalTrackId = audioPath.generic_string();
      if (hash.hash.has_value()) {
        mapped.metadata.contentHash = *hash.hash;
      }
      return mapped;
    } catch (const std::exception& error) {
      errors.push_back({.code = ScannerErrorCode::MetadataReadFailed,
                        .message = "TagReader metadata read failed",
                        .detail = error.what(),
                        .path = audioPath});
      return cachedSong;
    }
  }

  void reconcileLyrics(cache::CachedSong& song, const ScannerConfig& config, const cache::CachedRoot* cachedRoot,
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
    const auto cachedSong = cachedSongByPath(cachedRoot, song.metadata.filePath);
    const auto relativeSidecar = relativePathFor(song.metadata.filePath.parent_path(), sidecar);
    if (lrcHash.hash.has_value() && cachedSong.has_value() && cachedSong->metadata.externalLyricsHash == *lrcHash.hash) {
      ++skipped;
      song.externalLyrics = cachedSong->externalLyrics;
      song.metadata.externalLyricsPath = cachedSong->metadata.externalLyricsPath;
      song.metadata.externalLyricsHash = cachedSong->metadata.externalLyricsHash;
      song.metadata.externalLyricsMtime = cachedSong->metadata.externalLyricsMtime;
      selectEffectiveLyrics(song);
      return;
    }
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
  std::thread scanWorker_{[this] { scanWorkerLoop(); }};
  std::thread debounceThread_;
  std::atomic_bool cancellationRequested_{false};
  std::atomic_uint64_t eventVersion_{0};
  bool scanWorkerStopping_{false};
};

}

std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies) {
  return std::make_shared<OrchestratedFileScannerService>(std::move(dependencies));
}

}
