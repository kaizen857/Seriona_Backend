#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_scanner_cache.h"
#include "seriona/scanner/hash_utils.h"
#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"
#include "seriona/scanner/playlist_tree_builder.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
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

class OrchestratedFileScannerService final : public FileScannerService {
public:
  explicit OrchestratedFileScannerService(FileScannerServiceDependencies dependencies)
      : metadataReader_(std::move(dependencies.metadataReader)), databasePath_(std::move(dependencies.databasePath)),
        coverExportDir_(std::move(dependencies.coverExportDir)) {
    if (!metadataReader_) {
      metadataReader_ = std::make_shared<ProductionTagMetadataReader>();
    }
    if (databasePath_.empty()) {
      databasePath_ = defaultDatabasePath();
    }
    if (coverExportDir_.empty()) {
      coverExportDir_ = defaultCoverExportDir();
    }
  }

  void setEventSink(ScannerEventSink sink) override {
    std::scoped_lock lock{mutex_};
    sink_ = std::move(sink);
  }

  void configure(const ScannerConfig& config) override {
    std::scoped_lock lock{mutex_};
    config_ = config;
  }

  void scan(const std::vector<ScannerRoot>& roots, ScanMode) override {
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

  ScannerEventSink sink_{};
  ScannerConfig config_{};
  std::shared_ptr<TagMetadataReader> metadataReader_;
  std::filesystem::path databasePath_;
  std::filesystem::path coverExportDir_;
  PlaylistTreeSnapshot snapshot_{};
  mutable std::mutex mutex_;
  std::atomic_bool cancellationRequested_{false};
  std::atomic_uint64_t eventVersion_{0};
};

}

std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies) {
  return std::make_shared<OrchestratedFileScannerService>(std::move(dependencies));
}

}
