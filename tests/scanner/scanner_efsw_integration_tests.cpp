// efsw 集成测试（任务 9：重写旧监视器集成套件为真实 efsw 监视器场景）。
//
// 与 watcher 单元测试（scanner_watcher_tests.cpp，fake 注入事件）不同，本文件全部使用真实
// efsw 监视器与真实 TagReader/FFmpeg 解析：
//   - deps.watcherFactory 留空 → orchestrator 回填生产 EfswFolderWatcherFactory
//     （file_scanner_orchestrator.cpp:1204-1206）；
//   - deps.metadataReader 留空 → 回填 ProductionTagMetadataReader（file_scanner_orchestrator.cpp:1201-1203）；
//   因此夹具必须写真实可解析音频（最小 WAV）。
//
// 覆盖场景（计划任务 9 清单）：
//   1. 移入文件 → 精准增量（无整根重扫、无 ScanError、快照与缓存一致）。
//   2. 移入目录（含嵌套子树/既有文件）→ scoped 子树枚举 + 合并。
//   3. 移入目录内后续写入被实时捕获（watch 覆盖新子树）。
//   4. 移出（文件与目录均精准删除，回归保持）。
//   5. 根内移动（文件与目录，回归保持）。
//   6. 尖峰风暴（快速批量创建 + 目录移入 + 文件批量移入）。
//   7. 停止/析构竞态（含"事件流/并发中析构"，任务 4 审查门移交缺口）。
//   8. 大目录移入（≥100 子文件规模）。
//
// 语义依据（.omo/evidence/gate0/matrix.md，efsw 1.7.2 Linux inotify 实测）：
//   - 根外移入恒为 Add + Modified 两条（文件/目录均是），目录移入不逐事件上报既有子文件；
//   - 目录移入的递归 watch 在 Add 回调前注册，之后写入可被捕获；
//   - 同根 rename 为单条 Moved（oldFilename 相对旧名）；跨目录移动为单条 Moved（oldFilename 绝对）；
//   - 移出后 watch 被清理，向移出目录内写入 0 幽灵事件。
//
// 发布契约（file_scanner_orchestrator.cpp publishSnapshotEvents 顶部）：
//   - 全量/增量扫描 = ScanStarted + ProgressUpdated + 快照 + ScanCompleted；
//   - 文件级精准批次 = 快照 + ScanCompleted（不发 ScanStarted）；
//   - scoped 子树对账 = 仅快照（不发 ScanStarted/ScanCompleted）。
// 因此"无整根重扫"以 ScanStarted 计数不增长为准，"scoped 契约"以两者均不增长为准。

#include "scanner_test_harness.h"

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
#include <functional>
#include <memory>
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

// 事件日志：真实 efsw 回调为异步多线程投递，采集与计数必须加锁。
class RealEfswEventLog {
public:
  void push(ScannerEvent event) {
    std::scoped_lock lock{mutex_};
    events_.push_back(std::move(event));
  }

  [[nodiscard]] std::size_t count(ScannerEventType type) const {
    std::scoped_lock lock{mutex_};
    return static_cast<std::size_t>(std::ranges::count(events_, type, &ScannerEvent::type));
  }

  [[nodiscard]] std::size_t scanStartedCount() const { return count(ScannerEventType::ScanStarted); }
  [[nodiscard]] std::size_t scanCompletedCount() const { return count(ScannerEventType::ScanCompleted); }
  [[nodiscard]] std::size_t scanErrorCount() const { return count(ScannerEventType::ScanError); }
  [[nodiscard]] std::size_t snapshotUpdatedCount() const { return count(ScannerEventType::PlaylistSnapshotUpdated); }

private:
  mutable std::mutex mutex_;
  std::vector<ScannerEvent> events_;
};

// 真实 efsw 监视器 + 真实 TagReader：watcherFactory / metadataReader 均留空由 orchestrator 回填
// 生产实现。debounce 默认 10ms 保持用例快速；批量/风暴场景显式放大以减少拆批竞态。
[[nodiscard]] std::shared_ptr<FileScannerService> makeRealEfswService(
    test::TempScannerRoot& temp, RealEfswEventLog& log,
    std::chrono::milliseconds debounce = std::chrono::milliseconds{10}) {
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = nullptr,
      .watcherFactory = nullptr,
      .databasePath = temp.dbPath(),
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = debounce});
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

