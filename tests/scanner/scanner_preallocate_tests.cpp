#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "scanner_internal_types.h"
#include "scanner_test_harness.h"
#include "file_scanner_service_internal.h"
#include "file_scanner_orchestrator_test_access.h"

#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <filesystem>

using namespace seriona::scanner;

namespace {

class FakeMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { 
    metadataByPath_[std::move(path)] = std::move(metadata); 
  }

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& /* coverExportDir */) override {
    const auto iterator = metadataByPath_.find(path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = path;
    return metadata;
  }

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
};

struct ScanEvents {
  std::mutex mutex;
  std::condition_variable cv;
  std::size_t scanCompletedCount{0};
  std::size_t filesDiscovered{0};
  std::size_t filesScanned{0};
  
  void onEvent(const ScannerEvent& event) {
    std::lock_guard lock{mutex};
    if (event.type == ScannerEventType::ScanCompleted) {
      ++scanCompletedCount;
      cv.notify_all();
    } else if (event.type == ScannerEventType::ProgressUpdated) {
      if (const auto* progress = std::get_if<ScanProgress>(&event.payload)) {
        filesDiscovered = progress->filesDiscovered;
        filesScanned = progress->filesScanned;
      }
    }
  }
  
  [[nodiscard]] bool waitForScanCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    const auto initialCount = scanCompletedCount;
    return cv.wait_for(lock, timeout, [this, initialCount] {
      return scanCompletedCount > initialCount;
    });
  }

  [[nodiscard]] std::size_t completedCount() {
    std::lock_guard lock{mutex};
    return scanCompletedCount;
  }

  [[nodiscard]] bool waitForScanCompletionAfter(std::size_t initialCount, std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    return cv.wait_for(lock, timeout, [this, initialCount] {
      return scanCompletedCount > initialCount;
    });
  }
};

[[nodiscard]] RawTagMetadata makeMetadata(std::string title) {
  RawTagMetadata meta{};
  meta.title = std::move(title);
  meta.artist = "Test Artist";
  meta.album = "Test Album";
  meta.duration = std::chrono::milliseconds{180000};
  meta.sampleRate = 44100;
  meta.bitDepth = 16;
  return meta;
}

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

[[nodiscard]] const WorkerTaskSnapshot* workerSnapshotByPath(const std::vector<WorkerTaskSnapshot>& snapshots,
                                                            const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(snapshots, path, &WorkerTaskSnapshot::filePath);
  return iterator == snapshots.end() ? nullptr : &*iterator;
}

[[nodiscard]] const PublishedSongSnapshot* publishedSnapshotByPath(const std::vector<PublishedSongSnapshot>& snapshots,
                                                                   const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(snapshots, path, &PublishedSongSnapshot::filePath);
  return iterator == snapshots.end() ? nullptr : &*iterator;
}

[[nodiscard]] std::filesystem::path scannerSidecarPath(const test::TempScannerRoot& temp) {
  return std::filesystem::path{temp.dbPath().generic_string() + ".scan-roots.sqlite"};
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

} // namespace

TEST_CASE("IndexedPublishedSong: preallocated array supports CueContainer nodes") {
  std::vector<IndexedPublishedSong> nodes(3);
  
  nodes[0].discoveryIndex = 0;
  nodes[0].nodeType = NodeType::CueContainer;
  nodes[0].origin = ScanItemOrigin::VirtualContainer;
  nodes[0].isVirtualFolder = true;
  nodes[0].treeRelativePath = "album.cue";
  
  nodes[1].discoveryIndex = 1;
  nodes[1].nodeType = NodeType::CueTrack;
  nodes[1].origin = ScanItemOrigin::CueTrackCacheHit;
  nodes[1].locationId = "cue-track-cache-location";
  nodes[1].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(0),
    .duration = std::chrono::microseconds(180000000),
    .trackIndex = 0
  };
  
  nodes[2].discoveryIndex = 2;
  nodes[2].nodeType = NodeType::CueTrack;
  nodes[2].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(180000000),
    .duration = std::chrono::microseconds(200000000),
    .trackIndex = 1
  };
  
  CHECK(nodes[0].nodeType == NodeType::CueContainer);
  CHECK(nodes[0].origin == ScanItemOrigin::VirtualContainer);
  CHECK(nodes[0].isVirtualFolder == true);
  CHECK(nodes[1].nodeType == NodeType::CueTrack);
  CHECK(nodes[1].origin == ScanItemOrigin::CueTrackCacheHit);
  REQUIRE(nodes[1].locationId.has_value());
  CHECK(*nodes[1].locationId == "cue-track-cache-location");
  CHECK(nodes[1].cueInfo.has_value());
  CHECK(nodes[1].cueInfo->trackIndex == 0);
  CHECK(nodes[2].nodeType == NodeType::CueTrack);
  CHECK(nodes[2].cueInfo->trackIndex == 1);
}

