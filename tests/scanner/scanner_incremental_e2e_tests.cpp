#include "scanner_test_harness.h"

#include "file_scanner_orchestrator_test_access.h"
#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/playlist_tree_builder.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

class FakeMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    {
      std::lock_guard lock{mutex_};
      requestedPaths.push_back(request.path);
      requestedCoverDirs.push_back(request.coverExportDir);
    }
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata");
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;
  

  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

  [[nodiscard]] std::size_t readCount() const {
    std::lock_guard lock{mutex_};
    return requestedPaths.size();
  }

  std::vector<std::filesystem::path> requestedPaths;
  std::vector<std::filesystem::path> requestedCoverDirs;

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
  mutable std::mutex mutex_;
};

class FakeFolderThumbnailSeam {
public:
  void setResult(std::filesystem::path folder, std::optional<std::filesystem::path> result) {
    std::lock_guard lock{mutex_};
    results_[std::move(folder)] = std::move(result);
  }

  [[nodiscard]] FolderThumbnailExportSeam makeSeam() {
    return [this](const std::filesystem::path& folder) -> std::optional<std::filesystem::path> {
      std::lock_guard lock{mutex_};
      calledFolders.push_back(folder);
      const auto iterator = results_.find(folder);
      return iterator == results_.end() ? std::nullopt : iterator->second;
    };
  }

  [[nodiscard]] std::vector<std::filesystem::path> called() const {
    std::lock_guard lock{mutex_};
    return calledFolders;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::filesystem::path> calledFolders;
  std::map<std::filesystem::path, std::optional<std::filesystem::path>> results_;
};

class ScannerEventLog {
public:
  void push(ScannerEvent event) {
    std::lock_guard lock{mutex_};
    events_.push_back(std::move(event));
  }

  [[nodiscard]] std::size_t scanCompletedCount() const {
    std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count(events_, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }

  [[nodiscard]] std::vector<SongMetadata> fileScannedSongs() const {
    std::lock_guard lock{mutex_};
    std::vector<SongMetadata> songs;
    for (const auto& event : events_) {
      if (event.type == ScannerEventType::FileScanned && std::holds_alternative<SongMetadata>(event.payload)) {
        songs.push_back(std::get<SongMetadata>(event.payload));
      }
    }
    return songs;
  }

  [[nodiscard]] std::vector<ScanProgress> progressEvents() const {
    std::lock_guard lock{mutex_};
    std::vector<ScanProgress> progresses;
    for (const auto& event : events_) {
      if (event.type == ScannerEventType::ProgressUpdated && std::holds_alternative<ScanProgress>(event.payload)) {
        progresses.push_back(std::get<ScanProgress>(event.payload));
      }
    }
    return progresses;
  }

private:
  mutable std::mutex mutex_;
  std::vector<ScannerEvent> events_;
};

class TestCueProviderGuard {
public:
  explicit TestCueProviderGuard(TestCueSheetProvider provider) { setTestCueSheetProvider(std::move(provider)); }
  ~TestCueProviderGuard() { clearTestCueSheetProvider(); }

  TestCueProviderGuard(const TestCueProviderGuard&) = delete;
  TestCueProviderGuard& operator=(const TestCueProviderGuard&) = delete;
};

class WorkerTaskObserverGuard {
public:
  explicit WorkerTaskObserverGuard(WorkerTaskObserver observer) { setWorkerTaskObserver(std::move(observer)); }
  ~WorkerTaskObserverGuard() { clearWorkerTaskObserver(); }

  WorkerTaskObserverGuard(const WorkerTaskObserverGuard&) = delete;
  WorkerTaskObserverGuard& operator=(const WorkerTaskObserverGuard&) = delete;
};

class PublishedSongObserverGuard {
public:
  explicit PublishedSongObserverGuard(PublishedSongObserver observer) { setPublishedSongObserver(std::move(observer)); }
  ~PublishedSongObserverGuard() { clearPublishedSongObserver(); }

  PublishedSongObserverGuard(const PublishedSongObserverGuard&) = delete;
  PublishedSongObserverGuard& operator=(const PublishedSongObserverGuard&) = delete;
};

class CacheWriteObserverGuard {
public:
  explicit CacheWriteObserverGuard(CacheWriteObserver observer) { setCacheWriteObserver(std::move(observer)); }
  ~CacheWriteObserverGuard() { clearCacheWriteObserver(); }

  CacheWriteObserverGuard(const CacheWriteObserverGuard&) = delete;
  CacheWriteObserverGuard& operator=(const CacheWriteObserverGuard&) = delete;
};

class TreeBuilderObserverGuard {
public:
  explicit TreeBuilderObserverGuard(TreeBuilderObserver observer) { setTreeBuilderObserver(std::move(observer)); }
  ~TreeBuilderObserverGuard() { clearTreeBuilderObserver(); }

  TreeBuilderObserverGuard(const TreeBuilderObserverGuard&) = delete;
  TreeBuilderObserverGuard& operator=(const TreeBuilderObserverGuard&) = delete;
};

struct TimedSnapshot {
  PlaylistTreeSnapshot snapshot;
  std::chrono::milliseconds elapsed{0};
};

[[nodiscard]] RawTagMetadata rawMetadata(std::string title) {
  RawTagMetadata raw{};
  raw.title = std::move(title);
  raw.artist = "Artist";
  raw.album = "Album";
  raw.duration = std::chrono::milliseconds{120000};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.channels = 2;
  return raw;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeService(test::TempScannerRoot& temp,
                                                              std::shared_ptr<FakeMetadataReader> reader) {
  return makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                               .watcherFactory = nullptr,
                                                               .databasePath = temp.dbPath(),
                                                               .coverExportDir = temp.path() / "covers"});
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeServiceWithSeam(test::TempScannerRoot& temp,
                                                                      std::shared_ptr<FakeMetadataReader> reader,
                                                                      FolderThumbnailExportSeam seam) {
  return makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                               .watcherFactory = nullptr,
                                                               .databasePath = temp.dbPath(),
                                                               .coverExportDir = temp.path() / "covers",
                                                               .folderThumbnailSeam = std::move(seam)});
}

[[nodiscard]] std::filesystem::path scannerSidecarPath(const test::TempScannerRoot& temp) {
  return std::filesystem::path{temp.dbPath().generic_string() + ".scan-roots.sqlite"};
}

[[nodiscard]] std::int64_t filesystemMtimeNs(const std::filesystem::path& path) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::filesystem::last_write_time(path).time_since_epoch())
    .count();
}

[[nodiscard]] std::filesystem::path canonicalRootPath(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    canonical = path.lexically_normal();
  }
  return canonical;
}

void forceNextScanIncrementalForCurrentTree(const test::TempScannerRoot& temp) {
  const auto rootPath = canonicalRootPath(temp.path());
  const auto treeHash = computeDirectoryTreeHash(rootPath);
  REQUIRE(treeHash.hash.has_value());
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  auto scanRoot = sidecar.loadScanRoot(rootPath);
  REQUIRE(scanRoot.has_value());
  scanRoot->directoryTreeHash = *treeHash.hash;
  sidecar.updateScanRoot(*scanRoot);
}

[[nodiscard]] std::vector<SongMetadata> songsIn(const PlaylistTreeSnapshot& snapshot) {
  std::vector<SongMetadata> songs;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value()) {
      songs.push_back(*node.song);
    }
  }
  std::ranges::sort(songs, {}, &SongMetadata::filePath);
  return songs;
}

[[nodiscard]] std::vector<PlaylistNode> songNodesByPath(const PlaylistTreeSnapshot& snapshot,
                                                        const std::filesystem::path& path) {
  std::vector<PlaylistNode> nodes;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value() && node.song->filePath == path) {
      nodes.push_back(node);
    }
  }
  std::ranges::sort(nodes, [](const PlaylistNode& left, const PlaylistNode& right) {
    return left.song->offset.value_or(std::chrono::milliseconds{-1}) <
           right.song->offset.value_or(std::chrono::milliseconds{-1});
  });
  return nodes;
}

[[nodiscard]] const PlaylistNode* directoryNodeByDisplayName(const PlaylistTreeSnapshot& snapshot,
                                                             const std::string& displayName) {
  for (const auto& node : snapshot.nodes) {
    if (node.kind == PlaylistNodeKind::Directory && node.displayName == displayName) {
      return &node;
    }
  }
  return nullptr;
}

[[nodiscard]] const PlaylistNode* rootNodeOf(const PlaylistTreeSnapshot& snapshot) {
  if (!snapshot.rootNodeId.has_value()) {
    return nullptr;
  }
  for (const auto& node : snapshot.nodes) {
    if (node.nodeId == *snapshot.rootNodeId) {
      return &node;
    }
  }
  return nullptr;
}

[[nodiscard]] PlaylistNode normalizeForTreeDiff(const PlaylistNode& node) {
  auto copy = node;
  copy.thumbnailPath.reset();
  return copy;
}

void sortNodesForTreeDiff(std::vector<PlaylistNode>& nodes) {
  std::ranges::sort(nodes, [](const PlaylistNode& left, const PlaylistNode& right) {
    if (left.kind != right.kind) {
      return left.kind < right.kind;
    }
    return left.nodeId < right.nodeId;
  });
}

[[nodiscard]] bool lyricsEquivalent(const std::vector<LyricLine>& left, const std::vector<LyricLine>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].timestamp != right[index].timestamp || left[index].text != right[index].text) {
      return false;
    }
  }
  return true;
}