[[nodiscard]] std::vector<std::filesystem::path> sortedPaths(std::vector<std::filesystem::path> paths) {
  std::ranges::sort(paths);
  return paths;
}

[[nodiscard]] std::vector<std::filesystem::path> snapshotSongPaths(const FileScannerService& service) {
  std::vector<std::filesystem::path> paths;
  for (const auto& node : service.snapshot().nodes) {
    if (node.song.has_value()) {
      paths.push_back(node.song->filePath);
    }
  }
  return sortedPaths(std::move(paths));
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

[[nodiscard]] std::string scanRootHashInDatabase(const test::TempScannerRoot& temp) {
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto scanRoot = sidecar.loadScanRoot(canonicalRootPath(temp.path()));
  return scanRoot.has_value() ? scanRoot->directoryTreeHash : std::string{};
}

void checkSnapshotCachePathsMatch(const FileScannerService& service, const test::TempScannerRoot& temp) {
  const auto songPaths = snapshotSongPaths(service);
  std::vector<std::filesystem::path> locationPaths;
  for (const auto& location : locationsForRoot(temp)) {
    locationPaths.push_back(location.filePath);
  }
  CHECK(songPaths.size() == locationPaths.size());
  CHECK(songPaths == sortedPaths(std::move(locationPaths)));
}

void checkScanRootHashMatchesDisk(const test::TempScannerRoot& temp) {
  const auto currentHash = computeDirectoryTreeHash(canonicalRootPath(temp.path()));
  REQUIRE(currentHash.hash.has_value());
  CHECK(scanRootHashInDatabase(temp) == *currentHash.hash);
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

void waitForSnapshotSongCount(const FileScannerService& service, std::size_t expected,
                              std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
  CHECK(waitUntil([&] { return songsIn(service.snapshot()).size() == expected; }, timeout));
}

[[nodiscard]] bool waitForSnapshotPaths(const FileScannerService& service,
                                        std::vector<std::filesystem::path> expected,
                                        std::chrono::milliseconds timeout) {
  const auto wanted = sortedPaths(std::move(expected));
  return waitUntil([&] { return snapshotSongPaths(service) == wanted; }, timeout);
}

void waitForInitialScanCompleted(const RealEfswEventLog& log) {
  CHECK(waitUntil([&] { return log.scanCompletedCount() >= 1U; }, std::chrono::seconds{10}));
}

// 等待 scan 计数静默（quiet 时长内无新 ScanStarted）：若事件被兜底为整根重扫，其 ScanStarted
// 与全量读取会在窗口内出现，保证后续"无整根重扫"负向断言不因断言跑在重扫启动之前而假通过。
bool waitForScanQuiescence(const RealEfswEventLog& log, std::chrono::milliseconds quiet = std::chrono::milliseconds{500}) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  auto lastCount = log.scanStartedCount();
  auto lastChanged = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    const auto count = log.scanStartedCount();
    if (count != lastCount) {
      lastCount = count;
      lastChanged = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - lastChanged >= quiet) {
      return true;
    }
  }
  return false;
}

void stopAndDestroy(std::shared_ptr<FileScannerService> service) {
  service->stopWatching();
  service->stop();
  service.reset();
}

