#include "scanner_test_harness.h"

#include "file_scanner_orchestrator_test_access.h"
#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"

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

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override {
    {
      std::lock_guard lock{mutex_};
      requestedPaths.push_back(path);
      requestedCoverDirs.push_back(coverExportDir);
    }
    const auto iterator = metadataByPath_.find(path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata");
    }
    auto metadata = iterator->second;
    metadata.filePath = path;
    return metadata;
  }

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

}
}