// song 元数据等价性：逐字段比较，排除 thumbnailPath（缩略图路径属 orchestrator 层职责）。
[[nodiscard]] bool songMetadataEquivalent(const SongMetadata& left, const SongMetadata& right) {
  return left.trackId == right.trackId && left.filePath == right.filePath && left.title == right.title &&
         left.artist == right.artist && left.album == right.album && left.albumArtist == right.albumArtist &&
         left.genre == right.genre && left.trackNumber == right.trackNumber && left.discNumber == right.discNumber &&
         left.year == right.year && left.sampleRate == right.sampleRate && left.bitDepth == right.bitDepth &&
         left.channels == right.channels && left.fileSizeBytes == right.fileSizeBytes &&
         left.fileMtime == right.fileMtime && left.contentHash == right.contentHash &&
         left.effectiveLyricsSource == right.effectiveLyricsSource &&
         lyricsEquivalent(left.effectiveLyrics, right.effectiveLyrics) &&
         left.externalLyricsPath == right.externalLyricsPath && left.externalLyricsHash == right.externalLyricsHash &&
         left.externalLyricsMtime == right.externalLyricsMtime && left.sourceFilePath == right.sourceFilePath &&
         left.offset == right.offset && left.duration == right.duration && left.logicalTrackId == right.logicalTrackId &&
         left.artworkPath == right.artworkPath;
}

// 树 diff == 全量重建等价性：比较 nodeId/kind/displayName/parentNodeId/childNodeIds/song 元数据，
// 排除 node 级 thumbnailPath（由 orchestrator 层 resolveFolderThumbnails 解析，非 builder 职责）。
[[nodiscard]] bool treesEquivalent(const PlaylistTreeSnapshot& left, const PlaylistTreeSnapshot& right) {
  if (left.nodes.size() != right.nodes.size() || left.rootNodeId != right.rootNodeId) {
    return false;
  }
  auto leftNodes = left.nodes;
  auto rightNodes = right.nodes;
  for (auto& node : leftNodes) {
    node = normalizeForTreeDiff(node);
  }
  for (auto& node : rightNodes) {
    node = normalizeForTreeDiff(node);
  }
  sortNodesForTreeDiff(leftNodes);
  sortNodesForTreeDiff(rightNodes);
  for (std::size_t index = 0; index < leftNodes.size(); ++index) {
    const auto& leftNode = leftNodes[index];
    const auto& rightNode = rightNodes[index];
    if (leftNode.nodeId != rightNode.nodeId || leftNode.kind != rightNode.kind ||
        leftNode.displayName != rightNode.displayName || leftNode.parentNodeId != rightNode.parentNodeId ||
        leftNode.childNodeIds != rightNode.childNodeIds) {
      return false;
    }
    if (leftNode.song.has_value() != rightNode.song.has_value()) {
      return false;
    }
    if (leftNode.song.has_value() && !songMetadataEquivalent(*leftNode.song, *rightNode.song)) {
      return false;
    }
  }
  return true;
}

// 树一致性断言（"nodeId 三处同步"的核心检验）：每个有父引用的节点，其父存在且父的 childNodeIds
// 包含本节点；每个 childNodeIds 里的子节点，其 parentNodeId 精确指向本节点。双向校验杜绝悬空引用。
void requireNoDanglingReferences(const PlaylistTreeSnapshot& snapshot) {
  std::map<std::string, const PlaylistNode*> nodesById;
  for (const auto& node : snapshot.nodes) {
    nodesById[node.nodeId] = &node;
  }
  for (const auto& node : snapshot.nodes) {
    if (node.parentNodeId.has_value()) {
      const auto parent = nodesById.find(*node.parentNodeId);
      REQUIRE(parent != nodesById.end());
      REQUIRE(std::ranges::find(parent->second->childNodeIds, node.nodeId) != parent->second->childNodeIds.end());
    }
    for (const auto& childId : node.childNodeIds) {
      const auto child = nodesById.find(childId);
      REQUIRE(child != nodesById.end());
      REQUIRE(child->second->parentNodeId.has_value());
      CHECK(*child->second->parentNodeId == node.nodeId);
    }
  }
}

// 把歌曲的 relativePath 与 metadata 中的路径派生字段按前缀改写（== oldPrefix 或 oldPrefix+"/" 边界，
// 保留 "/"），供 renameSubtree 的"独立全量重建"等价性测试构造路径改写后的歌曲集。
[[nodiscard]] PlaylistTreeSong rewriteTreeSongPaths(const PlaylistTreeSong& song,
                                                    const std::string& oldPrefix,
                                                    const std::string& newPrefix) {
  const auto rewriteText = [&oldPrefix, &newPrefix](const std::string& text) {
    if (text == oldPrefix) {
      return newPrefix;
    }
    if (text.rfind(oldPrefix + "/", 0) == 0) {
      return newPrefix + text.substr(oldPrefix.size());
    }
    return text;
  };
  PlaylistTreeSong out = song;
  out.relativePath = std::filesystem::path{rewriteText(song.relativePath.generic_string())};
  if (!song.metadata.filePath.empty()) {
    out.metadata.filePath = std::filesystem::path{rewriteText(song.metadata.filePath.generic_string())};
  }
  if (!song.metadata.sourceFilePath.empty()) {
    out.metadata.sourceFilePath = std::filesystem::path{rewriteText(song.metadata.sourceFilePath.generic_string())};
  }
  out.metadata.logicalTrackId = rewriteText(song.metadata.logicalTrackId);
  out.metadata.trackId = rewriteText(song.metadata.trackId);
  return out;
}

// 用快照中 Track 节点的歌曲数据做一次独立的全量重建（物理路径 - root 还原 relativePath）。
[[nodiscard]] PlaylistTreeSnapshot rebuildSnapshotFrom(const PlaylistTreeSnapshot& snapshot,
                                                       const std::filesystem::path& root) {
  PlaylistTreeBuilder builder{"Library"};
  for (const auto& node : snapshot.nodes) {
    if (!node.song.has_value()) {
      continue;
    }
    std::error_code error;
    auto relative = std::filesystem::relative(node.song->filePath, root, error);
    if (error || relative.empty()) {
      relative = node.song->filePath.filename();
    }
    builder.addSong({.relativePath = relative.lexically_normal(), .metadata = *node.song});
  }
  return builder.publish();
}

[[nodiscard]] const SongMetadata& songByPath(const std::vector<SongMetadata>& songs, const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(songs, path, &SongMetadata::filePath);
  if (iterator == songs.end()) {
    throw std::runtime_error("missing song in incremental e2e test");
  }
  return *iterator;
}

[[nodiscard]] std::size_t publishedOriginCount(const std::vector<PublishedSongSnapshot>& songs,
                                               ScanItemOrigin origin) {
  return static_cast<std::size_t>(std::ranges::count(songs, origin, &PublishedSongSnapshot::origin));
}

[[nodiscard]] const PublishedSongSnapshot* publishedByOrigin(const std::vector<PublishedSongSnapshot>& songs,
                                                            ScanItemOrigin origin) {
  const auto iterator = std::ranges::find(songs, origin, &PublishedSongSnapshot::origin);
  return iterator == songs.end() ? nullptr : &*iterator;
}

template <typename Predicate>
[[nodiscard]] TimedSnapshot runScanAndWait(FileScannerService& service,
                                           ScannerEventLog& eventLog,
                                           const std::filesystem::path& root,
                                           ScanMode mode,
                                           Predicate predicate) {
  const auto startedAt = std::chrono::steady_clock::now();
  const auto completedBeforeScan = eventLog.scanCompletedCount();
  service.scan({ScannerRoot{.path = root}}, mode);
  for (auto attempts = 0; attempts < 5000; ++attempts) {
    auto snapshot = service.snapshot();
    if (predicate(snapshot) && eventLog.scanCompletedCount() > completedBeforeScan) {
      return TimedSnapshot{.snapshot = std::move(snapshot),
                           .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt)};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return TimedSnapshot{.snapshot = service.snapshot(),
                       .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt)};
}