// 场景 1：根外文件移入。efsw 报 Add + Modified（同批）→ 文件级精准 upsert：
// 无整根重扫（ScanStarted 不增长）、无 ScanError、快照与 SQLite 缓存一致，扫描根哈希刷新。
TEST_CASE("efsw integration file moved into root updates precisely without rescan or scan error") {
  test::TempScannerRoot temp{"scanner-efsw-file-move-in"};
  const auto root = temp.path();
  const auto staging = temp.path().parent_path() / ("seriona-efsw-file-move-in-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  writeMinimalWav(staging / "01.wav");

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();
  const auto completedBefore = log.scanCompletedCount();
  const auto snapshotBefore = log.snapshotUpdatedCount();
  REQUIRE(log.scanErrorCount() == 0U);

  std::filesystem::rename(staging / "01.wav", root / "01.wav", ec);
  REQUIRE_FALSE(ec);

  CHECK(waitForSnapshotPaths(*service, {root / "01.wav"}, std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));

  // 精准路径：无整根重扫、无 ScanError；文件级精准批次按发布契约发快照 + ScanCompleted。
  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanErrorCount() == 0U);
  CHECK(log.snapshotUpdatedCount() >= snapshotBefore + 1U);
  CHECK(log.scanCompletedCount() >= completedBefore + 1U);

  const auto songs = songsIn(service->snapshot());
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == (root / "01.wav"));
  checkSnapshotCachePathsMatch(*service, temp);
  const auto locations = locationsForRoot(temp);
  REQUIRE(locations.size() == 1U);
  CHECK(locations[0].filePath == (root / "01.wav"));
  checkScanRootHashMatchesDisk(temp);

  stopAndDestroy(service);
  std::filesystem::remove_all(staging, ec);
}

// 场景 2：根外目录整体移入（嵌套子树 + 既有文件 + 非音频杂物）。efsw 只报目录自身
// Add + Modified、不逐事件上报既有子文件 → scoped 子树枚举兜底，非音频被过滤；
// 按 scoped 发布契约只发快照（ScanStarted / ScanCompleted 均不增长），无 ScanError。
TEST_CASE("efsw integration directory moved into root enumerates subtree without rescan") {
  test::TempScannerRoot temp{"scanner-efsw-dir-move-in"};
  const auto root = temp.path();
  const auto staging = temp.path().parent_path() / ("seriona-efsw-dir-move-in-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  writeMinimalWav(staging / "album" / "a.wav");
  writeMinimalWav(staging / "album" / "nested" / "b.wav");
  writeMinimalWav(staging / "album" / "nested" / "deep" / "c.wav");
  {
    std::ofstream notes{staging / "album" / "readme.txt"};
    notes << "not audio";
  }

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();
  const auto completedBefore = log.scanCompletedCount();
  const auto snapshotBefore = log.snapshotUpdatedCount();

  std::filesystem::rename(staging / "album", root / "album", ec);
  REQUIRE_FALSE(ec);

  const std::vector<std::filesystem::path> expected{
      root / "album" / "a.wav", root / "album" / "nested" / "b.wav", root / "album" / "nested" / "deep" / "c.wav"};
  CHECK(waitForSnapshotPaths(*service, expected, std::chrono::seconds{15}));
  CHECK(waitForScanQuiescence(log));

  // scoped 子树对账接管：无整根重扫、不发 ScanCompleted、无 ScanError。
  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanCompletedCount() == completedBefore);
  CHECK(log.snapshotUpdatedCount() >= snapshotBefore + 1U);
  CHECK(log.scanErrorCount() == 0U);
  checkSnapshotCachePathsMatch(*service, temp);
  CHECK(locationsForRoot(temp).size() == expected.size());
  checkScanRootHashMatchesDisk(temp);

  stopAndDestroy(service);
  std::filesystem::remove_all(staging, ec);
}

