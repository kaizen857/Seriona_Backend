#include "scanner_test_harness.h"

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

private:
  mutable std::mutex mutex_;
  std::vector<ScannerEvent> events_;
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
  const auto completedBefore = eventLog.scanCompletedCount();
  const auto startedAt = std::chrono::steady_clock::now();
  service.scan({ScannerRoot{.path = root}}, mode);
  for (auto attempts = 0; attempts < 2000; ++attempts) {
    auto snapshot = service.snapshot();
    if (eventLog.scanCompletedCount() > completedBefore && predicate(snapshot)) {
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

  const auto unchanged = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental, [](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == 3U;
  });
  songs = songsIn(unchanged.snapshot);

  REQUIRE(songs.size() == 3U);
  CHECK(reader->readCount() == 3U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed Before");
  CHECK(songByPath(songs, deleted).title == "Deleted");

  writeText(changed, "changed bytes for phase 3 e2e");
  reader->put(changed, rawMetadata("Changed After"));
  const auto added = test::writeAudioFixture(temp.path(), "04-added.flac");
  reader->put(added, rawMetadata("Added"));
  forceNextScanIncrementalForCurrentTree(temp);
  const auto addedAndChanged = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                              [&added, &changed](const PlaylistTreeSnapshot& snapshot) {
                                                const auto currentSongs = songsIn(snapshot);
                                                return currentSongs.size() == 4U && songByPath(currentSongs, changed).title == "Changed After" &&
                                                       songByPath(currentSongs, added).title == "Added";
                                              });
  songs = songsIn(addedAndChanged.snapshot);

  REQUIRE(songs.size() == 4U);
  CHECK(reader->readCount() == 5U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed After");
  CHECK(songByPath(songs, deleted).title == "Deleted");
  CHECK(songByPath(songs, added).title == "Added");

  std::filesystem::remove(deleted);
  forceNextScanIncrementalForCurrentTree(temp);
  const auto deletion = runScanAndWait(*service, eventLog, temp.path(), ScanMode::Incremental,
                                       [&deleted](const PlaylistTreeSnapshot& snapshot) {
                                         const auto currentSongs = songsIn(snapshot);
                                         return currentSongs.size() == 3U &&
                                                std::ranges::none_of(currentSongs, [&deleted](const SongMetadata& song) { return song.filePath == deleted; });
                                       });
  songs = songsIn(deletion.snapshot);

  REQUIRE(songs.size() == 3U);
  CHECK(reader->readCount() == 5U);
  CHECK(songByPath(songs, stable).title == "Stable");
  CHECK(songByPath(songs, changed).title == "Changed After");
  CHECK(songByPath(songs, added).title == "Added");
  CHECK(std::ranges::none_of(songs, [&deleted](const SongMetadata& song) { return song.filePath == deleted; }));

  const auto fileScannedSongs = eventLog.fileScannedSongs();
  REQUIRE(fileScannedSongs.size() == 13U);
  CHECK(fileScannedSongs[0].filePath == stable);
  CHECK(fileScannedSongs[1].filePath == changed);
  CHECK(fileScannedSongs[2].filePath == deleted);
  CHECK(fileScannedSongs[3].filePath == stable);
  CHECK(fileScannedSongs[4].filePath == changed);
  CHECK(fileScannedSongs[5].filePath == deleted);
  CHECK(fileScannedSongs[6].filePath == stable);
  CHECK(fileScannedSongs[7].filePath == changed);
  CHECK(fileScannedSongs[8].filePath == deleted);
  CHECK(fileScannedSongs[9].filePath == added);
  CHECK(fileScannedSongs[10].filePath == stable);
  CHECK(fileScannedSongs[11].filePath == changed);
  CHECK(fileScannedSongs[12].filePath == added);

  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  CHECK(locations.size() == 3U);
  CHECK(std::ranges::none_of(locations, [&deleted](const cache::CachedLocation& location) { return location.filePath == deleted; }));

  std::cout << "phase3_incremental_e2e_observations "
            << "full_ms=" << full.elapsed.count() << ' '
            << "unchanged_ms=" << unchanged.elapsed.count() << ' '
            << "added_changed_ms=" << addedAndChanged.elapsed.count() << ' '
            << "deleted_ms=" << deletion.elapsed.count() << ' '
            << "reader_reads=" << reader->readCount() << ' '
            << "final_playlist_songs=" << songs.size() << '\n';
}

}
}