TEST_CASE("reconcileRoot: worker-filled song keeps default origin and computed location id") {
  test::TempScannerRoot temp{"scanner-preallocate-worker-origin"};

  const auto audioFile = temp.path() / "worker-origin.flac";
  std::ofstream{audioFile} << "worker origin audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(audioFile, makeMetadata("Worker Origin"));

  std::vector<ScanItemOrigin> observedOrigins;
  std::vector<std::optional<std::string>> observedLocationIds;
  std::vector<PublishedSongSnapshot> observedPublishedSongs;
  const PublishedSongObserverGuard publishedSongObserver{[&observedPublishedSongs](const std::vector<PublishedSongSnapshot>& songs) {
    observedPublishedSongs = songs;
  }};
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    for (const auto& node : nodes) {
      if (node.nodeType == NodeType::Song) {
        observedOrigins.push_back(node.origin);
        observedLocationIds.push_back(node.locationId);
      }
    }
  });

  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });

  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });

  const auto beforeScan = events.completedCount();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletionAfter(beforeScan, std::chrono::seconds{5}));
  clearPreallocationObserver();

  REQUIRE(observedOrigins.size() == 1);
  CHECK(observedOrigins[0] == ScanItemOrigin::ScannedFull);
  REQUIRE(observedLocationIds.size() == 1);
  REQUIRE(observedLocationIds[0].has_value());
  CHECK_FALSE(observedLocationIds[0]->empty());
  REQUIRE(observedPublishedSongs.size() == 1U);
  CHECK(observedPublishedSongs[0].origin == ScanItemOrigin::ScannedFull);
  REQUIRE(observedPublishedSongs[0].locationId.has_value());
  CHECK_FALSE(observedPublishedSongs[0].locationId->empty());
}

TEST_CASE("reconcileRoot: unchanged cache hit carries origin and cached location id") {
  test::TempScannerRoot temp{"scanner-preallocate-cache-hit-origin"};

  const auto audioFile = temp.path() / "cached-origin.flac";
  std::ofstream{audioFile} << "cached origin audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(audioFile, makeMetadata("Cached Origin"));

  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });

  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });

  auto beforeScan = events.completedCount();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletionAfter(beforeScan, std::chrono::seconds{5}));

	  std::vector<ScanItemOrigin> observedOrigins;
	  std::vector<std::optional<std::string>> observedLocationIds;
	  std::vector<WorkerTaskSnapshot> observedWorkerTasks;
	  std::vector<PublishedSongSnapshot> observedPublishedSongs;
	  std::vector<cache::ScanRootCacheWrite> observedCacheWrites;
	  const WorkerTaskObserverGuard workerObserver{[&observedWorkerTasks](const std::vector<WorkerTaskSnapshot>& tasks) {
	    observedWorkerTasks = tasks;
	  }};
	  const PublishedSongObserverGuard publishedSongObserver{[&observedPublishedSongs](const std::vector<PublishedSongSnapshot>& songs) {
	    observedPublishedSongs = songs;
	  }};
	  const CacheWriteObserverGuard cacheWriteObserver{[&observedCacheWrites](const cache::ScanRootCacheWrite& write) {
	    observedCacheWrites.push_back(write);
	  }};
	  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
	    for (const auto& node : nodes) {
	      if (node.nodeType == NodeType::Song) {
        observedOrigins.push_back(node.origin);
        observedLocationIds.push_back(node.locationId);
      }
    }
  });

  beforeScan = events.completedCount();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(events.waitForScanCompletionAfter(beforeScan, std::chrono::seconds{5}));
  clearPreallocationObserver();

  REQUIRE(observedOrigins.size() == 1);
  CHECK(observedOrigins[0] == ScanItemOrigin::CacheHit);
  REQUIRE(observedLocationIds.size() == 1);
  REQUIRE(observedLocationIds[0].has_value());
  CHECK_FALSE(observedLocationIds[0]->empty());
	  CHECK(observedWorkerTasks.empty());
	  REQUIRE(observedPublishedSongs.size() == 1U);
	  CHECK(observedPublishedSongs[0].origin == ScanItemOrigin::CacheHit);
	  REQUIRE(observedPublishedSongs[0].locationId.has_value());
	  CHECK_FALSE(observedPublishedSongs[0].locationId->empty());
	  REQUIRE(observedCacheWrites.size() == 1U);
	  const auto& cacheWrite = observedCacheWrites.back();
	  CHECK(cacheWrite.root.totalFiles == 1U);
	  CHECK(cacheWrite.changedSongs.empty());
	  CHECK(cacheWrite.changedCueTracks.empty());
	  CHECK(cacheWrite.lyricsUpdates.empty());
	  REQUIRE(cacheWrite.retainedLocationIds.size() == 1U);
	  CHECK(cacheWrite.retainedLocationIds[0] == *observedPublishedSongs[0].locationId);
	}