// 场景 3：移入目录内后续写入被实时捕获。目录移入后 efsw 已在 Add 回调前注册递归 watch
// （Gate 0 S1），向新子树（含嵌套层）写入的文件走文件级精准 upsert，不再触发扫描。
TEST_CASE("efsw integration write into moved-in subtree is captured live") {
  test::TempScannerRoot temp{"scanner-efsw-subtree-write"};
  const auto root = temp.path();
  const auto staging = temp.path().parent_path() / ("seriona-efsw-subtree-write-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  writeMinimalWav(staging / "incoming" / "a.wav");
  writeMinimalWav(staging / "incoming" / "nested" / "pre.wav");

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));

  std::filesystem::rename(staging / "incoming", root / "incoming", ec);
  REQUIRE_FALSE(ec);
  CHECK(waitForSnapshotPaths(*service, {root / "incoming" / "a.wav", root / "incoming" / "nested" / "pre.wav"},
                             std::chrono::seconds{15}));
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();

  // 向移入子树的嵌套层写入新文件：递归 watch 必须覆盖（Gate 0：注册先于 Add 回调）。
  writeMinimalWav(root / "incoming" / "nested" / "late.wav");
  CHECK(waitForSnapshotPaths(*service,
                             {root / "incoming" / "a.wav", root / "incoming" / "nested" / "pre.wav",
                              root / "incoming" / "nested" / "late.wav"},
                             std::chrono::seconds{15}));
  CHECK(waitForScanQuiescence(log));

  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanErrorCount() == 0U);
  checkSnapshotCachePathsMatch(*service, temp);
  const auto locations = locationsForRoot(temp);
  REQUIRE(locations.size() == 3U);
  CHECK(std::ranges::any_of(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == (root / "incoming" / "nested" / "late.wav");
  }));

  stopAndDestroy(service);
  std::filesystem::remove_all(staging, ec);
}

// 场景 4a：单文件移出根。efsw 报单条 Delete；适配层按磁盘状态判为 File + 音频扩展名门禁
// 通过 → 精准 destroy，无整根重扫、无 ScanError，快照与缓存同步清空。
TEST_CASE("efsw integration single file moved out converges precisely") {
  test::TempScannerRoot temp{"scanner-efsw-single-move-out"};
  const auto root = temp.path();
  const auto staging = temp.path().parent_path() / ("seriona-efsw-single-move-out-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  std::filesystem::create_directories(staging);
  writeMinimalWav(root / "music" / "01.wav");

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();

  std::filesystem::rename(root / "music" / "01.wav", staging / "01.wav", ec);
  REQUIRE_FALSE(ec);

  CHECK(waitUntil(
      [&] {
        return snapshotSongPaths(*service).empty() && locationsForRoot(temp).empty() &&
               std::filesystem::exists(staging / "01.wav");
      },
      std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));

  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanErrorCount() == 0U);
  CHECK(songsIn(service->snapshot()).empty());
  checkSnapshotCachePathsMatch(*service, temp);

  stopAndDestroy(service);
  std::filesystem::remove_all(staging, ec);
}