TEST_CASE("scanner incremental e2e covers full unchanged changed added and deleted snapshots") {
  test::TempScannerRoot temp{"scanner-incremental-e2e"};
  const auto stable = test::writeAudioFixture(temp.path(), "01-stable.flac");
  const auto changed = test::writeAudioFixture(temp.path(), "02-changed.flac");
  const auto deleted = test::writeAudioFixture(temp.path(), "03-deleted.flac");
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(stable, rawMetadata("Stable"));
  reader->put(changed, rawMetadata("Changed Before"));
  reader->put(deleted, rawMetadata("Deleted"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 3U;
  });
  auto songs = songsIn(full.snapshot);

  REQUIRE(songs.size() == 3U);
  CHECK(reader->readCount() == 3U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed Before");
  CHECK(songByPath(songs, deleted).title == "Deleted");

  writeText(changed, "changed bytes for phase 3 e2e");
  std::this_thread::sleep_for(std::chrono::milliseconds{5}); // mtime granularity guard
  reader->put(changed, rawMetadata("Changed After"));
  const auto added = test::writeAudioFixture(temp.path(), "04-added.flac");
  reader->put(added, rawMetadata("Added"));
  const auto addedAndChanged = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                              [](const PlaylistTreeSnapshot& snapshot) {
                                                return songsIn(snapshot).size() == 4U;
                                              });
  songs = songsIn(addedAndChanged.snapshot);

	  REQUIRE(songs.size() == 4U);
	  CHECK(reader->readCount() == 7U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed After");
  CHECK(songByPath(songs, deleted).title == "Deleted");
  CHECK(songByPath(songs, added).title == "Added");

  std::filesystem::remove(deleted);
  const auto deletion = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                       [](const PlaylistTreeSnapshot& snapshot) {
                                         return songsIn(snapshot).size() == 3U;
                                       });
  songs = songsIn(deletion.snapshot);

	  REQUIRE(songs.size() == 3U);
	  CHECK(reader->readCount() == 10U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed After");
  CHECK(songByPath(songs, added).title == "Added");
  CHECK(std::ranges::none_of(songs, [&deleted](const SongMetadata& song) { return song.filePath == deleted; }));

  const auto fileScannedSongs = eventLog.fileScannedSongs();
  REQUIRE(fileScannedSongs.size() == 10U);
  CHECK(fileScannedSongs[0].filePath == stable);
  CHECK(fileScannedSongs[1].filePath == changed);
  CHECK(fileScannedSongs[2].filePath == deleted);
  CHECK(fileScannedSongs[3].filePath == stable);
  CHECK(fileScannedSongs[4].filePath == changed);
  CHECK(fileScannedSongs[5].filePath == deleted);
  CHECK(fileScannedSongs[6].filePath == added);
  CHECK(fileScannedSongs[7].filePath == stable);
  CHECK(fileScannedSongs[8].filePath == changed);
  CHECK(fileScannedSongs[9].filePath == added);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  CHECK(locations.size() == 3U);
  CHECK(std::ranges::none_of(locations, [&deleted](const cache::CachedLocation& location) { return location.filePath == deleted; }));

  std::cout << "phase3_incremental_e2e_observations "
            << "full_ms=" << full.elapsed.count() << ' '
            << "added_changed_ms=" << addedAndChanged.elapsed.count() << ' '
            << "deleted_ms=" << deletion.elapsed.count() << ' '
            << "reader_reads=" << reader->readCount() << ' '
            << "final_playlist_songs=" << songs.size() << '\n';
}

TEST_CASE("scanner incremental e2e emits no cache-hit FileScanned events and reports skipped progress") {
  test::TempScannerRoot temp{"scanner-incremental-cache-hit-events"};
  const auto audio = test::writeAudioFixture(temp.path(), "stable.flac");
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(audio, rawMetadata("Stable"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 1U;
  });
  auto songs = songsIn(full.snapshot);

  REQUIRE(songs.size() == 1U);
  CHECK(reader->readCount() == 1U);

  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                          [](const PlaylistTreeSnapshot& snapshot) {
                                            return songsIn(snapshot).size() == 1U;
                                          });
  songs = songsIn(incremental.snapshot);

  REQUIRE(songs.size() == 1U);
  CHECK(reader->readCount() == 1U);
  const auto fileScannedSongs = eventLog.fileScannedSongs();
  REQUIRE(fileScannedSongs.size() == 1U);
  CHECK(fileScannedSongs[0].filePath == audio);
  const auto progressEvents = eventLog.progressEvents();
  REQUIRE(progressEvents.size() >= 2U);
  CHECK(progressEvents.back().filesDiscovered == 1U);
  CHECK(progressEvents.back().filesScanned == 0U);
  CHECK(progressEvents.back().filesSkipped == 1U);
  CHECK(progressEvents.back().filesScanned + progressEvents.back().filesSkipped == progressEvents.back().filesDiscovered);
}

TEST_CASE("scanner incremental e2e relies on scan-root directory tree hash before cache hit") {
  test::TempScannerRoot temp{"scanner-incremental-scan-root-hash"};
  const auto audio = test::writeAudioFixture(temp.path(), "stable.flac");
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(audio, rawMetadata("Stable Before Stale Hash"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 1U;
  });
  auto songs = songsIn(full.snapshot);

  REQUIRE(songs.size() == 1U);
  CHECK(songByPath(songs, audio).title == "Stable Before Stale Hash");
  CHECK(reader->readCount() == 1U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  auto scanRoot = sidecar.loadScanRoot(rootPath);
  REQUIRE(scanRoot.has_value());
  CHECK_FALSE(scanRoot->directoryTreeHash.empty());
  scanRoot->directoryTreeHash = "stale-directory-tree-hash";
  sidecar.updateScanRoot(*scanRoot);

  reader->put(audio, rawMetadata("Stable After Stale Hash"));
  const auto fallbackFull = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                           [](const PlaylistTreeSnapshot& snapshot) {
                                             return songsIn(snapshot).size() == 1U;
                                           });
  songs = songsIn(fallbackFull.snapshot);

  REQUIRE(songs.size() == 1U);
  const auto progressEvents = eventLog.progressEvents();
  REQUIRE(progressEvents.size() >= 2U);
  CHECK(progressEvents.back().filesScanned == 1U);
  CHECK(progressEvents.back().filesSkipped == 0U);
  const auto currentTreeHash = computeDirectoryTreeHash(rootPath);
  REQUIRE(currentTreeHash.hash.has_value());
  const auto refreshedScanRoot = sidecar.loadScanRoot(rootPath);
  REQUIRE(refreshedScanRoot.has_value());
  CHECK(refreshedScanRoot->directoryTreeHash == *currentTreeHash.hash);
}