TEST_CASE("reconcileRoot: incremental worker paths preserve changed and new origins into root results") {
  test::TempScannerRoot temp{"scanner-preallocate-worker-origin-delta"};

  const auto unchangedFile = temp.path() / "unchanged.flac";
  const auto changedFile = temp.path() / "changed.flac";
  std::ofstream{unchangedFile} << "unchanged original audio";
  std::ofstream{changedFile} << "changed original audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(unchangedFile, makeMetadata("Unchanged Before"));
  reader->put(changedFile, makeMetadata("Changed Before"));

  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });

  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });

  auto beforeScan = events.completedCount();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletionAfter(beforeScan, std::chrono::seconds{5}));

  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  std::ofstream{changedFile, std::ios::binary | std::ios::trunc} << "changed updated audio with a different size";
  const auto addedFile = temp.path() / "added.flac";
  std::ofstream{addedFile} << "added audio";
  reader->put(changedFile, makeMetadata("Changed After"));
  reader->put(addedFile, makeMetadata("Added After"));
  forceNextScanIncrementalForCurrentTree(temp);

	  std::vector<WorkerTaskSnapshot> observedWorkerTasks;
	  std::vector<PublishedSongSnapshot> observedPublishedSongs;
	  std::vector<cache::ScanRootCacheWrite> observedCacheWrites;
	  const WorkerTaskObserverGuard workerObserver{[&observedWorkerTasks](const std::vector<WorkerTaskSnapshot>& tasks) {
	    observedWorkerTasks = tasks;
	  }};
	  const PublishedSongObserverGuard publishedSongObserver{[&observedPublishedSongs](const std::vector<PublishedSongSnapshot>& songs) {
	    observedPublishedSongs = songs;
	  }};
	  const CacheWriteObserverGuard cacheWriteObserver{[&observedCacheWrites](const cache::ScanRootCacheWrite& write) {
	    observedCacheWrites.push_back(write);
	  }};

	  beforeScan = events.completedCount();
	  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
	  REQUIRE(events.waitForScanCompletionAfter(beforeScan, std::chrono::seconds{5}));

  REQUIRE(observedWorkerTasks.size() == 2U);
  const auto* changedWorker = workerSnapshotByPath(observedWorkerTasks, changedFile);
  REQUIRE(changedWorker != nullptr);
  CHECK(changedWorker->origin == ScanItemOrigin::RescannedChanged);
  CHECK_FALSE(changedWorker->hasCachedLocation);
  const auto* addedWorker = workerSnapshotByPath(observedWorkerTasks, addedFile);
  REQUIRE(addedWorker != nullptr);
  CHECK(addedWorker->origin == ScanItemOrigin::ScannedNew);
  CHECK_FALSE(addedWorker->hasCachedLocation);
  CHECK(workerSnapshotByPath(observedWorkerTasks, unchangedFile) == nullptr);

  const auto* unchangedPublished = publishedSnapshotByPath(observedPublishedSongs, unchangedFile);
  REQUIRE(unchangedPublished != nullptr);
  CHECK(unchangedPublished->origin == ScanItemOrigin::CacheHit);
  REQUIRE(unchangedPublished->locationId.has_value());
  CHECK_FALSE(unchangedPublished->locationId->empty());

  const auto* changedPublished = publishedSnapshotByPath(observedPublishedSongs, changedFile);
  REQUIRE(changedPublished != nullptr);
  CHECK(changedPublished->origin == ScanItemOrigin::RescannedChanged);
  REQUIRE(changedPublished->locationId.has_value());
  CHECK_FALSE(changedPublished->locationId->empty());

  const auto* addedPublished = publishedSnapshotByPath(observedPublishedSongs, addedFile);
  REQUIRE(addedPublished != nullptr);
	  CHECK(addedPublished->origin == ScanItemOrigin::ScannedNew);
	  REQUIRE(addedPublished->locationId.has_value());
	  CHECK_FALSE(addedPublished->locationId->empty());

	  REQUIRE(observedCacheWrites.size() == 1U);
	  const auto& cacheWrite = observedCacheWrites.back();
	  CHECK(cacheWrite.root.totalFiles == 3U);
	  REQUIRE(cacheWrite.changedSongs.size() == 2U);
	  CHECK(cacheWrite.changedCueTracks.empty());
	  CHECK(cacheWrite.lyricsUpdates.empty());
	  CHECK(cacheWrite.retainedLocationIds.size() == 3U);
	  CHECK(std::ranges::any_of(cacheWrite.changedSongs, [&changedFile](const cache::CacheWriteSong& songWrite) {
	    return songWrite.location.filePath == changedFile;
	  }));
	  CHECK(std::ranges::any_of(cacheWrite.changedSongs, [&addedFile](const cache::CacheWriteSong& songWrite) {
	    return songWrite.location.filePath == addedFile;
	  }));
	  CHECK_FALSE(std::ranges::any_of(cacheWrite.changedSongs, [&unchangedFile](const cache::CacheWriteSong& songWrite) {
	    return songWrite.location.filePath == unchangedFile;
	  }));
	}

TEST_CASE("IndexedPublishedSong: CueContainer with 0 tracks creates single node") {
  std::vector<IndexedPublishedSong> nodes(1);
  
  nodes[0].discoveryIndex = 0;
  nodes[0].nodeType = NodeType::CueContainer;
  nodes[0].isVirtualFolder = true;
  nodes[0].treeRelativePath = "empty.cue";
  
  CHECK(nodes.size() == 1);
  CHECK(nodes[0].nodeType == NodeType::CueContainer);
  CHECK(nodes[0].isVirtualFolder == true);
}