// 场景 4b：目录移出根（回归保持）。efsw 产生最深优先的级联 Delete，路径已不存在无 stat
// 依据 → 适配层判为 File；分类器以树中已知目录前缀（preciseDirRemovalPrefixes）识别为
// 精准子树删除（removeSubtree + deleteLocationsByPathPrefix），不再回落全根重扫；
// 快照/缓存/磁盘三方一致。
TEST_CASE("efsw integration directory moved out of root converges snapshot sqlite and disk") {
  test::TempScannerRoot temp{"scanner-efsw-dir-move-out"};
  const auto root = temp.path();
  const auto music = root / "music";
  std::filesystem::create_directories(music);
  writeMinimalWav(music / "01.wav");
  const auto movedOut = temp.path().parent_path() / ("seriona-efsw-moved-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(movedOut, ec);

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  waitForSnapshotSongCount(*service, 1U);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto baseline = log.scanStartedCount();

  std::filesystem::rename(music, movedOut, ec);
  REQUIRE_FALSE(ec);

  CHECK(waitUntil(
      [&] {
        return snapshotSongPaths(*service).empty() && locationsForRoot(temp).empty() &&
               !std::filesystem::exists(music);
      },
      std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));

  const auto songs = songsIn(service->snapshot());
  CHECK(songs.empty());
  CHECK(std::ranges::none_of(songs, [&music](const SongMetadata& song) { return song.filePath == (music / "01.wav"); }));
  checkSnapshotCachePathsMatch(*service, temp);
  // 目录 mv 出根已走精准删除（File kind + 树中已知目录前缀），无回落重扫。
  CHECK(log.scanStartedCount() == baseline);
  CHECK(log.scanErrorCount() == 0U);

  stopAndDestroy(service);
  std::filesystem::remove_all(movedOut, ec);
}

// 场景 5：根内移动（回归保持）。efsw 报单条 Moved（文件/目录均是；目录 watch 路径自动迁移），
// 分类器走 renameSubtree 精准改写树与缓存：旧路径无残留、新路径三方（快照/缓存/磁盘）一致、
// 无整根重扫。
TEST_CASE("efsw integration root internal rename keeps paths precise") {
  test::TempScannerRoot temp{"scanner-efsw-rename-in-root"};
  const auto root = temp.path();
  const auto music = root / "music";
  writeMinimalWav(music / "01.wav");
  writeMinimalWav(music / "nested" / "02.wav");

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  waitForSnapshotSongCount(*service, 2U);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto dirRenameBaseline = log.scanStartedCount();

  // 目录根内改名：单条 Moved，watch 路径迁移；写入不再依赖旧路径。
  std::error_code ec;
  std::filesystem::rename(music, root / "pop", ec);
  REQUIRE_FALSE(ec);
  CHECK(waitForSnapshotPaths(*service, {root / "pop" / "01.wav", root / "pop" / "nested" / "02.wav"},
                             std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));
  CHECK(log.scanStartedCount() == dirRenameBaseline);
  CHECK(log.scanErrorCount() == 0U);
  checkSnapshotCachePathsMatch(*service, temp);
  {
    const auto locations = locationsForRoot(temp);
    REQUIRE(locations.size() == 2U);
    CHECK(std::ranges::none_of(locations, [&music](const cache::CachedLocation& location) {
      return location.filePath == (music / "01.wav");
    }));
  }

  // 文件根内改名：同样是单条 Moved → 精准改名，不再触发扫描。
  const auto fileRenameBaseline = log.scanStartedCount();
  std::filesystem::rename(root / "pop" / "01.wav", root / "pop" / "renamed.wav", ec);
  REQUIRE_FALSE(ec);
  CHECK(waitForSnapshotPaths(*service, {root / "pop" / "renamed.wav", root / "pop" / "nested" / "02.wav"},
                             std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));
  CHECK(log.scanStartedCount() == fileRenameBaseline);
  CHECK(log.scanErrorCount() == 0U);
  checkSnapshotCachePathsMatch(*service, temp);
  checkScanRootHashMatchesDisk(temp);

  stopAndDestroy(service);
}

// 场景 6：尖峰风暴。阶段 A：100 个音频快速批量创建（文件级精准）；阶段 B：含 25 个音频的
// 目录整体移入（scoped 子树枚举，与阶段 A 事件可能同批 → "精准 + scope" 混合批次）；
// 阶段 C：15 个音频逐个快速移入（文件级精准）。全部收敛且无整根重扫、无 ScanError。
// debounce 用生产默认 50ms，确保成对事件（Add + Modified）恒同批。
TEST_CASE("efsw integration burst storm of creates and moves converges without rescan") {
  test::TempScannerRoot temp{"scanner-efsw-burst-storm"};
  const auto root = temp.path();
  std::filesystem::create_directories(root / "burst");
  const auto stagingDir = temp.path().parent_path() / ("seriona-efsw-storm-dir-" + temp.path().filename().string());
  const auto stagingFiles = temp.path().parent_path() / ("seriona-efsw-storm-files-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(stagingDir, ec);
  std::filesystem::remove_all(stagingFiles, ec);
  for (int i = 0; i < 20; ++i) {
    writeMinimalWav(stagingDir / ("m" + std::to_string(i) + ".wav"));
  }
  for (int i = 0; i < 5; ++i) {
    writeMinimalWav(stagingDir / "nested" / ("n" + std::to_string(i) + ".wav"));
  }
  for (int i = 0; i < 15; ++i) {
    writeMinimalWav(stagingFiles / ("f" + std::to_string(i) + ".wav"));
  }

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log, std::chrono::milliseconds{50});
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();

  for (int i = 0; i < 100; ++i) {
    writeMinimalWav(root / "burst" / ("s" + std::to_string(i) + ".wav"));
  }
  std::filesystem::rename(stagingDir, root / "storm-moved", ec);
  REQUIRE_FALSE(ec);
  for (int i = 0; i < 15; ++i) {
    std::filesystem::rename(stagingFiles / ("f" + std::to_string(i) + ".wav"),
                            root / ("f" + std::to_string(i) + ".wav"), ec);
    REQUIRE_FALSE(ec);
  }

  std::vector<std::filesystem::path> expected;
  for (int i = 0; i < 100; ++i) {
    expected.push_back(root / "burst" / ("s" + std::to_string(i) + ".wav"));
  }
  for (int i = 0; i < 20; ++i) {
    expected.push_back(root / "storm-moved" / ("m" + std::to_string(i) + ".wav"));
  }
  for (int i = 0; i < 5; ++i) {
    expected.push_back(root / "storm-moved" / "nested" / ("n" + std::to_string(i) + ".wav"));
  }
  for (int i = 0; i < 15; ++i) {
    expected.push_back(root / ("f" + std::to_string(i) + ".wav"));
  }

  CHECK(waitForSnapshotPaths(*service, expected, std::chrono::seconds{60}));
  CHECK(waitForScanQuiescence(log));
  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanErrorCount() == 0U);
  checkSnapshotCachePathsMatch(*service, temp);
  CHECK(locationsForRoot(temp).size() == expected.size());

  stopAndDestroy(service);
  std::filesystem::remove_all(stagingFiles, ec);
}

