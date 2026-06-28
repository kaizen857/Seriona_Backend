#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
#include "seriona/scanner/directory_tree_hash.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

class FakeServiceMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }
  void fail(std::filesystem::path path, std::string message) { failures_[std::move(path)] = std::move(message); }
  void blockUntilReleased() noexcept { blockReads_ = true; }
  void blockPathUntilReleased(std::filesystem::path path) {
    blockPath_ = std::move(path);
  }
  void resetRelease() {
    std::lock_guard lock{mutex_};
    released_ = false;
    blocked_ = false;
  }
  void release() {
    {
      std::lock_guard lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }
  [[nodiscard]] bool waitForBlockedRead(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return blocked_; });
  }

  [[nodiscard]] bool waitForReadCount(std::size_t expectedCount, std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, expectedCount] { return requestedPaths.size() >= expectedCount; });
  }

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override {
    {
      std::lock_guard lock{mutex_};
      requestedPaths.push_back(path);
      requestedCoverDirs.push_back(coverExportDir);
    }
    changed_.notify_all();
    if (blockReads_ || path == blockPath_) {
      std::unique_lock lock{mutex_};
      blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return released_; });
    }
    const auto failure = failures_.find(path);
    if (failure != failures_.end()) {
      throw std::runtime_error(failure->second);
    }
    auto iterator = metadataByPath_.find(path);
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
  std::map<std::filesystem::path, std::string> failures_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool blockReads_{false};
  std::filesystem::path blockPath_{};
  bool blocked_{false};
  bool released_{false};
};

[[nodiscard]] RawTagMetadata rawMetadata(std::string title, std::vector<RawTagLyricLine> lyrics = {}) {
  RawTagMetadata raw{};
  raw.title = std::move(title);
  raw.artist = "Artist";
  raw.album = "Album";
  raw.embeddedLyrics = std::move(lyrics);
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
                                                              std::shared_ptr<FakeServiceMetadataReader> reader) {
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

[[nodiscard]] std::vector<SongMetadata> songsInPublishedOrder(const PlaylistTreeSnapshot& snapshot) {
  std::vector<SongMetadata> songs;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value()) {
      songs.push_back(*node.song);
    }
  }
  return songs;
}