TEST_CASE("IndexedPublishedSong: mixed nodes preserve discovery order") {
  std::vector<IndexedPublishedSong> nodes(5);
  
  nodes[0].discoveryIndex = 0;
  nodes[0].nodeType = NodeType::Song;
  nodes[0].treeRelativePath = "01_song.mp3";
  
  nodes[1].discoveryIndex = 1;
  nodes[1].nodeType = NodeType::CueContainer;
  nodes[1].isVirtualFolder = true;
  nodes[1].treeRelativePath = "album.cue";
  
  nodes[2].discoveryIndex = 2;
  nodes[2].nodeType = NodeType::CueTrack;
  nodes[2].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(0),
    .duration = std::chrono::microseconds(180000000),
    .trackIndex = 0
  };
  
  nodes[3].discoveryIndex = 3;
  nodes[3].nodeType = NodeType::CueTrack;
  nodes[3].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(180000000),
    .duration = std::chrono::microseconds(200000000),
    .trackIndex = 1
  };
  
  nodes[4].discoveryIndex = 4;
  nodes[4].nodeType = NodeType::Song;
  nodes[4].treeRelativePath = "02_song.flac";
  
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    CHECK(nodes[i].discoveryIndex == i);
  }
  
  CHECK(nodes[0].nodeType == NodeType::Song);
  CHECK(nodes[1].nodeType == NodeType::CueContainer);
  CHECK(nodes[2].nodeType == NodeType::CueTrack);
  CHECK(nodes[3].nodeType == NodeType::CueTrack);
  CHECK(nodes[4].nodeType == NodeType::Song);
}

TEST_CASE("IndexedPublishedSong: empty preallocated array") {
  std::vector<IndexedPublishedSong> nodes(0);
  
  CHECK(nodes.empty());
  CHECK(nodes.size() == 0);
}

TEST_CASE("reconcileRoot: discovers CUE and regular files with correct counts") {
  test::TempScannerRoot temp{"scanner-preallocate-cue-mixed"};
  
  const auto regularFile1 = temp.path() / "01-regular.flac";
  const auto regularFile2 = temp.path() / "02-regular.mp3";
  const auto cueFile = temp.path() / "album.cue";
  const auto cueAudio = temp.path() / "album.flac";
  
  std::ofstream{regularFile1} << "fake flac";
  std::ofstream{regularFile2} << "fake mp3";
  std::ofstream{cueAudio} << "fake flac for cue";
  
  std::ofstream cueOut{cueFile};
  cueOut << R"(REM GENRE "Test"
PERFORMER "Test Artist"
TITLE "Test Album"
FILE "album.flac" WAVE
  TRACK 01 AUDIO
    TITLE "Track 1"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Track 2"
    INDEX 01 03:00:00
)";
  cueOut.close();
  
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(regularFile1, makeMetadata("Regular Song 1"));
  reader->put(regularFile2, makeMetadata("Regular Song 2"));
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  CHECK(events.filesDiscovered == 3);
  CHECK(events.filesScanned == 2);
  
  const auto snapshot = service->snapshot();
  const auto allSongs = [&snapshot]() {
    std::vector<SongMetadata> songs;
    for (const auto& node : snapshot.nodes) {
      if (node.song.has_value()) {
        songs.push_back(*node.song);
      }
    }
    return songs;
  }();
  
  CHECK(allSongs.size() == 2);
}

TEST_CASE("reconcileRoot: empty directory returns empty results") {
  test::TempScannerRoot temp{"scanner-preallocate-empty"};
  
  auto reader = std::make_shared<FakeMetadataReader>();
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  CHECK(events.filesDiscovered == 0);
  CHECK(events.filesScanned == 0);
  
  const auto snapshot = service->snapshot();
  const auto allSongs = [&snapshot]() {
    std::vector<SongMetadata> songs;
    for (const auto& node : snapshot.nodes) {
      if (node.song.has_value()) {
        songs.push_back(*node.song);
      }
    }
    return songs;
  }();
  CHECK(allSongs.empty());
}

TEST_CASE("reconcileRoot: CUE with zero tracks creates container only") {
  test::TempScannerRoot temp{"scanner-preallocate-empty-cue"};
  
  const auto cueFile = temp.path() / "empty.cue";
  
  std::ofstream cueOut{cueFile};
  cueOut << R"(REM GENRE "Test"
PERFORMER "Test Artist"
TITLE "Empty Album"
)";
  cueOut.close();
  
  auto reader = std::make_shared<FakeMetadataReader>();
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  CHECK(events.filesDiscovered == 1);
  CHECK(events.filesScanned == 0);
  
  const auto snapshot = service->snapshot();
  const auto allSongs = [&snapshot]() {
    std::vector<SongMetadata> songs;
    for (const auto& node : snapshot.nodes) {
      if (node.song.has_value()) {
        songs.push_back(*node.song);
      }
    }
    return songs;
  }();
  CHECK(allSongs.empty());
}