// 场景 8（大目录移入，≥100 子文件规模）：100 个直属文件 + 20 个嵌套文件 + 非音频杂物一次
// 目录移入；scoped 子树枚举一次读完，无整根重扫（ScanStarted 不增长）、无 ScanCompleted
// （scoped 契约）、无 ScanError，快照/缓存一致，耗时可接受。
TEST_CASE("efsw integration large directory move-in enumerates 100+ files precisely") {
  constexpr int kDirectFiles = 100;
  constexpr int kNestedDirs = 4;
  constexpr int kFilesPerNested = 5;
  test::TempScannerRoot temp{"scanner-efsw-large-dir-in"};
  const auto root = temp.path();
  const auto staging = temp.path().parent_path() / ("seriona-efsw-large-dir-in-" + temp.path().filename().string());
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  for (int i = 0; i < kDirectFiles; ++i) {
    writeMinimalWav(staging / "album" / ("track-" + std::to_string(i) + ".wav"));
  }
  for (int directory = 0; directory < kNestedDirs; ++directory) {
    for (int i = 0; i < kFilesPerNested; ++i) {
      writeMinimalWav(staging / "album" / ("disc-" + std::to_string(directory)) /
                      ("song-" + std::to_string(i) + ".wav"));
    }
  }
  {
    std::ofstream notes{staging / "album" / "cover.jpg"};
    notes << "not audio";
  }

  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log, std::chrono::milliseconds{50});
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  CHECK(waitForScanQuiescence(log));
  const auto startedBefore = log.scanStartedCount();
  const auto completedBefore = log.scanCompletedCount();

  const auto start = std::chrono::steady_clock::now();
  std::filesystem::rename(staging / "album", root / "album", ec);
  REQUIRE_FALSE(ec);

  std::vector<std::filesystem::path> expected;
  for (int i = 0; i < kDirectFiles; ++i) {
    expected.push_back(root / "album" / ("track-" + std::to_string(i) + ".wav"));
  }
  for (int directory = 0; directory < kNestedDirs; ++directory) {
    for (int i = 0; i < kFilesPerNested; ++i) {
      expected.push_back(root / "album" / ("disc-" + std::to_string(directory)) /
                         ("song-" + std::to_string(i) + ".wav"));
    }
  }
  CHECK(expected.size() >= 100U);

  CHECK(waitForSnapshotPaths(*service, expected, std::chrono::seconds{60}));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  CHECK(waitForScanQuiescence(log));

  CHECK(log.scanStartedCount() == startedBefore);
  CHECK(log.scanCompletedCount() == completedBefore);
  CHECK(log.scanErrorCount() == 0U);
  CHECK(elapsed < std::chrono::seconds{60});
  checkSnapshotCachePathsMatch(*service, temp);
  CHECK(locationsForRoot(temp).size() == expected.size());
  checkScanRootHashMatchesDisk(temp);

  stopAndDestroy(service);
  std::filesystem::remove_all(staging, ec);
}