template <typename Predicate>
[[nodiscard]] PlaylistTreeSnapshot waitForSnapshot(const FileScannerService& service, Predicate predicate) {
  for (auto attempts = 0; attempts < 2000; ++attempts) {
    auto snapshot = service.snapshot();
    if (predicate(snapshot)) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return service.snapshot();
}

[[nodiscard]] PlaylistTreeSnapshot waitForSongs(const FileScannerService& service, std::size_t expectedCount) {
  return waitForSnapshot(service, [expectedCount](const PlaylistTreeSnapshot& snapshot) {
    return songsIn(snapshot).size() == expectedCount;
  });
}

void waitForReaderCount(const FakeServiceMetadataReader& reader, std::size_t expectedCount) {
  for (auto attempts = 0; attempts < 100 && reader.readCount() != expectedCount; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

[[nodiscard]] std::vector<ScannerError> errorsFrom(const std::vector<ScannerEvent>& events) {
  std::vector<ScannerError> errors;
  for (const auto& event : events) {
    if (event.type == ScannerEventType::ScanError && std::holds_alternative<ScannerError>(event.payload)) {
      errors.push_back(std::get<ScannerError>(event.payload));
    }
  }
  return errors;
}

[[nodiscard]] const SongMetadata& songByPath(const std::vector<SongMetadata>& songs, const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(songs, path, &SongMetadata::filePath);
  if (iterator == songs.end()) {
    throw std::runtime_error("missing song in scanner service test");
  }
  return *iterator;
}

[[nodiscard]] const PlaylistNode& nodeById(const PlaylistTreeSnapshot& snapshot, std::string_view nodeId) {
  const auto iterator = std::ranges::find_if(snapshot.nodes, [nodeId](const PlaylistNode& node) {
    return node.nodeId == nodeId;
  });
  if (iterator == snapshot.nodes.end()) {
    throw std::runtime_error("missing playlist node in scanner service test");
  }
  return *iterator;
}

class ScannerEventLog {
public:
  void push(ScannerEvent event) {
    std::lock_guard lock{mutex_};
    events_.push_back(std::move(event));
  }

  [[nodiscard]] std::vector<ScannerError> errors() const {
    std::lock_guard lock{mutex_};
    return errorsFrom(events_);
  }

  [[nodiscard]] bool waitForEvent(ScannerEventType type, std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock{mutex_};
        if (std::ranges::any_of(events_, [type](const ScannerEvent& event) { return event.type == type; })) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
  }

  [[nodiscard]] bool waitForEventCount(ScannerEventType type, std::size_t expectedCount, std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock{mutex_};
        const auto count = static_cast<std::size_t>(std::ranges::count(events_, type, &ScannerEvent::type));
        if (count >= expectedCount) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
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

class ScopedEnvVar {
public:
  ScopedEnvVar(std::string name, std::string value) : name_{std::move(name)} {
    if (const auto* existing = std::getenv(name_.c_str())) {
      previous_ = existing;
    }
    setenv(name_.c_str(), value.c_str(), 1);
  }

  ~ScopedEnvVar() {
    if (previous_.has_value()) {
      setenv(name_.c_str(), previous_->c_str(), 1);
      return;
    }
    unsetenv(name_.c_str());
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

TEST_CASE("scanner service scans hashes caches lyrics and skips unchanged rereads") {
  test::TempScannerRoot temp{"scanner-service-cache-hit"};
  const auto first = test::writeAudioFixture(temp.path(), "song.flac");
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external one\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First", {RawTagLyricLine{std::chrono::milliseconds{500}, "embedded one"}}));
  reader->put(second, rawMetadata("Second", {RawTagLyricLine{std::chrono::milliseconds{700}, "embedded two"}}));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto firstSnapshot = waitForSongs(*service, 2U);
  const auto firstSongs = songsIn(firstSnapshot);

  REQUIRE(firstSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(firstSongs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(firstSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(firstSongs[1].effectiveLyrics.size() == 1U);
  CHECK(firstSongs[1].effectiveLyrics[0].text == "external one");

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto cachedSongs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(cachedSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(cachedSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(cachedSongs[1].effectiveLyrics[0].text == "external one");
}

TEST_CASE("scanner service cache lookup reuses a high-count cached root") {
  test::TempScannerRoot temp{"scanner-service-cache-lookup"};
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  std::vector<std::filesystem::path> audioPaths;
  constexpr auto cachedSongCount = 96U;
  audioPaths.reserve(cachedSongCount);
  for (auto index = 0U; index < cachedSongCount; ++index) {
    auto filename = std::string{"track-"} + std::to_string(index) + ".flac";
    auto audioPath = test::writeAudioFixture(temp.path(), std::move(filename));
    reader->put(audioPath, rawMetadata("Track " + std::to_string(index)));
    audioPaths.push_back(std::move(audioPath));
  }
  const auto& tailAudio = audioPaths.back();
  writeText(tailAudio.parent_path() / "track-95.lrc", "[00:01.00]tail cached lyric\n");
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(waitForSongs(*service, cachedSongCount));

  REQUIRE(songs.size() == cachedSongCount);
  CHECK(reader->readCount() == cachedSongCount);
  CHECK(songByPath(songs, tailAudio).effectiveLyricsSource == LyricsSource::ExternalLrc);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(waitForSongs(*service, cachedSongCount));

  CHECK(reader->readCount() == cachedSongCount);
  REQUIRE(songs.size() == cachedSongCount);
  const auto& tailSong = songByPath(songs, tailAudio);
  CHECK(tailSong.effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(tailSong.effectiveLyrics.size() == 1U);
  CHECK(tailSong.effectiveLyrics[0].text == "tail cached lyric");
}

TEST_CASE("scanner service rereads changed audio and reparses only changed lrc") {
  test::TempScannerRoot temp{"scanner-service-reconcile"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external one\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Before", {RawTagLyricLine{std::chrono::milliseconds{300}, "embedded before"}}));
  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  static_cast<void>(waitForSongs(*service, 1U));
  CHECK(reader->readCount() == 1U);

  writeText(temp.path() / "song.lrc", "[00:02.00]external two\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(waitForSnapshot(*service, [](const PlaylistTreeSnapshot& snapshot) {
    const auto currentSongs = songsIn(snapshot);
    return currentSongs.size() == 1U && !currentSongs[0].effectiveLyrics.empty() && currentSongs[0].effectiveLyrics[0].text == "external two";
  }));

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external two");

  writeText(audio, "changed audio bytes");
  reader->put(audio, rawMetadata("After", {RawTagLyricLine{std::chrono::milliseconds{400}, "embedded after"}}));
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(waitForSnapshot(*service, [](const PlaylistTreeSnapshot& snapshot) {
    const auto currentSongs = songsIn(snapshot);
    return currentSongs.size() == 1U && currentSongs[0].title == "After";
  }));

  CHECK(reader->readCount() == 2U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "After");
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songs[0].effectiveLyrics[0].text == "external two");
}

TEST_CASE("scanner service handles new and deleted lrc without tagreader and prunes deleted audio") {
  test::TempScannerRoot temp{"scanner-service-delete"};
  const auto embeddedAudio = test::writeAudioFixture(temp.path(), "embedded.flac");
  const auto plainAudio = test::writeAudioFixture(temp.path(), "plain.flac");
  const auto deletedAudio = test::writeAudioFixture(temp.path(), "deleted.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(embeddedAudio, rawMetadata("Embedded", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded lyric"}}));
  reader->put(plainAudio, rawMetadata("Plain"));
  reader->put(deletedAudio, rawMetadata("Deleted"));
  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForReaderCount(*reader, 3U);
  CHECK(reader->readCount() == 3U);

  writeText(temp.path() / "embedded.lrc", "[00:01.00]external embedded\n");
  writeText(temp.path() / "plain.lrc", "[00:01.00]external plain\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(waitForSnapshot(*service, [&embeddedAudio, &plainAudio](const PlaylistTreeSnapshot& snapshot) {
    const auto currentSongs = songsIn(snapshot);
    return currentSongs.size() == 3U && songByPath(currentSongs, embeddedAudio).effectiveLyricsSource == LyricsSource::ExternalLrc &&
           songByPath(currentSongs, plainAudio).effectiveLyricsSource == LyricsSource::ExternalLrc;
  }));

  CHECK(reader->readCount() == 3U);
  REQUIRE(songs.size() == 3U);
  CHECK(songByPath(songs, embeddedAudio).effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songByPath(songs, plainAudio).effectiveLyricsSource == LyricsSource::ExternalLrc);

  std::filesystem::remove(temp.path() / "embedded.lrc");
  std::filesystem::remove(temp.path() / "plain.lrc");
  std::filesystem::remove(deletedAudio);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  waitForReaderCount(*reader, 3U);
  songs = songsIn(waitForSongs(*service, 2U));

  CHECK(reader->readCount() == 3U);
  REQUIRE(songs.size() == 2U);
  const auto& embeddedSong = songByPath(songs, embeddedAudio);
  CHECK(embeddedSong.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(embeddedSong.effectiveLyrics.size() == 1U);
  CHECK(embeddedSong.effectiveLyrics[0].text == "embedded lyric");
  const auto& plainSong = songByPath(songs, plainAudio);
  CHECK(plainSong.effectiveLyricsSource == LyricsSource::None);
  CHECK(plainSong.effectiveLyrics.empty());
}

TEST_CASE("scanner service supports single file roots with same basename lrc") {
  test::TempScannerRoot temp{"scanner-service-single-file"};
  const auto audio = test::writeAudioFixture(temp.path(), "single.flac");
  writeText(temp.path() / "single.lrc", "[00:01.00]single external\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Single", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = audio}}, ScanMode::Full);
  const auto snapshot = waitForSongs(*service, 1U);
  const auto songs = songsIn(snapshot);

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == audio);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songs[0].effectiveLyrics[0].text == "single external");

  CHECK(snapshot.nodes.size() == 2U);
  CHECK(nodeById(snapshot, "track:single.flac").parentNodeId == snapshot.rootNodeId);
}

TEST_CASE("scanner service publishes nested directory hierarchy for directory roots") {
  test::TempScannerRoot temp{"scanner-service-hierarchy"};
  const auto audio = test::writeAudioFixture(temp.path(), "artists/album/nested.flac");
  writeText(temp.path() / "artists" / "album" / "nested.lrc", "[00:01.00]nested external\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Nested"));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto snapshot = waitForSongs(*service, 1U);
  const auto songs = songsIn(snapshot);

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == audio);
  const auto& artists = nodeById(snapshot, "dir:artists");
  const auto& album = nodeById(snapshot, "dir:artists/album");
  const auto& track = nodeById(snapshot, "track:artists/album/nested.flac");
  CHECK(artists.kind == PlaylistNodeKind::Directory);
  CHECK(album.kind == PlaylistNodeKind::Directory);
  CHECK(track.kind == PlaylistNodeKind::Track);
  CHECK(artists.parentNodeId == snapshot.rootNodeId);
  REQUIRE(album.parentNodeId.has_value());
  CHECK(*album.parentNodeId == artists.nodeId);
  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == album.nodeId);
  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId.ends_with(".lrc"); }));
}

