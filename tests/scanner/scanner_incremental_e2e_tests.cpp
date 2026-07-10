#include "scanner_test_harness.h"

#include "file_scanner_orchestrator_test_access.h"
#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
#include "seriona/scanner/directory_tree_hash.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
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
  return std::filesystem::path{temp.dbPath().generic_string() + ".scan-roots-v3.sqlite"};
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
  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
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

[[nodiscard]] const SongMetadata& songByPath(const std::vector<SongMetadata>& songs, const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(songs, path, &SongMetadata::filePath);
  if (iterator == songs.end()) {
    throw std::runtime_error("missing song in incremental e2e test");
  }
  return *iterator;
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
  CHECK(reader->readCount() == 5U);
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
  CHECK(reader->readCount() == 5U);
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

  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
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

TEST_CASE("scanner incremental e2e characterizes cache-hit FileScanned events") {
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
  REQUIRE(fileScannedSongs.size() == 2U);
  CHECK(fileScannedSongs[0].filePath == audio);
  CHECK(fileScannedSongs[1].filePath == audio);
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

  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
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
  const auto fullReadCount = reader->readCount();

  auto songs = songsIn(full.snapshot);
  REQUIRE(songs.size() == 3U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(songByPath(songs, standalone).title == "Bonus Track");
  std::vector<SongMetadata> cueTracks;
  for (const auto& song : songs) {
    if (song.filePath == cueFile) {
      cueTracks.push_back(song);
    }
  }
  std::ranges::sort(cueTracks, {}, &SongMetadata::offset);
  REQUIRE(cueTracks.size() == 2U);
  CHECK(cueTracks[0].sourceFilePath == referencedAudio);
  CHECK(cueTracks[0].logicalTrackId == cueFile.generic_string() + "#track0");
  CHECK(cueTracks[0].trackId == cueTracks[0].logicalTrackId);
  CHECK(cueTracks[0].offset == std::chrono::milliseconds{0});
  CHECK(cueTracks[0].duration == std::chrono::milliseconds{180000});
  CHECK(cueTracks[1].sourceFilePath == referencedAudio);
  CHECK(cueTracks[1].logicalTrackId == cueFile.generic_string() + "#track1");
  CHECK(cueTracks[1].trackId == cueTracks[1].logicalTrackId);
  CHECK(cueTracks[1].offset == std::chrono::milliseconds{180000});
  CHECK(cueTracks[1].duration == std::chrono::milliseconds{200000});

  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  auto locations = sidecar.loadLocationsByRoot(rootPath);
  CHECK(std::ranges::count(locations, cueFile, &cache::CachedLocation::filePath) == 2U);
  CHECK(std::ranges::any_of(locations, [&standalone](const cache::CachedLocation& location) {
    return location.filePath == standalone;
  }));

  const auto incremental = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 3U;
  });

  songs = songsIn(incremental.snapshot);
  REQUIRE(songs.size() == 3U);
  CHECK(std::ranges::count(songs, cueFile, &SongMetadata::filePath) == 2U);
  CHECK(songByPath(songs, standalone).title == "Bonus Track");
  CHECK(reader->readCount() == fullReadCount);
}

}
}
