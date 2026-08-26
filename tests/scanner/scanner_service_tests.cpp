#include "scanner_test_harness.h"

#include "file_scanner_orchestrator_test_access.h"
#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"

#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

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

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    {
      std::lock_guard lock{mutex_};
      requestedPaths.push_back(request.path);
      requestedCoverDirs.push_back(request.coverExportDir);
    }
    changed_.notify_all();
    if (blockReads_ || request.path == blockPath_) {
      std::unique_lock lock{mutex_};
      blocked_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return released_; });
    }
    const auto failure = failures_.find(request.path);
    if (failure != failures_.end()) {
      throw std::runtime_error(failure->second);
    }
    auto iterator = metadataByPath_.find(request.path);
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

[[nodiscard]] std::filesystem::path scannerSidecarPath(const std::filesystem::path& databasePath) {
  return std::filesystem::path{databasePath.generic_string() + ".scan-roots.sqlite"};
}

[[nodiscard]] std::filesystem::path scannerSidecarPath(const test::TempScannerRoot& temp) {
  return scannerSidecarPath(temp.dbPath());
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

  [[nodiscard]] std::size_t eventCount(ScannerEventType type) const {
    std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count(events_, type, &ScannerEvent::type));
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

class SqliteReadHandle final {
public:
  explicit SqliteReadHandle(const std::filesystem::path& databasePath) {
    const auto path = databasePath.generic_string();
    REQUIRE(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  }

  ~SqliteReadHandle() {
    if (db_ != nullptr) {
      static_cast<void>(sqlite3_close(db_));
    }
  }

  SqliteReadHandle(const SqliteReadHandle&) = delete;
  SqliteReadHandle& operator=(const SqliteReadHandle&) = delete;

  [[nodiscard]] sqlite3* get() const noexcept { return db_; }

private:
  sqlite3* db_{};
};

class SqliteWriteHandle final {
public:
  explicit SqliteWriteHandle(const std::filesystem::path& databasePath) {
    const auto path = databasePath.generic_string();
    REQUIRE(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  }

  ~SqliteWriteHandle() {
    if (db_ != nullptr) {
      static_cast<void>(sqlite3_close(db_));
    }
  }

  SqliteWriteHandle(const SqliteWriteHandle&) = delete;
  SqliteWriteHandle& operator=(const SqliteWriteHandle&) = delete;

  [[nodiscard]] sqlite3* get() const noexcept { return db_; }

private:
  sqlite3* db_{};
};

void pointLocationToMissingContent(const std::filesystem::path& databasePath, const std::string& locationId) {
  SqliteWriteHandle handle{databasePath};
  REQUIRE(sqlite3_exec(handle.get(), "PRAGMA foreign_keys=OFF;", nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(handle.get(),
                             "UPDATE locations SET content_id=?1 WHERE location_id=?2;",
                             -1,
                             &statement,
                             nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(statement, 1, "missing-content", -1, SQLITE_TRANSIENT) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(statement, 2, locationId.c_str(), static_cast<int>(locationId.size()), SQLITE_TRANSIENT) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
  REQUIRE(sqlite3_changes(handle.get()) == 1);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_exec(handle.get(), "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr) == SQLITE_OK);
}

[[nodiscard]] std::vector<std::string> sqliteTableNames(sqlite3* db) {
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db,
                             "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;",
                             -1,
                             &statement,
                             nullptr) == SQLITE_OK);
  std::vector<std::string> names;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* name = sqlite3_column_text(statement, 0);
    names.emplace_back(reinterpret_cast<const char*>(name));
  }
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return names;
}

[[nodiscard]] int sqliteUserVersion(sqlite3* db) {
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const auto version = sqlite3_column_int(statement, 0);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return version;
}

[[nodiscard]] int tableRowCount(sqlite3* db, const std::string& tableName) {
  sqlite3_stmt* statement = nullptr;
  const std::string sql = "SELECT COUNT(*) FROM " + tableName + ";";
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const auto count = sqlite3_column_int(statement, 0);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return count;
}

[[nodiscard]] std::vector<std::string> scannerSchemaTables() {
  return {"content", "locations", "lyrics", "scan_errors", "scan_roots"};
}

class ScopedEnvVar {
public:
  ScopedEnvVar(std::string name, std::string value) : name_{std::move(name)} {
    if (const auto* existing = std::getenv(name_.c_str())) {
      previous_ = existing;
    }
#if defined(_WIN32)
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }

  ~ScopedEnvVar() {
    if (previous_.has_value()) {
#if defined(_WIN32)
      _putenv_s(name_.c_str(), previous_->c_str());
#else
      setenv(name_.c_str(), previous_->c_str(), 1);
#endif
      return;
    }
#if defined(_WIN32)
    SetEnvironmentVariableA(name_.c_str(), nullptr);
#else
    unsetenv(name_.c_str());
#endif
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

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

class IncrementalPlanObserverGuard {
public:
  explicit IncrementalPlanObserverGuard(IncrementalPlanObserver observer) { setIncrementalPlanObserver(std::move(observer)); }
  ~IncrementalPlanObserverGuard() { clearIncrementalPlanObserver(); }

  IncrementalPlanObserverGuard(const IncrementalPlanObserverGuard&) = delete;
  IncrementalPlanObserverGuard& operator=(const IncrementalPlanObserverGuard&) = delete;
};

class CacheWriteObserverGuard {
public:
  explicit CacheWriteObserverGuard(CacheWriteObserver observer) { setCacheWriteObserver(std::move(observer)); }
  ~CacheWriteObserverGuard() { clearCacheWriteObserver(); }

  CacheWriteObserverGuard(const CacheWriteObserverGuard&) = delete;
  CacheWriteObserverGuard& operator=(const CacheWriteObserverGuard&) = delete;
};

class PreallocationObserverGuard {
public:
  explicit PreallocationObserverGuard(PreallocationObserver observer) {
    setPreallocationObserver(std::move(observer));
  }

  ~PreallocationObserverGuard() {
    if (active_) {
      clearPreallocationObserver();
    }
  }

  void reset() {
    clearPreallocationObserver();
    active_ = false;
  }

  PreallocationObserverGuard(const PreallocationObserverGuard&) = delete;
  PreallocationObserverGuard& operator=(const PreallocationObserverGuard&) = delete;

private:
  bool active_{true};
};

[[nodiscard]] cache::CachedLocation cachedLocationForPath(cache::SQLiteCache& cache,
                                                          const std::filesystem::path& rootPath,
                                                          const std::filesystem::path& filePath) {
  const auto locations = cache.loadLocationsByRoot(rootPath);
  const auto iterator = std::ranges::find(locations, filePath, &cache::CachedLocation::filePath);
  if (iterator == locations.end()) {
    throw std::runtime_error("missing cached location for scanner service test");
  }
  return *iterator;
}

TEST_CASE("scanner service scans hashes caches lyrics and skips unchanged rereads") {
  test::TempScannerRoot temp{"scanner-service-cache-hit"};
  const auto first = test::writeAudioFixture(temp.path(), "song.flac");
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external one\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First", {RawTagLyricLine{std::chrono::milliseconds{500}, "embedded one"}}));
  reader->put(second, rawMetadata("Second", {RawTagLyricLine{std::chrono::milliseconds{700}, "embedded two"}}));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });
  auto parseCallCount = std::size_t{0};
  const LrcParseObserverGuard parseObserver{[&parseCallCount](const std::filesystem::path&) { ++parseCallCount; }};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  const auto firstSnapshot = service->snapshot();
  const auto firstSongs = songsIn(firstSnapshot);

  REQUIRE(firstSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(firstSongs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(firstSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(firstSongs[1].effectiveLyrics.size() == 1U);
  CHECK(firstSongs[1].effectiveLyrics[0].text == "external one");
  CHECK(parseCallCount == 1U);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  const auto firstLocation = cachedLocationForPath(sidecar, rootPath, first);
  REQUIRE(firstLocation.externalLrcHash.has_value());
  CHECK(sidecar.loadLyrics(firstLocation.locationId, "external").size() == 1U);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));
  const auto cachedSongs = songsIn(service->snapshot());

  REQUIRE(cachedSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(cachedSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(cachedSongs[1].effectiveLyrics[0].text == "external one");
  CHECK(parseCallCount == 1U);
  const auto cachedLocation = cachedLocationForPath(sidecar, rootPath, first);
  CHECK(cachedLocation.externalLrcHash == firstLocation.externalLrcHash);
}

TEST_CASE("scanner service runScan characterizes main database scanner schema side effect") {
  test::TempScannerRoot temp{"scanner-service-main-db-schema"};
  const auto databasePath = temp.dbPath("scanner-main.sqlite");
  const auto sidecarPath = scannerSidecarPath(databasePath);
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Song"));
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = reader,
                                                                       .watcherFactory = nullptr,
                                                                       .databasePath = databasePath,
                                                                       .coverExportDir = temp.path() / "covers"});
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  CHECK_FALSE(std::filesystem::exists(databasePath));
  CHECK_FALSE(std::filesystem::exists(sidecarPath));

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(reader->readCount() == 1U);
  REQUIRE(std::filesystem::exists(databasePath));
  REQUIRE(std::filesystem::exists(sidecarPath));
  service.reset();

  SqliteReadHandle mainDatabase{databasePath};
  CHECK(sqliteUserVersion(mainDatabase.get()) == 3);
  CHECK(sqliteTableNames(mainDatabase.get()) == scannerSchemaTables());
  CHECK(tableRowCount(mainDatabase.get(), "locations") == 0);

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = sidecarPath}};
  const auto rootPath = canonicalRootPath(temp.path());
  CHECK(sidecar.loadScanRoot(rootPath).has_value());
  CHECK(sidecar.loadLocationsByRoot(rootPath).size() == 1U);
}

TEST_CASE("scanner service clears stale external lrc after a cached song sees malformed sidecar") {
  test::TempScannerRoot temp{"scanner-service-stale-lrc-cleanup"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external valid\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{300}, "embedded fallback"}}));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external valid");
  CHECK(reader->readCount() == 1U);

  writeText(temp.path() / "song.lrc", "[00:99.00]malformed stale external\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));
  songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "embedded fallback");
  CHECK(std::ranges::none_of(songs[0].effectiveLyrics, [](const LyricLine& line) {
    return line.text == "external valid" || line.text == "malformed stale external";
  }));
  CHECK(std::ranges::any_of(eventLog.errors(), [](const ScannerError& error) {
    return error.message == "failed to parse external lyrics";
  }));

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto location = cachedLocationForPath(sidecar, canonicalRootPath(temp.path()), audio);
  CHECK_FALSE(location.externalLrcHash.has_value());
  CHECK_FALSE(location.externalLrcPath.has_value());
  CHECK(sidecar.loadLyrics(location.locationId, "external").empty());
}

