#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"
#include "file_scanner_orchestrator_test_access.h"
#include "path_utf8.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/song_identity.h"

#include <doctest.h>
#include <sqlite3.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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
                                                               .folderThumbnailSeam = nullptr,
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

// 等待快照中出现指定路径（rename 类用例：song 数不变，按计数等待会被陈旧快照立刻满足）。
void waitForSnapshotSongPath(const FileScannerService& service, const std::filesystem::path& path) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service.snapshot());
    if (std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == path; })) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for watcher snapshot song path");
}

// 等待快照重新发布（generatedAt 推进）：用于"内容数不变但必须等某次扫描真正落地"的同步点。
// 不能用 version：它是 per-builder 计数，整树重建换新 builder 会重置。
void waitForSnapshotRepublish(const FileScannerService& service, std::chrono::steady_clock::time_point before) {
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    if (service.snapshot().generatedAt > before) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for snapshot republish");
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

// waitForLyrics 前提是快照恰有一首歌；本任务歌词用例是多曲快照，按路径定位。
void waitForSongLyrics(const FileScannerService& service, const std::filesystem::path& path, LyricsSource source,
                       std::string_view text) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service.snapshot());
    const auto found = std::ranges::find_if(songs, [&](const SongMetadata& song) { return song.filePath == path; });
    if (found != songs.end() && found->effectiveLyricsSource == source && found->effectiveLyrics.size() == 1U &&
        found->effectiveLyrics[0].text == text) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for song lyrics reconciliation");
}

[[nodiscard]] std::size_t scanStartedCount(const std::vector<ScannerEvent>& events) {
  return static_cast<std::size_t>(std::ranges::count_if(events, [](const ScannerEvent& event) {
    return event.type == ScannerEventType::ScanStarted;
  }));
}

// 等待事件计数到达阈值（代替固定沉降窗）：发布在慢机上可能迟到 >30ms；
// Snapshot 事件先于 ScanCompleted 推送，故该阈值满足后等值 CHECK 不再有竞态。
void waitForScanCompletedCount(const std::vector<ScannerEvent>& events, std::mutex& mutex, std::size_t expected) {
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{mutex};
      if (static_cast<std::size_t>(std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type)) >=
          expected) {
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for ScanCompleted event count");
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

[[nodiscard]] WatchEvent directoryEvent(std::filesystem::path path, WatchEffectKind effect) {
  return WatchEvent{.path = std::move(path),
                    .pathKind = WatchPathKind::Directory,
                    .effectKind = effect,
                    .associated = {}};
}

[[nodiscard]] std::size_t eventTypeCount(const std::vector<ScannerEvent>& events, ScannerEventType type) {
  return static_cast<std::size_t>(std::ranges::count(events, type, &ScannerEvent::type));
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

[[nodiscard]] WatchEvent renamedFileEvent(std::filesystem::path oldPath, std::filesystem::path newPath) {
  WatchEvent event = fileEvent(std::move(oldPath), WatchEffectKind::Renamed);
  event.associated.push_back(fileEvent(std::move(newPath), WatchEffectKind::Renamed));
  return event;
}

class LrcParseObserverGuard {
public:
  explicit LrcParseObserverGuard(LrcParseObserver observer) { setLrcParseObserver(std::move(observer)); }
  ~LrcParseObserverGuard() { clearLrcParseObserver(); }

  LrcParseObserverGuard(const LrcParseObserverGuard&) = delete;
  LrcParseObserverGuard& operator=(const LrcParseObserverGuard&) = delete;
};

class LyricsHashProviderGuard {
public:
  explicit LyricsHashProviderGuard(TestLyricsSidecarHashProvider provider) {
    setTestLyricsSidecarHashProvider(std::move(provider));
  }
  ~LyricsHashProviderGuard() { clearTestLyricsSidecarHashProvider(); }

  LyricsHashProviderGuard(const LyricsHashProviderGuard&) = delete;
  LyricsHashProviderGuard& operator=(const LyricsHashProviderGuard&) = delete;
};

// 删掉 scan_roots 表但保留 locations/content：SQLiteCache 构造/prepareStatements 成功，
// loadScanRoot 抛错 —— 驱动 Reconcile 的"缓存不可读即中止"路径（B4）。
void dropScanRootTable(const std::filesystem::path& databasePath) {
  sqlite3* database = nullptr;
  REQUIRE(sqlite3_open_v2(pathToUtf8(databasePath).c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_exec(database, "PRAGMA foreign_keys=OFF; DROP TABLE IF EXISTS scan_roots;", nullptr, nullptr,
                       nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_close(database) == SQLITE_OK);
}

// 等待指定路径的歌曲标题更新（单文件根用例）。
void waitForSongTitle(const FileScannerService& service, const std::filesystem::path& path, std::string_view title) {
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service.snapshot());
    const auto found = std::ranges::find_if(songs, [&](const SongMetadata& song) { return song.filePath == path; });
    if (found != songs.end() && found->title == title) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for watcher snapshot song title");
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
  // rename 对约定：primary = 旧路径（MOVED_FROM），associated = 新路径（MOVED_TO）。
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
  // .lrc 修改是 T0 歌词对账（不发 ScanStarted），且排除出树哈希 → 不刷新 hash、不触发扫描。
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }

  std::filesystem::remove(lrc);
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Destroyed));
  waitForLyrics(*service, LyricsSource::EmbeddedTag, "embedded");
  songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(songs[0].effectiveLyrics[0].text == "embedded");
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }
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
                                                                       .folderThumbnailSeam = nullptr,
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
  // 目录 mv 出根时监视器可能同时上报 move-self（Other/Directory → moveSelfByRaw）
  // 与 destroy（Destroyed/Directory → destroyByKey），
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
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
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

