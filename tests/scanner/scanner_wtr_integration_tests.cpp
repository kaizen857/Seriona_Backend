#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

// 最小可解析 WAV（16-bit PCM 单声道 44.1kHz，0.2s 静音）：真实 TagReader/FFmpeg 可解析，
// 移植自 tools/watch_root_move_audit.cpp 的同名写入器（注意小端字节序）。
void writeMinimalWav(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  constexpr std::uint16_t kChannels = 1, kBits = 16;
  constexpr std::uint32_t kSampleRate = 44100, kSamples = kSampleRate / 5;
  constexpr std::uint32_t kDataBytes = kSamples * kChannels * (kBits / 8);
  std::ofstream out{path, std::ios::binary};
  out.write("RIFF", 4);
  const std::uint32_t riffSize = kDataBytes + 36;
  out.write(reinterpret_cast<const char*>(&riffSize), 4);
  out.write("WAVEfmt ", 8);
  const std::uint32_t fmtSize = 16;
  const std::uint16_t audioFmt = 1;
  out.write(reinterpret_cast<const char*>(&fmtSize), 4);
  out.write(reinterpret_cast<const char*>(&audioFmt), 2);
  out.write(reinterpret_cast<const char*>(&kChannels), 2);
  out.write(reinterpret_cast<const char*>(&kSampleRate), 4);
  const std::uint32_t byteRate = kSampleRate * kChannels * (kBits / 8);
  out.write(reinterpret_cast<const char*>(&byteRate), 4);
  const std::uint16_t blockAlign = kChannels * (kBits / 8);
  out.write(reinterpret_cast<const char*>(&blockAlign), 2);
  out.write(reinterpret_cast<const char*>(&kBits), 2);
  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&kDataBytes), 4);
  for (std::uint32_t i = 0; i != kSamples; ++i) {
    const std::uint16_t zero = 0;
    out.write(reinterpret_cast<const char*>(&zero), 2);
  }
}

// 真实 wtr 集成测试使用真实 watcher：deps.watcherFactory 留空，orchestrator 回填
// 生产 WtrFolderWatcherFactory（file_scanner_orchestrator.cpp:1016-1018）。
class RealWtrEventLog {
public:
  void push(ScannerEvent event) {
    std::scoped_lock lock{mutex_};
    events_.push_back(std::move(event));
  }

  [[nodiscard]] std::size_t scanStartedCount() const {
    std::scoped_lock lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count(events_, ScannerEventType::ScanStarted, &ScannerEvent::type));
  }

  [[nodiscard]] std::size_t scanCompletedCount() const {
    std::scoped_lock lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count(events_, ScannerEventType::ScanCompleted, &ScannerEvent::type));
  }

private:
  mutable std::mutex mutex_;
  std::vector<ScannerEvent> events_;
};

[[nodiscard]] std::shared_ptr<FileScannerService> makeRealWtrService(test::TempScannerRoot& temp, RealWtrEventLog& log) {
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = nullptr,
      .watcherFactory = nullptr,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{10}});
  service->setEventSink([&log](ScannerEvent event) { log.push(std::move(event)); });
  return service;
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

[[nodiscard]] std::vector<cache::CachedLocation> locationsForRoot(const test::TempScannerRoot& temp) {
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  return sidecar.loadLocationsByRoot(canonicalRootPath(temp.path()));
}

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return predicate();
}

void waitForSnapshotSongCount(const FileScannerService& service, std::size_t expected) {
  CHECK(waitUntil([&] { return songsIn(service.snapshot()).size() == expected; }, std::chrono::seconds{10}));
}

void waitForInitialScanCompleted(const RealWtrEventLog& log) {
  CHECK(waitUntil([&] { return log.scanCompletedCount() >= 1U; }, std::chrono::seconds{10}));
}

// 观测到真实 wtr 把 mv 出根的被监视目录报告为 file/other（path_type=file + trailing slash），
// 分类器只把 Directory+Other 当作 moveSelf 精准删除，file/other 走回落全根重扫
// （file_scanner_orchestrator.cpp:2359-2364）。故 mv 出根最多回落一次，仍收敛。
void waitForMovedOutConverged(const FileScannerService& service, const test::TempScannerRoot& temp,
                              const std::filesystem::path& movedAway) {
  CHECK(waitUntil(
      [&] {
        return songsIn(service.snapshot()).empty() && locationsForRoot(temp).empty() &&
               !std::filesystem::exists(movedAway);
      },
      std::chrono::seconds{10}));
}