TEST_CASE("scanner incremental e2e persists cache after cue container pseudo nodes") {
  test::TempScannerRoot temp{"scanner-incremental-cue-cache"};
  const auto cueFile = temp.path() / "album.cue";
  const auto referencedAudio = test::writeAudioFixture(temp.path(), "album.flac");
  const auto standalone = test::writeAudioFixture(temp.path(), "bonus.flac");
  writeText(cueFile, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n  TRACK 02 AUDIO\n");
  const auto fixtureNow = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(cueFile, fixtureNow - std::chrono::hours{2});
  std::filesystem::last_write_time(referencedAudio, fixtureNow - std::chrono::hours{1});

  std::atomic_size_t cueReadCount{0};
  const TestCueProviderGuard cueProvider{[&referencedAudio, &cueReadCount](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    cueReadCount.fetch_add(1U, std::memory_order_relaxed);
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 180000000,
             .title = "Cue Track 1",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1},
            {.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 200000000,
             .title = "Cue Track 2",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 2}};
  }};

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, rawMetadata("Bare Album File"));
  reader->put(standalone, rawMetadata("Bonus Track"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 3U;
  });
  const auto fullReadCount = reader->readCount();
  const auto fullCueReadCount = cueReadCount.load(std::memory_order_relaxed);

  auto songs = songsIn(full.snapshot);
  auto fullCueNodes = songNodesByPath(full.snapshot, cueFile);
  REQUIRE(songs.size() == 3U);
  REQUIRE(fullCueNodes.size() == 2U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(songByPath(songs, standalone).title == "Bonus Track");
  std::vector<SongMetadata> cueTracks;
  for (const auto& song : songs) {
    if (song.filePath == cueFile) {
      cueTracks.push_back(song);
    }
  }
  std::ranges::sort(cueTracks, {}, &SongMetadata::logicalTrackId);
  REQUIRE(cueTracks.size() == 2U);
  CHECK(cueTracks[0].sourceFilePath == referencedAudio);
  CHECK(cueTracks[0].logicalTrackId == cueFile.generic_string() + "#track0");
  CHECK(cueTracks[0].trackId == cueTracks[0].logicalTrackId);
  CHECK(cueTracks[0].offset == std::chrono::milliseconds{0});
  CHECK(cueTracks[0].duration == std::chrono::milliseconds{180000});
  CHECK(cueTracks[1].sourceFilePath == referencedAudio);
  CHECK(cueTracks[1].logicalTrackId == cueFile.generic_string() + "#track1");
  CHECK(cueTracks[1].trackId == cueTracks[1].logicalTrackId);
  CHECK(cueTracks[1].offset == std::chrono::milliseconds{0});
  CHECK(cueTracks[1].duration == std::chrono::milliseconds{200000});
  CHECK(cueTracks[0].offset == cueTracks[1].offset);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  auto locations = sidecar.loadLocationsByRoot(rootPath);
  CHECK(std::ranges::count(locations, cueFile, &cache::CachedLocation::filePath) == 2U);
  CHECK(std::ranges::any_of(locations, [&standalone](const cache::CachedLocation& location) {
    return location.filePath == standalone;
  }));
  std::vector<cache::CachedLocation> cueLocations;
  for (const auto& location : locations) {
    if (location.filePath == cueFile) {
      cueLocations.push_back(location);
    }
  }
  REQUIRE(cueLocations.size() == 2U);
  REQUIRE(std::ranges::all_of(cueLocations, [](const cache::CachedLocation& location) {
    return location.cueTrackIndex.has_value();
  }));
  std::ranges::sort(cueLocations, [](const cache::CachedLocation& left, const cache::CachedLocation& right) {
    return *left.cueTrackIndex < *right.cueTrackIndex;
  });
  REQUIRE(cueLocations[0].cueTrackOffset.has_value());
  REQUIRE(cueLocations[1].cueTrackOffset.has_value());
  CHECK(cueLocations[0].cueTrackOffset == cueLocations[1].cueTrackOffset);
  CHECK(cueLocations[0].locationId != cueLocations[1].locationId);
  const auto cueFileSize = std::filesystem::file_size(cueFile);
  const auto sourceFileSize = std::filesystem::file_size(referencedAudio);
  REQUIRE(cueFileSize != sourceFileSize);
  const auto cueFileMtimeNs = filesystemMtimeNs(cueFile);
  const auto sourceFileMtimeNs = filesystemMtimeNs(referencedAudio);
  REQUIRE(cueFileMtimeNs != sourceFileMtimeNs);
  for (std::size_t index = 0; index < cueLocations.size(); ++index) {
    auto location = cueLocations[index];
    CHECK(location.sourceFilePath == referencedAudio);
    CHECK(location.fileSizeBytes == cueFileSize);
    CHECK(location.fileMtimeNs == cueFileMtimeNs);
    REQUIRE(location.cueTrackIndex.has_value());
    CHECK(*location.cueTrackIndex == index);
    REQUIRE(location.cueTrackOffset.has_value());
    CHECK(*location.cueTrackOffset == std::chrono::milliseconds{0});
    REQUIRE(location.cueFileSizeBytes.has_value());
    CHECK(*location.cueFileSizeBytes == cueFileSize);
    REQUIRE(location.cueFileMtimeNs.has_value());
    CHECK(*location.cueFileMtimeNs == cueFileMtimeNs);
    REQUIRE(location.sourceFileSizeBytes.has_value());
    CHECK(*location.sourceFileSizeBytes == sourceFileSize);
    REQUIRE(location.sourceFileMtimeNs.has_value());
    CHECK(*location.sourceFileMtimeNs == sourceFileMtimeNs);

    const auto artwork = temp.dbPath("art-" + std::to_string(index) + ".jpg");
    const auto thumbnail = temp.dbPath("thumb-" + std::to_string(index) + ".jpg");
    writeText(artwork, "artwork");
    writeText(thumbnail, "thumbnail");
    location.artworkPath = artwork;
    location.thumbnailPath = thumbnail;
    location.lyricsSource = LyricsSource::EmbeddedTag;
    sidecar.upsertLocation(location);
 	    sidecar.replaceLyrics(location.locationId,
	                          "embedded",
	                          {LyricLine{.timestamp = std::chrono::milliseconds{static_cast<std::int64_t>(index) * 1000},
	                                     .text = "cached embedded lyric " + std::to_string(index)}});
		  }

          const auto fileScannedCountBeforeCacheHit = eventLog.fileScannedSongs().size();
	
			  std::vector<WorkerTaskSnapshot> cacheHitWorkerTasks;
		  std::vector<PublishedSongSnapshot> cacheHitPublishedSongs;
		  std::vector<cache::ScanRootCacheWrite> cacheHitWrites;
		  const WorkerTaskObserverGuard workerObserver{[&cacheHitWorkerTasks](const std::vector<WorkerTaskSnapshot>& tasks) {
		    cacheHitWorkerTasks = tasks;
		  }};
		  const PublishedSongObserverGuard publishedObserver{[&cacheHitPublishedSongs](const std::vector<PublishedSongSnapshot>& songs) {
		    cacheHitPublishedSongs = songs;
		  }};
		  const CacheWriteObserverGuard cacheWriteObserver{[&cacheHitWrites](const cache::ScanRootCacheWrite& write) {
		    cacheHitWrites.push_back(write);
		  }};

	  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental, [](const PlaylistTreeSnapshot& snapshot) {
	    return songsIn(snapshot).size() == 3U;
	  });

  songs = songsIn(incremental.snapshot);
  const auto incrementalCueNodes = songNodesByPath(incremental.snapshot, cueFile);
  REQUIRE(songs.size() == 3U);
  REQUIRE(incrementalCueNodes.size() == 2U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(std::ranges::none_of(songs, [&referencedAudio](const SongMetadata& song) {
    return song.filePath == referencedAudio;
  }));
	  CHECK(songByPath(songs, standalone).title == "Bonus Track");
	  CHECK(reader->readCount() == fullReadCount);
	  CHECK(cueReadCount.load(std::memory_order_relaxed) == fullCueReadCount);
	  CHECK(cacheHitWorkerTasks.empty());
		  CHECK(publishedOriginCount(cacheHitPublishedSongs, ScanItemOrigin::VirtualContainer) == 1U);
		  CHECK(publishedOriginCount(cacheHitPublishedSongs, ScanItemOrigin::CueTrackCacheHit) == 2U);
		  CHECK(publishedOriginCount(cacheHitPublishedSongs, ScanItemOrigin::CacheHit) == 1U);
		  REQUIRE(cacheHitWrites.size() == 1U);
		  CHECK(cacheHitWrites.back().root.totalFiles == cacheHitPublishedSongs.size());
		  CHECK(cacheHitWrites.back().changedSongs.empty());
			  CHECK(cacheHitWrites.back().changedCueTracks.empty());
			  CHECK(cacheHitWrites.back().lyricsUpdates.empty());
			  CHECK(cacheHitWrites.back().retainedLocationIds.size() == 3U);
			  CHECK(eventLog.fileScannedSongs().size() == fileScannedCountBeforeCacheHit);
			  for (const auto& publishedSong : cacheHitPublishedSongs) {
		    if (publishedSong.origin == ScanItemOrigin::CacheHit || publishedSong.origin == ScanItemOrigin::CueTrackCacheHit) {
		      REQUIRE(publishedSong.locationId.has_value());
	      CHECK_FALSE(publishedSong.locationId->empty());
	    }
	  }
	  for (std::size_t index = 0; index < incrementalCueNodes.size(); ++index) {
    const auto& fullNode = fullCueNodes[index];
    const auto& cachedNode = incrementalCueNodes[index];
    REQUIRE(fullNode.song.has_value());
    REQUIRE(cachedNode.song.has_value());
    CHECK(cachedNode.nodeId == fullNode.nodeId);
    CHECK(cachedNode.parentNodeId == fullNode.parentNodeId);
    CHECK(cachedNode.song->trackId == fullNode.song->trackId);
    CHECK(cachedNode.song->logicalTrackId == fullNode.song->logicalTrackId);
    CHECK(cachedNode.song->filePath == cueFile);
    CHECK(cachedNode.song->sourceFilePath == referencedAudio);
    CHECK(cachedNode.song->offset == fullNode.song->offset);
    CHECK(cachedNode.song->duration == fullNode.song->duration);
    CHECK(cachedNode.song->artworkPath == temp.dbPath("art-" + std::to_string(index) + ".jpg"));
    CHECK(cachedNode.song->thumbnailPath == temp.dbPath("thumb-" + std::to_string(index) + ".jpg"));
    CHECK(cachedNode.song->effectiveLyricsSource == LyricsSource::EmbeddedTag);
    REQUIRE(cachedNode.song->effectiveLyrics.size() == 1U);
	    CHECK(cachedNode.song->effectiveLyrics[0].text == "cached embedded lyric " + std::to_string(index));
	  }
}

TEST_CASE("scanner incremental e2e marks newly added cue reader tracks as cue scanned new") {
  test::TempScannerRoot temp{"scanner-incremental-cue-new-origin"};
  const auto standalone = test::writeAudioFixture(temp.path(), "standalone.flac");

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(standalone, rawMetadata("Standalone Before Cue"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 1U;
  });
  auto songs = songsIn(full.snapshot);
  REQUIRE(songs.size() == 1U);
  CHECK(songByPath(songs, standalone).title == "Standalone Before Cue");

  const auto cueFile = temp.path() / "new-album.cue";
  const auto referencedAudio = test::writeAudioFixture(temp.path(), "new-album.flac");
  writeText(cueFile, "FILE \"new-album.flac\" WAVE\n  TRACK 01 AUDIO\n");
  forceNextScanIncrementalForCurrentTree(temp);
  const TestCueProviderGuard cueProvider{[&referencedAudio](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "new-album.cue") {
      return {};
    }
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 180000000,
             .title = "New Cue Track",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1}};
  }};

	  std::vector<WorkerTaskSnapshot> newCueWorkerTasks;
	  std::vector<PublishedSongSnapshot> newCuePublishedSongs;
	  std::vector<cache::ScanRootCacheWrite> newCueCacheWrites;
	  const WorkerTaskObserverGuard workerObserver{[&newCueWorkerTasks](const std::vector<WorkerTaskSnapshot>& tasks) {
	    newCueWorkerTasks = tasks;
	  }};
	  const PublishedSongObserverGuard publishedObserver{[&newCuePublishedSongs](const std::vector<PublishedSongSnapshot>& publishedSongs) {
	    newCuePublishedSongs = publishedSongs;
	  }};
	  const CacheWriteObserverGuard cacheWriteObserver{[&newCueCacheWrites](const cache::ScanRootCacheWrite& write) {
	    newCueCacheWrites.push_back(write);
	  }};

  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                          [](const PlaylistTreeSnapshot& snapshot) {
                                            return songsIn(snapshot).size() == 2U;
                                          });

  songs = songsIn(incremental.snapshot);
  REQUIRE(songs.size() == 2U);
  CHECK(songByPath(songs, cueFile).title == "New Cue Track");
  CHECK(std::ranges::none_of(songs, [&referencedAudio](const SongMetadata& song) {
    return song.filePath == referencedAudio;
  }));
  CHECK(newCueWorkerTasks.empty());
  CHECK(publishedOriginCount(newCuePublishedSongs, ScanItemOrigin::CacheHit) == 1U);
  CHECK(publishedOriginCount(newCuePublishedSongs, ScanItemOrigin::VirtualContainer) == 1U);
  CHECK(publishedOriginCount(newCuePublishedSongs, ScanItemOrigin::CueTrackScannedNew) == 1U);
	  const auto* newCueTrack = publishedByOrigin(newCuePublishedSongs, ScanItemOrigin::CueTrackScannedNew);
	  REQUIRE(newCueTrack != nullptr);
	  REQUIRE(newCueTrack->locationId.has_value());
	  CHECK_FALSE(newCueTrack->locationId->empty());
	  REQUIRE(newCueCacheWrites.size() == 1U);
	  CHECK(newCueCacheWrites.back().changedSongs.empty());
	  REQUIRE(newCueCacheWrites.back().changedCueTracks.size() == 1U);
	  CHECK(newCueCacheWrites.back().changedCueTracks[0].location.filePath == cueFile);
	  CHECK(newCueCacheWrites.back().retainedLocationIds.size() == 2U);
	}