TEST_CASE("scanner watcher precisely removes a moved-out directory with a file-kind destroy cascade") {
  // 真实 efsw 对"目录 mv 出根"产生最深优先的级联 Delete：事件到达时路径已不存在，
  // 适配层按磁盘状态判为 File（watchPathKindFrom 无 stat 依据）。分类器须把这类
  // "已消失但树中已知的目录路径"识别为精准子树删除（preciseDirRemovalPrefixes），
  // 不能回落全根重扫。本用例按该形状注入同一 debounce 批次（背靠背、无 sleep）：
  // 最深目录 → 中间目录 → 目录本身，全部 File kind + Destroyed。
  test::TempScannerRoot temp{"scanner-watcher-move-out-file-kind"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music / "empty" / "deeper");
  const auto track = test::writeAudioFixture(music, "01.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(track, rawMetadata("Moved Out File Kind"));
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
  const auto completedBefore = [&events, &eventsMutex] {
    std::scoped_lock lock{eventsMutex};
    return static_cast<std::size_t>(
      std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }();

  const auto movedOut = temp.path().parent_path() / ("seriona-move-out-file-kind-" + temp.path().filename().string());
  std::error_code moveError;
  std::filesystem::remove_all(movedOut, moveError);
  std::filesystem::rename(music, movedOut, moveError);
  REQUIRE_FALSE(moveError);

  watchers->states[0]->callback(fileEvent(music / "empty" / "deeper", WatchEffectKind::Destroyed));
  watchers->states[0]->callback(fileEvent(music / "empty", WatchEffectKind::Destroyed));
  watchers->states[0]->callback(fileEvent(music, WatchEffectKind::Destroyed));

  waitForSnapshotSongCount(*service, 0U);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  {
    std::scoped_lock lock{eventsMutex};
    // 目录 mv 出根已走精准删除：仅初始扫描，无回落重扫。
    CHECK(scanStartedCount(events) == 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(temp.path())).empty());
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

// CUE 移出形状回归夹具（新语义，设计 §3.2 / Oracle B3）：完整扫描（含 seam CUE 轨）→
// startWatching → 目录移出根 + 注入 File kind Destroyed → 等待收敛。
// 断言 ScanStarted 不增：目录移出不再回落扫描（无论 Full 还是 Reconcile），而是
//   1) 共置 cue+源 = 纯精准前缀删除；
//   2) cue 移出、源在外 = 孤儿源重 upsert 为可见曲目；
//   3) 源移出、cue 在外 = cueRefresh scope（cue 父目录）重解析。
struct MovedOutCueOutcome {
  std::size_t scanStartedBefore{0};
  std::size_t scanStartedAfter{0};
  std::vector<SongMetadata> songs;
  std::shared_ptr<FileScannerService> service;
};

MovedOutCueOutcome runMovedOutCuePreciseScenario(
    test::TempScannerRoot& temp, const std::filesystem::path& destroyedDir, std::size_t visibleSongsBeforeMove,
    const std::function<bool(const std::vector<SongMetadata>&)>& converged,
    const std::function<void(FakeWatcherMetadataReader&)>& configureReader) {
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  configureReader(*reader);
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, visibleSongsBeforeMove);
  clearTestCueSheetProvider();
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }

  const auto movedOut = temp.path().parent_path() / ("seriona-move-out-cue-precise-" + temp.path().filename().string());
  std::error_code moveError;
  std::filesystem::remove_all(movedOut, moveError);
  std::filesystem::rename(destroyedDir, movedOut, moveError);
  REQUIRE_FALSE(moveError);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  watchers->states[0]->callback(fileEvent(destroyedDir, WatchEffectKind::Destroyed));

  for (auto attempt = 0; attempt != 2000; ++attempt) {
    if (converged(songsIn(service->snapshot()))) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  // 沉降窗口：若批次被兜底为扫描（Full/Reconcile），其 ScanStarted 会在窗口内出现，负向断言防竞态。
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  MovedOutCueOutcome outcome;
  outcome.songs = songsIn(service->snapshot());
  outcome.service = service;
  {
    std::scoped_lock lock{eventsMutex};
    outcome.scanStartedBefore = startedBefore;
    outcome.scanStartedAfter = scanStartedCount(events);
  }
  // events/eventsMutex 是本函数局部量：返回后调用方仍可能对 service 发起扫描（全量重建对比），
  // 必须摘除引用它们的 sink，避免扫描线程回调悬垂引用。
  service->setEventSink(nullptr);
  return outcome;
}

TEST_CASE("scanner watcher moved-out cue directory converges precisely without fallback rescan") {
  // 形状 1（共置 cue+源，用户命中形状）：fileUnder && sourceUnder → 两个交叉集合都不命中 →
  // 纯精准前缀删除；无孤儿、无 cueRefresh、无扫描。
  {
    test::TempScannerRoot temp{"scanner-watcher-move-out-cue-colocated"};
    const auto music = temp.path() / "music";
    std::filesystem::create_directories(music);
    const auto cueFile = music / "album.cue";
    const auto referencedAudio = music / "album.flac";
    writeText(cueFile, "REM DUMMY COMMENT\n");
    writeText(referencedAudio, "fake referenced audio");
    const auto outcome = runMovedOutCuePreciseScenario(
        temp, music, 1U, [](const std::vector<SongMetadata>& songs) { return songs.empty(); },
        [&referencedAudio](FakeWatcherMetadataReader& reader) {
          reader.put(referencedAudio, rawMetadata("Bare album file"));
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
        });
    CHECK(outcome.scanStartedAfter == outcome.scanStartedBefore);
    CHECK(outcome.songs.empty());
  }

  // 形状 2（cue 移出、源在外）：cue 没了、源还在 → 交叉收集器产出孤儿源，重 upsert 为可见
  // 普通曲目（源重新可见），同样不触发扫描。
  {
    test::TempScannerRoot temp{"scanner-watcher-move-out-cue-orphan"};
    const auto music = temp.path() / "music";
    const auto audioDir = temp.path() / "audio";
    std::filesystem::create_directories(music);
    std::filesystem::create_directories(audioDir);
    const auto cueFile = music / "album.cue";
    const auto orphanAudio = audioDir / "album.flac";
    writeText(cueFile, "REM DUMMY COMMENT\n");
    writeText(orphanAudio, "fake orphan audio");
    const auto outcome = runMovedOutCuePreciseScenario(
        temp, music, 1U,
        [&orphanAudio](const std::vector<SongMetadata>& songs) {
          return songs.size() == 1U && songs[0].filePath == orphanAudio;
        },
        [&orphanAudio](FakeWatcherMetadataReader& reader) {
          reader.put(orphanAudio, rawMetadata("Orphan Source"));
          setTestCueSheetProvider([&orphanAudio](const std::filesystem::path& cuePath)
                                      -> std::vector<TestCueTrackData> {
            if (cuePath.filename() == "album.cue") {
              return {{.audioFilePath = orphanAudio,
                       .offset = 0,
                       .duration = 180000000,
                       .title = "Cue Track 1",
                       .artist = "Cue Artist",
                       .album = "Cue Album",
                       .trackNumber = 1}};
            }
            return {};
          });
        });
    CHECK(outcome.scanStartedAfter == outcome.scanStartedBefore);
    REQUIRE(outcome.songs.size() == 1U);
    CHECK(outcome.songs[0].filePath == orphanAudio);
    CHECK(outcome.songs[0].title == "Orphan Source");

    // 补丁结果 == 全量重建：孤儿 upsert 后再跑一次用户显式 Full 扫描，快照与缓存必须一致
    // （设计 §3.4：任何 CUE 局部改动必须同时更新 allSongs_/树/缓存，否则与全量重建分歧）。
    cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
    const auto patchedLocations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
    REQUIRE(patchedLocations.size() == 1U);
    CHECK(patchedLocations[0].filePath == orphanAudio);

    // 必须等待 Full 真正完成（快照重新发布）再采样：补丁快照已满足歌曲数，只等计数
    // 会在 Full 未落地时提前对比（对比恒真）。
    const auto publishedBefore = outcome.service->snapshot().generatedAt;
    outcome.service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
    waitForSnapshotRepublish(*outcome.service, publishedBefore);
    const auto rebuiltSongs = songsIn(outcome.service->snapshot());
    REQUIRE(rebuiltSongs.size() == outcome.songs.size());
    for (std::size_t index = 0; index < rebuiltSongs.size(); ++index) {
      CHECK(rebuiltSongs[index].filePath == outcome.songs[index].filePath);
      CHECK(rebuiltSongs[index].title == outcome.songs[index].title);
      CHECK(rebuiltSongs[index].artist == outcome.songs[index].artist);
      CHECK(rebuiltSongs[index].album == outcome.songs[index].album);
      CHECK(rebuiltSongs[index].logicalTrackId == outcome.songs[index].logicalTrackId);
    }
    const auto rebuiltLocations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
    REQUIRE(rebuiltLocations.size() == patchedLocations.size());
    std::vector<std::string> patchedIds;
    patchedIds.reserve(patchedLocations.size());
    for (const auto& location : patchedLocations) {
      patchedIds.push_back(location.locationId);
    }
    std::vector<std::string> rebuiltIds;
    rebuiltIds.reserve(rebuiltLocations.size());
    for (const auto& location : rebuiltLocations) {
      rebuiltIds.push_back(location.locationId);
    }
    std::ranges::sort(patchedIds);
    std::ranges::sort(rebuiltIds);
    CHECK(rebuiltIds == patchedIds);
  }

  // 形状 3（源移出、cue 在外）：cue 还在、源没了 → cueRefreshTargets → T1 scope = cue 父目录
  // 重解析；scoped 契约不发 ScanStarted。music 内含可见曲目保证它是树中已知目录。
  {
    test::TempScannerRoot temp{"scanner-watcher-move-out-cue-refresh"};
    const auto music = temp.path() / "music";
    const auto cues = temp.path() / "cues";
    std::filesystem::create_directories(music);
    std::filesystem::create_directories(cues);
    const auto visibleAudio = music / "visible.flac";
    const auto hiddenAudio = music / "hidden.flac";
    const auto cueFile = cues / "outside.cue";
    writeText(visibleAudio, "fake visible audio");
    writeText(hiddenAudio, "fake hidden source audio");
    writeText(cueFile, "REM DUMMY COMMENT\n");
    const auto outcome = runMovedOutCuePreciseScenario(
        temp, music, 2U, [](const std::vector<SongMetadata>& songs) { return songs.empty(); },
        [&visibleAudio, &hiddenAudio](FakeWatcherMetadataReader& reader) {
          reader.put(visibleAudio, rawMetadata("Visible Track"));
          reader.put(hiddenAudio, rawMetadata("Hidden Source"));
          setTestCueSheetProvider([&hiddenAudio](const std::filesystem::path& cuePath)
                                      -> std::vector<TestCueTrackData> {
            if (cuePath.filename() == "outside.cue") {
              return {{.audioFilePath = hiddenAudio,
                       .offset = 0,
                       .duration = 180000000,
                       .title = "Cue Track 1",
                       .artist = "Cue Artist",
                       .album = "Cue Album",
                       .trackNumber = 1}};
            }
            return {};
          });
        });
    CHECK(outcome.scanStartedAfter == outcome.scanStartedBefore);
    CHECK(outcome.songs.empty());
  }
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

  waitForSnapshotSongPath(*service, pop / "01.flac");
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

  waitForSnapshotSongPath(*service, pop / "album.cue");
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

  waitForSnapshotSongPath(*service, pop / "album.cue");
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

TEST_CASE("scanner watcher cue file rename rewrites logicalTrackId and tree keys") {
  test::TempScannerRoot temp{"scanner-watcher-rename-cue-file"};
  const auto music = temp.path() / "music";
  std::filesystem::create_directories(music);
  const auto oldCue = music / "album.cue";
  const auto newCue = music / "album-renamed.cue";
  const auto referencedAudio = music / "album.flac";
  writeText(oldCue, "REM DUMMY COMMENT\n");
  writeText(referencedAudio, "fake referenced audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(referencedAudio, rawMetadata("Bare album file"));
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
  waitForSnapshotSongCount(*service, 1U);
  std::string logicalBefore;
  for (const auto& node : service->snapshot().nodes) {
    if (node.song.has_value() && node.song->filePath == oldCue) {
      logicalBefore = node.song->logicalTrackId;
    }
  }
  REQUIRE_FALSE(logicalBefore.empty());
  // 缓存侧基线：rename 前 cue 轨 locations 行的身份（locationId 含路径，rename 后必须重算）。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  std::string oldLocationId;
  std::uint64_t cueFileSize = 0;
  std::int64_t cueFileMtimeNs = 0;
  std::optional<std::chrono::milliseconds> cueOffset;
  std::optional<std::uint32_t> cueIndex;
  for (const auto& location : sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()))) {
    if (location.filePath == oldCue) {
      oldLocationId = location.locationId;
      cueFileSize = location.fileSizeBytes;
      cueFileMtimeNs = location.fileMtimeNs;
      cueOffset = location.cueTrackOffset;
      cueIndex = location.cueTrackIndex;
    }
  }
  REQUIRE_FALSE(oldLocationId.empty());
  clearTestCueSheetProvider();
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // .cue 文件级 rename（efsw 单条 Moved，旧路径为 primary）：树键 = logicalTrackId =
  // <absCue>#trackN，前缀改写必须命中 '#' 分支（B7 三处修复点）。
  std::filesystem::rename(oldCue, newCue);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  WatchEvent rename = WatchEvent{.path = oldCue, .pathKind = WatchPathKind::File,
                                 .effectKind = WatchEffectKind::Renamed, .associated = {}};
  rename.associated.push_back(WatchEvent{.path = newCue, .pathKind = WatchPathKind::File,
                                         .effectKind = WatchEffectKind::Renamed, .associated = {}});
  watchers->states[0]->callback(rename);

  waitForSnapshotSongPath(*service, newCue);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  const auto snapshot = service->snapshot();
  const auto trackSuffix = logicalBefore.substr(logicalBefore.rfind('#'));
  const auto oldCuePrefix = logicalBefore.substr(0, logicalBefore.rfind('#'));
  CHECK(std::ranges::none_of(snapshot.nodes, [&](const PlaylistNode& node) {
    return node.nodeId.find(oldCuePrefix) != std::string::npos || node.nodeId.find("album.cue") != std::string::npos;
  }));
  const SongMetadata* renamedSong = nullptr;
  std::string renamedNodeId;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value() && node.song->filePath == newCue) {
      renamedSong = &*node.song;
      renamedNodeId = node.nodeId;
    }
  }
  REQUIRE(renamedSong != nullptr);
  CHECK(renamedSong->logicalTrackId != logicalBefore);
  CHECK(renamedSong->logicalTrackId.rfind(oldCuePrefix, 0) != 0);
  CHECK(renamedSong->logicalTrackId.ends_with(trackSuffix));
  CHECK(renamedSong->trackId == renamedSong->logicalTrackId);
  // 树节点身份 = "track:" + logicalTrackId：键同样必须指向新 cue 路径，否则旧键残留为幽灵轨节点。
  CHECK(renamedNodeId == "track:" + renamedSong->logicalTrackId);
  // 缓存侧：旧 cue 路径行消失；新行 file_path 指向新 cue 路径且 locationId 按新路径重算
  // （computeLocationId 纳入路径，故必须与旧值不同且等于新路径的期望值）。
  const auto renamedLocations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  CHECK(std::ranges::none_of(renamedLocations, [&](const cache::CachedLocation& location) {
    return location.filePath == oldCue;
  }));
  const cache::CachedLocation* renamedLocation = nullptr;
  for (const auto& location : renamedLocations) {
    if (location.filePath == newCue) {
      renamedLocation = &location;
    }
  }
  REQUIRE(renamedLocation != nullptr);
  CHECK(renamedLocation->locationId != oldLocationId);
  const auto cueMtime = std::filesystem::file_time_type{
      std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds{cueFileMtimeNs})};
  CHECK(renamedLocation->locationId == computeLocationId(newCue, cueFileSize, cueMtime, cueOffset, cueIndex));
  {
    std::scoped_lock lock{eventsMutex};
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

TEST_CASE("scanner watcher move-self with existing path converges via scoped reconcile without rescan") {
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
  std::size_t startedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }

  // 目录仍存在于磁盘（歧义：可能是移出后被同名目录顶替）：新语义按目录 scope 收敛
  // （scope 内增量计划命中缓存 → 标签零重读），不再回落重扫。
  watchers->states[0]->callback(directorySelfEvent(music));
  bool sawSnapshot = false;
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) > snapshotBefore) {
        sawSnapshot = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(sawSnapshot);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == track);
  CHECK(reader->readCount() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
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
  bool incrementalScanCompleted = false;
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      const auto completed = static_cast<std::size_t>(
        std::ranges::count(events, ScannerEventType::ScanCompleted, &ScannerEvent::type));
      incrementalScanCompleted = completed > completedBefore;
    }
    if (incrementalScanCompleted && songsIn(service->snapshot()).size() == 2U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  REQUIRE(incrementalScanCompleted);
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
      .folderThumbnailSeam = nullptr,
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
      .folderThumbnailSeam = nullptr,
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
  // 守卫语义（波 4.1 MAJOR-2 / 设计 §13.3-3）：locations.root_path 外键指向 scan_roots，
  // 而 scan_roots 行只在扫描完成时写入。新 root 尚未扫描时，其首个 create 无法精准
  // upsert（否则插入 root_path 无外键父行 → FOREIGN KEY constraint failed），
  // 分类器守卫（loadScanRoot 无值）改为提交该 root 的 Reconcile（永不 Full）；
  // Reconcile 写入 scan_roots，后续 create 才能走精准更新。本用例断言收敛行为
  // （歌曲进快照 + 触发一次扫描），FK 警告消除由 tools/watch_root_move_audit 场景 9 的日志验证。
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

  // 守卫改走 Reconcile（永不 Full）：musicB 未扫描 → 不精准 upsert → 提交 Reconcile，
  // Reconcile 写入 musicB 的 scan_roots 记录，b.flac 进入快照。
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

TEST_CASE("scanner watcher keeps unavailable root entries and cache when root disappears") {
  // B1：根路径暂时性丢失（卸载/网络卷/删除）不得清空既有索引与缓存。runScan 按 root 合并：
  // 缺失 root 的对账中止（hash 缺失），其条目与 locations/scan_roots 行原样保留；
  // 其余 root 正常增量收敛。
  test::TempScannerRoot temp{"scanner-watcher-root-unavailable"};
  const auto musicA = temp.path() / "musicA";
  const auto musicB = temp.path() / "musicB";
  std::filesystem::create_directories(musicA);
  std::filesystem::create_directories(musicB);
  const auto trackA = test::writeAudioFixture(musicA, "a.flac");
  const auto trackB = test::writeAudioFixture(musicB, "b.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(trackA, rawMetadata("Track A"));
  reader->put(trackB, rawMetadata("Track B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = musicA}, ScannerRoot{.path = musicB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = musicA}, ScannerRoot{.path = musicB}});
  REQUIRE(watchers->states.size() == 2U);

  const auto movedB = temp.path().parent_path() / ("seriona-root-gone-" + temp.path().filename().string());
  std::error_code moveError;
  std::filesystem::remove_all(movedB, moveError);
  std::filesystem::rename(musicB, movedB, moveError);
  REQUIRE_FALSE(moveError);

  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  // watcher 失事件信号 → Reconcile（永不 Full）：musicA 增量对账命中缓存（零标签重读），
  // musicB 对账中止（hash 缺失）→ 条目保留。
  watchers->states[0]->callback(watcherMessage("w/sys/q_overflow@"));
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 2U);
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackA; }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackB; }));
  CHECK(reader->readCount() == 2U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(musicA)).size() == 1U);
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(musicB)).size() == 1U);
  CHECK(sidecar.loadScanRoot(canonicalRootPath(musicB)).has_value());
}