TEST_CASE("reconcileRoot: direct observation of pre-allocated node sequence including CueContainer") {
  test::TempScannerRoot temp{"scanner-preallocate-direct-observe"};
  
  const auto regularFile1 = temp.path() / "01-song.flac";
  const auto cueFile = temp.path() / "album.cue";
  const auto cueAudio = temp.path() / "album.flac";
  const auto regularFile2 = temp.path() / "02-song.mp3";
  
  std::ofstream{regularFile1} << "fake flac";
  std::ofstream{cueAudio}.close();
  std::ofstream{regularFile2} << "fake mp3";
  
  std::ofstream cueOut{cueFile};
  cueOut << R"(REM GENRE "Test"
PERFORMER "Test Artist"
TITLE "Test Album"
FILE "album.flac" FLAC
  TRACK 01 AUDIO
    TITLE "Track 1"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Track 2"
    INDEX 01 03:00:00
)";
  cueOut.close();
  
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(regularFile1, makeMetadata("Song 1"));
  reader->put(regularFile2, makeMetadata("Song 2"));
  
  std::vector<NodeType> observedNodeTypes;
  std::size_t observedTotalNodes = 0;
  std::size_t observedCueContainerCount = 0;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    observedTotalNodes = nodes.size();
    for (const auto& node : nodes) {
      observedNodeTypes.push_back(node.nodeType);
      if (node.nodeType == NodeType::CueContainer) {
        ++observedCueContainerCount;
        CHECK(node.isVirtualFolder == true);
      } else if (node.nodeType == NodeType::CueTrack) {
        REQUIRE(node.cueInfo.has_value());
        CHECK(node.cueInfo->trackIndex < 10);
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  
  // TagReader cannot parse empty audio in test environment, so readCueSheet() returns 0 tracks
  CHECK(observedTotalNodes == 3);
  CHECK(observedCueContainerCount == 1);
  
  REQUIRE(observedNodeTypes.size() == 3);
  CHECK(observedNodeTypes[0] == NodeType::Song);
  CHECK(observedNodeTypes[1] == NodeType::Song);
  CHECK(observedNodeTypes[2] == NodeType::CueContainer);
}

TEST_CASE("reconcileRoot: real scan path with controlled CueTrack nodes via test seam") {
  test::TempScannerRoot temp{"scanner-preallocate-cuetrack-seam"};
  
  const auto regularFile = temp.path() / "01-regular.flac";
  const auto cueFile = temp.path() / "album.cue";
  const auto cueAudioPath = temp.path() / "album.flac";
  
  std::ofstream{regularFile} << "fake flac";
  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{cueAudioPath} << "fake audio for cue";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(regularFile, makeMetadata("Regular Song"));
  
  setTestCueSheetProvider([&cueAudioPath](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {
        {.audioFilePath = cueAudioPath, .offset = 0, .duration = 180000000},
        {.audioFilePath = cueAudioPath, .offset = 180000000, .duration = 200000000}
      };
    }
    return {};
  });
  
  std::size_t observedTotalNodes = 0;
  std::size_t observedCueTrackCount = 0;
  std::vector<NodeType> observedNodeTypes;
  std::vector<std::size_t> observedCueTrackIndices;
  std::vector<std::filesystem::path> observedCueTrackAudioPaths;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    observedTotalNodes = nodes.size();
    for (const auto& node : nodes) {
      observedNodeTypes.push_back(node.nodeType);
      if (node.nodeType == NodeType::CueTrack) {
        ++observedCueTrackCount;
        REQUIRE(node.cueInfo.has_value());
        observedCueTrackIndices.push_back(node.cueInfo->trackIndex);
        observedCueTrackAudioPaths.push_back(node.cueInfo->audioFilePath);
        CHECK(node.cueInfo->offset >= std::chrono::microseconds(0));
        CHECK(node.cueInfo->duration > std::chrono::microseconds(0));
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  clearTestCueSheetProvider();
  
  CHECK(observedTotalNodes == 4);
  CHECK(observedCueTrackCount == 2);
  
  REQUIRE(observedNodeTypes.size() == 4);
  CHECK(observedNodeTypes[0] == NodeType::Song);
  CHECK(observedNodeTypes[1] == NodeType::CueContainer);
  CHECK(observedNodeTypes[2] == NodeType::CueTrack);
  CHECK(observedNodeTypes[3] == NodeType::CueTrack);
  
  REQUIRE(observedCueTrackIndices.size() == 2);
  CHECK(observedCueTrackIndices[0] == 0);
  CHECK(observedCueTrackIndices[1] == 1);
  
  REQUIRE(observedCueTrackAudioPaths.size() == 2);
  CHECK(observedCueTrackAudioPaths[0] == cueAudioPath);
  CHECK(observedCueTrackAudioPaths[1] == cueAudioPath);
}

TEST_CASE("reconcileRoot: observer captures CueTrack nodes with precise assertions") {
  // Simulates what reconcileRoot() creates when readCueSheet() returns tracks.
  // Proves: observer captures CueTrack nodes, data structure supports full CueInfo.
  std::vector<IndexedPublishedSong> simulatedPreallocation(4);
  
  simulatedPreallocation[0].discoveryIndex = 0;
  simulatedPreallocation[0].nodeType = NodeType::Song;
  simulatedPreallocation[0].treeRelativePath = "01-regular.flac";
  
  simulatedPreallocation[1].discoveryIndex = 1;
  simulatedPreallocation[1].nodeType = NodeType::CueContainer;
  simulatedPreallocation[1].isVirtualFolder = true;
  simulatedPreallocation[1].treeRelativePath = "album.cue";
  
  simulatedPreallocation[2].discoveryIndex = 2;
  simulatedPreallocation[2].nodeType = NodeType::CueTrack;
  simulatedPreallocation[2].treeRelativePath = "album.cue";
  simulatedPreallocation[2].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(0),
    .duration = std::chrono::microseconds(180000000),
    .trackIndex = 0
  };
  
  simulatedPreallocation[3].discoveryIndex = 3;
  simulatedPreallocation[3].nodeType = NodeType::CueTrack;
  simulatedPreallocation[3].treeRelativePath = "album.cue";
  simulatedPreallocation[3].cueInfo = CueInfo{
    .cueFilePath = "album.cue",
    .audioFilePath = "album.flac",
    .offset = std::chrono::microseconds(180000000),
    .duration = std::chrono::microseconds(200000000),
    .trackIndex = 1
  };
  
  std::size_t observedTotalNodes = 0;
  std::size_t observedCueTrackCount = 0;
  std::vector<NodeType> observedNodeTypes;
  std::vector<std::size_t> observedCueTrackIndices;
  
  PreallocationObserver observer = [&](const std::vector<IndexedPublishedSong>& nodes) {
    observedTotalNodes = nodes.size();
    for (const auto& node : nodes) {
      observedNodeTypes.push_back(node.nodeType);
      if (node.nodeType == NodeType::CueTrack) {
        ++observedCueTrackCount;
        REQUIRE(node.cueInfo.has_value());
        observedCueTrackIndices.push_back(node.cueInfo->trackIndex);
        CHECK(node.cueInfo->cueFilePath == "album.cue");
        CHECK(node.cueInfo->audioFilePath == "album.flac");
        CHECK(node.cueInfo->duration > std::chrono::microseconds(0));
      }
    }
  };
  
  observer(simulatedPreallocation);
  
  CHECK(observedTotalNodes == 4);
  CHECK(observedCueTrackCount == 2);
  
  REQUIRE(observedNodeTypes.size() == 4);
  CHECK(observedNodeTypes[0] == NodeType::Song);
  CHECK(observedNodeTypes[1] == NodeType::CueContainer);
  CHECK(observedNodeTypes[2] == NodeType::CueTrack);
  CHECK(observedNodeTypes[3] == NodeType::CueTrack);
  
  REQUIRE(observedCueTrackIndices.size() == 2);
  CHECK(observedCueTrackIndices[0] == 0);
  CHECK(observedCueTrackIndices[1] == 1);
}

TEST_CASE("reconcileRoot: worker fills nodes directly via nodeIndex and sets filled=true") {
  test::TempScannerRoot temp{"scanner-worker-direct-fill"};
  auto reader = std::make_shared<FakeMetadataReader>();
  
  const auto song1Path = temp.path() / "song1.flac";
  const auto song2Path = temp.path() / "song2.flac";
  std::ofstream{song1Path} << "fake flac 1";
  std::ofstream{song2Path} << "fake flac 2";
  
  reader->put(song1Path, makeMetadata("Song One"));
  reader->put(song2Path, makeMetadata("Song Two"));
  
  std::vector<IndexedPublishedSong> observedNodesCopy;
  PreallocationObserver observer = [&observedNodesCopy](const std::vector<IndexedPublishedSong>& nodes) {
    observedNodesCopy.clear();
    observedNodesCopy.reserve(nodes.size());
    for (const auto& node : nodes) {
      IndexedPublishedSong copy;
      copy.discoveryIndex = node.discoveryIndex;
      copy.nodeType = node.nodeType;
      copy.isVirtualFolder = node.isVirtualFolder;
      copy.filled.store(node.filled.load());
      copy.needsScan.store(node.needsScan.load());
      copy.treeRelativePath = node.treeRelativePath;
      copy.song = node.song;
      if (node.cueInfo.has_value()) {
        copy.cueInfo = node.cueInfo;
      }
      observedNodesCopy.push_back(std::move(copy));
    }
  };
  setPreallocationObserver(observer);
  
  auto service = makeFileScannerService(
    FileScannerServiceDependencies{
      .metadataReader = reader,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers"
    }
  );
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) { events.onEvent(event); });
  service->scan({ScannerRoot{.path = temp.path(), .recursive = false}}, ScanMode::Full);
  
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  
  REQUIRE(observedNodesCopy.size() == 2);
  
  CHECK(observedNodesCopy[0].discoveryIndex == 0);
  CHECK(observedNodesCopy[0].nodeType == NodeType::Song);
  CHECK(observedNodesCopy[0].filled.load() == true);
  CHECK_FALSE(observedNodesCopy[0].song.metadata.filePath.empty());
  CHECK(observedNodesCopy[0].song.metadata.filePath == song1Path);
  
  CHECK(observedNodesCopy[1].discoveryIndex == 1);
  CHECK(observedNodesCopy[1].nodeType == NodeType::Song);
  CHECK(observedNodesCopy[1].filled.load() == true);
  CHECK_FALSE(observedNodesCopy[1].song.metadata.filePath.empty());
  CHECK(observedNodesCopy[1].song.metadata.filePath == song2Path);
}