TEST_CASE("scanner service records failures malformed lrc cancellation and preserves cache") {
  test::TempScannerRoot temp{"scanner-service-errors"};
  const auto good = test::writeAudioFixture(temp.path(), "good.flac");
  const auto broken = test::writeAudioFixture(temp.path(), "broken.flac");
  writeText(temp.path() / "good.lrc", "[00:99.00]bad timestamp\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(good, rawMetadata("Good", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded good"}}));
  reader->fail(broken, "broken metadata");
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(waitForSongs(*service, 1U));
  auto errors = eventLog.errors();

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "Good");
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(reader->readCount() == 2U);
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.code == ScannerErrorCode::MetadataReadFailed; }));
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.message == "failed to parse external lyrics"; }));

  service->stop();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(service->snapshot());
  REQUIRE(eventLog.waitForEvent(ScannerEventType::ScanStopped, std::chrono::seconds{1}));
  errors = eventLog.errors();

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "Good");
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.code == ScannerErrorCode::Cancelled; }));
}

TEST_CASE("scanner service processes audio candidates through the worker pool") {
  test::TempScannerRoot temp{"scanner-service-worker-pool"};
  const auto blocked = test::writeAudioFixture(temp.path(), "blocked.flac");
  const auto parallel = test::writeAudioFixture(temp.path(), "parallel.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(blocked, rawMetadata("Blocked"));
  reader->put(parallel, rawMetadata("Parallel"));
  reader->blockPathUntilReleased(blocked);
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForBlockedRead(std::chrono::seconds{1}));
  waitForReaderCount(*reader, 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(service->snapshot().nodes.empty());

  reader->release();
  const auto songs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(songs.size() == 2U);
  CHECK(songByPath(songs, blocked).title == "Blocked");
  CHECK(songByPath(songs, parallel).title == "Parallel");
}

TEST_CASE("scanner service honors configured worker and tagreader concurrency") {
  test::TempScannerRoot temp{"scanner-service-config-concurrency"};
  const auto blocked = test::writeAudioFixture(temp.path(), "01-blocked.flac");
  const auto parallel = test::writeAudioFixture(temp.path(), "02-parallel.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(blocked, rawMetadata("Blocked"));
  reader->put(parallel, rawMetadata("Parallel"));
  reader->blockUntilReleased();
  auto service = makeService(temp, reader);
  service->configure(ScannerConfig{.workerCount = 2U, .tagReaderConcurrency = 2});

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForReadCount(2U, std::chrono::seconds{1}));
  CHECK(service->snapshot().nodes.empty());

  reader->release();
  const auto songs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(songs.size() == 2U);
  CHECK(reader->readCount() == 2U);
}

TEST_CASE("scanner service can force serial fallback from scanner config") {
  test::TempScannerRoot temp{"scanner-service-config-serial"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  reader->blockUntilReleased();
  auto service = makeService(temp, reader);
  service->configure(ScannerConfig{.workerCount = 1U, .tagReaderConcurrency = 1});

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForReadCount(1U, std::chrono::seconds{1}));
  CHECK_FALSE(reader->waitForReadCount(2U, std::chrono::milliseconds{20}));

  reader->release();
  const auto songs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(songs.size() == 2U);
  CHECK(reader->readCount() == 2U);
}

TEST_CASE("scanner service applies env worker overrides and disable concurrency wins") {
  ScopedEnvVar workers{"SERIONA_SCANNER_WORKERS", "3"};
  ScopedEnvVar tagReaders{"SERIONA_SCANNER_TAGREADER_CONCURRENCY", "3"};
  ScopedEnvVar disableConcurrency{"SERIONA_SCANNER_DISABLE_CONCURRENCY", "1"};
  test::TempScannerRoot temp{"scanner-service-env-serial"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  reader->blockUntilReleased();
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForReadCount(1U, std::chrono::seconds{1}));
  CHECK_FALSE(reader->waitForReadCount(2U, std::chrono::milliseconds{20}));

  reader->release();
  const auto songs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(songs.size() == 2U);
  CHECK(reader->readCount() == 2U);
}

TEST_CASE("scanner service ignores malformed env overrides and keeps config fallback") {
  ScopedEnvVar workers{"SERIONA_SCANNER_WORKERS", "invalid"};
  ScopedEnvVar tagReaders{"SERIONA_SCANNER_TAGREADER_CONCURRENCY", "0"};
  ScopedEnvVar disableConcurrency{"SERIONA_SCANNER_DISABLE_CONCURRENCY", "maybe"};
  test::TempScannerRoot temp{"scanner-service-env-malformed"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  reader->blockUntilReleased();
  auto service = makeService(temp, reader);
  service->configure(ScannerConfig{.workerCount = 2U, .tagReaderConcurrency = 2});

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForReadCount(2U, std::chrono::seconds{1}));

  reader->release();
  const auto songs = songsIn(waitForSongs(*service, 2U));

  REQUIRE(songs.size() == 2U);
  CHECK(reader->readCount() == 2U);
}

TEST_CASE("scanner config disables incremental mode and can force full scans") {
  test::TempScannerRoot temp{"scanner-service-config-scan-mode"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  const auto databasePath = temp.path().parent_path() / (temp.path().filename().generic_string() + "-cache.sqlite");
  const auto sidecarPath = std::filesystem::path{databasePath.generic_string() + ".scan-roots-v3.sqlite"};
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Song"));
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = reader,
                                                                       .watcherFactory = nullptr,
                                                                       .databasePath = databasePath,
                                                                       .coverExportDir = temp.path() / "covers"});
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });
  const auto rootPath = canonicalRootPath(temp.path());

  auto lastScanMode = [&sidecarPath, &rootPath] {
    cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = sidecarPath}};
    auto scanRoot = sidecar.loadScanRoot(rootPath);
    REQUIRE(scanRoot.has_value());
    return scanRoot->lastScanMode;
  };

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));
  CHECK(lastScanMode() == ScanMode::Incremental);

  service->configure(ScannerConfig{.enableIncrementalScan = false});
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 3U, std::chrono::seconds{1}));
  CHECK(lastScanMode() == ScanMode::Full);

  service->configure(ScannerConfig{.forceFull = true});
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 4U, std::chrono::seconds{1}));
  CHECK(lastScanMode() == ScanMode::Full);
}

