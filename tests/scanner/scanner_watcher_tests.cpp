#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"
#include "file_scanner_orchestrator_test_access.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

class FakeWatcherMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) {
    std::scoped_lock lock{mutex_};
    metadataByPath_[std::move(path)] = std::move(metadata);
  }

  void setReadDelay(std::chrono::milliseconds delay) noexcept { readDelay_ = delay; }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    const auto active = activeReads_.fetch_add(1U) + 1U;
    maxConcurrentReads_.store(std::max(maxConcurrentReads_.load(), active));
    if (readDelay_ > std::chrono::milliseconds{0}) {
      std::this_thread::sleep_for(readDelay_);
    }

    try {
      std::scoped_lock lock{mutex_};
      requestedPaths.push_back(request.path);
      auto iterator = metadataByPath_.find(request.path);
      if (iterator == metadataByPath_.end()) {
        throw std::runtime_error("missing fake metadata");
      }
      auto metadata = iterator->second;
      metadata.filePath = request.path;
      activeReads_.fetch_sub(1U);
      return metadata;
    } catch (...) {
      activeReads_.fetch_sub(1U);
      throw;
    }
  

  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

  [[nodiscard]] std::size_t readCount() const noexcept {
    std::scoped_lock lock{mutex_};
    return requestedPaths.size();
  }

  [[nodiscard]] std::size_t maxConcurrentReads() const noexcept { return maxConcurrentReads_.load(); }

  std::vector<std::filesystem::path> requestedPaths;

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
  std::chrono::milliseconds readDelay_{0};
  std::atomic_size_t activeReads_{0};
  std::atomic_size_t maxConcurrentReads_{0};
  mutable std::mutex mutex_;
};

class CapturedFolderWatcher final : public FolderWatcher {
public:
  struct State {
    std::filesystem::path root;
    WatchEventCallback callback;
    bool closed{false};
  };

  explicit CapturedFolderWatcher(std::shared_ptr<State> state) : state_(std::move(state)) {}

  void close() noexcept override { state_->closed = true; }

  void emit(WatchEvent event) {
    if (!state_->closed) {
      state_->callback(event);
    }
  }

  [[nodiscard]] std::shared_ptr<State> state() const noexcept { return state_; }

private:
  std::shared_ptr<State> state_;
};

class CapturingWatcherFactory final : public FolderWatcherFactory {
public:
  void throwAfterCreating(std::size_t count) noexcept { throwAfterCreated_ = count; }

  [[nodiscard]] std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                     WatchEventCallback callback) override {
    auto state = std::make_shared<CapturedFolderWatcher::State>();
    state->root = root;
    state->callback = std::move(callback);
    states.push_back(state);
    if (throwAfterCreated_ != 0U && states.size() >= throwAfterCreated_) {
      throw std::runtime_error("fake watcher startup failed");
    }
    return std::make_unique<CapturedFolderWatcher>(std::move(state));
  }

  std::vector<std::shared_ptr<CapturedFolderWatcher::State>> states;

private:
  std::size_t throwAfterCreated_{0};
};

[[nodiscard]] RawTagMetadata rawMetadata(std::string title, std::vector<RawTagLyricLine> lyrics = {}) {
  RawTagMetadata raw{};
  raw.title = std::move(title);
  raw.artist = "Artist";
  raw.album = "Album";
  raw.embeddedLyrics = std::move(lyrics);
  raw.duration = std::chrono::milliseconds{120000};
  return raw;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeWatcherService(test::TempScannerRoot& temp,
                                                                     std::shared_ptr<FakeWatcherMetadataReader> reader,
                                                                     std::shared_ptr<CapturingWatcherFactory> watcherFactory) {
  return makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                               .watcherFactory = std::move(watcherFactory),
                                                               .databasePath = temp.dbPath(),
                                                               .coverExportDir = temp.path() / "covers",
                                                               .watcherDebounce = std::chrono::milliseconds{5}});
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

void waitForReadCount(const FakeWatcherMetadataReader& reader, std::size_t expected) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (reader.readCount() >= expected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for fake TagReader read count");
}

void waitForSnapshotSongCount(const FileScannerService& service, std::size_t expected) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (songsIn(service.snapshot()).size() == expected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for watcher reconciliation snapshot");
}

void waitForLyrics(const FileScannerService& service, LyricsSource source, std::string_view text) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service.snapshot());
    if (songs.size() == 1U && songs[0].effectiveLyricsSource == source && songs[0].effectiveLyrics.size() == 1U &&
        songs[0].effectiveLyrics[0].text == text) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for watcher lyrics reconciliation");
}

