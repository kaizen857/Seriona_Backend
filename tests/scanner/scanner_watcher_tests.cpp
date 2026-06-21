#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

class FakeWatcherMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path&) override {
    std::scoped_lock lock{mutex_};
    requestedPaths.push_back(path);
    auto iterator = metadataByPath_.find(path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata");
    }
    auto metadata = iterator->second;
    metadata.filePath = path;
    return metadata;
  }

  [[nodiscard]] std::size_t readCount() const noexcept {
    std::scoped_lock lock{mutex_};
    return requestedPaths.size();
  }

  std::vector<std::filesystem::path> requestedPaths;

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
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
  [[nodiscard]] std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                     WatchEventCallback callback) override {
    auto state = std::make_shared<CapturedFolderWatcher::State>();
    state->root = root;
    state->callback = std::move(callback);
    states.push_back(state);
    return std::make_unique<CapturedFolderWatcher>(std::move(state));
  }

  std::vector<std::shared_ptr<CapturedFolderWatcher::State>> states;
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
  for (auto attempt = 0; attempt != 100; ++attempt) {
    if (reader.readCount() >= expected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for fake TagReader read count");
}

void waitForSnapshotSongCount(const FileScannerService& service, std::size_t expected) {
  for (auto attempt = 0; attempt != 100; ++attempt) {
    if (songsIn(service.snapshot()).size() == expected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  FAIL("timed out waiting for watcher reconciliation snapshot");
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

TEST_CASE("scanner watcher debounces create modify destroy rename into hash-first rescans") {
  test::TempScannerRoot temp{"scanner-watcher-debounce"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  CHECK(watchers->states[0]->root == temp.path());
  CHECK(reader->readCount() == 1U);

  const auto created = test::writeAudioFixture(temp.path(), "created.flac");
  reader->put(created, rawMetadata("Created"));
  const auto renamed = temp.path() / "renamed.flac";
  std::filesystem::rename(first, renamed);
  reader->put(renamed, rawMetadata("Renamed"));
  WatchEvent rename = fileEvent(renamed, WatchEffectKind::Renamed);
  rename.associated.push_back(fileEvent(first, WatchEffectKind::Renamed));
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
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);
  CHECK(reader->readCount() == 1U);

  const auto lrc = temp.path() / "song.lrc";
  writeText(lrc, "[00:02.00]external\n");
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Modified));
  waitForSnapshotSongCount(*service, 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
  auto songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external");

  std::filesystem::remove(lrc);
  watchers->states[0]->callback(fileEvent(lrc, WatchEffectKind::Destroyed));
  std::this_thread::sleep_for(std::chrono::milliseconds{30});
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
  auto service = makeWatcherService(temp, reader, watchers);
  std::vector<ScannerEvent> events;
  std::mutex eventsMutex;
  service->setEventSink([&events, &eventsMutex](ScannerEvent event) {
    std::scoped_lock lock{eventsMutex};
    events.push_back(std::move(event));
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  service->startWatching({ScannerRoot{.path = temp.path()}});
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  reader->put(second, rawMetadata("Second"));
  REQUIRE(watchers->states.size() == 1U);
  watchers->states[0]->callback(watcherMessage("w_sys_q_overflow"));

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

TEST_CASE("scanner watcher stop closes watcher and ignores later callbacks") {
  test::TempScannerRoot temp{"scanner-watcher-stop"};
  const auto first = test::writeAudioFixture(temp.path(), "first.flac");
  auto reader = std::make_shared<FakeWatcherMetadataReader>();
  reader->put(first, rawMetadata("First"));
  auto watchers = std::make_shared<CapturingWatcherFactory>();
  auto service = makeWatcherService(temp, reader, watchers);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
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

}
}