TEST_CASE("scanner incremental e2e falls back to cue reader when cue or source fingerprint changes") {
  test::TempScannerRoot temp{"scanner-incremental-cue-cache-miss"};
  const auto cueFile = temp.path() / "album.cue";
  const auto referencedAudio = test::writeAudioFixture(temp.path(), "album.flac");
  writeText(cueFile, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n");
  const auto fixtureNow = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(cueFile, fixtureNow - std::chrono::hours{2});
  std::filesystem::last_write_time(referencedAudio, fixtureNow - std::chrono::hours{1});

  std::atomic_size_t cueReadCount{0};
  std::string cueTitle = "Cue Before";
  const TestCueProviderGuard cueProvider{[&referencedAudio, &cueReadCount, &cueTitle](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    cueReadCount.fetch_add(1U, std::memory_order_relaxed);
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 180000000,
             .title = cueTitle,
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1}};
  }};

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, rawMetadata("Bare Album File"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 1U;
  });
  auto songs = songsIn(full.snapshot);
  REQUIRE(songs.size() == 1U);
  CHECK(songByPath(songs, cueFile).title == "Cue Before");
  const auto fullCueReadCount = cueReadCount.load(std::memory_order_relaxed);

	  cueTitle = "Cue After Source Change";
	  writeText(referencedAudio, "changed source bytes for cue cache miss");
	  std::vector<WorkerTaskSnapshot> sourceChangeWorkerTasks;
	  std::vector<PublishedSongSnapshot> sourceChangePublishedSongs;
		  std::vector<cache::ScanRootCacheWrite> sourceChangeCacheWrites;
		  {
		    const WorkerTaskObserverGuard workerObserver{[&sourceChangeWorkerTasks](const std::vector<WorkerTaskSnapshot>& tasks) {
		      sourceChangeWorkerTasks = tasks;
		    }};
		    const PublishedSongObserverGuard publishedObserver{[&sourceChangePublishedSongs](const std::vector<PublishedSongSnapshot>& publishedSongs) {
		      sourceChangePublishedSongs = publishedSongs;
		    }};
		    const CacheWriteObserverGuard cacheWriteObserver{[&sourceChangeCacheWrites](const cache::ScanRootCacheWrite& write) {
		      sourceChangeCacheWrites.push_back(write);
		    }};
		    const auto afterSourceChange = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
		                                                 [](const PlaylistTreeSnapshot& snapshot) {
		                                                   return songsIn(snapshot).size() == 1U;
	                                                 });
	    songs = songsIn(afterSourceChange.snapshot);
	  }
	  REQUIRE(songs.size() == 1U);
	  CHECK(songByPath(songs, cueFile).title == "Cue After Source Change");
	  CHECK(sourceChangeWorkerTasks.empty());
	  CHECK(publishedOriginCount(sourceChangePublishedSongs, ScanItemOrigin::VirtualContainer) == 1U);
	  CHECK(publishedOriginCount(sourceChangePublishedSongs, ScanItemOrigin::CueTrackRescannedChanged) == 1U);
	  const auto* changedCueTrack = publishedByOrigin(sourceChangePublishedSongs, ScanItemOrigin::CueTrackRescannedChanged);
	  REQUIRE(changedCueTrack != nullptr);
	  REQUIRE(changedCueTrack->locationId.has_value());
		  CHECK_FALSE(changedCueTrack->locationId->empty());
		  REQUIRE(sourceChangeCacheWrites.size() == 1U);
		  CHECK(sourceChangeCacheWrites.back().changedSongs.empty());
		  REQUIRE(sourceChangeCacheWrites.back().changedCueTracks.size() == 1U);
		  CHECK(sourceChangeCacheWrites.back().changedCueTracks[0].location.filePath == cueFile);
		  const auto afterSourceCueReadCount = cueReadCount.load(std::memory_order_relaxed);
		  CHECK(afterSourceCueReadCount > fullCueReadCount);

  cueTitle = "Cue After Cue Change";
  writeText(cueFile, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\nREM changed cue fingerprint\n");
  const auto afterCueChange = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                            [](const PlaylistTreeSnapshot& snapshot) {
                                              return songsIn(snapshot).size() == 1U;
                                            });
  songs = songsIn(afterCueChange.snapshot);
  REQUIRE(songs.size() == 1U);
  CHECK(songByPath(songs, cueFile).title == "Cue After Cue Change");
  CHECK(cueReadCount.load(std::memory_order_relaxed) > afterSourceCueReadCount);
}

TEST_CASE("scanner incremental e2e preserves readable cue tracks on partial cache miss") {
  test::TempScannerRoot temp{"scanner-incremental-cue-partial-miss"};
  const auto cueFile = temp.path() / "album.cue";
  const auto firstAudio = test::writeAudioFixture(temp.path(), "disc-a.flac");
  const auto secondAudio = test::writeAudioFixture(temp.path(), "disc-b.flac");
  writeText(cueFile,
            "FILE \"disc-a.flac\" WAVE\n  TRACK 01 AUDIO\n"
            "FILE \"disc-b.flac\" WAVE\n  TRACK 02 AUDIO\n");
  const auto fixtureNow = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(cueFile, fixtureNow - std::chrono::hours{2});
  std::filesystem::last_write_time(firstAudio, fixtureNow - std::chrono::hours{1});
  std::filesystem::last_write_time(secondAudio, fixtureNow - std::chrono::minutes{30});

  std::atomic_size_t cueReadCount{0};
  bool partialMode = false;
  const TestCueProviderGuard cueProvider{[&](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    cueReadCount.fetch_add(1U, std::memory_order_relaxed);
    std::vector<TestCueTrackData> tracks{{.audioFilePath = firstAudio,
                                          .offset = 0,
                                          .duration = 180000000,
                                          .title = partialMode ? "Readable Track After Miss" : "Readable Track Before Miss",
                                          .artist = "Cue Artist",
                                          .album = "Cue Album",
                                          .trackNumber = 1}};
    if (!partialMode) {
      tracks.push_back({.audioFilePath = secondAudio,
                        .offset = 180000000,
                        .duration = 200000000,
                        .title = "Removed Source Track",
                        .artist = "Cue Artist",
                        .album = "Cue Album",
                        .trackNumber = 2});
    }
    return tracks;
  }};

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(firstAudio, rawMetadata("Bare Disc A"));
  reader->put(secondAudio, rawMetadata("Bare Disc B"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 2U;
  });
  auto songs = songsIn(full.snapshot);
  REQUIRE(songs.size() == 2U);
  const auto fullCueReadCount = cueReadCount.load(std::memory_order_relaxed);

  partialMode = true;
  std::filesystem::remove(secondAudio);
  const auto partial = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                      [](const PlaylistTreeSnapshot& snapshot) {
                                        return songsIn(snapshot).size() == 1U;
                                      });

  songs = songsIn(partial.snapshot);
  REQUIRE(songs.size() == 1U);
  CHECK(songByPath(songs, cueFile).title == "Readable Track After Miss");
  CHECK(songByPath(songs, cueFile).sourceFilePath == firstAudio);
  CHECK(std::ranges::none_of(songs, [&](const SongMetadata& song) {
    return song.filePath == firstAudio || song.filePath == secondAudio;
  }));
  CHECK(cueReadCount.load(std::memory_order_relaxed) > fullCueReadCount);
}