TEST_CASE("scanner service publishes worker results in discovered file order") {
  test::TempScannerRoot temp{"scanner-service-event-ordering"};
  const auto first = test::writeAudioFixture(temp.path(), "01-first.flac");
  const auto second = test::writeAudioFixture(temp.path(), "02-second.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First"));
  reader->put(second, rawMetadata("Second"));
  reader->blockPathUntilReleased(first);
  auto service = makeService(temp, reader);
  std::vector<SongMetadata> publishedSongs;
  std::mutex publishedMutex;
  service->setEventSink([&publishedSongs, &publishedMutex](ScannerEvent event) {
    if (event.type == ScannerEventType::FileScanned && std::holds_alternative<SongMetadata>(event.payload)) {
      std::lock_guard lock{publishedMutex};
      publishedSongs.push_back(std::get<SongMetadata>(event.payload));
    }
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);

  REQUIRE(reader->waitForBlockedRead(std::chrono::seconds{1}));
  waitForReaderCount(*reader, 2U);
  reader->release();
  const auto snapshotSongs = songsInPublishedOrder(waitForSongs(*service, 2U));

  std::vector<SongMetadata> eventSongs;
  {
    std::lock_guard lock{publishedMutex};
    eventSongs = publishedSongs;
  }
  REQUIRE(eventSongs.size() == 2U);
  REQUIRE(snapshotSongs.size() == 2U);
  CHECK(eventSongs[0].filePath == first);
  CHECK(eventSongs[1].filePath == second);
  CHECK(snapshotSongs[0].filePath == first);
  CHECK(snapshotSongs[1].filePath == second);
}