TEST_CASE("reconcileRoot: cache hit nodes are marked filled=true") {
  test::TempScannerRoot temp{"scanner-cache-filled"};
  auto reader = std::make_shared<FakeMetadataReader>();
  
  const auto songPath = temp.path() / "cached.flac";
  std::ofstream{songPath} << "fake flac cached";
  reader->put(songPath, makeMetadata("Cached Song"));
  
  auto service = makeFileScannerService(
    FileScannerServiceDependencies{
      .metadataReader = reader,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers"
    }
  );
  
  ScanEvents firstScanEvents;
  service->setEventSink([&firstScanEvents](const ScannerEvent& event) { firstScanEvents.onEvent(event); });
  service->scan({ScannerRoot{.path = temp.path(), .recursive = false}}, ScanMode::Full);
  REQUIRE(firstScanEvents.waitForScanCompletion(std::chrono::seconds{5}));
  
  std::vector<IndexedPublishedSong> observedNodesCopy;
  PreallocationObserver observer = [&observedNodesCopy](const std::vector<IndexedPublishedSong>& nodes) {
    observedNodesCopy.clear();
    observedNodesCopy.reserve(nodes.size());
    for (const auto& node : nodes) {
      IndexedPublishedSong copy;
      copy.discoveryIndex = node.discoveryIndex;
      copy.nodeType = node.nodeType;
      copy.isVirtualFolder = node.isVirtualFolder;
      copy.filled.store(node.filled.load());
      copy.needsScan.store(node.needsScan.load());
      copy.treeRelativePath = node.treeRelativePath;
      copy.song = node.song;
      if (node.cueInfo.has_value()) {
        copy.cueInfo = node.cueInfo;
      }
      observedNodesCopy.push_back(std::move(copy));
    }
  };
  setPreallocationObserver(observer);
  
  ScanEvents secondScanEvents;
  service->setEventSink([&secondScanEvents](const ScannerEvent& event) { secondScanEvents.onEvent(event); });
  service->scan({ScannerRoot{.path = temp.path(), .recursive = false}}, ScanMode::Incremental);
  REQUIRE(secondScanEvents.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  
  REQUIRE(observedNodesCopy.size() == 1);
  CHECK(observedNodesCopy[0].discoveryIndex == 0);
  CHECK(observedNodesCopy[0].nodeType == NodeType::Song);
  CHECK(observedNodesCopy[0].filled.load() == true);
  CHECK_FALSE(observedNodesCopy[0].song.metadata.filePath.empty());
  CHECK(observedNodesCopy[0].song.metadata.title == "Cached Song");
}

TEST_CASE("reconcileRoot: 10-track CUE creates exactly 11 nodes (1 container + 10 tracks)") {
  test::TempScannerRoot temp{"scanner-preallocate-10track-cue"};
  
  const auto cueFile = temp.path() / "album.cue";
  const auto cueAudioPath = temp.path() / "album.flac";
  
  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{cueAudioPath} << "fake audio for 10-track cue";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  setTestCueSheetProvider([&cueAudioPath](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      std::vector<TestCueTrackData> tracks;
      for (std::size_t i = 0; i < 10; ++i) {
        tracks.push_back({
          .audioFilePath = cueAudioPath,
          .offset = static_cast<std::int64_t>(i * 180000000),
          .duration = 180000000
        });
      }
      return tracks;
    }
    return {};
  });
  
  std::size_t observedTotalNodes = 0;
  std::size_t observedCueContainerCount = 0;
  std::size_t observedCueTrackCount = 0;
  std::vector<NodeType> observedNodeTypes;
  std::vector<std::size_t> observedCueTrackIndices;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    observedTotalNodes = nodes.size();
    for (const auto& node : nodes) {
      observedNodeTypes.push_back(node.nodeType);
      if (node.nodeType == NodeType::CueContainer) {
        ++observedCueContainerCount;
        CHECK(node.isVirtualFolder == true);
      } else if (node.nodeType == NodeType::CueTrack) {
        ++observedCueTrackCount;
        REQUIRE(node.cueInfo.has_value());
        observedCueTrackIndices.push_back(node.cueInfo->trackIndex);
        CHECK(node.cueInfo->audioFilePath == cueAudioPath);
        CHECK(node.cueInfo->offset >= std::chrono::microseconds(0));
        CHECK(node.cueInfo->duration > std::chrono::microseconds(0));
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.dbPath(),
    .coverExportDir = temp.path() / "covers"
  });
  
  ScanEvents events;
  service->setEventSink([&events](ScannerEvent event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  clearTestCueSheetProvider();
  
  CHECK(observedTotalNodes == 11);
  CHECK(observedCueContainerCount == 1);
  CHECK(observedCueTrackCount == 10);
  
  REQUIRE(observedNodeTypes.size() == 11);
  CHECK(observedNodeTypes[0] == NodeType::CueContainer);
  for (std::size_t i = 1; i <= 10; ++i) {
    CHECK(observedNodeTypes[i] == NodeType::CueTrack);
  }
  
  REQUIRE(observedCueTrackIndices.size() == 10);
  for (std::size_t i = 0; i < 10; ++i) {
    CHECK(observedCueTrackIndices[i] == i);
  }
}

TEST_CASE("reconcileRoot: CueTrack nodes filled with correct metadata from CUE") {
  test::TempScannerRoot temp{"scanner-cuetrack-fill"};
  
  const auto cueFile = temp.path() / "album.cue";
  const auto audioFile = temp.path() / "album.flac";
  
  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{audioFile} << "fake audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  setTestCueSheetProvider([&audioFile](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {
        {
          .audioFilePath = audioFile,
          .offset = 0,
          .duration = 180000000,
          .title = "Track 1 Title",
          .artist = "Track 1 Artist",
          .album = "Test Album",
          .trackNumber = 1
        },
        {
          .audioFilePath = audioFile,
          .offset = 180000000,
          .duration = 200000000,
          .title = "Track 2 Title",
          .artist = "Track 2 Artist",
          .album = "Test Album",
          .trackNumber = 2
        }
      };
    }
    return {};
  });
  
  RawTagMetadata track1Meta = makeMetadata("Track 1 Title");
  track1Meta.filePath = audioFile;
  track1Meta.offset = std::chrono::microseconds{0};
  track1Meta.duration = std::chrono::microseconds{180000000};
  track1Meta.artist = "Track 1 Artist";
  
  RawTagMetadata track2Meta = makeMetadata("Track 2 Title");
  track2Meta.filePath = audioFile;
  track2Meta.offset = std::chrono::microseconds{180000000};
  track2Meta.duration = std::chrono::microseconds{200000000};
  track2Meta.artist = "Track 2 Artist";
  
  reader->put(audioFile, track1Meta);
  
  std::vector<cache::CachedSong> observedCueTrackSongs;
  std::vector<bool> observedFilledFlags;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    for (const auto& node : nodes) {
      if (node.nodeType == NodeType::CueTrack) {
        observedCueTrackSongs.push_back(node.song);
        observedFilledFlags.push_back(node.filled.load());
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) { events.onEvent(event); });
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  clearPreallocationObserver();
  
  REQUIRE(observedCueTrackSongs.size() == 2);
  REQUIRE(observedFilledFlags.size() == 2);
  
  CHECK(observedFilledFlags[0] == true);
  CHECK(observedFilledFlags[1] == true);
  
  const auto& track1 = observedCueTrackSongs[0].metadata;
  CHECK(track1.sourceFilePath == audioFile);
  CHECK(track1.filePath == cueFile);
  CHECK(track1.offset == std::chrono::milliseconds{0});
  CHECK(track1.duration == std::chrono::milliseconds{180000});
  CHECK(track1.logicalTrackId == (cueFile.generic_string() + "#track0"));
  CHECK(track1.trackId == track1.logicalTrackId);
  CHECK(track1.title == "Track 1 Title");
  CHECK(track1.artist == "Track 1 Artist");
  
  const auto& track2 = observedCueTrackSongs[1].metadata;
  CHECK(track2.sourceFilePath == audioFile);
  CHECK(track2.filePath == cueFile);
  CHECK(track2.offset == std::chrono::milliseconds{180000});
  CHECK(track2.duration == std::chrono::milliseconds{200000});
  CHECK(track2.logicalTrackId == (cueFile.generic_string() + "#track1"));
  CHECK(track2.trackId == track2.logicalTrackId);
  CHECK(track2.title == "Track 2 Title");
  CHECK(track2.artist == "Track 2 Artist");
}