TEST_CASE("scanner incremental e2e hides cached cue source audio before worker scheduling") {
  test::TempScannerRoot temp{"scanner-incremental-cue-source-hide"};
  const auto cueFile = temp.path() / "album.cue";
  const auto referencedAudio = test::writeAudioFixture(temp.path(), "album.flac");
  const auto standalone = test::writeAudioFixture(temp.path(), "bonus.flac");
  const std::string originalCueText = "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n  TRACK 02 AUDIO\n";
  writeText(cueFile, originalCueText);
  const auto fixtureNow = std::filesystem::file_time_type::clock::now();
  const auto cachedCueMtime = fixtureNow - std::chrono::hours{2};
  std::filesystem::last_write_time(cueFile, cachedCueMtime);
  std::filesystem::last_write_time(referencedAudio, fixtureNow - std::chrono::hours{1});

  const TestCueProviderGuard cueProvider{[&referencedAudio](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 180000000,
             .title = "Cue Track 1",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1},
            {.audioFilePath = referencedAudio,
             .offset = 180000000,
             .duration = 200000000,
             .title = "Cue Track 2",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 2}};
  }};

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, rawMetadata("Bare Album File"));
  reader->put(standalone, rawMetadata("Bonus Track"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 3U;
  });
  auto songs = songsIn(full.snapshot);

  REQUIRE(songs.size() == 3U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(songByPath(songs, standalone).title == "Bonus Track");
  const auto fullReadCount = reader->readCount();

  std::string cueTextWithoutFileCommand = "REM cached source path fallback";
  cueTextWithoutFileCommand.resize(originalCueText.size(), ' ');
  writeText(cueFile, cueTextWithoutFileCommand);
  std::filesystem::last_write_time(cueFile, cachedCueMtime);

  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                          [](const PlaylistTreeSnapshot&) {
                                            return true;
                                          });

  songs = songsIn(incremental.snapshot);
  REQUIRE(songs.size() == 3U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(std::ranges::none_of(songs, [&referencedAudio](const SongMetadata& song) {
    return song.filePath == referencedAudio;
  }));
  CHECK(songByPath(songs, standalone).title == "Bonus Track");
  CHECK(reader->readCount() == fullReadCount);
}

TEST_CASE("scanner e2e resolves folder thumbnails after full scan") {
  test::TempScannerRoot temp{"scanner-folder-thumbnail-full"};
  const auto folderA = temp.path() / "A";
  const auto folderB = temp.path() / "B";
  const auto folderBZ = folderB / "z";
  const auto folderC = temp.path() / "C";
  std::filesystem::create_directories(folderA);
  std::filesystem::create_directories(folderBZ);
  std::filesystem::create_directories(folderC);

  const auto aTrack1 = test::writeAudioFixture(folderA, "01-a.flac");
  const auto aTrack2 = test::writeAudioFixture(folderA, "02-b.flac");
  const auto bTrack = test::writeAudioFixture(folderB, "01-b.flac");
  const auto cTrack = test::writeAudioFixture(folderBZ, "03-c.flac");
  const auto dTrack = test::writeAudioFixture(folderC, "01-d.flac");

  auto reader = std::make_shared<FakeMetadataReader>();
  auto aOne = rawMetadata("A One");
  aOne.thumbnailPath = temp.path() / "art-a1.png";
  reader->put(aTrack1, aOne);
  auto aTwo = rawMetadata("A Two");
  aTwo.thumbnailPath = temp.path() / "art-a2.png";
  reader->put(aTrack2, aTwo);
  auto bOne = rawMetadata("B One");
  bOne.thumbnailPath = temp.path() / "art-b.png";
  reader->put(bTrack, bOne);
  auto cOne = rawMetadata("C One");
  cOne.thumbnailPath = temp.path() / "art-c.png";
  reader->put(cTrack, cOne);
  reader->put(dTrack, rawMetadata("D One"));

  auto seam = std::make_shared<FakeFolderThumbnailSeam>();
  const auto exportedA = temp.path() / "covers" / "exported-a.png";
  seam->setResult(folderA, exportedA);
  seam->setResult(folderB, std::nullopt);
  seam->setResult(folderBZ, std::nullopt);
  seam->setResult(folderC, std::nullopt);

  auto service = makeServiceWithSeam(temp, reader, seam->makeSeam());
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto result = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full,
                                     [](const PlaylistTreeSnapshot& snapshot) { return songsIn(snapshot).size() == 5U; });
  const auto& snapshot = result.snapshot;
  REQUIRE(songsIn(snapshot).size() == 5U);

  const auto* nodeA = directoryNodeByDisplayName(snapshot, "A");
  const auto* nodeB = directoryNodeByDisplayName(snapshot, "B");
  const auto* nodeBZ = directoryNodeByDisplayName(snapshot, "z");
  const auto* nodeC = directoryNodeByDisplayName(snapshot, "C");
  REQUIRE(nodeA != nullptr);
  REQUIRE(nodeB != nullptr);
  REQUIRE(nodeBZ != nullptr);
  REQUIRE(nodeC != nullptr);

  CHECK(nodeA->thumbnailPath == exportedA.generic_string());
  CHECK(nodeB->thumbnailPath == (temp.path() / "art-b.png").generic_string());
  CHECK(nodeBZ->thumbnailPath == (temp.path() / "art-c.png").generic_string());
  CHECK_FALSE(nodeC->thumbnailPath.has_value());
  const auto* root = rootNodeOf(snapshot);
  REQUIRE(root != nullptr);
  CHECK_FALSE(root->thumbnailPath.has_value());

  const auto called = seam->called();
  REQUIRE(called.size() == 4U);
  CHECK(called[0] == folderA);
  CHECK(called[1] == folderB);
  CHECK(called[2] == folderBZ);
  CHECK(called[3] == folderC);
}

TEST_CASE("scanner e2e resolves folder thumbnails consistently after incremental scan") {
  test::TempScannerRoot temp{"scanner-folder-thumbnail-incremental"};
  const auto folderA = temp.path() / "A";
  const auto folderB = temp.path() / "B";
  std::filesystem::create_directories(folderA);
  std::filesystem::create_directories(folderB);

  const auto aTrack = test::writeAudioFixture(folderA, "01-a.flac");
  const auto bTrack = test::writeAudioFixture(folderB, "01-b.flac");

  auto reader = std::make_shared<FakeMetadataReader>();
  auto aMeta = rawMetadata("A One");
  aMeta.thumbnailPath = temp.path() / "art-a.png";
  reader->put(aTrack, aMeta);
  auto bMeta = rawMetadata("B One");
  bMeta.thumbnailPath = temp.path() / "art-b.png";
  reader->put(bTrack, bMeta);

  auto seam = std::make_shared<FakeFolderThumbnailSeam>();
  const auto exportedA = temp.path() / "covers" / "exported-a.png";
  seam->setResult(folderA, exportedA);
  seam->setResult(folderB, std::nullopt);

  auto service = makeServiceWithSeam(temp, reader, seam->makeSeam());
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full,
                                   [](const PlaylistTreeSnapshot& snapshot) { return songsIn(snapshot).size() == 2U; });
  {
    const auto& snapshot = full.snapshot;
    const auto* nodeA = directoryNodeByDisplayName(snapshot, "A");
    const auto* nodeB = directoryNodeByDisplayName(snapshot, "B");
    REQUIRE(nodeA != nullptr);
    REQUIRE(nodeB != nullptr);
    CHECK(nodeA->thumbnailPath == exportedA.generic_string());
    CHECK(nodeB->thumbnailPath == (temp.path() / "art-b.png").generic_string());
    const auto* root = rootNodeOf(snapshot);
    REQUIRE(root != nullptr);
    CHECK_FALSE(root->thumbnailPath.has_value());
    const auto called = seam->called();
    REQUIRE(called.size() == 2U);
    CHECK(called[0] == folderA);
    CHECK(called[1] == folderB);
  }

  const auto added = test::writeAudioFixture(folderB, "00-aa.flac");
  auto addedMeta = rawMetadata("B Earlier");
  addedMeta.thumbnailPath = temp.path() / "art-aa.png";
  reader->put(added, addedMeta);

  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                          [](const PlaylistTreeSnapshot& snapshot) { return songsIn(snapshot).size() == 3U; });
  {
    const auto& snapshot = incremental.snapshot;
    REQUIRE(songsIn(snapshot).size() == 3U);
    const auto* nodeA = directoryNodeByDisplayName(snapshot, "A");
    const auto* nodeB = directoryNodeByDisplayName(snapshot, "B");
    REQUIRE(nodeA != nullptr);
    REQUIRE(nodeB != nullptr);
    CHECK(nodeA->thumbnailPath == exportedA.generic_string());
    CHECK(nodeB->thumbnailPath == (temp.path() / "art-aa.png").generic_string());
    const auto* root = rootNodeOf(snapshot);
    REQUIRE(root != nullptr);
    CHECK_FALSE(root->thumbnailPath.has_value());
    const auto called = seam->called();
    REQUIRE(called.size() == 4U);
    CHECK(called[2] == folderA);
    CHECK(called[3] == folderB);
  }
}

TEST_CASE("scanner seeds long-lived tree builder member and replaces it on fallback rescan") {
  test::TempScannerRoot temp{"scanner-tree-builder-seed"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  std::vector<TreeBuilderSeededSnapshot> observations;
  {
    TreeBuilderObserverGuard guard([&observations](const TreeBuilderSeededSnapshot& snapshot) {
      observations.push_back(snapshot);
    });

    const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
      return songsIn(snapshot).size() == 2U;
    });
    REQUIRE(songsIn(full.snapshot).size() == 2U);
    REQUIRE(observations.size() == 1U);
    CHECK(observations.back().seeded);
    CHECK(observations.back().generation == 1U);
    CHECK(observations.back().songCount == 2U);

    const auto fallback = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
      return songsIn(snapshot).size() == 2U;
    });
    REQUIRE(songsIn(fallback.snapshot).size() == 2U);
    REQUIRE(observations.size() == 2U);
    CHECK(observations.back().seeded);
    CHECK(observations.back().generation == 2U);
    CHECK(observations.back().songCount == 2U);
  }

  const auto finalSnapshot = service->snapshot();
  CHECK(songsIn(finalSnapshot).size() == 2U);
  const auto independent = rebuildSnapshotFrom(finalSnapshot, canonicalRootPath(temp.path()));
  CHECK(treesEquivalent(finalSnapshot, independent));
}