// 事件流活跃期间析构服务：生产者线程持续写入触发事件，主线程确认至少一批事件已处理、
// 生产者仍在写时直接 service.reset()（析构内部 stopWatching join efsw 读线程与去抖线程）。
// 覆盖任务 4 审查门移交的"事件流/并发中析构"缺口（原矩阵仅覆盖静默期析构）。
void runDestructorDuringEventFlowRound(int round) {
  test::TempScannerRoot temp{"scanner-efsw-destruct-race-" + std::to_string(round)};
  const auto root = temp.path();
  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  CHECK(waitUntil([&] { return log.scanCompletedCount() >= 1U; }, std::chrono::seconds{10}));
  service->startWatching({ScannerRoot{.path = root}});

  std::atomic<bool> stopProducer{false};
  std::atomic<std::uint64_t> produced{0};
  std::thread producer([&] {
    std::uint64_t index = 0;
    const auto writeNext = [&] {
      writeMinimalWav(root / ("race-" + std::to_string(index) + ".wav"));
      ++index;
      produced.store(index, std::memory_order_relaxed);
    };
    // 阶段 1：突发 5 个 + 停顿 30ms（> debounce）：让去抖窗口关闭、至少一批事件被真实处理，
    // 证明事件流在前向推进。
    for (int batch = 0; batch < 4 && !stopProducer.load(std::memory_order_relaxed); ++batch) {
      for (int i = 0; i < 5; ++i) {
        writeNext();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    // 阶段 2：连续写入（1ms 间隔，短于 debounce 窗口）→ 事件持续在途，直到主线程完成析构。
    while (!stopProducer.load(std::memory_order_relaxed)) {
      writeNext();
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  });
  struct Joiner {
    std::thread& thread;
    ~Joiner() {
      if (thread.joinable()) {
        thread.join();
      }
    }
  } joiner{producer};

  const auto snapshotsBefore = log.snapshotUpdatedCount();
  // 阶段 1 已有一批事件被处理（证明去抖/精准链路在工作）。
  CHECK(waitUntil([&] { return produced.load() >= 5U && log.snapshotUpdatedCount() > snapshotsBefore; },
                  std::chrono::seconds{5}));
  // 生产者进入阶段 2（连续写入）→ 事件持续投递/在途，此刻直接析构（不先 stopWatching），
  // 让析构与生产者写入、efsw 读线程投递并发。
  CHECK(waitUntil([&] { return produced.load() >= 20U; }, std::chrono::seconds{5}));
  service.reset();
  stopProducer.store(true, std::memory_order_relaxed);
  producer.join();
  CHECK(std::filesystem::exists(root));
  CHECK(produced.load() > 0U);
}

TEST_CASE("efsw integration stop and destructor during active event stream stay safe") {
  for (int round = 0; round < 3; ++round) {
    runDestructorDuringEventFlowRound(round);
  }

  // 停止语义：stopWatching 返回后事件不再进入（watcher 关闭 + 去抖线程 join），
  // 之后磁盘新文件不会改变快照。
  test::TempScannerRoot temp{"scanner-efsw-stop-quiesce"};
  const auto root = temp.path();
  RealEfswEventLog log;
  auto service = makeRealEfswService(temp, log);
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  waitForInitialScanCompleted(log);
  service->startWatching({ScannerRoot{.path = root}});
  writeMinimalWav(root / "seed.wav");
  CHECK(waitForSnapshotPaths(*service, {root / "seed.wav"}, std::chrono::seconds{10}));
  CHECK(waitForScanQuiescence(log));

  service->stopWatching();
  const auto beforeStop = snapshotSongPaths(*service);
  writeMinimalWav(root / "after-stop.wav");
  std::this_thread::sleep_for(std::chrono::milliseconds{200});
  CHECK(snapshotSongPaths(*service) == beforeStop);
  CHECK(std::filesystem::exists(root / "after-stop.wav"));

  service->stop();
  service.reset();
}

} // namespace
} // namespace seriona::scanner