TEST_CASE("scanner service clears stale external lrc after hash io failure") {
  test::TempScannerRoot temp{"scanner-service-lrc-hash-failure"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external valid\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{300}, "embedded fallback"}}));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);

  const LyricsHashProviderGuard hashFailure{[](const std::filesystem::path& path, const HashOptions&) {
    return FileHashResult{.hash = std::nullopt,
                          .errors = {HashError{.code = HashErrorCode::IoFailure,
                                                .scannerError = ScannerError{.code = ScannerErrorCode::RootUnavailable,
                                                                             .message = "test lrc hash io failure",
                                                                             .detail = {},
                                                                             .path = path}}}};
  }};
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));
  songs = songsIn(service->snapshot());

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(songs[0].effectiveLyrics[0].text == "embedded fallback");
  CHECK(std::ranges::any_of(eventLog.errors(), [](const ScannerError& error) {
    return error.message == "test lrc hash io failure";
  }));
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto location = cachedLocationForPath(sidecar, canonicalRootPath(temp.path()), audio);
  CHECK_FALSE(location.externalLrcHash.has_value());
  CHECK_FALSE(location.externalLrcPath.has_value());
  CHECK(sidecar.loadLyrics(location.locationId, "external").empty());
}

TEST_CASE("scanner service delta recording deletes stale empty embedded and external lyrics rows") {
  test::TempScannerRoot temp{"scanner-service-empty-lyrics-delta"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external before\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Before", {RawTagLyricLine{std::chrono::milliseconds{300}, "embedded before"}}));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  const auto locationBefore = cachedLocationForPath(sidecar, rootPath, audio);
  REQUIRE(sidecar.loadLyrics(locationBefore.locationId, "embedded").size() == 1U);
  REQUIRE(sidecar.loadLyrics(locationBefore.locationId, "external").size() == 1U);

  std::filesystem::remove(temp.path() / "song.lrc");
  reader->put(audio, rawMetadata("After"));
  std::vector<cache::ScanRootCacheWrite> cacheWrites;
  const CacheWriteObserverGuard cacheWriteObserver{[&cacheWrites](const cache::ScanRootCacheWrite& write) {
    cacheWrites.push_back(write);
  }};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));

  REQUIRE(cacheWrites.size() == 1U);
  REQUIRE(cacheWrites.back().changedSongs.size() == 1U);
  CHECK(cacheWrites.back().changedSongs[0].song.embeddedLyrics.empty());
  CHECK(cacheWrites.back().changedSongs[0].song.externalLyrics.empty());
  const auto locationAfter = cachedLocationForPath(sidecar, rootPath, audio);
  CHECK(locationAfter.locationId == locationBefore.locationId);
  CHECK(sidecar.loadLyrics(locationAfter.locationId, "embedded").empty());
  CHECK(sidecar.loadLyrics(locationAfter.locationId, "external").empty());
  CHECK_FALSE(locationAfter.externalLrcHash.has_value());
  CHECK_FALSE(locationAfter.externalLrcPath.has_value());
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
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  auto songs = songsIn(waitForSnapshot(*service, [](const PlaylistTreeSnapshot& snapshot) {
    const auto currentSongs = songsIn(snapshot);
    return currentSongs.size() == 1U && !currentSongs[0].effectiveLyrics.empty() && currentSongs[0].effectiveLyrics[0].text == "external two";
  }));

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external two");
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto locationAfterLrcOnly = cachedLocationForPath(sidecar, canonicalRootPath(temp.path()), audio);
  const auto lyricsAfterLrcOnly = sidecar.loadLyrics(locationAfterLrcOnly.locationId, "external");
  REQUIRE(lyricsAfterLrcOnly.size() == 1U);
  CHECK(lyricsAfterLrcOnly[0].text == "external two");
  REQUIRE(locationAfterLrcOnly.externalLrcHash.has_value());

  writeText(audio, "changed audio bytes");
  std::this_thread::sleep_for(std::chrono::milliseconds{5}); // mtime granularity guard
  reader->put(audio, rawMetadata("After", {RawTagLyricLine{std::chrono::milliseconds{400}, "embedded after"}}));
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
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
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  waitForReaderCount(*reader, 3U);
  CHECK(reader->readCount() == 3U);

  writeText(temp.path() / "embedded.lrc", "[00:01.00]external embedded\n");
  writeText(temp.path() / "plain.lrc", "[00:01.00]external plain\n");
  forceNextScanIncrementalForCurrentTree(temp);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));
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
  forceNextScanIncrementalForCurrentTree(temp);
  std::vector<cache::ScanRootCacheWrite> deleteWrites;
  const CacheWriteObserverGuard cacheWriteObserver{[&deleteWrites](const cache::ScanRootCacheWrite& write) {
    deleteWrites.push_back(write);
  }};
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 3U, std::chrono::seconds{1}));
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

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  REQUIRE(deleteWrites.size() == 1U);
  CHECK(deleteWrites.back().retainedLocationIds.size() == 2U);
  const auto locationsAfterDelete = sidecar.loadLocationsByRoot(rootPath);
  CHECK(std::ranges::none_of(locationsAfterDelete, [&deletedAudio](const cache::CachedLocation& location) {
    return location.filePath == deletedAudio;
  }));
  const auto embeddedLocation = cachedLocationForPath(sidecar, rootPath, embeddedAudio);
  CHECK_FALSE(embeddedLocation.externalLrcHash.has_value());
  CHECK(sidecar.loadLyrics(embeddedLocation.locationId, "external").empty());
  const auto plainLocation = cachedLocationForPath(sidecar, rootPath, plainAudio);
  CHECK_FALSE(plainLocation.externalLrcHash.has_value());
  CHECK(sidecar.loadLyrics(plainLocation.locationId, "external").empty());
}