TEST_CASE("scanner fallback rescan produces no duplicate tree and equals full rebuild") {
  test::TempScannerRoot temp{"scanner-tree-builder-no-duplicate"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto full = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 2U;
  });
  const auto fallback = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Full, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 2U;
  });

  REQUIRE(songsIn(full.snapshot).size() == 2U);
  REQUIRE(songsIn(fallback.snapshot).size() == 2U);
  CHECK(treesEquivalent(full.snapshot, fallback.snapshot));

  const auto root = canonicalRootPath(temp.path());
  const auto independent = rebuildSnapshotFrom(full.snapshot, root);
  CHECK(treesEquivalent(fallback.snapshot, independent));

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(root);
  CHECK(locations.size() == 2U);
}

[[nodiscard]] SongMetadata treeSong(std::string title, std::filesystem::path path, std::chrono::milliseconds duration) {
  SongMetadata metadata{};
  metadata.title = std::move(title);
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.duration = duration;
  metadata.logicalTrackId = path.generic_string();
  return metadata;
}

[[nodiscard]] SongMetadata treeCueContainer(std::filesystem::path cuePath) {
  SongMetadata metadata{};
  metadata.filePath = cuePath;
  metadata.logicalTrackId = cuePath.generic_string();
  metadata.duration = std::chrono::milliseconds{0};
  return metadata;
}

[[nodiscard]] SongMetadata treeCueTrack(std::string title,
                                         std::filesystem::path cuePath,
                                         std::filesystem::path sourceAudioPath,
                                         std::uint32_t trackNumber,
                                         std::string logicalTrackId) {
  SongMetadata metadata{};
  metadata.title = std::move(title);
  metadata.filePath = cuePath;
  metadata.sourceFilePath = std::move(sourceAudioPath);
  metadata.offset = std::chrono::seconds{static_cast<int>(trackNumber - 1U) * 60};
  metadata.duration = std::chrono::seconds{60};
  metadata.trackNumber = trackNumber;
  metadata.logicalTrackId = std::move(logicalTrackId);
  metadata.trackId = metadata.logicalTrackId;
  return metadata;
}

TEST_CASE("playlist tree builder removeSubtree drops directory subtree including cue logical-track keys") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addDirectory({.relativePath = "music", .displayName = "music"});
  builder.addSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});
  builder.addSong({.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Outro", "music/live.cue", "music/live.flac", 2U, "custom_album_logical_id")});
  builder.addSong({.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})});
  builder.addSong({.relativePath = "other/keep.flac", .metadata = treeSong("Keep", "other/keep.flac", std::chrono::seconds{90})});

  const auto before = builder.publish();
  REQUIRE(songsIn(before).size() == 6U);
  REQUIRE(std::ranges::find_if(before.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:custom_album_logical_id"; }) != before.nodes.end());

  CHECK(builder.removeSubtree("music"));
  const auto after = builder.publish();

  const auto songs = songsIn(after);
  REQUIRE(songs.size() == 2U);
  CHECK(songByPath(songs, "loose.flac").title == "Loose");
  CHECK(songByPath(songs, "other/keep.flac").title == "Keep");
  CHECK(std::ranges::none_of(after.nodes, [](const PlaylistNode& node) {
    return node.nodeId == "dir:music" || node.nodeId.starts_with("dir:music/") ||
           node.nodeId.starts_with("track:music/");
  }));
  CHECK(std::ranges::none_of(after.nodes, [](const PlaylistNode& node) {
    return node.nodeId == "track:custom_album_logical_id";
  }));
  const auto* root = rootNodeOf(after);
  REQUIRE(root != nullptr);
  REQUIRE(root->childNodeIds.size() == 2U);
  CHECK(root->childNodeIds[0] == "dir:other");
  CHECK(root->childNodeIds[1] == "track:loose.flac");
  const auto stats = builder.stats();
  CHECK(stats.songCount == 2U);
  CHECK(stats.totalDuration == std::chrono::seconds{120});
  CHECK_FALSE(builder.removeSubtree("music"));
}

TEST_CASE("playlist tree builder removeSubtree on a track prunes emptied parent directory") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "artist/alpha.flac", .metadata = treeSong("Alpha", "artist/alpha.flac", std::chrono::seconds{10})});
  builder.addSong({.relativePath = "artist/beta.flac", .metadata = treeSong("Beta", "artist/beta.flac", std::chrono::seconds{20})});
  builder.addSong({.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})});

  CHECK(builder.removeSubtree("artist/alpha.flac"));
  auto after = builder.publish();
  CHECK(songsIn(after).size() == 2U);
  CHECK(std::ranges::none_of(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:artist/alpha.flac"; }));
  REQUIRE(directoryNodeByDisplayName(after, "artist") != nullptr);
  const auto* root = rootNodeOf(after);
  REQUIRE(root != nullptr);
  REQUIRE(root->childNodeIds.size() == 2U);
  CHECK(root->childNodeIds[0] == "dir:artist");
  CHECK(root->childNodeIds[1] == "track:loose.flac");

  CHECK(builder.removeSubtree("artist/beta.flac"));
  after = builder.publish();
  CHECK(songsIn(after).size() == 1U);
  CHECK(directoryNodeByDisplayName(after, "artist") == nullptr);
  const auto* rootAfter = rootNodeOf(after);
  REQUIRE(rootAfter != nullptr);
  REQUIRE(rootAfter->childNodeIds.size() == 1U);
  CHECK(rootAfter->childNodeIds[0] == "track:loose.flac");
  CHECK(builder.stats().songCount == 1U);

  CHECK_FALSE(builder.removeSubtree("artist/ghost.flac"));
  CHECK_FALSE(builder.removeSubtree("."));
}

TEST_CASE("playlist tree builder removeSubtree result equals independent full rebuild from reduced song set") {
  const std::vector<PlaylistTreeSong> fullSet = {
      {.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})},
      {.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})},
      {.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")},
      {.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")},
      {.relativePath = "music/live.cue", .metadata = treeCueTrack("Outro", "music/live.cue", "music/live.flac", 2U, "custom_album_logical_id")},
      {.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})},
      {.relativePath = "other/keep.flac", .metadata = treeSong("Keep", "other/keep.flac", std::chrono::seconds{90})},
  };

  PlaylistTreeBuilder builder{"Library"};
  for (const auto& entry : fullSet) {
    builder.addSong(entry);
  }
  const auto before = builder.publish();
  REQUIRE(songsIn(before).size() == 6U);

  CHECK(builder.removeSubtree("music"));
  const auto after = builder.publish();

  PlaylistTreeBuilder expected{"Library"};
  for (const auto& entry : fullSet) {
    if (entry.relativePath.generic_string().starts_with("music/")) {
      continue;
    }
    expected.addSong(entry);
  }
  const auto expectedSnapshot = expected.publish();
  REQUIRE(songsIn(expectedSnapshot).size() == 2U);
  CHECK(treesEquivalent(after, expectedSnapshot));
}

TEST_CASE("playlist tree builder renameSubtree rewrites subtree keys node ids and parent references") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addDirectory({.relativePath = "music", .displayName = "music"});
  builder.addSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});
  builder.addSong({.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")});
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Outro", "music/live.cue", "music/live.flac", 2U, "custom_album_logical_id")});
  builder.addSong({.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})});

  const auto before = builder.publish();
  REQUIRE(songsIn(before).size() == 5U);
  REQUIRE(std::ranges::find_if(before.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:music/live.cue"; }) != before.nodes.end());

  CHECK(builder.renameSubtree("music", "pop"));
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  const auto songs = songsIn(after);
  REQUIRE(songs.size() == 5U);
  CHECK(songByPath(songs, "pop/01.flac").title == "One");
  CHECK(songByPath(songs, "pop/02.flac").title == "Two");
  CHECK(songByPath(songs, "loose.flac").title == "Loose");
  CHECK(songByPath(songs, "pop/01.flac").filePath == "pop/01.flac");
  CHECK(songByPath(songs, "pop/01.flac").logicalTrackId == "pop/01.flac");
  auto cueNodes = songNodesByPath(after, "pop/live.cue");
  REQUIRE(cueNodes.size() == 2U);
  CHECK(cueNodes[0].song->title == "Intro");
  CHECK(cueNodes[0].song->logicalTrackId == "pop/live.cue#track0");
  CHECK(cueNodes[0].song->trackId == "pop/live.cue#track0");
  CHECK(cueNodes[0].song->sourceFilePath == "pop/live.flac");
  CHECK(cueNodes[1].song->title == "Outro");
  CHECK(cueNodes[1].song->logicalTrackId == "custom_album_logical_id");
  CHECK(cueNodes[1].song->sourceFilePath == "pop/live.flac");

  CHECK(std::ranges::none_of(after.nodes, [](const PlaylistNode& node) {
    return node.nodeId == "dir:music" || node.nodeId.starts_with("dir:music/") || node.nodeId.starts_with("track:music/");
  }));
  CHECK(std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:pop"; }) != after.nodes.end());
  CHECK(std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:pop/live.cue"; }) != after.nodes.end());
  CHECK(std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:pop/01.flac"; }) != after.nodes.end());
  const auto introIterator = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:pop/live.cue#track0"; });
  REQUIRE(introIterator != after.nodes.end());
  CHECK(introIterator->parentNodeId == "dir:pop/live.cue");
  const auto outroIterator = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:custom_album_logical_id"; });
  REQUIRE(outroIterator != after.nodes.end());
  CHECK(outroIterator->parentNodeId == "dir:pop/live.cue");

  CHECK(builder.stats().songCount == 5U);
  CHECK(builder.stats().totalDuration == std::chrono::seconds{60 + 60 + 60 + 60 + 30});

  CHECK_FALSE(builder.renameSubtree("pop", "pop"));
  CHECK_FALSE(builder.renameSubtree("ghost", "phantom"));
  CHECK_FALSE(builder.renameSubtree(".", "root"));
}

TEST_CASE("playlist tree builder renameSubtree result equals independent full rebuild from path-rewritten song set") {
  const std::vector<PlaylistTreeSong> fullSet = {
      {.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})},
      {.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})},
      {.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")},
      {.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")},
      {.relativePath = "music/live.cue", .metadata = treeCueTrack("Outro", "music/live.cue", "music/live.flac", 2U, "custom_album_logical_id")},
      {.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})},
      {.relativePath = "music/sub/keep.flac", .metadata = treeSong("Keep", "music/sub/keep.flac", std::chrono::seconds{90})},
  };

  PlaylistTreeBuilder builder{"Library"};
  for (const auto& entry : fullSet) {
    builder.addSong(entry);
  }
  const auto before = builder.publish();
  REQUIRE(songsIn(before).size() == 6U);

  CHECK(builder.renameSubtree("music", "pop"));
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  PlaylistTreeBuilder expected{"Library"};
  for (const auto& entry : fullSet) {
    expected.addSong(rewriteTreeSongPaths(entry, "music", "pop"));
  }
  const auto expectedSnapshot = expected.publish();
  REQUIRE(songsIn(expectedSnapshot).size() == 6U);
  CHECK(treesEquivalent(after, expectedSnapshot));
}