TEST_CASE("scanner watcher scoped reconcile enumerates a directory moved into the root") {
  test::TempScannerRoot temp{"scanner-watcher-scoped-dir"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
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

  // 根外目录（含嵌套子目录 + 2 个音频）整体移入：efsw 只报目录自身 Add+Modified，
  // 既有子文件无逐事件 → scoped 对账必须自行枚举子树（Gate 0 S1）。
  const auto outside = temp.path().parent_path() / ("seriona-scoped-dir-in-" + temp.path().filename().string());
  std::error_code cleanupError;
  std::filesystem::remove_all(outside, cleanupError);
  const auto stagedA = test::writeAudioFixture(outside, "a.flac");
  const auto stagedB = test::writeAudioFixture(outside / "nested", "b.flac");
  const auto incoming = temp.path() / "incoming";
  std::filesystem::rename(outside, incoming);
  const auto trackA = incoming / stagedA.filename();
  const auto trackB = incoming / "nested" / stagedB.filename();
  reader->put(trackA, rawMetadata("Scoped A"));
  reader->put(trackB, rawMetadata("Scoped B"));

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Created));
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 3U);
  // 沉降窗口：若事件被拆批/命中兜底而排入全根重扫，其 ScanStarted 与全量读取会在本窗口内出现，
  // 保证下方"无整根重扫"负向断言不因断言跑在重扫启动之前而假通过。
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  const auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackA; }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackB; }));
  // 子树枚举读取 2 个移入文件；全根只有 3 个文件被读 → 无整根重扫。
  CHECK(reader->readCount() == 3U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::ScanCompleted) == completedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) == snapshotBefore + 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 3U);
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) { return location.filePath == trackA; }));
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) { return location.filePath == trackB; }));
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

TEST_CASE("scanner watcher scoped reconcile dedups nested and duplicate directory scopes") {
  test::TempScannerRoot temp{"scanner-watcher-scoped-dedup"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  // 去抖窗口放大到 30ms：确保 5 条 scope 事件落入同一批（嵌套/重复去重的前提）。
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = reader,
                                                                       .watcherFactory = watchers,
                                                                       .databasePath = temp.dbPath(),
                                                                       .coverExportDir = temp.path() / "covers",
                                                                       .folderThumbnailSeam = nullptr,
                                                                       .watcherDebounce = std::chrono::milliseconds{30}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto outside = temp.path().parent_path() / ("seriona-scoped-nested-" + temp.path().filename().string());
  std::error_code cleanupError;
  std::filesystem::remove_all(outside, cleanupError);
  const auto stagedA = test::writeAudioFixture(outside, "a.flac");
  const auto stagedB = test::writeAudioFixture(outside / "nested", "b.flac");
  const auto incoming = temp.path() / "incoming";
  std::filesystem::rename(outside, incoming);
  const auto nested = incoming / "nested";
  const auto trackA = incoming / stagedA.filename();
  const auto trackB = nested / stagedB.filename();
  reader->put(trackA, rawMetadata("Scoped A"));
  reader->put(trackB, rawMetadata("Scoped B"));

  std::vector<std::vector<PublishedSongSnapshot>> publishedBatches;
  std::mutex publishedMutex;
  setPublishedSongObserver([&publishedBatches, &publishedMutex](const std::vector<PublishedSongSnapshot>& songs) {
    std::scoped_lock lock{publishedMutex};
    publishedBatches.push_back(songs);
  });
  struct PublishedObserverClear {
    ~PublishedObserverClear() { clearPublishedSongObserver(); }
  } publishedObserverClear;

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Created));
  watchers->states[0]->callback(directoryEvent(nested, WatchEffectKind::Created));
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Created));
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Modified));
  watchers->states[0]->callback(directoryEvent(nested, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 3U);
  // 沉降窗口：兜底重扫若被排入，其 ScanStarted/全量读取会在本窗口内出现，负向断言防竞态。
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  {
    std::scoped_lock lock{publishedMutex};
    // 嵌套后的 A/B 合并为一次子树扫描；若 B 被独立扫描或重复扫描，本段会出现第二批。
    REQUIRE(publishedBatches.size() == 1U);
    CHECK(publishedBatches[0].size() == 2U);
  }
  // 每个移入文件只读一次：合并去重后 1（种子）+ 2（A/a + A/nested/b）= 3。
  CHECK(reader->readCount() == 3U);
  const auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackA; }));
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == trackB; }));
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::ScanCompleted) == completedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) == snapshotBefore + 1U);
  }
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

TEST_CASE("scanner watcher keeps a moved-in file on the precise classifier path") {
  test::TempScannerRoot temp{"scanner-watcher-file-in"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
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

  const auto outside = temp.path().parent_path() / ("seriona-file-in-" + temp.path().filename().string());
  std::error_code cleanupError;
  std::filesystem::remove_all(outside, cleanupError);
  const auto staged = test::writeAudioFixture(outside, "incoming.flac");
  const auto incoming = temp.path() / staged.filename();
  std::filesystem::rename(staged, incoming);
  reader->put(incoming, rawMetadata("Moved In File"));

  std::size_t startedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  // 移入文件 = Add + Modified（完整路径）：仍走既有文件级精准 upsert，不进入 scoped。
  watchers->states[0]->callback(fileEvent(incoming, WatchEffectKind::Created));
  watchers->states[0]->callback(fileEvent(incoming, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 2U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  CHECK(reader->readCount() == 2U);
  const auto songs = songsIn(service->snapshot());
  CHECK(std::ranges::any_of(songs, [&](const SongMetadata& song) { return song.filePath == incoming; }));
  {
    std::scoped_lock lock{eventsMutex};
    // 文件级新增沿用既有精准路径：无 ScanStarted（无全根重扫），快照仅 +1。
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) == snapshotBefore + 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 2U);
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) { return location.filePath == incoming; }));
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

// 发布契约（scoped 路径，publishSnapshotEvents 顶部契约第 3 类）与歌词对账（任务 8）：
// 移入目录内的音频带同名 sidecar 时，scoped 子树枚举经 reconcileRoot 对账外部歌词；
// 同批只发 1 个 PlaylistSnapshotUpdated，不发 ScanStarted/ScanCompleted。B 无 sidecar =
// "缺失不崩溃且保留内嵌"负向边界。
TEST_CASE("scanner watcher scoped reconcile publishes snapshot only and picks up sidecar lyrics") {
  test::TempScannerRoot temp{"scanner-watcher-scoped-lyrics"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose", {RawTagLyricLine{std::chrono::milliseconds{100}, "loose embedded"}}));
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

  const auto outside = temp.path().parent_path() / ("seriona-scoped-lyrics-" + temp.path().filename().string());
  std::error_code cleanupError;
  std::filesystem::remove_all(outside, cleanupError);
  const auto stagedA = test::writeAudioFixture(outside, "a.flac");
  const auto stagedB = test::writeAudioFixture(outside / "nested", "b.flac");
  writeText(outside / "a.lrc", "[00:02.00]moved external\n");
  const auto incoming = temp.path() / "incoming";
  std::filesystem::rename(outside, incoming);
  const auto trackA = incoming / stagedA.filename();
  const auto trackB = incoming / "nested" / stagedB.filename();
  reader->put(trackA, rawMetadata("Scoped A", {RawTagLyricLine{std::chrono::milliseconds{100}, "a embedded"}}));
  reader->put(trackB, rawMetadata("Scoped B", {RawTagLyricLine{std::chrono::milliseconds{100}, "b embedded"}}));

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Created));
  watchers->states[0]->callback(directoryEvent(incoming, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 3U);
  waitForSongLyrics(*service, trackA, LyricsSource::ExternalLrc, "moved external");
  // 沉降窗口：兜底重扫若被排入，其 ScanStarted 与全量读取会在本窗口内出现，负向断言防竞态。
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  const auto songs = songsIn(service->snapshot());
  const auto foundB = std::ranges::find_if(songs, [&](const SongMetadata& song) { return song.filePath == trackB; });
  REQUIRE(foundB != songs.end());
  CHECK(foundB->effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(foundB->effectiveLyrics.size() == 1U);
  CHECK(foundB->effectiveLyrics[0].text == "b embedded");
  CHECK(reader->readCount() == 3U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::ScanCompleted) == completedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) == snapshotBefore + 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 3U);
  const auto foundLocation = std::ranges::find_if(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == trackA;
  });
  REQUIRE(foundLocation != locations.end());
  CHECK(foundLocation->lyricsSource == LyricsSource::ExternalLrc);
  const auto cachedExternalLyrics = sidecar.loadLyrics(foundLocation->locationId, "external");
  REQUIRE(cachedExternalLyrics.size() == 1U);
  CHECK(cachedExternalLyrics[0].text == "moved external");
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

// 发布契约（文件级精准路径，契约第 2 类）与歌词对账（任务 8）：音频落点前磁盘已有同名
// sidecar 时，精准 upsert 必须完成外部歌词对账；该路径同时保留既有 ScanCompleted 契约
// （commit 24186ef 起，不得压制）。只报音频事件 = 覆盖 sidecar 事件与音频事件被拆批的窗口。
TEST_CASE("scanner watcher precise file upsert reconciles sidecar lyrics and keeps ScanCompleted contract") {
  test::TempScannerRoot temp{"scanner-watcher-precise-lyrics"};
  const auto seed = test::writeAudioFixture(temp.path(), "seed.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(seed, rawMetadata("Seed"));
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

  const auto fresh = test::writeAudioFixture(temp.path(), "fresh.flac");
  writeText(temp.path() / "fresh.lrc", "[00:03.00]fresh external\n");
  reader->put(fresh, rawMetadata("Fresh", {RawTagLyricLine{std::chrono::milliseconds{100}, "fresh embedded"}}));

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  watchers->states[0]->callback(fileEvent(fresh, WatchEffectKind::Created));
  watchers->states[0]->callback(fileEvent(fresh, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 2U);
  waitForSongLyrics(*service, fresh, LyricsSource::ExternalLrc, "fresh external");
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  CHECK(reader->readCount() == 2U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) == snapshotBefore + 1U);
    CHECK(eventTypeCount(events, ScannerEventType::ScanCompleted) == completedBefore + 1U);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 2U);
  const auto foundLocation = std::ranges::find_if(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == fresh;
  });
  REQUIRE(foundLocation != locations.end());
  CHECK(foundLocation->lyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(foundLocation->externalLrcPath.has_value());
  CHECK(foundLocation->externalLrcPath->filename() == "fresh.lrc");
  const auto cachedExternalLyrics = sidecar.loadLyrics(foundLocation->locationId, "external");
  REQUIRE(cachedExternalLyrics.size() == 1U);
  CHECK(cachedExternalLyrics[0].text == "fresh external");
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
}