[[nodiscard]] std::size_t scanStartedCount(const std::vector<ScannerEvent>& events) {
  return static_cast<std::size_t>(std::ranges::count_if(events, [](const ScannerEvent& event) {
    return event.type == ScannerEventType::ScanStarted;
  }));
}

// 等待回落重扫真正开始，而不是固定 sleep：慢机器（CI runner）上 30ms 常常
// 不够，断言会在重扫启动前就跑完。
void waitForScanStartedCount(const std::vector<ScannerEvent>& events, std::mutex& mutex, std::size_t expected) {
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{mutex};
      if (scanStartedCount(events) >= expected) {
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for fallback rescan to start");
}


[[nodiscard]] WatchEvent fileEvent(std::filesystem::path path, WatchEffectKind effect) {
  return WatchEvent{.path = std::move(path), .pathKind = WatchPathKind::File, .effectKind = effect, .associated = {}};
}

[[nodiscard]] WatchEvent watcherMessage(std::filesystem::path message) {
  return WatchEvent{.path = std::move(message),
                    .pathKind = WatchPathKind::Watcher,
                    .effectKind = WatchEffectKind::Other,
                    .associated = {}};
}

[[nodiscard]] WatchEvent directorySelfEvent(std::filesystem::path path) {
  return WatchEvent{.path = std::move(path),
                    .pathKind = WatchPathKind::Directory,
                    .effectKind = WatchEffectKind::Other,
                    .associated = {}};
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

[[nodiscard]] std::string scanRootHashInDatabase(const test::TempScannerRoot& temp) {
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto scanRoot = sidecar.loadScanRoot(canonicalRootPath(temp.path()));
  if (!scanRoot.has_value()) {
    return {};
  }
  return scanRoot->directoryTreeHash;
}

TEST_CASE("scanner watcher debounces create modify rename into precise classifier updates") {
  test::TempScannerRoot temp{"scanner-watcher-debounce"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  CHECK(watchers->states[0]->root == temp.path());
  CHECK(reader->readCount() == 1U);

  const auto created = test::writeAudioFixture(temp.path(), "created.flac");
  reader->put(created, rawMetadata("Created"));
  const auto renamed = temp.path() / "renamed.flac";
  std::filesystem::rename(first, renamed);
  reader->put(renamed, rawMetadata("Renamed"));
  std::this_thread::sleep_for(std::chrono::milliseconds{3}); // mtime granularity guard
  // wtr rename 对约定：primary = 旧路径（MOVED_FROM），associated = 新路径（MOVED_TO）。
  WatchEvent rename = fileEvent(first, WatchEffectKind::Renamed);
  rename.associated.push_back(fileEvent(renamed, WatchEffectKind::Renamed));
  watchers->states[0]->callback(fileEvent(created, WatchEffectKind::Created));
  watchers->states[0]->callback(fileEvent(renamed, WatchEffectKind::Modified));
  watchers->states[0]->callback(rename);

  waitForReadCount(*reader, 3U);
  waitForSnapshotSongCount(*service, 2U);
  auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == created; }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == renamed; }));

  std::filesystem::remove(created);
  watchers->states[0]->callback(fileEvent(created, WatchEffectKind::Destroyed));
  waitForSnapshotSongCount(*service, 1U);
}

TEST_CASE("scanner watcher updates lrc only without TagReader and handles delete fallback") {
  test::TempScannerRoot temp{"scanner-watcher-lrc"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(audio, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  CHECK(reader->readCount() == 1U);

  const auto lrc = temp.path() / "song.lrc";
  writeText(lrc, "[00:02.00]external\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{3}); // mtime granularity guard
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Modified));
  waitForLyrics(*service, LyricsSource::ExternalLrc, "external");
  auto songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external");

  std::filesystem::remove(lrc);
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Destroyed));
  waitForLyrics(*service, LyricsSource::EmbeddedTag, "embedded");
  songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(songs[0].effectiveLyrics[0].text == "embedded");
}