TEST_CASE("playlist tree builder renameSubtree leaves sibling directory prefixes untouched") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "A/one.flac", .metadata = treeSong("A One", "A/one.flac", std::chrono::seconds{10})});
  builder.addSong({.relativePath = "AB/two.flac", .metadata = treeSong("AB Two", "AB/two.flac", std::chrono::seconds{20})});
  builder.addSong({.relativePath = "loose.flac", .metadata = treeSong("Loose", "loose.flac", std::chrono::seconds{30})});

  CHECK(builder.renameSubtree("A", "D"));
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  const auto songs = songsIn(after);
  REQUIRE(songs.size() == 3U);
  CHECK(songByPath(songs, "D/one.flac").title == "A One");
  CHECK(songByPath(songs, "AB/two.flac").title == "AB Two");
  CHECK(songByPath(songs, "loose.flac").title == "Loose");
  CHECK(std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:AB"; }) != after.nodes.end());
  CHECK(std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:AB/two.flac"; }) != after.nodes.end());
  CHECK(std::ranges::none_of(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:A" || node.nodeId.starts_with("track:A/"); }));
}

TEST_CASE("playlist tree builder renameSubtree creates intermediate directories when moving deeper") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});

  CHECK(builder.renameSubtree("music", "nested/pop"));
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  REQUIRE(songsIn(after).size() == 1U);
  CHECK(songByPath(songsIn(after), "nested/pop/01.flac").title == "One");
  const auto nestedIterator = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:nested"; });
  REQUIRE(nestedIterator != after.nodes.end());
  const auto popIterator = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:nested/pop"; });
  REQUIRE(popIterator != after.nodes.end());
  CHECK(popIterator->parentNodeId == nestedIterator->nodeId);
  const auto* root = rootNodeOf(after);
  REQUIRE(root != nullptr);
  REQUIRE(std::ranges::find(root->childNodeIds, "dir:nested") != root->childNodeIds.end());
}

TEST_CASE("playlist tree builder renameSubtree prunes a directory emptied by moving its only child out") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "artist/alpha.flac", .metadata = treeSong("Alpha", "artist/alpha.flac", std::chrono::seconds{10})});

  CHECK(builder.renameSubtree("artist/alpha.flac", "renamed.flac"));
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  REQUIRE(songsIn(after).size() == 1U);
  CHECK(songByPath(songsIn(after), "renamed.flac").title == "Alpha");
  CHECK(directoryNodeByDisplayName(after, "artist") == nullptr);
}

TEST_CASE("playlist tree builder upsertSong inserts a new song node and creates its parent directory") {
  PlaylistTreeBuilder builder{"Library"};
  const bool inserted = builder.upsertSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});
  CHECK(inserted);
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  REQUIRE(songsIn(after).size() == 1U);
  CHECK(songByPath(songsIn(after), "music/01.flac").title == "One");
  const auto* musicDir = directoryNodeByDisplayName(after, "music");
  REQUIRE(musicDir != nullptr);
  const auto trackIt = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/01.flac"; });
  REQUIRE(trackIt != after.nodes.end());
  CHECK(trackIt->parentNodeId == musicDir->nodeId);
  CHECK(builder.stats().songCount == 1U);
  CHECK(builder.stats().totalDuration == std::chrono::seconds{60});

  PlaylistTreeBuilder expected{"Library"};
  expected.addSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});
  CHECK(treesEquivalent(after, expected.publish()));
}

TEST_CASE("playlist tree builder upsertSong updates existing song without duplicating node") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "music/01.flac", .metadata = treeSong("One", "music/01.flac", std::chrono::seconds{60})});
  builder.addSong({.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})});
  const auto before = builder.publish();
  REQUIRE(songsIn(before).size() == 2U);
  const auto beforeIt = std::ranges::find_if(before.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/01.flac"; });
  REQUIRE(beforeIt != before.nodes.end());
  const auto nodeIdBefore = beforeIt->nodeId;

  auto updated = treeSong("One Updated", "music/01.flac", std::chrono::seconds{75});
  const bool inserted = builder.upsertSong({.relativePath = "music/01.flac", .metadata = updated});
  CHECK_FALSE(inserted);
  const auto after = builder.publish();
  requireNoDanglingReferences(after);

  REQUIRE(songsIn(after).size() == 2U);
  CHECK(builder.stats().songCount == 2U);
  CHECK(builder.stats().totalDuration == std::chrono::seconds{75 + 60});
  const auto afterIt = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/01.flac"; });
  REQUIRE(afterIt != after.nodes.end());
  CHECK(afterIt->nodeId == nodeIdBefore);
  CHECK(afterIt->displayName == "One Updated");
  REQUIRE(afterIt->song.has_value());
  CHECK(afterIt->song->title == "One Updated");
  CHECK(afterIt->song->duration == std::chrono::seconds{75});

  PlaylistTreeBuilder expected{"Library"};
  expected.addSong({.relativePath = "music/01.flac", .metadata = updated});
  expected.addSong({.relativePath = "music/02.flac", .metadata = treeSong("Two", "music/02.flac", std::chrono::seconds{60})});
  CHECK(treesEquivalent(after, expected.publish()));
}

TEST_CASE("playlist tree builder upsertSong routes cue tracks into cue virtual directory without key conflict") {
  PlaylistTreeBuilder builder{"Library"};
  builder.addSong({.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")});
  CHECK(builder.upsertSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")}));
  SongMetadata composite{};
  composite.filePath = "music/live.cue";
  composite.sourceFilePath = "music/live.flac";
  composite.offset = std::chrono::milliseconds{60000};
  composite.duration = std::chrono::seconds{60};
  composite.title = "Composite";
  CHECK(builder.upsertSong({.relativePath = "music/live.cue", .metadata = composite}));

  const auto after = builder.publish();
  requireNoDanglingReferences(after);
  REQUIRE(songsIn(after).size() == 2U);
  const auto introIt = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/live.cue#track0"; });
  REQUIRE(introIt != after.nodes.end());
  CHECK(introIt->parentNodeId == "dir:music/live.cue");
  const auto compositeIt = std::ranges::find_if(after.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/live.cue#music/live.flac@60000"; });
  REQUIRE(compositeIt != after.nodes.end());
  CHECK(compositeIt->parentNodeId == "dir:music/live.cue");

  auto compositeUpdated = composite;
  compositeUpdated.title = "Composite Updated";
  const bool inserted = builder.upsertSong({.relativePath = "music/live.cue", .metadata = compositeUpdated});
  CHECK_FALSE(inserted);
  const auto afterModify = builder.publish();
  requireNoDanglingReferences(afterModify);
  REQUIRE(songsIn(afterModify).size() == 2U);
  const auto updatedIt = std::ranges::find_if(afterModify.nodes, [](const PlaylistNode& node) { return node.nodeId == "track:music/live.cue#music/live.flac@60000"; });
  REQUIRE(updatedIt != afterModify.nodes.end());
  CHECK(updatedIt->nodeId == compositeIt->nodeId);
  REQUIRE(updatedIt->song.has_value());
  CHECK(updatedIt->song->title == "Composite Updated");

  PlaylistTreeBuilder expected{"Library"};
  expected.addSong({.relativePath = "music/live.cue", .metadata = treeCueContainer("music/live.cue")});
  expected.addSong({.relativePath = "music/live.cue", .metadata = treeCueTrack("Intro", "music/live.cue", "music/live.flac", 1U, "music/live.cue#track0")});
  expected.addSong({.relativePath = "music/live.cue", .metadata = compositeUpdated});
  CHECK(treesEquivalent(afterModify, expected.publish()));
}

}
}