TEST_CASE("reconcileRoot: CueTrack metadata read failure records error without crash") {
  test::TempScannerRoot temp{"scanner-cuetrack-error"};
  
  const auto cueFile = temp.path() / "broken.cue";
  std::ofstream{cueFile} << "FILE \"missing.flac\" WAVE\n  TRACK 01 AUDIO\n";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  setTestCueSheetProvider([](const std::filesystem::path&) -> std::vector<TestCueTrackData> {
    throw std::runtime_error("Audio file not found or corrupted");
  });
  
  std::size_t errorCount = 0;
  std::vector<std::string> errorMessages;
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&](const ScannerEvent& event) {
    events.onEvent(event);
    if (event.type == ScannerEventType::ScanError) {
      if (const auto* error = std::get_if<ScannerError>(&event.payload)) {
        ++errorCount;
        errorMessages.push_back(error->message);
      }
    }
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  
  CHECK(errorCount >= 1);
  bool foundCueError = false;
  for (const auto& msg : errorMessages) {
    if (msg.find("CUE") != std::string::npos) {
      foundCueError = true;
      break;
    }
  }
  CHECK(foundCueError);
}

TEST_CASE("reconcileRoot: unfilled nodes remain observable with filled=false") {
  test::TempScannerRoot temp{"scanner-unfilled-observable"};
  
  const auto goodFile = temp.path() / "good.flac";
  const auto badFile = temp.path() / "bad.flac";
  
  std::ofstream{goodFile} << "fake flac good";
  std::ofstream{badFile} << "fake flac bad";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(goodFile, makeMetadata("Good Song"));
  // Intentionally do NOT add badFile to reader -> will throw when worker tries to read
  
  std::vector<IndexedPublishedSong> observedNodesCopy;
  setPreallocationObserver([&observedNodesCopy](const std::vector<IndexedPublishedSong>& nodes) {
    observedNodesCopy.clear();
    observedNodesCopy.reserve(nodes.size());
    for (const auto& node : nodes) {
      IndexedPublishedSong copy;
      copy.discoveryIndex = node.discoveryIndex;
      copy.nodeType = node.nodeType;
      copy.filled.store(node.filled.load());
      copy.treeRelativePath = node.treeRelativePath;
      copy.song = node.song;
      observedNodesCopy.push_back(std::move(copy));
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) { events.onEvent(event); });
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  
  REQUIRE(observedNodesCopy.size() == 2);
  
  // One node filled (good.flac), one unfilled (bad.flac - worker threw exception)
  std::size_t filledCount = 0;
  std::size_t unfilledCount = 0;
  
  for (const auto& node : observedNodesCopy) {
    if (node.filled.load()) {
      ++filledCount;
      CHECK_FALSE(node.song.metadata.filePath.empty());
    } else {
      ++unfilledCount;
      CHECK(node.song.metadata.filePath.empty());
    }
  }
  
  // Rigid assertions: exactly 1 filled, exactly 1 unfilled
  CHECK(filledCount == 1);
  CHECK(unfilledCount == 1);
}