TEST_CASE("real wtr integration directory moved out of root converges snapshot sqlite and disk") {
  test::TempScannerRoot temp{"scanner-wtr-move-out"};
  const auto root = temp.path();
  const auto music = root / "music";
  std::filesystem::create_directories(music);
  const auto movedOut =
      temp.path().parent_path() / ("seriona-wtr-moved-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(movedOut, ec);

  RealWtrEventLog log;
  auto service = makeRealWtrService(temp, log);

  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});

  writeMinimalWav(music / "01.wav");
  waitForSnapshotSongCount(*service, 1U);
  // 沉降：让 create 可能触发的回落（dir/create 事件）先完成，再取基线。
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  const auto baseline = log.scanStartedCount();

  std::filesystem::rename(music, movedOut, ec);
  REQUIRE_FALSE(ec);

  waitForMovedOutConverged(*service, temp, root / "music");

  const auto songs = songsIn(service->snapshot());
  CHECK(songs.empty());
  CHECK(std::ranges::none_of(songs, [&music](const SongMetadata& song) { return song.filePath == (music / "01.wav"); }));

  const auto locations = locationsForRoot(temp);
  CHECK(locations.empty());

  // mv 出根：真实 wtr 报告 file/other → 分类器回落全根重扫一次即收敛，不无限增长。
  CHECK(log.scanStartedCount() <= baseline + 1U);

  service->stopWatching();
  service->stop();
  service.reset();
  std::filesystem::remove_all(movedOut, ec);
}

TEST_CASE("real wtr integration root internal directory rename updates paths without residue") {
  test::TempScannerRoot temp{"scanner-wtr-rename-in-root"};
  const auto root = temp.path();
  const auto music = root / "music";
  const auto pop = root / "pop";
  std::filesystem::create_directories(music);

  RealWtrEventLog log;
  auto service = makeRealWtrService(temp, log);

  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});

  writeMinimalWav(music / "01.wav");
  waitForSnapshotSongCount(*service, 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  const auto baseline = log.scanStartedCount();

  std::error_code ec;
  std::filesystem::rename(music, pop, ec);
  REQUIRE_FALSE(ec);

  // 三方收敛：快照路径更新、SQLite 路径更新、磁盘新路径存在，且旧路径无残留。
  const auto converged = [&] {
    const auto songs = songsIn(service->snapshot());
    const auto locations = locationsForRoot(temp);
    if (songs.size() != 1U || locations.size() != 1U) {
      return false;
    }
    if (songs[0].filePath != (pop / "01.wav")) {
      return false;
    }
    if (locations[0].filePath != (pop / "01.wav")) {
      return false;
    }
    if (std::ranges::any_of(songs, [&music](const SongMetadata& song) { return song.filePath == (music / "01.wav"); })) {
      return false;
    }
    if (std::ranges::any_of(locations, [&music](const cache::CachedLocation& location) {
          return location.filePath == (music / "01.wav");
        })) {
      return false;
    }
    return std::filesystem::exists(pop / "01.wav");
  };
  CHECK(waitUntil(converged, std::chrono::seconds{10}));

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == (pop / "01.wav"));

  const auto locations = locationsForRoot(temp);
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == (pop / "01.wav"));
  CHECK(std::ranges::none_of(locations, [&music](const cache::CachedLocation& location) {
    return location.filePath == (music / "01.wav");
  }));

  // 根内 rename：真实 wtr 会产生 dir/rename + 后续 file/other，可能回落一次，仍收敛且不无限增长。
  CHECK(log.scanStartedCount() <= baseline + 1U);

  service->stopWatching();
  service->stop();
  service.reset();
}

TEST_CASE("real wtr integration file create modify delete converge without unbounded scans") {
  test::TempScannerRoot temp{"scanner-wtr-file-ops"};
  const auto root = temp.path();
  const auto wav = root / "song.wav";

  RealWtrEventLog log;
  auto service = makeRealWtrService(temp, log);

  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  std::this_thread::sleep_for(std::chrono::milliseconds{100});

  const auto baseline = log.scanStartedCount();

  writeMinimalWav(wav);
  waitForSnapshotSongCount(*service, 1U);
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  const auto afterCreate = log.scanStartedCount();

  std::this_thread::sleep_for(std::chrono::milliseconds{3}); // mtime granularity guard
  writeMinimalWav(wav);
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  CHECK(songsIn(service->snapshot()).size() == 1U);
  const auto afterModify = log.scanStartedCount();

  std::error_code ec;
  std::filesystem::remove(wav, ec);
  waitForSnapshotSongCount(*service, 0U);
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  const auto afterDelete = log.scanStartedCount();

  // 精准路径：modify/delete 由分类器精准处理（File+Modified/Destroyed），scan 计数不增长；
  // create 的 dir/create 事件可能触发一次回落，故允许 <=1。任何情况都不无限增长。
  CHECK(afterModify == afterCreate);
  CHECK(afterDelete == afterModify);
  CHECK(afterCreate - baseline <= 1U);

  const auto locations = locationsForRoot(temp);
  CHECK(locations.empty());

  service->stopWatching();
  service->stop();
  service.reset();
}

} // namespace
} // namespace seriona::scanner