TEST_CASE("scanner watcher warning error and overflow messages force root reconciliation") {
  test::TempScannerRoot temp{"scanner-watcher-warning"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  REQUIRE(watchers->states.size() == 1U);
  watchers->states[0]->callback(watcherMessage("w/sys/q_overflow@"));

  waitForReadCount(*reader, 2U);
  waitForSnapshotSongCount(*service, 2U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(std::ranges::any_of(events, [](const ScannerEvent& event) {
      if (event.type != ScannerEventType::ScanError || !std::holds_alternative<ScannerError>(event.payload)) {
        return false;
      }
      return std::get<ScannerError>(event.payload).message == "watcher requested root reconciliation";
    }));
  }
}

TEST_CASE("scanner watcher lifecycle messages do not trigger incremental scans") {
  test::TempScannerRoot temp{"scanner-watcher-lifecycle"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  watchers->states[0]->callback(watcherMessage("s/self/live@" + temp.path().generic_string()));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }

  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  watchers->states[0]->callback(fileEvent(second, WatchEffectKind::Created));

  waitForReadCount(*reader, 2U);
  waitForSnapshotSongCount(*service, 2U);
  {
    std::scoped_lock lock{eventsMutex};
    // 波 4.1：文件 create 走分类器精准 upsertSong，不触发 ScanStarted。
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher stop closes watcher and ignores later callbacks") {
  test::TempScannerRoot temp{"scanner-watcher-stop"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  auto watcherState = watchers->states[0];
  service->stopWatching();
  CHECK(watcherState->closed);

  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  watcherState->callback(fileEvent(second, WatchEffectKind::Created));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  CHECK(reader->readCount() == 1U);
  CHECK(songsIn(service->snapshot()).size() == 1U);
}

TEST_CASE("scanner watcher startup failure leaves no live callback into service") {
  test::TempScannerRoot temp{"scanner-watcher-startup-failure"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  watchers->throwAfterCreating(1U);
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK_THROWS_AS(service->startWatching({ScannerRoot{.path = temp.path()}}), std::runtime_error);
  REQUIRE(watchers->states.size() == 1U);
  const auto callback = watchers->states[0]->callback;
  service.reset();

  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  CHECK_NOTHROW(callback(fileEvent(second, WatchEffectKind::Created)));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  CHECK(reader->readCount() == 1U);
}

TEST_CASE("scanner watcher queues full watch events with generation bump") {
  test::TempScannerRoot temp{"scanner-watcher-event-queue"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  std::vector<WatcherEventQueueSnapshot> snapshots;
  std::mutex snapshotsMutex;
  setWatcherEventQueueObserver([&snapshots, &snapshotsMutex](const WatcherEventQueueSnapshot& snapshot) {
    std::scoped_lock lock{snapshotsMutex};
    snapshots.push_back(snapshot);
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  WatchEvent event = fileEvent(temp.path() / "new.flac", WatchEffectKind::Created);
  event.associated.push_back(fileEvent(temp.path() / "old.flac", WatchEffectKind::Renamed));
  watchers->states[0]->callback(event);

  {
    std::scoped_lock lock{snapshotsMutex};
    REQUIRE(snapshots.size() == 1U);
    const auto& snapshot = snapshots[0];
    REQUIRE(snapshot.event.has_value());
    CHECK(snapshot.event->path == event.path);
    CHECK(snapshot.event->pathKind == WatchPathKind::File);
    CHECK(snapshot.event->effectKind == WatchEffectKind::Created);
    REQUIRE(snapshot.event->associated.size() == 1U);
    CHECK(snapshot.event->associated[0].path == event.associated[0].path);
    CHECK(snapshot.event->associated[0].pathKind == WatchPathKind::File);
    CHECK(snapshot.event->associated[0].effectKind == WatchEffectKind::Renamed);
    CHECK(snapshot.eventQueueSize == 1U);
    CHECK(snapshot.messageQueueSize == 0U);
    CHECK(snapshot.dirtyGeneration == 1U);
    CHECK_FALSE(snapshot.fallbackRescan);
  }

  clearWatcherEventQueueObserver();
}

TEST_CASE("scanner watcher keeps watcher messages out of the event queue") {
  test::TempScannerRoot temp{"scanner-watcher-message-queue"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  std::vector<WatcherEventQueueSnapshot> snapshots;
  std::mutex snapshotsMutex;
  setWatcherEventQueueObserver([&snapshots, &snapshotsMutex](const WatcherEventQueueSnapshot& snapshot) {
    std::scoped_lock lock{snapshotsMutex};
    snapshots.push_back(snapshot);
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  watchers->states[0]->callback(watcherMessage("w/sys/q_overflow@"));
  {
    std::scoped_lock lock{snapshotsMutex};
    REQUIRE(snapshots.size() == 1U);
    const auto& snapshot = snapshots[0];
    CHECK_FALSE(snapshot.event.has_value());
    CHECK(snapshot.eventQueueSize == 0U);
    CHECK(snapshot.messageQueueSize == 1U);
    CHECK(snapshot.dirtyGeneration == 1U);
  }

  watchers->states[0]->callback(fileEvent(temp.path() / "x.flac", WatchEffectKind::Created));
  {
    std::scoped_lock lock{snapshotsMutex};
    REQUIRE(snapshots.size() == 2U);
    const auto& snapshot = snapshots[1];
    REQUIRE(snapshot.event.has_value());
    CHECK(snapshot.event->path == temp.path() / "x.flac");
    CHECK(snapshot.eventQueueSize == 1U);
    CHECK(snapshot.dirtyGeneration == 2U);
  }

  clearWatcherEventQueueObserver();
}

TEST_CASE("scanner watcher ignores non-actionable events without queueing or waking") {
  test::TempScannerRoot temp{"scanner-watcher-non-actionable"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  std::vector<WatcherEventQueueSnapshot> snapshots;
  std::mutex snapshotsMutex;
  setWatcherEventQueueObserver([&snapshots, &snapshotsMutex](const WatcherEventQueueSnapshot& snapshot) {
    std::scoped_lock lock{snapshotsMutex};
    snapshots.push_back(snapshot);
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  watchers->states[0]->callback(watcherMessage("s/self/live@" + temp.path().generic_string()));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{snapshotsMutex};
    CHECK(snapshots.empty());
  }

  watchers->states[0]->callback(fileEvent(temp.path() / "x.flac", WatchEffectKind::Created));
  {
    std::scoped_lock lock{snapshotsMutex};
    REQUIRE(snapshots.size() == 1U);
    CHECK(snapshots[0].dirtyGeneration == 1U);
  }

  clearWatcherEventQueueObserver();
}

TEST_CASE("scanner watcher marks fallback rescan when event queue overflows") {
  test::TempScannerRoot temp{"scanner-watcher-event-overflow"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = reader,
                                                                       .watcherFactory = watchers,
                                                                       .databasePath = temp.dbPath(),
                                                                       .coverExportDir = temp.path() / "covers",
                                                                       .watcherDebounce = std::chrono::milliseconds{1000}});

  bool sawFallback = false;
  std::size_t maxEventQueueSize = 0;
  std::mutex stateMutex;
  setWatcherEventQueueObserver([&](const WatcherEventQueueSnapshot& snapshot) {
    std::scoped_lock lock{stateMutex};
    sawFallback = sawFallback || snapshot.fallbackRescan;
    maxEventQueueSize = std::max(maxEventQueueSize, snapshot.eventQueueSize);
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  for (std::size_t i = 0; i != 1100; ++i) {
    watchers->states[0]->callback(fileEvent(temp.path() / ("file" + std::to_string(i) + ".flac"), WatchEffectKind::Created));
  }

  {
    std::scoped_lock lock{stateMutex};
    CHECK(sawFallback);
    CHECK(maxEventQueueSize >= 1024U);
  }

  clearWatcherEventQueueObserver();
}

TEST_CASE("scanner service serializes concurrent manual and watcher scans") {
  test::TempScannerRoot temp{"scanner-watcher-serialized-scans"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  reader->setReadDelay(std::chrono::milliseconds{25});
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  std::mutex startMutex;
  std::condition_variable startCv;
  auto readyCount = 0;
  auto start = false;
  auto scanAction = [&] {
    {
      std::unique_lock lock{startMutex};
      ++readyCount;
      startCv.notify_all();
      startCv.wait(lock, [&start] { return start; });
    }
    service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  };
  std::thread firstScan{scanAction};
  std::thread secondScan{scanAction};
  {
    std::unique_lock lock{startMutex};
    startCv.wait(lock, [&readyCount] { return readyCount == 2; });
    start = true;
  }
  startCv.notify_all();
  firstScan.join();
  secondScan.join();

  CHECK(reader->maxConcurrentReads() <= 1U);
}

TEST_CASE("scanner watcher precisely removes a directory moved out of the root") {
  test::TempScannerRoot temp{"scanner-watcher-move-out"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto track = test::writeAudioFixture(music, "01.flac");
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Moved Out Track"));
  reader->put(loose, rawMetadata("Loose Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto movedOut = temp.path().parent_path() / ("seriona-moved-out-" + temp.path().filename().string());
  std::error_code moveError;
  std::filesystem::remove_all(movedOut, moveError);
  std::filesystem::rename(music, movedOut);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  watchers->states[0]->callback(directorySelfEvent(music));

  waitForSnapshotSongCount(*service, 1U);
  auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == loose);
  CHECK(std::ranges::none_of(songs, [&track](const SongMetadata& song) { return song.filePath == track; }));
  {
    std::scoped_lock lock{eventsMutex};
    // 移出根是精准 removeSubtree + deleteLocationsByPathPrefix，不触发全根 ScanStarted。
    CHECK(scanStartedCount(events) == 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  CHECK(locations.size() == 1U);
  CHECK(std::ranges::none_of(locations, [&track](const cache::CachedLocation& location) {
    return location.filePath == track;
  }));
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

TEST_CASE("scanner watcher dedups same-path move-self and flush destroy within one batch") {
  // 波 2（wtr-fae-flush todo 3）：目录 mv 出根时 wtr 可能同时报 IN_MOVE_SELF
  // （Other/Directory → moveSelfByRaw）与 flush destroy（Destroyed/Directory → destroyByKey），
  // 两路都汇入 removes。applyClassifierBatch 的批内去重（按 pathKey(abs)，保留首个）保证
  // 同路径 remove 只应用一次：精准删除、单次发布、不回落重扫、无重复 ScannerEvent。
  test::TempScannerRoot temp{"scanner-watcher-remove-dedup"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto track = test::writeAudioFixture(music, "01.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Dedup Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  const auto versionBefore = service->snapshot().version;
  const auto completedBefore = [&events, &eventsMutex] {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }();
  const auto snapshotEventsBefore = [&events, &eventsMutex] {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::PlaylistSnapshotUpdated, &ScannerEvent::type));
  }();

  const auto movedOut = temp.path().parent_path() / ("seriona-remove-dedup-" + temp.path().filename().string());
  std::error_code moveError;
  std::filesystem::remove_all(movedOut, moveError);
  std::filesystem::rename(music, movedOut);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  // 同批注入两条同路径 remove 事件（背靠背、无 sleep 间隔，保证落入同一 debounce 批次）。
  watchers->states[0]->callback(directorySelfEvent(music));
  watchers->states[0]->callback(WatchEvent{.path = music,
                                            .pathKind = WatchPathKind::Directory,
                                            .effectKind = WatchEffectKind::Destroyed,
                                            .associated = {}});

  waitForSnapshotSongCount(*service, 0U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{eventsMutex};
    // 双 remove 合并后仍走精准删除路径，不触发全根 ScanStarted（非回落重扫）。
    CHECK(scanStartedCount(events) == 1U);
    // 批内去重后同路径 remove 只发布一次完成/快照事件：恰好 +1，无重复 ScannerEvent。
    CHECK(completedBefore + 1U == static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type)));
    CHECK(snapshotEventsBefore + 1U == static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::PlaylistSnapshotUpdated, &ScannerEvent::type)));
  }
  // 快照版本单调递增（单次发布，无重复 Snapshot）。
  CHECK(service->snapshot().version > versionBefore);
  // 精准删除只应用一次的效果：根下无残留位置。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(temp.path())).empty());
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

TEST_CASE("scanner watcher root-internal directory rename converges without rescan") {
  test::TempScannerRoot temp{"scanner-watcher-rename-in-root"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto track = test::writeAudioFixture(music, "01.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Renamed In Root"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto pop = temp.path() / "pop";
  std::filesystem::rename(music, pop);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  WatchEvent rename = WatchEvent{.path = music, .pathKind = WatchPathKind::Directory, .effectKind = WatchEffectKind::Renamed, .associated = {}};
  rename.associated.push_back(WatchEvent{.path = pop, .pathKind = WatchPathKind::Directory, .effectKind = WatchEffectKind::Renamed, .associated = {}});
  watchers->states[0]->callback(rename);

  waitForSnapshotSongCount(*service, 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == (pop / "01.flac"));
  CHECK(songs[0].title == "Renamed In Root");
  CHECK(reader->readCount() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    // 根内 rename 走 renameSubtree + 路径改写，songCount 不变且不触发 ScanStarted。
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher root-internal rename keeps CUE source audio hidden (no ghost track)") {
  test::TempScannerRoot temp{"scanner-watcher-rename-cue-hidden"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto cueFile = music / "album.cue";
  const auto referencedAudio = music / "album.flac";
  const auto standaloneAudio = music / "bonus.flac";
  // .cue 内容不解析出 FILE 引用：album.flac 作为独立曲目被扫描发布（进入 allSongs_），
  // 同时测试 seam 的 CUE 轨又引用它 → 种子期通过 cueSourcePaths 在树中隐藏。
  writeText(cueFile, "REM DUMMY COMMENT\n");
  writeText(referencedAudio, "fake referenced audio");
  writeText(standaloneAudio, "fake standalone audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(referencedAudio, rawMetadata("Bare album file"));
  reader->put(standaloneAudio, rawMetadata("Bonus Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  setTestCueSheetProvider([&referencedAudio](const std::filesystem::path& cuePath)
                              -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {{.audioFilePath = referencedAudio,
               .offset = 0,
               .duration = 180000000,
               .title = "Cue Track 1",
               .artist = "Cue Artist",
               .album = "Cue Album",
               .trackNumber = 1}};
    }
    return {};
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  clearTestCueSheetProvider();
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto pop = temp.path() / "pop";
  std::filesystem::rename(music, pop);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  WatchEvent rename = WatchEvent{.path = music, .pathKind = WatchPathKind::Directory,
                                 .effectKind = WatchEffectKind::Renamed, .associated = {}};
  rename.associated.push_back(WatchEvent{.path = pop, .pathKind = WatchPathKind::Directory,
                                         .effectKind = WatchEffectKind::Renamed, .associated = {}});
  watchers->states[0]->callback(rename);

  const auto waitForSnapshotPath = [&service](const std::filesystem::path& path) {
    for (auto attempt = 0; attempt != 1000; ++attempt) {
      const auto songs = songsIn(service->snapshot());
      if (std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == path; })) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    FAIL("timed out waiting for renamed snapshot path");
  };
  waitForSnapshotPath(pop / "album.cue");
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  // 根内 rename 后 CUE 源音频（album.flac）必须仍隐藏：只有 CUE 轨 + standalone 轨可见，
  // 不允许出现幽灵可见 B/album.flac 曲目（补丁结果 == 全量重建）。
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 2U);
  CHECK(std::ranges::none_of(songs, [&](const SongMetadata& song) { return song.filePath == (pop / "album.flac"); }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (pop / "album.cue"); }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (pop / "bonus.flac"); }));
  {
    std::scoped_lock lock{eventsMutex};
    // 根内 rename 走精准分类器更新，不触发全根重扫。
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher root-internal rename with overlapping sibling prefix keeps sibling CUE tracks") {
  test::TempScannerRoot temp{"scanner-watcher-rename-sibling-prefix"};
  const auto music = temp.path() / "music";
  const auto musicbox = temp.path() / "musicbox";
  std::filesystem::create_directories(music);
  std::filesystem::create_directories(musicbox);
  const auto musicCue = music / "album.cue";
  const auto musicReferenced = music / "album.flac";
  const auto musicBonus = music / "bonus.flac";
  const auto boxCue = musicbox / "box.cue";
  const auto boxReferenced = musicbox / "album.flac";
  const auto boxBonus = musicbox / "bonus.flac";
  // .cue 内容不解析出 FILE 引用：源音频作为独立曲目被扫描发布（进入 allSongs_），
  // 同时 seam 的 CUE 轨又引用它 → 种子期通过 cueSourcePaths 在树中隐藏。
  // musicbox 前缀与 music 重叠（musicbox 以 music 开头），用于暴露绝对 key 改写的边界缺陷。
  writeText(musicCue, "REM DUMMY COMMENT\n");
  writeText(musicReferenced, "fake referenced audio");
  writeText(musicBonus, "fake standalone audio");
  writeText(boxCue, "REM DUMMY COMMENT\n");
  writeText(boxReferenced, "fake referenced audio");
  writeText(boxBonus, "fake standalone audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(musicReferenced, rawMetadata("Music album file"));
  reader->put(musicBonus, rawMetadata("Music Bonus Track"));
  reader->put(boxReferenced, rawMetadata("MusicBox album file"));
  reader->put(boxBonus, rawMetadata("MusicBox Bonus Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  setTestCueSheetProvider([&musicCue, &boxCue, &musicReferenced, &boxReferenced](const std::filesystem::path& cuePath)
                              -> std::vector<TestCueTrackData> {
    if (cuePath == musicCue) {
      return {{.audioFilePath = musicReferenced,
               .offset = 0,
               .duration = 180000000,
               .title = "Music Cue Track 1",
               .artist = "Cue Artist",
               .album = "Music Cue Album",
               .trackNumber = 1}};
    }
    if (cuePath == boxCue) {
      return {{.audioFilePath = boxReferenced,
               .offset = 0,
               .duration = 180000000,
               .title = "MusicBox Cue Track 1",
               .artist = "Cue Artist",
               .album = "MusicBox Cue Album",
               .trackNumber = 1}};
    }
    return {};
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 4U);
  clearTestCueSheetProvider();
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto pop = temp.path() / "pop";
  std::filesystem::rename(music, pop);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  WatchEvent rename = WatchEvent{.path = music, .pathKind = WatchPathKind::Directory,
                                 .effectKind = WatchEffectKind::Renamed, .associated = {}};
  rename.associated.push_back(WatchEvent{.path = pop, .pathKind = WatchPathKind::Directory,
                                         .effectKind = WatchEffectKind::Renamed, .associated = {}});
  watchers->states[0]->callback(rename);

  const auto waitForSnapshotPath = [&service](const std::filesystem::path& path) {
    for (auto attempt = 0; attempt != 1000; ++attempt) {
      const auto songs = songsIn(service->snapshot());
      if (std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == path; })) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    FAIL("timed out waiting for renamed snapshot path");
  };
  waitForSnapshotPath(pop / "album.cue");
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  // 根内 rename 只影响 music→pop：pop 内 CUE 轨/独立音频路径更新且无 music/ 残留。
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 4U);
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (pop / "album.cue"); }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (pop / "bonus.flac"); }));
  CHECK(std::ranges::none_of(songs, [&](const SongMetadata& song) {
    return song.filePath.generic_string().find("/music/") != std::string::npos;
  }));
  // musicbox 内 CUE 轨/独立音频路径完全不变。
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (musicbox / "box.cue"); }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == (musicbox / "bonus.flac"); }));
  // MAJOR-1 红判据：兄弟目录前缀重叠时，绝对 key 改写不得幻影命中 musicbox（无 popbox 节点身份）。
  const auto snapshot = service->snapshot();
  CHECK(std::ranges::none_of(snapshot.nodes, [&](const PlaylistNode& node) {
    return node.nodeId.find("popbox") != std::string::npos;
  }));
  {
    std::scoped_lock lock{eventsMutex};
    // 根内 rename 走精准分类器更新，不触发全根重扫。
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher drops ghost create events for paths absent on disk") {
  test::TempScannerRoot temp{"scanner-watcher-ghost-create"};
  const auto track = test::writeAudioFixture(temp.path(), "real.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Real"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto ghost = temp.path() / "ghost.flac";
  watchers->states[0]->callback(fileEvent(ghost, WatchEffectKind::Created));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  // 幽灵事件：磁盘上不存在该路径 → 分类器丢弃，不读 TagReader、不新增歌曲。
  CHECK(reader->readCount() == 1U);
  CHECK(songsIn(service->snapshot()).size() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher falls back to rescan when move-self path still exists") {
  test::TempScannerRoot temp{"scanner-watcher-move-self-ambiguous"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto track = test::writeAudioFixture(music, "01.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Ambiguous"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 目录仍存在于磁盘（歧义：可能是移出后被同名目录顶替）→ 无法精准判定 → 回落全根重扫。
  watchers->states[0]->callback(directorySelfEvent(music));
  waitForSnapshotSongCount(*service, 1U);
  waitForScanStartedCount(events, eventsMutex, 2U);
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == track);
}

TEST_CASE("scanner watcher refreshes scan-root hash so the next reconcile stays incremental") {
  test::TempScannerRoot temp{"scanner-watcher-hash-refresh"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  watchers->states[0]->callback(fileEvent(second, WatchEffectKind::Created));
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);

  // 精准 create 后 hash 必须写回 scan_roots，否则 60s 对账判 Full 全量重扫。
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);

  const auto completedBefore = [&events, &eventsMutex] {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    std::scoped_lock lock{eventsMutex};
    const auto completed = static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
    if (completed > completedBefore && songsIn(service->snapshot()).size() == 2U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(reader->readCount() == 2U);
  CHECK(songsIn(service->snapshot()).size() == 2U);
}

void waitForReconciliationMessage(const std::vector<ScannerEvent>& events, std::mutex& eventsMutex, const std::string& detail) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      const auto found = std::ranges::any_of(events, [&](const ScannerEvent& event) {
        if (event.type != ScannerEventType::ScanError || !std::holds_alternative<ScannerError>(event.payload)) {
          return false;
        }
        const auto& error = std::get<ScannerError>(event.payload);
        return error.message == "watcher requested root reconciliation" && error.detail == detail;
      });
      if (found) {
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for reconciliation message");
}

TEST_CASE("scanner watcher C1 exact message matching triggers reconciliation only for error messages") {
  test::TempScannerRoot temp{"scanner-watcher-c1-match"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto reconciliationCount = [&events, &eventsMutex]() -> std::size_t {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(std::ranges::count_if(events, [](const ScannerEvent& event) {
      if (event.type != ScannerEventType::ScanError || !std::holds_alternative<ScannerError>(event.payload)) {
        return false;
      }
      return std::get<ScannerError>(event.payload).message == "watcher requested root reconciliation";
    }));
  };

  // result::e 的独立完成标记 e@ → 精确触发对账。
  watchers->states[0]->callback(watcherMessage("e@"));
  waitForReconciliationMessage(events, eventsMutex, "e@");

  // w/sys/q_overflow@ 与 w/sys/partial@ → 触发对账。
  watchers->states[0]->callback(watcherMessage("w/sys/q_overflow@"));
  waitForReconciliationMessage(events, eventsMutex, "w/sys/q_overflow@");
  watchers->states[0]->callback(watcherMessage("w/sys/partial@"));
  waitForReconciliationMessage(events, eventsMutex, "w/sys/partial@");

  // s/self/live@（watch 生命周期）→ 不触发对账（显式排除 s/ 前缀）。
  const auto beforeLifecycle = reconciliationCount();
  watchers->states[0]->callback(watcherMessage("s/self/live@" + temp.path().generic_string()));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  CHECK(reconciliationCount() == beforeLifecycle);
}

TEST_CASE("scanner watcher periodic reconcile rescans silent disk changes without events") {
  test::TempScannerRoot temp{"scanner-watcher-periodic-reconcile"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{30}});

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // watcher 未捕获的磁盘变化：直接写文件，不发射任何事件。
  const auto silent = test::writeAudioFixture(temp.path(), "silent.flac");
  reader->put(silent, rawMetadata("Silent"));

  // 短对账周期（30ms）在无事件时周期性探测 hash → 变化 → 触发重扫 → 快照收敛到 2 首。
  waitForSnapshotSongCount(*service, 2U);
  const auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == silent; }));
  CHECK(reader->readCount() >= 2U);
}

TEST_CASE("scanner watcher periodic reconcile does not publish when nothing changed") {
  test::TempScannerRoot temp{"scanner-watcher-periodic-noop"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{30}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    std::scoped_lock lock{eventsMutex};
    if (std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type) >= 1U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto completedBefore = [&events, &eventsMutex]() -> std::size_t {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }();

  // 等待多个对账周期（无磁盘变化）→ hash 不变 → 零发布（不重扫、不触发 ScanCompleted）。
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  CHECK(reader->readCount() == 1U);
  CHECK(songsIn(service->snapshot()).size() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    const auto completed = static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
    CHECK(completed == completedBefore);
  }
}

TEST_CASE("scanner watcher falls back to rescan for first create on an un-scanned new root") {
  // 守卫语义（波 4.1 MAJOR-2）：locations.root_path 外键指向 scan_roots，
  // 而 scan_roots 行只在扫描完成时写入。新 root 尚未扫描时，其首个 create 无法精准
  // upsert（否则插入 root_path 无外键父行 → FOREIGN KEY constraint failed），
  // 分类器守卫（loadScanRoot 无值）直接回落全根重扫；重扫写入该 root 的 scan_roots，
  // 后续 create 才能走精准更新。本用例断言回落后的收敛行为（歌曲进快照 + 触发重扫），
  // FK 警告消除由 tools/watch_root_move_audit 场景 9 的日志验证。
  test::TempScannerRoot temp{"scanner-watcher-new-root"};
  const auto musicA = temp.path() / "musicA";
  const auto musicB = temp.path() / "musicB";
  std::filesystem::create_directories(musicA);
  std::filesystem::create_directories(musicB);
  const auto trackA = test::writeAudioFixture(musicA, "a.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(trackA, rawMetadata("Track A"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  // 只扫描 musicA；musicB 是新 root，scan_roots 表无其记录。
  service->scan({ScannerRoot{.path = musicA}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);

  // 同时监视两个根：musicA 已扫描，musicB 尚未扫描。
  service->startWatching({ScannerRoot{.path = musicA}, ScannerRoot{.path = musicB}});
  REQUIRE(watchers->states.size() == 2U);
  const auto scannedCountBefore = [&events, &eventsMutex]() -> std::size_t {
    std::scoped_lock lock{eventsMutex};
    return scanStartedCount(events);
  }();

  // 向未扫描的 musicB 写首个文件并触发 create 事件。
  const auto trackB = test::writeAudioFixture(musicB, "b.flac");
  reader->put(trackB, rawMetadata("Track B"));
  std::this_thread::sleep_for(std::chrono::milliseconds{3}); // mtime granularity guard
  watchers->states[1]->callback(fileEvent(trackB, WatchEffectKind::Created));

  // 守卫回落：musicB 未扫描 → 不精准 upsert → 触发全根重扫，
  // 重扫写入 musicB 的 scan_roots 记录，b.flac 进入快照。
  waitForSnapshotSongCount(*service, 2U);
  auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackB; }));
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) >= scannedCountBefore + 1U);
  }

  // 回落重扫后 musicB 进入 scan_roots。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto scannedB = sidecar.loadScanRoot(canonicalRootPath(musicB));
  REQUIRE(scannedB.has_value());
  const auto musicBHash = computeDirectoryTreeHash(canonicalRootPath(musicB));
  REQUIRE(musicBHash.hash.has_value());
  CHECK(scannedB->directoryTreeHash == *musicBHash.hash);

  // 第二个 create（musicB 已入 scan_roots）走精准 upsert，不再触发 ScanStarted。
  const auto trackC = test::writeAudioFixture(musicB, "c.flac");
  reader->put(trackC, rawMetadata("Track C"));
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  watchers->states[1]->callback(fileEvent(trackC, WatchEffectKind::Created));
  waitForSnapshotSongCount(*service, 3U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == scannedCountBefore + 1U);
  }
}

}
}