TEST_CASE("scanner service treats lrc hash cancellation as typed cancellation and preserves cache") {
  test::TempScannerRoot temp{"scanner-service-lrc-cancel"};
  const auto kept = test::writeAudioFixture(temp.path(), "kept.flac");
  const auto deleted = test::writeAudioFixture(temp.path(), "deleted.flac");
  writeText(temp.path() / "kept.lrc", "[00:01.00]kept external\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(kept, rawMetadata("Kept", {RawTagLyricLine{std::chrono::milliseconds{300}, "kept embedded"}}));
  reader->put(deleted, rawMetadata("Deleted"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  REQUIRE(sidecar.loadLocationsByRoot(rootPath).size() == 2U);
  const auto originalKeptLocation = cachedLocationForPath(sidecar, rootPath, kept);
  REQUIRE(originalKeptLocation.externalLrcHash.has_value());
  REQUIRE(sidecar.loadLyrics(originalKeptLocation.locationId, "external").size() == 1U);

	  std::filesystem::remove(deleted);
	  std::vector<cache::ScanRootCacheWrite> cancelledWrites;
	  const CacheWriteObserverGuard cacheWriteObserver{[&cancelledWrites](const cache::ScanRootCacheWrite& write) {
	    cancelledWrites.push_back(write);
	  }};
	  const LyricsHashProviderGuard hashCancellation{[](const std::filesystem::path& path, const HashOptions&) {
	    return FileHashResult{.hash = std::nullopt,
	                          .errors = {HashError{.code = HashErrorCode::Cancelled,
                                                .scannerError = ScannerError{.code = ScannerErrorCode::Cancelled,
                                                                             .message = "typed test cancellation without magic text",
                                                                             .detail = {},
                                                                             .path = path}}}};
  }};
	  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
	  REQUIRE(eventLog.waitForEvent(ScannerEventType::ScanStopped, std::chrono::seconds{1}));

	  CHECK(cancelledWrites.empty());
	  const auto locationsAfterCancel = sidecar.loadLocationsByRoot(rootPath);
  CHECK(locationsAfterCancel.size() == 2U);
  CHECK(std::ranges::any_of(locationsAfterCancel, [&deleted](const cache::CachedLocation& location) {
    return location.filePath == deleted;
  }));
  const auto keptLocationAfterCancel = cachedLocationForPath(sidecar, rootPath, kept);
  CHECK(keptLocationAfterCancel.externalLrcHash == originalKeptLocation.externalLrcHash);
  const auto keptExternalLyrics = sidecar.loadLyrics(keptLocationAfterCancel.locationId, "external");
  REQUIRE(keptExternalLyrics.size() == 1U);
  CHECK(keptExternalLyrics[0].text == "kept external");
}

TEST_CASE("scanner service does not retain a cache hit whose content hydrate fails") {
  test::TempScannerRoot temp{"scanner-service-bad-cache-hit-retained"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Good Cache"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  const auto originalLocation = cachedLocationForPath(sidecar, rootPath, audio);
  pointLocationToMissingContent(scannerSidecarPath(temp), originalLocation.locationId);
  reader->fail(audio, "forced metadata failure after bad cache hydrate");

  std::vector<IncrementalPlanSnapshot> snapshots;
  const IncrementalPlanObserverGuard observer{[&snapshots](const IncrementalPlanSnapshot& snapshot) {
    snapshots.push_back(snapshot);
  }};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 2U, std::chrono::seconds{1}));

  REQUIRE_FALSE(snapshots.empty());
  CHECK(snapshots.back().retainedLocationIds.empty());
  CHECK(sidecar.loadLocationsByRoot(rootPath).empty());
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

TEST_CASE("scanner service contains queued scan exceptions and keeps the worker alive") {
  test::TempScannerRoot temp{"scanner-service-run-scan-exception"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Recovered"));
  auto service = makeService(temp, reader);
  ScannerEventLog eventLog;
  service->setEventSink([&eventLog](ScannerEvent event) { eventLog.push(std::move(event)); });

  std::atomic<std::size_t> injectionCount{0U};
  PreallocationObserverGuard preallocationObserver{[&injectionCount](const std::vector<IndexedPublishedSong>&) {
    if (injectionCount.fetch_add(1U) == 0U) {
      throw std::filesystem::filesystem_error{"forced preallocation failure",
                                              std::make_error_code(std::errc::io_error)};
    }
  }};

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEvent(ScannerEventType::ScanError, std::chrono::seconds{1}));

  const auto errors = eventLog.errors();
  REQUIRE(errors.size() == 1U);
  CHECK(errors.front().code == ScannerErrorCode::CacheUnavailable);
  CHECK(errors.front().detail.find("forced preallocation failure") != std::string::npos);
  CHECK(injectionCount.load() == 1U);
  CHECK(eventLog.eventCount(ScannerEventType::ScanCompleted) == 0U);
  CHECK(eventLog.eventCount(ScannerEventType::ScanStopped) == 0U);

  preallocationObserver.reset();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(eventLog.waitForEventCount(ScannerEventType::ScanCompleted, 1U, std::chrono::seconds{1}));

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs.front().filePath == audio);
  CHECK(songs.front().title == "Recovered");
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
  const auto databasePath = temp.dbPath("scanner-cache.sqlite");
  const auto sidecarPath = std::filesystem::path{databasePath.generic_string() + ".scan-roots.sqlite"};
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
    cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = sidecarPath}};
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
  std::this_thread::sleep_for(std::chrono::milliseconds{5}); // mtime granularity guard
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
	  REQUIRE(fileScannedSongs.size() == 5U);
	  CHECK(fileScannedSongs[3].filePath == changed);
	  CHECK(fileScannedSongs[4].filePath == added);
	  CHECK(std::ranges::none_of(fileScannedSongs.begin() + 3, fileScannedSongs.end(), [&unchanged](const SongMetadata& song) {
	    return song.filePath == unchanged;
	  }));
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
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