TEST_CASE("scanner service incremental integration reuses unchanged scans only added and changed and prunes deleted") {
  test::TempScannerRoot temp{"scanner-service-incremental-integration"};
  const auto unchanged = test::writeAudioFixture(temp.path(), "01-unchanged.flac");
  const auto changed = test::writeAudioFixture(temp.path(), "02-changed.flac");
  const auto deleted = test::writeAudioFixture(temp.path(), "03-deleted.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(unchanged, rawMetadata("Unchanged"));
  reader->put(changed, rawMetadata("Changed Before"));
  reader->put(deleted, rawMetadata("Deleted"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(waitForSongs(*service, 3U));

  REQUIRE(songs.size() == 3U);
  CHECK(reader->readCount() == 3U);

  writeText(changed, "changed bytes for incremental path");
  reader->put(changed, rawMetadata("Changed After"));
  const auto added = test::writeAudioFixture(temp.path(), "04-added.flac");
  reader->put(added, rawMetadata("Added"));
  std::filesystem::remove(deleted);
  forceNextScanIncrementalForCurrentTree(temp);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  songs = songsIn(waitForSnapshot(*service, [&added, &changed](const PlaylistTreeSnapshot& snapshot) {
    const auto currentSongs = songsIn(snapshot);
    return currentSongs.size() == 3U && songByPath(currentSongs, changed).title == "Changed After" &&
           songByPath(currentSongs, added).title == "Added";
  }));

  CHECK(reader->readCount() == 5U);
  REQUIRE(songs.size() == 3U);
  CHECK(songByPath(songs, unchanged).title == "Unchanged");
  CHECK(songByPath(songs, changed).title == "Changed After");
  CHECK(songByPath(songs, added).title == "Added");
  CHECK(std::ranges::none_of(songs, [&deleted](const SongMetadata& song) { return song.filePath == deleted; }));
  const auto fileScannedSongs = eventLog.fileScannedSongs();
  REQUIRE(fileScannedSongs.size() == 6U);
  CHECK(fileScannedSongs[3].filePath == unchanged);
  CHECK(fileScannedSongs[4].filePath == changed);
  CHECK(fileScannedSongs[5].filePath == added);
  cache::SQLiteCacheV3 sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locations = sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
  CHECK(locations.size() == 3U);
  CHECK(std::ranges::none_of(locations, [&deleted](const cache::CachedLocation& location) {
    return location.filePath == deleted;
  }));
}

TEST_CASE("scanner service returns from scan while scanner worker performs slow metadata reads") {
  test::TempScannerRoot temp{"scanner-service-async-scan"};
  const auto audio = test::writeAudioFixture(temp.path(), "slow.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Slow"));
  reader->blockUntilReleased();
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  const auto before = std::chrono::steady_clock::now();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto elapsed = std::chrono::steady_clock::now() - before;

  CHECK(elapsed < std::chrono::milliseconds{50});
  REQUIRE(reader->waitForBlockedRead(std::chrono::seconds{1}));
  CHECK(service->snapshot().nodes.empty());

  reader->release();
  for (auto attempts = 0; attempts < 100 && service->snapshot().nodes.empty(); ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "Slow");
  CHECK(eventLog.waitForEvent(ScannerEventType::ScanCompleted, std::chrono::seconds{1}));
}

}
}