// 歌词负向边界（任务 8）：sidecar 存在但不可解析 → 不崩溃、不回落重扫；其余元数据不受损，
// 外部歌词被清除并回退内嵌；解析错误按既有 ScanError（非致命 MetadataReadFailed）发布。
TEST_CASE("scanner watcher precise upsert degrades gracefully on a corrupt lyrics sidecar") {
  test::TempScannerRoot temp{"scanner-watcher-corrupt-lyrics"};
  const auto seed = test::writeAudioFixture(temp.path(), "seed.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(seed, rawMetadata("Seed"));
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

  const auto broken = test::writeAudioFixture(temp.path(), "broken.flac");
  writeText(temp.path() / "broken.lrc", "[bad-timestamp]broken line\n");
  reader->put(broken, rawMetadata("Broken", {RawTagLyricLine{std::chrono::milliseconds{100}, "broken embedded"}}));

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  watchers->states[0]->callback(fileEvent(broken, WatchEffectKind::Created));
  watchers->states[0]->callback(fileEvent(broken, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 2U);
  // 沉降窗口：损坏 sidecar 若被误升级为异常/兜底重扫，ScanStarted 会在断言前出现。
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  CHECK(reader->readCount() == 2U);
  const auto songs = songsIn(service->snapshot());
  const auto found = std::ranges::find_if(songs, [&](const SongMetadata& song) { return song.filePath == broken; });
  REQUIRE(found != songs.end());
  CHECK(found->title == "Broken");
  CHECK(found->effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(found->effectiveLyrics.size() == 1U);
  CHECK(found->effectiveLyrics[0].text == "broken embedded");
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(std::ranges::any_of(events, [&](const ScannerEvent& event) {
      if (event.type != ScannerEventType::ScanError || !std::holds_alternative<ScannerError>(event.payload)) {
        return false;
      }
      const auto& error = std::get<ScannerError>(event.payload);
      return error.message == "failed to parse external lyrics" && error.code == ScannerErrorCode::MetadataReadFailed &&
             error.path == temp.path() / "broken.lrc";
    }));
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  const auto foundLocation = std::ranges::find_if(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == broken;
  });
  REQUIRE(foundLocation != locations.end());
  CHECK(foundLocation->lyricsSource == LyricsSource::EmbeddedTag);
  CHECK(sidecar.loadLyrics(foundLocation->locationId, "external").empty());
}

TEST_CASE("scanner watcher keeps unaffected directory thumbnails across precise batches") {
  test::TempScannerRoot temp{"scanner-watcher-thumbnail-cache"};
  const auto dirAlpha = temp.path() / "alpha";
  const auto dirBeta = temp.path() / "beta";
  const auto songAlpha = test::writeAudioFixture(dirAlpha, "a.flac");
  const auto songBeta = test::writeAudioFixture(dirBeta, "b.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songAlpha, rawMetadata("Alpha"));
  reader->put(songBeta, rawMetadata("Beta"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  // 非 nullptr seam：alpha 有 marker 才解析出缩略图（模拟真实文件夹封面导出）。
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = [](const std::filesystem::path& directory) -> std::optional<std::filesystem::path> {
        if (std::filesystem::exists(directory / "cover.marker")) {
          return directory / "thumb.png";
        }
        return std::nullopt;
      },
      .watcherDebounce = std::chrono::milliseconds{5}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  writeText(dirAlpha / "cover.marker", "x");

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  const auto thumbnailFor = [&](std::string_view displayName) -> std::optional<std::string> {
    for (const auto& node : service->snapshot().nodes) {
      if (node.kind == PlaylistNodeKind::Directory && node.displayName == displayName) {
        return node.thumbnailPath;
      }
    }
    return std::nullopt;
  };
  const auto alphaThumbnail = thumbnailFor("alpha");
  REQUIRE(alphaThumbnail.has_value());
  CHECK(std::filesystem::path{*alphaThumbnail} == dirAlpha / "thumb.png");
  CHECK(thumbnailFor("beta") == std::nullopt);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 在 beta 目录精准创建文件：发布只重解析 beta 子树，alpha 必须从缓存回填缩略图
  // （R13.1 回归：未受影响目录的 thumbnailPath 不得被清成 nullopt）。
  const auto created = test::writeAudioFixture(dirBeta, "c.flac");
  reader->put(created, rawMetadata("Gamma"));
  watchers->states[0]->callback(fileEvent(created, WatchEffectKind::Created));
  waitForSnapshotSongCount(*service, 3U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto alphaAfter = thumbnailFor("alpha");
  REQUIRE(alphaAfter.has_value());
  CHECK(std::filesystem::path{*alphaAfter} == dirAlpha / "thumb.png");
  // 受影响目录 beta 重解析：无 marker → 保持无缩略图（不是残留陈旧值）。
  CHECK(thumbnailFor("beta") == std::nullopt);
}

TEST_CASE("scanner watcher cover event re-reads directory tags so song artwork is refreshed") {
  test::TempScannerRoot temp{"scanner-watcher-cover-reread"};
  const auto music = temp.path() / "music";
  const auto song = test::writeAudioFixture(music, "01.flac");
  const auto cover = music / "cover.jpg";
  writeText(cover, "fake cover bytes");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  auto withCover = rawMetadata("Song");
  withCover.coverPath = temp.path() / "export" / "cover.png";
  withCover.thumbnailPath = temp.path() / "export" / "thumb.png";
  reader->put(song, withCover);
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
  const auto artworkBefore = songsIn(service->snapshot())[0].artworkPath;
  REQUIRE(artworkBefore.has_value());
  REQUIRE_FALSE(artworkBefore->empty());
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 封面删除：谓词命中 → scope 强制重读该目录歌曲标签（R13.2）。音频 size/mtime 未变，
  // 若走缓存直灌则歌曲级 artwork 保持陈旧。
  reader->put(song, rawMetadata("Song"));
  std::filesystem::remove(cover);
  watchers->states[0]->callback(fileEvent(cover, WatchEffectKind::Destroyed));
  waitForReadCount(*reader, 2U);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    if (songs.size() == 1U && (!songs[0].artworkPath.has_value() || songs[0].artworkPath->empty())) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  const auto artworkCleared = !songs[0].artworkPath.has_value() || songs[0].artworkPath->empty();
  CHECK(artworkCleared);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }
}

TEST_CASE("scanner watcher scoped merge restores external cue rows and converges after cue unref") {
  test::TempScannerRoot temp{"scanner-watcher-scope-external-cue"};
  const auto scopeDir = temp.path() / "album";
  const auto cueDir = temp.path() / "cues";
  std::filesystem::create_directories(scopeDir);
  std::filesystem::create_directories(cueDir);
  const auto source = scopeDir / "s.flac";
  const auto cue = cueDir / "c.cue";
  writeText(source, "fake source audio");
  writeText(cue, "REM DUMMY COMMENT\n");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(source, rawMetadata("Source Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  setTestCueSheetProvider([&source](const std::filesystem::path& cuePath)
                              -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "c.cue") {
      return {{.audioFilePath = source,
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
  waitForSnapshotSongCount(*service, 1U);  // 只有 cue 轨可见（源被 cue 隐藏）
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 触发 album 目录的 scoped 对账（Modified Directory）：scope 内枚举会读到源音频，而 cue 在
  // scope 外仍引用它 → B2 补偿：cue 轨缓存行恢复、源不写普通 location 行、快照保持隐藏。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto scopeCacheState = [&] {
    const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
    return locations.size() == 1U && locations[0].filePath == cue && locations[0].sourceFilePath == source;
  };
  watchers->states[0]->callback(directoryEvent(scopeDir, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (scopeCacheState()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(scopeCacheState());
  CHECK(std::ranges::none_of(sidecar.loadLocationsByRoot(canonicalRootPath(temp.path())),
                             [&](const cache::CachedLocation& location) { return location.filePath == source; }));
  {
    const auto songs = songsIn(service->snapshot());
    REQUIRE(songs.size() == 1U);
    CHECK(songs[0].filePath == cue);
  }

  // cue 改为不再引用源：cue 父目录 scoped 重解析后，被抑制的源必须补孤儿 upsert，
  // 快照与缓存都要与全量重建一致（B2 家族收敛）。
  clearTestCueSheetProvider();
  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  writeText(cue, "REM DUMMY COMMENT v2\n");
  watchers->states[0]->callback(fileEvent(cue, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    if (songs.size() == 1U && songs[0].filePath == source) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto patchedSongs = songsIn(service->snapshot());
  REQUIRE(patchedSongs.size() == 1U);
  CHECK(patchedSongs[0].filePath == source);
  const auto patchedLocations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(patchedLocations.size() == 1U);
  CHECK(patchedLocations[0].filePath == source);

  const auto publishedBefore = service->snapshot().generatedAt;
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotRepublish(*service, publishedBefore);
  const auto rebuiltSongs = songsIn(service->snapshot());
  REQUIRE(rebuiltSongs.size() == patchedSongs.size());
  CHECK(rebuiltSongs[0].filePath == patchedSongs[0].filePath);
  const auto rebuiltLocations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(rebuiltLocations.size() == patchedLocations.size());
  CHECK(rebuiltLocations[0].filePath == patchedLocations[0].filePath);
}

TEST_CASE("scanner watcher scoped cost gate switches oversized scopes to reconcile") {
  test::TempScannerRoot temp{"scanner-watcher-scope-cost-gate"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
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

  // 播种 > max(500, root 文件数/10) 条 scope 前缀缓存行：成本门必须把 scoped 全读改判 Reconcile。
  const auto big = temp.path() / "big";
  std::filesystem::create_directories(big);
  cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  cache::CachedScanRoot seededRoot{};
  seededRoot.rootPath = canonicalRootPath(temp.path());
  seededRoot.directoryTreeHash = "seeded-hash";
  seededRoot.totalFiles = 501;
  cache.updateScanRoot(seededRoot);
  SongMetadata meta;
  meta.title = "Big";
  meta.duration = std::chrono::milliseconds{1000};
  cache.upsertContent("big-content", meta);
  for (int index = 0; index < 501; ++index) {
    cache::CachedLocation location{};
    location.locationId = "big-loc-" + std::to_string(index);
    location.contentId = "big-content";
    location.rootPath = canonicalRootPath(temp.path());
    location.filePath = big / ("t" + std::to_string(index) + ".flac");
    location.sourceFilePath = location.filePath;
    cache.upsertLocation(location);
  }

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  // Modified Directory → 候选 scope；成本门（501 > 500）→ requestReconcile（ScanStarted 可见），
  // 而不是 scoped 全读（scoped 契约不发 ScanStarted）。
  watchers->states[0]->callback(directoryEvent(big, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (scanStartedCount(events) > startedBefore) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) > startedBefore);
  }
  // Reconcile 只对账变化文件：磁盘 big 目录为空，无标签重读（成本门的目的）。
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  CHECK(reader->readCount() == 1U);
}

// T1 成本门边界（B5）：缓存行数恰好 == max(500, root 文件数/10) 时不越界，保持 scoped
// 语义（无 ScanStarted，仅 PlaylistSnapshotUpdated），而非误判为 Reconcile。
TEST_CASE("scanner watcher scoped cost gate boundary at exact threshold stays scoped") {
  test::TempScannerRoot temp{"scanner-watcher-scope-cost-gate-boundary"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
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

  // totalFiles=5000 → 阈值 = max(500, 500) = 500；恰好播种 500 行（== 阈值）。
  const auto big = temp.path() / "big";
  std::filesystem::create_directories(big);
  cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  cache::CachedScanRoot seededRoot{};
  seededRoot.rootPath = canonicalRootPath(temp.path());
  seededRoot.directoryTreeHash = "seeded-hash";
  seededRoot.totalFiles = 5000;
  cache.updateScanRoot(seededRoot);
  SongMetadata meta;
  meta.title = "Big";
  meta.duration = std::chrono::milliseconds{1000};
  cache.upsertContent("big-content", meta);
  for (int index = 0; index < 500; ++index) {
    cache::CachedLocation location{};
    location.locationId = "big-loc-" + std::to_string(index);
    location.contentId = "big-content";
    location.rootPath = canonicalRootPath(temp.path());
    location.filePath = big / ("t" + std::to_string(index) + ".flac");
    location.sourceFilePath = location.filePath;
    cache.upsertLocation(location);
  }

  std::size_t startedBefore = 0;
  std::size_t snapshotBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    snapshotBefore = eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated);
  }
  watchers->states[0]->callback(directoryEvent(big, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) > snapshotBefore) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
    CHECK(eventTypeCount(events, ScannerEventType::PlaylistSnapshotUpdated) > snapshotBefore);
  }
  // scoped 合并清掉 scope 前缀下的缓存行（磁盘 big 为空 = 磁盘真相）。
  const auto normalizedKey = [](const std::filesystem::path& path) { return pathToUtf8(path.lexically_normal()); };
  CHECK(cache.countLocationsByPathPrefix(normalizedKey(canonicalRootPath(temp.path())), normalizedKey(big)) == 0);
}

// 单文件根 Modified：根自身即已索引歌曲，精准 upsert 必须直接以根路径为 abs
// （root/basename 不存在，修复前静默 no-op、标签修改丢失），且不触发 ScanStarted。
TEST_CASE("scanner watcher single file root modified updates snapshot and cache without scan") {
  test::TempScannerRoot temp{"scanner-watcher-single-file-modified"};
  const auto audio = test::writeAudioFixture(temp.path(), "single.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(audio, rawMetadata("Single Before"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = audio}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);
  service->startWatching({ScannerRoot{.path = audio}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  reader->put(audio, rawMetadata("Single After"));
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  writeText(audio, "SERIONA_TEST_AUDIO\nframes=2048\nchanged\n");
  watchers->states[0]->callback(fileEvent(audio, WatchEffectKind::Modified));

  waitForSongTitle(*service, audio, "Single After");
  CHECK(reader->readCount() == 2U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(audio));
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == audio);
  const auto content = sidecar.loadContent(locations[0].contentId);
  REQUIRE(content.has_value());
  CHECK(content->metadata.title == "Single After");
}

// 单文件根 Destroyed：删除必须用根路径作为 abs，否则缓存零删除、allSongs_ 按 basename 擦除
// → 快照/缓存分歧；且根自身删除不回落重扫（无 ScanStarted）。
TEST_CASE("scanner watcher single file root destroyed clears snapshot and cache") {
  test::TempScannerRoot temp{"scanner-watcher-single-file-destroyed"};
  const auto audio = test::writeAudioFixture(temp.path(), "single.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(audio, rawMetadata("Single"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = audio}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = audio}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  std::error_code removeError;
  REQUIRE(std::filesystem::remove(audio, removeError));
  REQUIRE_FALSE(removeError);
  watchers->states[0]->callback(fileEvent(audio, WatchEffectKind::Destroyed));

  waitForSnapshotSongCount(*service, 0U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(audio)).empty());
}

// 未扫描单文件根的首个事件：树未种子化 → 提交该 root 的 Reconcile（lastScanMode 恒
// Incremental，永不升级 Full）；身份哈希写入后周期探测哈希命中，不再有空转扫描。
TEST_CASE("scanner watcher first event on an un-scanned single file root indexes via reconcile") {
  test::TempScannerRoot temp{"scanner-watcher-single-file-unscanned"};
  const auto audio = test::writeAudioFixture(temp.path(), "single.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(audio, rawMetadata("Single"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{30}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->startWatching({ScannerRoot{.path = audio}});
  REQUIRE(watchers->states.size() == 1U);
  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  watchers->states[0]->callback(fileEvent(audio, WatchEffectKind::Modified));

  waitForSnapshotSongCount(*service, 1U);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == audio);
  CHECK(reader->readCount() == 1U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(audio);
  const auto scanRoot = sidecar.loadScanRoot(rootPath);
  REQUIRE(scanRoot.has_value());
  CHECK(scanRoot->lastScanMode == ScanMode::Incremental);
  CHECK(sidecar.loadLocationsByRoot(rootPath).size() == 1U);

  // 周期探测静默窗口：Reconcile 记录身份哈希后哈希命中 → 不再排扫描（修复前会每 30ms 空转）。
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  std::size_t startedAfterReconcile = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedAfterReconcile = scanStartedCount(events);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedAfterReconcile);
  }
  CHECK(reader->readCount() == 1U);
}

// B8 收敛保证：用户 Incremental（树哈希命中）不得清除"事件丢失"置位的脏标记；
// 后续周期 Reconcile 仍被排入并收敛内容。
TEST_CASE("scanner watcher incremental scan keeps pending reconcile until a real reconcile") {
  test::TempScannerRoot temp{"scanner-watcher-dirty-flag"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]before\n");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{5},
      // 周期探测放到 200ms 之后：确保用户 Incremental 先于周期 Reconcile 运行，
      // 否则探测会先行合法地消费脏标记，测试无法区分"用户扫描清脏"。
      .reconcileInterval = std::chrono::milliseconds{200}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  waitForSongLyrics(*service, song, LyricsSource::ExternalLrc, "before");
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 歌词 hash 取消 → 批次置脏并提前结束（不排 Reconcile）：模拟事件丢失后的脏根。
  std::atomic_size_t cancelledProbes{0};
  {
    const LyricsHashProviderGuard cancellation{[&cancelledProbes](const std::filesystem::path& path, const HashOptions&) {
      cancelledProbes.fetch_add(1U);
      return FileHashResult{.hash = std::nullopt,
                            .errors = {HashError{.code = HashErrorCode::Cancelled,
                                                 .scannerError = ScannerError{.code = ScannerErrorCode::Cancelled,
                                                                              .message = "test cancellation",
                                                                              .detail = {},
                                                                              .path = path}}}};
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{3});
    writeText(temp.path() / "song.lrc", "[00:02.00]after\n");
    watchers->states[0]->callback(fileEvent(temp.path() / "song.lrc", WatchEffectKind::Modified));
    for (auto attempt = 0; attempt != 1000 && cancelledProbes.load() == 0U; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(cancelledProbes.load() >= 1U);
  }
  // 批次在 provider 返回后立即置脏；给去抖线程一个短窗口完成收尾。
  std::this_thread::sleep_for(std::chrono::milliseconds{5});

  std::size_t startedBeforeUserScan = 0;
  std::size_t completedBeforeUserScan = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBeforeUserScan = scanStartedCount(events);
    completedBeforeUserScan = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  // 用户 Incremental：.lrc 不进树哈希 → 哈希命中 → decision.mode == Incremental。
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  waitForScanCompletedCount(events, eventsMutex, completedBeforeUserScan + 1U);

  // 修复前：用户 Incremental 无条件清脏 → 周期探测哈希命中 → 永不再排 Reconcile。
  bool extraReconcileScheduled = false;
  for (auto attempt = 0; attempt != 400; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      // +1 = 用户扫描自身；> +1 即脏标记仍在、周期探测排入了 Reconcile。
      if (scanStartedCount(events) > startedBeforeUserScan + 1U) {
        extraReconcileScheduled = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(extraReconcileScheduled);
  waitForSongLyrics(*service, song, LyricsSource::ExternalLrc, "after");
}

// 数据卫生：伪/错误 Destroyed（路径仍在磁盘）不得删除活索引行；应置脏 + Reconcile。
TEST_CASE("scanner watcher keeps index when destroy event finds the file still on disk") {
  test::TempScannerRoot temp{"scanner-watcher-spurious-destroy"};
  const auto seed = test::writeAudioFixture(temp.path(), "seed.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(seed, rawMetadata("Seed"));
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

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  REQUIRE(std::filesystem::exists(seed));
  watchers->states[0]->callback(fileEvent(seed, WatchEffectKind::Destroyed));

  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore + 1U);
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == seed);
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == seed);
}

// 封面侧车 Renamed：与封面 Created 同等处理（父目录 scope + forceTagReread），
// 不得落入泛型 rename 分支回落 Reconcile 而让歌曲级 artwork 停留陈旧。
TEST_CASE("scanner watcher cover rename re-reads directory tags so song artwork is refreshed") {
  test::TempScannerRoot temp{"scanner-watcher-cover-rename"};
  const auto music = temp.path() / "music";
  const auto song = test::writeAudioFixture(music, "01.flac");
  const auto cover = music / "cover.jpg";
  const auto renamed = music / "artwork.png";
  writeText(cover, "fake cover bytes");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  auto withCover = rawMetadata("Song");
  withCover.coverPath = temp.path() / "export" / "cover.png";
  withCover.thumbnailPath = temp.path() / "export" / "thumb.png";
  reader->put(song, withCover);
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
  const auto artworkBefore = songsIn(service->snapshot())[0].artworkPath;
  REQUIRE(artworkBefore.has_value());
  REQUIRE_FALSE(artworkBefore->empty());
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  reader->put(song, rawMetadata("Song"));
  std::filesystem::rename(cover, renamed);
  watchers->states[0]->callback(renamedFileEvent(cover, renamed));
  waitForReadCount(*reader, 2U);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    if (songs.size() == 1U && (!songs[0].artworkPath.has_value() || songs[0].artworkPath->empty())) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK((!songs[0].artworkPath.has_value() || songs[0].artworkPath->empty()));
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }
}

// .CUE（大写）跨界定：isCueEntry 必须与 isCueSheetPath 同为大小写不敏感，否则 cue 目录
// 移除后孤儿源不重 upsert（TagReader::ReadCueSheet 不做扩展名检查，照常解析 .CUE）。
TEST_CASE("scanner watcher uppercase CUE removal restores the orphaned source audio") {
  test::TempScannerRoot temp{"scanner-watcher-uppercase-cue"};
  const auto cues = temp.path() / "cues";
  const auto album = temp.path() / "album";
  std::filesystem::create_directories(cues);
  std::filesystem::create_directories(album);
  const auto source = album / "s.flac";
  const auto cue = cues / "ALBUM.CUE";
  writeText(source, "fake source audio");
  writeText(cue, "REM DUMMY COMMENT\n");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(source, rawMetadata("Source Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  setTestCueSheetProvider([&source](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "ALBUM.CUE") {
      return {{.audioFilePath = source,
               .offset = 0,
               .duration = 180000000,
               .title = "Cue Track 1",
               .artist = "Cue Artist",
               .album = "Cue Album",
               .trackNumber = 1}};
    }
    return {};
  });
  struct CueProviderClear {
    ~CueProviderClear() { clearTestCueSheetProvider(); }
  } cueProviderClear;

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  std::error_code removeError;
  std::filesystem::remove_all(cues, removeError);
  REQUIRE_FALSE(removeError);
  watchers->states[0]->callback(directoryEvent(cues, WatchEffectKind::Destroyed));

  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    if (songs.size() == 1U && songs[0].filePath == source) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == source);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == source);
}

// 非 ASCII 根（CJK）的 scoped 合并：B2 补偿必须经 path 查缓存（Windows ACP 回归）；
// Linux 上两种写法等价，但代码路径被真实执行。
TEST_CASE("scanner watcher scoped merge restores external cue rows under a non-ASCII root") {
  test::TempScannerRoot temp{"scanner-watcher-cjk-scope-external-cue"};
  const auto root = temp.path() / "音乐库";
  const auto scopeDir = root / "album";
  const auto cueDir = root / "cues";
  std::filesystem::create_directories(scopeDir);
  std::filesystem::create_directories(cueDir);
  const auto source = scopeDir / "s.flac";
  const auto cue = cueDir / "c.cue";
  writeText(source, "fake source audio");
  writeText(cue, "REM DUMMY COMMENT\n");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(source, rawMetadata("Source Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);
  setTestCueSheetProvider([&source](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "c.cue") {
      return {{.audioFilePath = source,
               .offset = 0,
               .duration = 180000000,
               .title = "Cue Track 1",
               .artist = "Cue Artist",
               .album = "Cue Album",
               .trackNumber = 1}};
    }
    return {};
  });
  struct CueProviderClear {
    ~CueProviderClear() { clearTestCueSheetProvider(); }
  } cueProviderClear;

  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = root}});
  REQUIRE(watchers->states.size() == 1U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(root);
  const auto scopeCacheState = [&] {
    const auto locations = sidecar.loadLocationsByRoot(rootPath);
    return locations.size() == 1U && locations[0].filePath == cue && locations[0].sourceFilePath == source;
  };
  watchers->states[0]->callback(directoryEvent(scopeDir, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (scopeCacheState()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(scopeCacheState());
  CHECK(std::ranges::none_of(sidecar.loadLocationsByRoot(rootPath),
                             [&](const cache::CachedLocation& location) { return location.filePath == source; }));
}

// .lrc 精准更新与 memo：一个 .lrc 被两个 cue 轨共享 → 只解析一次；树更新按
// logicalTrackId 键落点；每个 cue 轨的缓存行 lyricsSource/外部歌词都正确。
TEST_CASE("scanner watcher cue sidecar lrc update refreshes all cue tracks with a single parse") {
  test::TempScannerRoot temp{"scanner-watcher-cue-lrc"};
  const auto cue = temp.path() / "album.cue";
  const auto source = temp.path() / "album.flac";
  // 含 FILE 行：discoverScannerPaths 的引用解析据此把源音频排除出普通音频候选，
  // 缓存里只应有 2 个 cue 轨行（与全量语义一致）。
  writeText(cue, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:01:00\n");
  writeText(source, "fake source audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(source, rawMetadata("Source Track"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  setTestCueSheetProvider([&source](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    return {{.audioFilePath = source,
             .offset = 0,
             .duration = 60000000,
             .title = "Track 1",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1},
            {.audioFilePath = source,
             .offset = 60000000,
             .duration = 60000000,
             .title = "Track 2",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 2}};
  });
  struct CueProviderClear {
    ~CueProviderClear() { clearTestCueSheetProvider(); }
  } cueProviderClear;

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::atomic_size_t parseCount{0};
  {
    const LrcParseObserverGuard parseObserver{[&parseCount](const std::filesystem::path&) {
      parseCount.fetch_add(1U);
    }};
    std::size_t startedBefore = 0;
    {
      std::scoped_lock lock{eventsMutex};
      startedBefore = scanStartedCount(events);
    }
    writeText(temp.path() / "album.lrc", "[00:02.00]cue external\n");
    watchers->states[0]->callback(fileEvent(temp.path() / "album.lrc", WatchEffectKind::Created));
    for (auto attempt = 0; attempt != 1000; ++attempt) {
      const auto songs = songsIn(service->snapshot());
      const auto allUpdated = songs.size() == 2U && std::ranges::all_of(songs, [](const SongMetadata& metadata) {
                                return metadata.effectiveLyricsSource == LyricsSource::ExternalLrc &&
                                       metadata.effectiveLyrics.size() == 1U &&
                                       metadata.effectiveLyrics[0].text == "cue external";
                              });
      if (allUpdated) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const auto songs = songsIn(service->snapshot());
    REQUIRE(songs.size() == 2U);
    CHECK(std::ranges::all_of(songs, [](const SongMetadata& metadata) {
      return metadata.effectiveLyricsSource == LyricsSource::ExternalLrc && metadata.effectiveLyrics.size() == 1U &&
             metadata.effectiveLyrics[0].text == "cue external";
    }));
    CHECK(parseCount.load() == 1U);
    {
      std::scoped_lock lock{eventsMutex};
      CHECK(scanStartedCount(events) == startedBefore);
    }
  }

  // 树键仍走 logicalTrackId：cue 轨节点 id == "track:" + logicalTrackId。
  std::size_t cueTrackNodes = 0;
  for (const auto& node : service->snapshot().nodes) {
    if (node.kind != PlaylistNodeKind::Track || !node.song.has_value() ||
        node.song->logicalTrackId.find("#track") == std::string::npos) {
      continue;
    }
    ++cueTrackNodes;
    CHECK(node.nodeId == "track:" + node.song->logicalTrackId);
  }
  CHECK(cueTrackNodes == 2U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 2U);
  for (const auto& location : locations) {
    CHECK(location.filePath == cue);
    CHECK(location.lyricsSource == LyricsSource::ExternalLrc);
    const auto external = sidecar.loadLyrics(location.locationId, "external");
    REQUIRE(external.size() == 1U);
    CHECK(external[0].text == "cue external");
  }
}

// 非递归根：addScope 必须透传真实 recursive=false —— 封面 scope 不得按递归子树枚举，
// 否则子树内容被误索引，与 runScopedScan 的非递归守卫矛盾。
TEST_CASE("scanner watcher non recursive root cover scope stays non recursive") {
  test::TempScannerRoot temp{"scanner-watcher-non-recursive-scope"};
  const auto loose = test::writeAudioFixture(temp.path(), "loose.flac");
  const auto sub = temp.path() / "sub";
  const auto deep = test::writeAudioFixture(sub / "nested", "deep.flac");
  const auto cover = sub / "cover.jpg";
  writeText(cover, "fake cover bytes");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(loose, rawMetadata("Loose"));
  reader->put(deep, rawMetadata("Deep"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path(), .recursive = false}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  CHECK(reader->readCount() == 1U);
  service->startWatching({ScannerRoot{.path = temp.path(), .recursive = false}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  watchers->states[0]->callback(fileEvent(cover, WatchEffectKind::Created));
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == loose);
  CHECK(reader->readCount() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(std::ranges::none_of(sidecar.loadLocationsByRoot(canonicalRootPath(temp.path())),
                             [&](const cache::CachedLocation& location) { return location.filePath == deep; }));
}

// B4：缓存不可读（scan_roots 缺失）→ Reconcile 中止：既有索引保留、零标签重读、无 Full。
TEST_CASE("scanner watcher reconcile aborts and keeps the index when the cache is unreadable") {
  test::TempScannerRoot temp{"scanner-watcher-reconcile-cache-unreadable"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
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

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  dropScanRootTable(scannerSidecarPath(temp));

  watchers->states[0]->callback(watcherMessage("w/sys/q_overflow@"));
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == song);
  CHECK(reader->readCount() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore + 1U);
  }
}


// 单文件 cue 根 Modified：cue 父目录在 root 之外，scope⊆root 守卫必须拒绝该 scope 并回落
// Reconcile。修复前会递归枚举父目录，在 cue 根下生成 "../" 幽灵节点与幽灵缓存行
// （父目录中未被 cue 引用的 sibling.flac 被当作普通音频索引）。
TEST_CASE("scanner watcher single file cue root modified never enumerates its parent directory") {
  test::TempScannerRoot temp{"scanner-watcher-single-file-cue"};
  const auto cues = temp.path() / "cues";
  const auto cue = cues / "album.cue";
  const auto sibling = cues / "sibling.flac";
  writeText(cue, "FILE \"other-source.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeText(sibling, "fake sibling audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(sibling, rawMetadata("Sibling Phantom"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  setTestCueSheetProvider([&cues](const std::filesystem::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {{.audioFilePath = cues / "other-source.flac",
               .offset = 0,
               .duration = 60000000,
               .title = "Cue Track",
               .artist = "Cue Artist",
               .album = "Cue Album",
               .trackNumber = 1}};
    }
    return {};
  });
  struct CueProviderClear {
    ~CueProviderClear() { clearTestCueSheetProvider(); }
  } cueProviderClear;

  service->scan({ScannerRoot{.path = cue}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = cue}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{3});  // mtime granularity guard
  writeText(cue, "FILE \"other-source.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\nREM changed\n");
  watchers->states[0]->callback(fileEvent(cue, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (scanStartedCount(events) > startedBefore) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == cue);
  for (const auto& node : service->snapshot().nodes) {
    CHECK(node.displayName != "..");
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(cue));
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == cue);
  CHECK(locations[0].sourceFilePath == cues / "other-source.flac");
  // sibling.flac 从未被当作普通音频标签读取（幽灵 scope 的判别信号）。
  CHECK(reader->readCount() == 0U);
}

// 封面文件自身作为单文件根 Modified：父目录在 root 之外，scope⊆root 守卫必须拒绝并回落
// Reconcile（修复前会递归枚举父目录，把未被引用的 sibling 音频索引成 "../" 幽灵条目）。
TEST_CASE("scanner watcher cover file root modified never enumerates its parent directory") {
  test::TempScannerRoot temp{"scanner-watcher-cover-file-root"};
  const auto covers = temp.path() / "covers";
  const auto cover = covers / "cover.jpg";
  const auto sibling = covers / "sibling.flac";
  writeText(cover, "fake cover bytes");
  writeText(sibling, "fake sibling audio");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(sibling, rawMetadata("Sibling Phantom"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  service->scan({ScannerRoot{.path = cover}}, ScanMode::Full);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  service->startWatching({ScannerRoot{.path = cover}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{3});  // mtime granularity guard
  writeText(cover, "changed cover bytes");
  watchers->states[0]->callback(fileEvent(cover, WatchEffectKind::Modified));
  for (auto attempt = 0; attempt != 2000; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (scanStartedCount(events) > startedBefore) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  CHECK(songsIn(service->snapshot()).empty());
  for (const auto& node : service->snapshot().nodes) {
    CHECK(node.displayName != "..");
  }
  CHECK(reader->readCount() == 0U);
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(cover)).empty());
}

// 跨根封面 rename：每个端点按自身归属 root 建 scope。修复前两端共用旧端 root，新端 scope
// 越界被守卫拒绝后只对账旧根，新根歌曲 artwork 永不刷新。
TEST_CASE("scanner watcher cross root cover rename refreshes both roots artwork") {
  test::TempScannerRoot temp{"scanner-watcher-cross-root-cover-rename"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "album";
  const auto albumB = rootB / "album";
  const auto songA = test::writeAudioFixture(albumA, "a.flac");
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  const auto coverA = albumA / "cover.jpg";
  const auto coverB = albumB / "artwork.png";
  writeText(coverA, "cover a");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  auto metaA = rawMetadata("A");
  metaA.coverPath = temp.path() / "export" / "a-cover.png";
  metaA.thumbnailPath = temp.path() / "export" / "a-thumb.png";
  auto metaB = rawMetadata("B");
  metaB.coverPath = temp.path() / "export" / "b-cover.png";
  metaB.thumbnailPath = temp.path() / "export" / "b-thumb.png";
  reader->put(songA, metaA);
  reader->put(songB, metaB);
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  std::filesystem::rename(coverA, coverB);
  watchers->states[0]->callback(renamedFileEvent(coverA, coverB));

  // 修复前只对账 rootA（readCount=3）；修复后两个根各自 forceTagReread → readCount=4。
  waitForReadCount(*reader, 4U);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    const auto artworkCleared = [&](const std::filesystem::path& path) {
      const auto found = std::ranges::find_if(songs, [&](const SongMetadata& song) { return song.filePath == path; });
      return found != songs.end() && (!found->artworkPath.has_value() || found->artworkPath->empty());
    };
    if (songs.size() == 2U && artworkCleared(songA) && artworkCleared(songB)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 2U);
  for (const auto& song : songs) {
    CHECK((!song.artworkPath.has_value() || song.artworkPath->empty()));
  }
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
}

// 根级封面 rename（parent == root）：无子目录 scope 可入，必须走整根 scope 强制重读
// （成本门内），否则歌曲 artwork 永远停留陈旧（Reconcile 直灌缓存、哈希刷新后探测不再触发）。
TEST_CASE("scanner watcher root level cover rename re-reads tags through a whole root scope") {
  test::TempScannerRoot temp{"scanner-watcher-root-cover-rename"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto cover = temp.path() / "cover.jpg";
  const auto renamed = temp.path() / "artwork.png";
  writeText(cover, "fake cover bytes");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  auto withCover = rawMetadata("Song");
  withCover.coverPath = temp.path() / "export" / "cover.png";
  withCover.thumbnailPath = temp.path() / "export" / "thumb.png";
  reader->put(song, withCover);
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
  const auto artworkBefore = songsIn(service->snapshot())[0].artworkPath;
  REQUIRE(artworkBefore.has_value());
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  reader->put(song, rawMetadata("Song"));
  std::filesystem::rename(cover, renamed);
  watchers->states[0]->callback(renamedFileEvent(cover, renamed));

  waitForReadCount(*reader, 2U);
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto songs = songsIn(service->snapshot());
    if (songs.size() == 1U && (!songs[0].artworkPath.has_value() || songs[0].artworkPath->empty())) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK((!songs[0].artworkPath.has_value() || songs[0].artworkPath->empty()));
  // 整根 scope 走 scoped 对账路径：只发 PlaylistSnapshotUpdated，不发 ScanStarted（非 Full）。
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 1U);
  CHECK((!locations[0].artworkPath.has_value() || locations[0].artworkPath->empty()));
}

// 根级封面变更超过成本门（max(500, rootFiles/10)）：回落 Reconcile（有界兜底，不整根重读
// 标签、不 Full）；这是已记录的接受限制：大根封面事件不强制刷新歌曲 artwork。
TEST_CASE("scanner watcher root level cover change over the cost gate falls back to a bounded reconcile") {
  test::TempScannerRoot temp{"scanner-watcher-root-cover-cost-gate"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto cover = temp.path() / "cover.jpg";
  const auto renamed = temp.path() / "artwork.png";
  writeText(cover, "fake cover bytes");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
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

  // totalFiles=5000 → 阈值 = max(500, 500) = 500；播种 501 行 + 真实行 = 502 > 阈值。
  cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  cache::CachedScanRoot seededRoot{};
  seededRoot.rootPath = canonicalRootPath(temp.path());
  seededRoot.directoryTreeHash = "seeded-hash";
  seededRoot.totalFiles = 5000;
  cache.updateScanRoot(seededRoot);
  SongMetadata meta;
  meta.title = "Fake";
  meta.duration = std::chrono::milliseconds{1000};
  cache.upsertContent("fake-content", meta);
  for (int index = 0; index < 501; ++index) {
    cache::CachedLocation location{};
    location.locationId = "fake-loc-" + std::to_string(index);
    location.contentId = "fake-content";
    location.rootPath = canonicalRootPath(temp.path());
    location.filePath = temp.path() / ("fake" + std::to_string(index) + ".flac");
    location.sourceFilePath = location.filePath;
    cache.upsertLocation(location);
  }

  std::size_t startedBefore = 0;
  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  std::filesystem::rename(cover, renamed);
  watchers->states[0]->callback(renamedFileEvent(cover, renamed));

  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  // 兜底 Reconcile：仅一次扫描、零标签重读（音频未变 → locationId 命中直灌缓存），非 Full。
  CHECK(reader->readCount() == 1U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore + 1U);
  }
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == song);
  const auto scanRoot = cache.loadScanRoot(canonicalRootPath(temp.path()));
  REQUIRE(scanRoot.has_value());
  CHECK(scanRoot->lastScanMode == ScanMode::Incremental);
}

// action == None 的缓存写门槛：readExternalLyrics=false 时 .lrc 事件不得改写 location 行的
// lyrics_source / external_lrc_hash（与全量扫描语义一致）。
TEST_CASE("scanner watcher lrc event does not rewrite cache rows when external lyrics are disabled") {
  test::TempScannerRoot temp{"scanner-watcher-lrc-disabled-gate"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto lrc = temp.path() / "song.lrc";
  writeText(lrc, "[00:01.00]before\n");

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
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
  waitForSongLyrics(*service, song, LyricsSource::ExternalLrc, "before");

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  auto locations = sidecar.loadLocationsByRoot(rootPath);
  REQUIRE(locations.size() == 1U);
  REQUIRE(locations[0].externalLrcHash.has_value());
  const auto oldHash = *locations[0].externalLrcHash;
  CHECK(locations[0].lyricsSource == LyricsSource::ExternalLrc);

  service->configure(ScannerConfig{.readExternalLyrics = false});
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  const auto before = service->snapshot().generatedAt;
  std::this_thread::sleep_for(std::chrono::milliseconds{3});  // mtime granularity guard
  writeText(lrc, "[00:02.00]after\n");
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Modified));
  waitForSnapshotRepublish(*service, before);

  locations = sidecar.loadLocationsByRoot(rootPath);
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].lyricsSource == LyricsSource::ExternalLrc);
  CHECK(locations[0].externalLrcHash == oldHash);
  const auto external = sidecar.loadLyrics(locations[0].locationId, "external");
  REQUIRE(external.size() == 1U);
  CHECK(external[0].text == "before");
}

// 跨根同 rel 布局的 T0 目录删除：rootA/album 被移出根只应擦除 rootA 的条目与缓存行。
// 修复前 allSongs_ 按 treeRelativePath 前缀跨根擦除，rootB 的同 rel 歌曲从快照/树永久消失
// （其缓存行仍在 → 快照/缓存分歧，直到 rootB 重扫才恢复）。
TEST_CASE("scanner watcher cross root directory removal keeps the other root's songs") {
  test::TempScannerRoot temp{"scanner-watcher-cross-root-remove"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "album";
  const auto albumB = rootB / "album";
  const auto songA = test::writeAudioFixture(albumA, "a.flac");
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);

  std::error_code removeError;
  std::filesystem::remove_all(albumA, removeError);
  REQUIRE_FALSE(removeError);
  watchers->states[0]->callback(directorySelfEvent(albumA));

  waitForSnapshotSongCount(*service, 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == songB);
  // 合并树 album 节点由 rootB 的存活条目重建：目录节点仍在、track 节点 id 为 rootB 的 rel。
  bool albumNodeFound = false;
  bool trackNodeFound = false;
  for (const auto& node : service->snapshot().nodes) {
    if (node.kind == PlaylistNodeKind::Directory && node.displayName == "album") {
      albumNodeFound = true;
    }
    if (node.song.has_value() && node.song->filePath == songB) {
      trackNodeFound = true;
      CHECK(node.nodeId == "track:album/b.flac");
    }
  }
  CHECK(albumNodeFound);
  CHECK(trackNodeFound);
  // 缓存：rootA 行删除、rootB 行保留（SQL 本就按根作用域，不得被跨根擦除连坐）。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  CHECK(sidecar.loadLocationsByRoot(canonicalRootPath(rootA)).empty());
  const auto locationsB = sidecar.loadLocationsByRoot(canonicalRootPath(rootB));
  REQUIRE(locationsB.size() == 1U);
  CHECK(locationsB[0].filePath == songB);
  {
    std::scoped_lock lock{eventsMutex};
    // 精准删除路径：无全根 ScanStarted（不回落重扫）。
    CHECK(scanStartedCount(events) == 1U);
  }
}

// 跨根同 rel 布局的目录 rename：rootA/album → rootA/album-renamed 只应改写 rootA 的条目。
// 修复前 rewriteAllSongsForRename 按 rel 前缀改写 rootB 的 treeRelativePath（绝对路径不匹配
// 而保持 rootB 原值）→ 树节点挂在 rootA 的新 rel 下、物理路径却指向 rootB（快照指向不一致）。
TEST_CASE("scanner watcher cross root directory rename keeps the other root's paths") {
  test::TempScannerRoot temp{"scanner-watcher-cross-root-rename"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "album";
  const auto albumB = rootB / "album";
  const auto songA = test::writeAudioFixture(albumA, "a.flac");
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeWatcherService(temp, reader, watchers);
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);

  const auto albumRenamed = rootA / "album-renamed";
  std::error_code renameError;
  std::filesystem::rename(albumA, albumRenamed, renameError);
  REQUIRE_FALSE(renameError);
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  WatchEvent rename = directoryEvent(albumA, WatchEffectKind::Renamed);
  rename.associated.push_back(directoryEvent(albumRenamed, WatchEffectKind::Renamed));
  watchers->states[0]->callback(rename);

  waitForSnapshotSongPath(*service, albumRenamed / "a.flac");
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (songsIn(service->snapshot()).size() == 2U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 2U);
  // rootB 的 track 节点必须留在 album 前缀下（修复前被改写到 rootA 的 album-renamed 前缀）。
  std::optional<std::string> trackNodeIdA;
  std::optional<std::string> trackNodeIdB;
  bool albumNodeFound = false;
  bool renamedNodeFound = false;
  for (const auto& node : service->snapshot().nodes) {
    if (node.kind == PlaylistNodeKind::Directory && node.displayName == "album") {
      albumNodeFound = true;
    }
    if (node.kind == PlaylistNodeKind::Directory && node.displayName == "album-renamed") {
      renamedNodeFound = true;
    }
    if (node.song.has_value()) {
      if (node.song->filePath == albumRenamed / "a.flac") {
        trackNodeIdA = node.nodeId;
      }
      if (node.song->filePath == songB) {
        trackNodeIdB = node.nodeId;
      }
    }
  }
  CHECK(albumNodeFound);
  CHECK(renamedNodeFound);
  REQUIRE(trackNodeIdA.has_value());
  REQUIRE(trackNodeIdB.has_value());
  CHECK(*trackNodeIdA == "track:album-renamed/a.flac");
  CHECK(*trackNodeIdB == "track:album/b.flac");
  // rename 不重读标签（路径改写来自缓存/既有条目）。
  CHECK(reader->readCount() == 2U);
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == 1U);
  }
}

// 跨根同 rel 布局的文件夹缩略图缓存：删除 rootA/album 后 album 节点只剩 rootB 的歌曲，
// 必须用 rootB 的物理目录解析/回填；rootB 的缓存行不得被 rootA 的删除擦除，也不得被
// rootA 的路径回填。修复前 rootB 条目被跨根擦除 → album 节点消失（缩略图与歌曲一起丢失）。
TEST_CASE("scanner watcher cross root directory removal keeps the other root's folder thumbnail") {
  test::TempScannerRoot temp{"scanner-watcher-cross-root-thumbnail"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "album";
  const auto albumB = rootB / "album";
  const auto songA = test::writeAudioFixture(albumA, "a.flac");
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  writeText(albumA / "cover.marker", "a");
  writeText(albumB / "cover.marker", "b");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = [](const std::filesystem::path& directory) -> std::optional<std::filesystem::path> {
        if (std::filesystem::exists(directory / "cover.marker")) {
          return directory / "thumb.png";
        }
        return std::nullopt;
      },
      .watcherDebounce = std::chrono::milliseconds{5}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  const auto albumThumbnail = [&]() -> std::optional<std::string> {
    for (const auto& node : service->snapshot().nodes) {
      if (node.kind == PlaylistNodeKind::Directory && node.displayName == "album") {
        return node.thumbnailPath;
      }
    }
    return std::nullopt;
  };

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  const auto initialThumbnail = albumThumbnail();
  REQUIRE(initialThumbnail.has_value());
  // 首个物理根（rootA，allSongs_ 按 root 顺序合并）赢得解析。
  CHECK(std::filesystem::path{*initialThumbnail} == albumA / "thumb.png");
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);

  std::error_code removeError;
  std::filesystem::remove_all(albumA, removeError);
  REQUIRE_FALSE(removeError);
  watchers->states[0]->callback(directorySelfEvent(albumA));
  waitForSnapshotSongCount(*service, 1U);
  // album 节点必须切换为 rootB 的物理目录解析值（rootB 缓存行被误擦且未重解析 → nullopt）。
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    const auto thumbnail = albumThumbnail();
    if (thumbnail.has_value() && std::filesystem::path{*thumbnail} == albumB / "thumb.png") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto afterRemoval = albumThumbnail();
  REQUIRE(afterRemoval.has_value());
  CHECK(std::filesystem::path{*afterRemoval} == albumB / "thumb.png");

  // 未受影响批次（rootB 其他目录新增文件）必须从 rootB 的缓存行回填 album 缩略图：
  // 若 rootB 的行被 rootA 的删除连坐擦除，此处会清成 nullopt。
  const auto otherSong = test::writeAudioFixture(rootB / "other", "c.flac");
  reader->put(otherSong, rawMetadata("C"));
  watchers->states[1]->callback(fileEvent(otherSong, WatchEffectKind::Created));
  waitForSnapshotSongCount(*service, 2U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  const auto afterUnrelated = albumThumbnail();
  REQUIRE(afterUnrelated.has_value());
  CHECK(std::filesystem::path{*afterUnrelated} == albumB / "thumb.png");
}

// 根级封面事件（wholeRoot scope）只应重解析该根自身的目录缩略图，不得把整库目录都当作
// "受影响"（空 touched 路径语义）而重跑 seam。修复前 rootB 的目录也会被重新解析。
TEST_CASE("scanner watcher whole root cover scope re-resolves only its own root thumbnails") {
  test::TempScannerRoot temp{"scanner-watcher-whole-root-thumbnail-scope"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "albumA";
  const auto albumB = rootB / "albumB";
  const auto songA = test::writeAudioFixture(albumA, "a.flac");
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  writeText(albumA / "cover.marker", "a");
  writeText(albumB / "cover.marker", "b");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  std::mutex seamMutex;
  std::vector<std::filesystem::path> seamDirectories;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = [&seamDirectories, &seamMutex](
                                 const std::filesystem::path& directory) -> std::optional<std::filesystem::path> {
        {
          std::scoped_lock lock{seamMutex};
          seamDirectories.push_back(directory);
        }
        if (std::filesystem::exists(directory / "cover.marker")) {
          return directory / "thumb.png";
        }
        return std::nullopt;
      },
      .watcherDebounce = std::chrono::milliseconds{5}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);
  {
    std::scoped_lock lock{seamMutex};
    seamDirectories.clear();
  }

  // rootA 根级封面新增 → wholeRoot scope（forceTagReread 重读 rootA 全部标签）。
  const auto cover = rootA / "cover.jpg";
  const auto before = service->snapshot().generatedAt;
  writeText(cover, "fake cover bytes");
  watchers->states[0]->callback(fileEvent(cover, WatchEffectKind::Created));
  waitForReadCount(*reader, 3U);
  waitForSnapshotRepublish(*service, before);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  bool resolvedRootA = false;
  bool resolvedRootB = false;
  {
    std::scoped_lock lock{seamMutex};
    for (const auto& directory : seamDirectories) {
      if (directory == albumA) {
        resolvedRootA = true;
      }
      if (directory == albumB) {
        resolvedRootB = true;
      }
    }
  }
  // rootA 的目录受本批影响（重解析）；rootB 的目录必须走缓存回填，不触发 seam。
  CHECK(resolvedRootA);
  CHECK_FALSE(resolvedRootB);
}

// wholeRoot 且该根没有子目录（受影响目录集合为空）：不得落入"空 touched 路径 = 整库受影响"
// 语义去重解析其他根的目录（哨兵路径保证零 seam 调用）。
TEST_CASE("scanner watcher whole root cover scope without subdirectories resolves no other root thumbnails") {
  test::TempScannerRoot temp{"scanner-watcher-whole-root-thumbnail-empty"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto songA = test::writeAudioFixture(rootA, "a.flac");
  const auto albumB = rootB / "albumB";
  const auto songB = test::writeAudioFixture(albumB, "b.flac");
  writeText(albumB / "cover.marker", "b");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(songA, rawMetadata("A"));
  reader->put(songB, rawMetadata("B"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  std::mutex seamMutex;
  std::vector<std::filesystem::path> seamDirectories;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = [&seamDirectories, &seamMutex](
                                 const std::filesystem::path& directory) -> std::optional<std::filesystem::path> {
        {
          std::scoped_lock lock{seamMutex};
          seamDirectories.push_back(directory);
        }
        if (std::filesystem::exists(directory / "cover.marker")) {
          return directory / "thumb.png";
        }
        return std::nullopt;
      },
      .watcherDebounce = std::chrono::milliseconds{5}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  service->startWatching({ScannerRoot{.path = rootA}, ScannerRoot{.path = rootB}});
  REQUIRE(watchers->states.size() == 2U);
  {
    std::scoped_lock lock{seamMutex};
    seamDirectories.clear();
  }

  // rootA 根级封面新增 → wholeRoot scope；rootA 无子目录，受影响目录集合为空。
  const auto cover = rootA / "cover.jpg";
  const auto before = service->snapshot().generatedAt;
  writeText(cover, "fake cover bytes");
  watchers->states[0]->callback(fileEvent(cover, WatchEffectKind::Created));
  waitForReadCount(*reader, 3U);
  waitForSnapshotRepublish(*service, before);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  std::size_t seamCalls = 0;
  {
    std::scoped_lock lock{seamMutex};
    seamCalls = seamDirectories.size();
  }
  // 修复前空 touched 路径 = 整库受影响 → albumB 也会被重解析。
  CHECK(seamCalls == 0U);
}

#if !defined(_WIN32)
// 不可读目录根（chmod 000）：opendir 失败但 stat 成功。必须识别为 rootUnavailable（保留既有
// 索引与缓存、上报错误），绝不能把空枚举当磁盘真相合并而清空该根。
TEST_CASE("scanner watcher unreadable directory root keeps the index and reports an error") {
  test::TempScannerRoot temp{"scanner-watcher-unreadable-root"};
  if (::geteuid() == 0) {
    MESSAGE("running as root: permission bits are not enforced; skipping");
    return;
  }
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
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

  struct RootPermissionGuard {
    std::filesystem::path path;
    ~RootPermissionGuard() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } permissionGuard{temp.path()};
  std::error_code permissionError;
  std::filesystem::permissions(temp.path(), std::filesystem::perms::none, permissionError);
  REQUIRE_FALSE(permissionError);

  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == song);
  CHECK(reader->readCount() == 1U);
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == song);
  bool errorReported = false;
  {
    std::scoped_lock lock{eventsMutex};
    errorReported = std::ranges::any_of(events, [&](const ScannerEvent& event) {
      if (event.type != ScannerEventType::ScanError || !std::holds_alternative<ScannerError>(event.payload)) {
        return false;
      }
      const auto& error = std::get<ScannerError>(event.payload);
      return error.path.has_value() && canonicalRootPath(*error.path) == canonicalRootPath(temp.path());
    });
  }
  CHECK(errorReported);
}

// 子项遍历/分类错误（symlink 环 → Error 条目）：可读根不得被当作不可用（根条目仍处理、活行
// 保留）。本用例的置脏信号来自 denied 子目录（目录自身被显式探测为 PermissionDenied 且其
// 隐藏歌曲被本次 prune 擦除，location 计数下降）——由周期 Reconcile 重读恢复；纯 symlink 环
// 错误但不发生 prune 时不再反复置脏（避免每周期一次永久 Reconcile 循环）。
TEST_CASE("scanner watcher child traversal error marks the root dirty and restores pruned rows") {
  test::TempScannerRoot temp{"scanner-watcher-child-traversal-error"};
  if (::geteuid() == 0) {
    MESSAGE("running as root: permission bits are not enforced; skipping");
    return;
  }
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto denied = temp.path() / "denied";
  const auto hidden = test::writeAudioFixture(denied, "hidden.flac");
  const auto linkA = temp.path() / "a-link";
  const auto linkB = temp.path() / "b-link";
  std::error_code linkError;
  std::filesystem::create_symlink("b-link", linkA, linkError);
  if (linkError) {
    MESSAGE("symlink creation unavailable: " << linkError.message());
    return;
  }
  std::filesystem::create_symlink("a-link", linkB, linkError);
  REQUIRE_FALSE(linkError);

  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
  reader->put(hidden, rawMetadata("Hidden"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{40}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  service->configure(ScannerConfig{.followSymlinks = true});

  struct DeniedPermissionGuard {
    std::filesystem::path path;
    ~DeniedPermissionGuard() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } permissionGuard{denied};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::error_code permissionError;
  std::filesystem::permissions(denied, std::filesystem::perms::none, permissionError);
  REQUIRE_FALSE(permissionError);
  std::size_t completedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
  }
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  // 根未被整体跳过：可读的 song.flac 仍在索引（Error 条目不构成 rootUnavailable）。
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (songsIn(service->snapshot()).size() == 1U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto partialSongs = songsIn(service->snapshot());
  REQUIRE(partialSongs.size() == 1U);
  CHECK(partialSongs[0].filePath == song);

  // 部分枚举置脏：文件系统无任何变化，周期探测也必须排入一次 Reconcile（哈希门控被绕过）。
  std::size_t startedAfterPartial = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedAfterPartial = scanStartedCount(events);
  }
  bool dirtyDrivenReconcile = false;
  for (auto attempt = 0; attempt != 600; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (scanStartedCount(events) > startedAfterPartial) {
        dirtyDrivenReconcile = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(dirtyDrivenReconcile);

  // 错误条件解除后，后续 Reconcile 重读缓存缺失文件 → 被 prune 的行恢复（不永久丢失）。
  std::filesystem::permissions(denied, std::filesystem::perms::owner_all, permissionError);
  REQUIRE_FALSE(permissionError);
  waitForSnapshotSongCount(*service, 2U);
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 2U);
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == hidden;
  }));
}

// 持续存在的遍历错误（symlink 环）若未 prune 任何行，不得每次探测都置脏：哈希未变 + 无脏标记
// → 周期探测不排程，不产生每周期一次的永久 Reconcile 循环。修复前"枚举不完整即无条件置脏"
// 会让探测每 40ms 排入一次全根 Reconcile（本用例在 300ms ≈ 7 个周期内断言零新增扫描）。
TEST_CASE("scanner watcher persistent traversal error without pruned rows does not loop reconciles") {
  test::TempScannerRoot temp{"scanner-watcher-traversal-error-no-loop"};
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto linkA = temp.path() / "a-link";
  const auto linkB = temp.path() / "b-link";
  std::error_code linkError;
  std::filesystem::create_symlink("b-link", linkA, linkError);
  if (linkError) {
    MESSAGE("symlink creation unavailable: " << linkError.message());
    return;
  }
  std::filesystem::create_symlink("a-link", linkB, linkError);
  REQUIRE_FALSE(linkError);
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{40}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });
  service->configure(ScannerConfig{.followSymlinks = true});

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  std::size_t startedBefore = 0;
  {
    std::scoped_lock lock{eventsMutex};
    startedBefore = scanStartedCount(events);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{300});
  {
    std::scoped_lock lock{eventsMutex};
    CHECK(scanStartedCount(events) == startedBefore);
  }
  // 遍历错误仍被 surface（部分枚举信号保留，只是不再反复置脏）。
  std::size_t scanErrorCount = 0;
  {
    std::scoped_lock lock{eventsMutex};
    scanErrorCount = eventTypeCount(events, ScannerEventType::ScanError);
  }
  CHECK(scanErrorCount > 0U);
}

// 无 symlink 环：chmod 000 子目录（含一首歌）必须被显式探测为 PermissionDenied 条目 → 部分
// 枚举 prune 掉隐藏歌曲 → 置脏，周期探测排入 Reconcile（磁盘无任何变化）；权限恢复后隐藏
// 歌曲回归。修复前 skip_permission_denied 静默跳过目录内容：无错误条目 → 不置脏，目录树哈希
// 也未变 → 周期探测永不排程，被 prune 的行永久丢失。
TEST_CASE("scanner watcher unreadable subdirectory marks the root dirty and restores hidden songs") {
  test::TempScannerRoot temp{"scanner-watcher-unreadable-subdir"};
  if (::geteuid() == 0) {
    MESSAGE("running as root: permission bits are not enforced; skipping");
    return;
  }
  const auto song = test::writeAudioFixture(temp.path(), "song.flac");
  const auto denied = temp.path() / "denied";
  const auto hidden = test::writeAudioFixture(denied, "hidden.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(song, rawMetadata("Song"));
  reader->put(hidden, rawMetadata("Hidden"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = watchers,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{5},
      .reconcileInterval = std::chrono::milliseconds{40}});
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  struct DeniedPermissionGuard {
    std::filesystem::path path;
    ~DeniedPermissionGuard() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } permissionGuard{denied};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForSnapshotSongCount(*service, 2U);
  CHECK(reader->readCount() == 2U);

  std::error_code permissionError;
  std::filesystem::permissions(denied, std::filesystem::perms::none, permissionError);
  REQUIRE_FALSE(permissionError);
  std::size_t completedBefore = 0;
  std::size_t startedBeforePartial = 0;
  {
    std::scoped_lock lock{eventsMutex};
    completedBefore = eventTypeCount(events, ScannerEventType::ScanCompleted);
    // 基线取在本次 Full 之前：本次 Full 自身 +1，脏驱动的周期 Reconcile 再 +1（+2 即置脏证据）。
    startedBeforePartial = scanStartedCount(events);
  }
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForScanCompletedCount(events, eventsMutex, completedBefore + 1U);
  // 部分枚举的 prune：隐藏歌曲从快照消失，可读歌曲保留（根未被整体跳过）。
  for (auto attempt = 0; attempt != 1000; ++attempt) {
    if (songsIn(service->snapshot()).size() == 1U) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  const auto partialSongs = songsIn(service->snapshot());
  REQUIRE(partialSongs.size() == 1U);
  CHECK(partialSongs[0].filePath == song);

  // Full 之后再开始监视：周期探测只可能在置脏后运行，避免探测先于本次 Full 抢先 prune 的竞态。
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  // 置脏：文件系统无任何变化，周期探测也必须排入一次 Reconcile（哈希门控被绕过）。
  bool dirtyDrivenReconcile = false;
  for (auto attempt = 0; attempt != 600; ++attempt) {
    {
      std::scoped_lock lock{eventsMutex};
      if (scanStartedCount(events) >= startedBeforePartial + 2U) {
        dirtyDrivenReconcile = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  CHECK(dirtyDrivenReconcile);

  // 权限恢复后隐藏歌曲回归（不永久丢失）：哈希变化驱动的探测 Reconcile 重读缓存缺失文件。
  std::filesystem::permissions(denied, std::filesystem::perms::owner_all, permissionError);
  REQUIRE_FALSE(permissionError);
  waitForSnapshotSongCount(*service, 2U);
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  REQUIRE(locations.size() == 2U);
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == hidden;
  }));
}
#endif
}
}
