// T8【后端】删除命令：removeLocation 服务级行为测试（TDD 先行）。
// 覆盖：DeleteTrack 单文件删除后磁盘/缓存/树快照一致；DeleteFolder 递归删除
// 目录树；文件不存在幂等成功；拒绝删除扫描根本身。
#include <doctest/doctest.h>

#include "file_scanner_service_internal.h"
#include "scanner_test_harness.h"

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"
#include "seriona/scanner/cache/sqlite_cache.h"

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
#include <utility>
#include <vector>

using namespace seriona::scanner;
namespace fs = std::filesystem;
namespace test = seriona::scanner::test;

namespace {

class FakeMetadataReader final : public TagMetadataReader {
public:
  void put(fs::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + request.path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;
  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

private:
  std::map<fs::path, RawTagMetadata> metadataByPath_;
};

struct ScanState {
  std::atomic<bool> completed{false};
  std::mutex mutex{};
  std::condition_variable cv{};
  PlaylistTreeSnapshot snapshot{};

  void onEvent(const ScannerEvent& event) {
    if (event.type != ScannerEventType::ScanCompleted) {
      return;
    }
    {
      std::lock_guard lock{mutex};
      if (const auto* snap = std::get_if<PlaylistTreeSnapshot>(&event.payload)) {
        snapshot = *snap;  //  与等待侧同一把锁，避免快照被无同步写入
      }
      completed.store(true);
    }
    cv.notify_all();
  }

  [[nodiscard]] bool waitForScan(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    return cv.wait_for(lock, timeout, [this] { return completed.load(); });
  }
};

[[nodiscard]] RawTagMetadata makeMetadata(std::string title) {
  RawTagMetadata metadata{};
  metadata.title = std::move(title);
  metadata.artist = "Test Artist";
  metadata.album = "Test Album";
  metadata.duration = std::chrono::milliseconds{180000};
  metadata.sampleRate = 48000;
  metadata.bitDepth = 24;
  metadata.channels = 2;
  return metadata;
}

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

struct ServiceFixture {
  test::TempScannerRoot temp{"scanner-remove-location"};
  fs::path databasePath{temp.dbPath("library.sqlite")};
  fs::path root{temp.path() / "music"};
  std::shared_ptr<FakeMetadataReader> reader{std::make_shared<FakeMetadataReader>()};
  std::shared_ptr<FileScannerService> service{};
  ScanState scanState{};

  ServiceFixture() {
    fs::create_directories(root);
    service = makeFileScannerService(FileScannerServiceDependencies{
        .metadataReader = reader,
        .watcherFactory = nullptr,
        .databasePath = databasePath,
        .coverExportDir = temp.path() / "covers",
        .folderThumbnailSeam = nullptr,
        .watcherDebounce = std::chrono::milliseconds{10},
        .reconcileInterval = std::chrono::milliseconds{60000}});
    service->setEventSink([this](const ScannerEvent& event) { scanState.onEvent(event); });
  }

  ~ServiceFixture() {
    //  service 声明在 scanState 之前，析构逆序会先销毁 scanState，
    //  而此时去抖线程可能仍在往事件回调里投递快照，写的是已释放内存
    //  （ASan：heap-use-after-free，Linux 上表现为偶发 munmap_chunk 崩溃）。
    //  故先停服务、再摘掉回调，之后成员析构才是安全的。
    if (service) {
      service->stopWatching();
      service->stop();
      service->setEventSink(nullptr);
    }
  }

  ServiceFixture(const ServiceFixture&) = delete;
  ServiceFixture& operator=(const ServiceFixture&) = delete;
  ServiceFixture(ServiceFixture&&) = delete;
  ServiceFixture& operator=(ServiceFixture&&) = delete;

  void putSong(const fs::path& relative, std::string title) {
    const auto absolute = root / relative;
    fs::create_directories(absolute.parent_path());
    writeText(absolute, "fake audio");
    reader->put(absolute, makeMetadata(std::move(title)));
  }

  void scanAndWatch() {
    service->scan({ScannerRoot{.path = root, .recursive = true}}, ScanMode::Full);
    REQUIRE(scanState.waitForScan(std::chrono::seconds{10}));
    service->startWatching({ScannerRoot{.path = root, .recursive = true}});
  }
};

[[nodiscard]] bool snapshotHasSong(const PlaylistTreeSnapshot& snapshot, const fs::path& absolute) {
  return std::ranges::any_of(snapshot.nodes, [&](const PlaylistNode& node) {
    return node.song.has_value() && node.song->filePath == absolute;
  });
}

[[nodiscard]] bool cacheHasLocation(const fs::path& databasePath, const fs::path& root, const fs::path& absolute) {
  // 与 file_scanner_orchestrator 的 scanRootDatabasePath 保持同构：
  // 缓存库路径 = <databasePath>.scan-roots.sqlite。
  const auto cacheDb = fs::path{databasePath.generic_string() + ".scan-roots.sqlite"};
  const cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = cacheDb}};
  const auto locations = cache.loadLocationsByRoot(root);
  return std::ranges::any_of(locations, [&](const cache::CachedLocation& location) {
    return location.filePath == absolute;
  });
}

}  // namespace

TEST_CASE("scanner removeLocation deletes a single file from disk, cache and tree snapshot") {
  ServiceFixture fixture{};
  const auto trackA = fixture.root / "a.flac";
  const auto trackB = fixture.root / "b.flac";
  fixture.putSong("a.flac", "Alpha");
  fixture.putSong("b.flac", "Beta");
  fixture.scanAndWatch();

  REQUIRE(snapshotHasSong(fixture.service->snapshot(), trackA));
  REQUIRE(snapshotHasSong(fixture.service->snapshot(), trackB));
  REQUIRE(cacheHasLocation(fixture.databasePath, fixture.root, trackA));

  const bool removed = fixture.service->removeLocation(trackA);

  CHECK(removed);
  CHECK_FALSE(fs::exists(trackA));
  CHECK(fs::exists(trackB));
  // 树快照不再包含被删曲目
  CHECK_FALSE(snapshotHasSong(fixture.service->snapshot(), trackA));
  CHECK(snapshotHasSong(fixture.service->snapshot(), trackB));
  // locations 缓存行被删除（deleteLocationsByPathPrefix 生效）
  CHECK_FALSE(cacheHasLocation(fixture.databasePath, fixture.root, trackA));
  CHECK(cacheHasLocation(fixture.databasePath, fixture.root, trackB));
}

TEST_CASE("scanner removeLocation recursively deletes a folder subtree") {
  ServiceFixture fixture{};
  const auto folder = fixture.root / "folder";
  const auto nestedFile = folder / "sub" / "nested.flac";
  const auto keep = fixture.root / "keep.flac";
  fixture.putSong("folder/x.flac", "Folder One");
  fixture.putSong("folder/sub/nested.flac", "Folder Nested");
  fixture.putSong("keep.flac", "Keep");
  fixture.scanAndWatch();

  REQUIRE(snapshotHasSong(fixture.service->snapshot(), folder / "x.flac"));
  REQUIRE(snapshotHasSong(fixture.service->snapshot(), nestedFile));
  REQUIRE(cacheHasLocation(fixture.databasePath, fixture.root, nestedFile));

  const bool removed = fixture.service->removeLocation(folder);

  CHECK(removed);
  CHECK_FALSE(fs::exists(folder));  // 递归全删（含 sub 子目录）
  CHECK(fs::exists(keep));
  CHECK_FALSE(snapshotHasSong(fixture.service->snapshot(), folder / "x.flac"));
  CHECK_FALSE(snapshotHasSong(fixture.service->snapshot(), nestedFile));
  CHECK_FALSE(cacheHasLocation(fixture.databasePath, fixture.root, folder / "x.flac"));
  CHECK_FALSE(cacheHasLocation(fixture.databasePath, fixture.root, nestedFile));
  CHECK(cacheHasLocation(fixture.databasePath, fixture.root, keep));
}

TEST_CASE("scanner removeLocation is idempotent for missing targets") {
  ServiceFixture fixture{};
  const auto trackA = fixture.root / "a.flac";
  fixture.putSong("a.flac", "Alpha");
  fixture.scanAndWatch();

  const auto missing = fixture.root / "never-existed.flac";
  CHECK_FALSE(fs::exists(missing));

  const bool removed = fixture.service->removeLocation(missing);

  CHECK(removed);  // 文件不存在 = 幂等成功
  CHECK(snapshotHasSong(fixture.service->snapshot(), trackA));
  CHECK(cacheHasLocation(fixture.databasePath, fixture.root, trackA));
}

TEST_CASE("scanner removeLocation refuses to remove a scan root") {
  ServiceFixture fixture{};
  fixture.putSong("a.flac", "Alpha");
  fixture.scanAndWatch();

  const bool removed = fixture.service->removeLocation(fixture.root);

  CHECK_FALSE(removed);  // 扫描根是保留边界
  CHECK(fs::exists(fixture.root));
  CHECK(fs::exists(fixture.root / "a.flac"));
}

TEST_CASE("scanner removeRoot clears index and cache for an explicit root removal") {
  ServiceFixture fixture{};
  fixture.putSong("a.flac", "Alpha");
  fixture.putSong("b.flac", "Beta");
  fixture.scanAndWatch();
  REQUIRE(snapshotHasSong(fixture.service->snapshot(), fixture.root / "a.flac"));
  REQUIRE(cacheHasLocation(fixture.databasePath, fixture.root, fixture.root / "a.flac"));

  const bool removed = fixture.service->removeRoot(fixture.root);

  CHECK(removed);
  // 显式根移除是最终清理出口：快照歌曲、locations、scan_roots 行都必须清空
  // （空树仍保留 Library 根 Directory 节点，故断言"无歌曲节点"而非 nodes 为空）。
  CHECK(std::ranges::none_of(fixture.service->snapshot().nodes, [](const PlaylistNode& node) {
    return node.song.has_value();
  }));
  CHECK_FALSE(cacheHasLocation(fixture.databasePath, fixture.root, fixture.root / "a.flac"));
  CHECK_FALSE(cacheHasLocation(fixture.databasePath, fixture.root, fixture.root / "b.flac"));
  const auto cacheDb = fs::path{fixture.databasePath.generic_string() + ".scan-roots.sqlite"};
  const cache::SQLiteCache cache{cache::ScannerCacheConfig{.databasePath = cacheDb}};
  CHECK_FALSE(cache.loadScanRoot(fixture.root).has_value());
  CHECK(cache.loadLocationsByRoot(fixture.root).empty());
  // 磁盘文件保持不动：removeRoot 只清索引，不删用户文件。
  CHECK(fs::exists(fixture.root / "a.flac"));
  CHECK(fs::exists(fixture.root / "b.flac"));
}

TEST_CASE("scanner removeRoot refuses unknown paths and non-root locations") {
  ServiceFixture fixture{};
  fixture.putSong("folder/a.flac", "Alpha");
  fixture.scanAndWatch();

  CHECK_FALSE(fixture.service->removeRoot(fixture.root / "folder"));
  CHECK_FALSE(fixture.service->removeRoot(fixture.root / "never-existed"));
  CHECK(snapshotHasSong(fixture.service->snapshot(), fixture.root / "folder" / "a.flac"));
  CHECK(cacheHasLocation(fixture.databasePath, fixture.root, fixture.root / "folder" / "a.flac"));
}

// 跨根同 rel 布局：removeLocation(rootA/album) 只应清 rootA 的条目与缓存行；rootB 的同 rel
// 歌曲必须保留在快照/树中（修复前 allSongs_ 按 treeRelativePath 前缀跨根擦除 → rootB 的歌曲
// 从快照永久消失，而其缓存行仍在 → 快照/缓存分歧，直到重扫才恢复）。
TEST_CASE("scanner removeLocation cross root keeps the other root's songs") {
  test::TempScannerRoot temp{"scanner-remove-location-cross-root"};
  const auto rootA = temp.path() / "rootA";
  const auto rootB = temp.path() / "rootB";
  const auto albumA = rootA / "album";
  const auto albumB = rootB / "album";
  const auto songA = albumA / "a.flac";
  const auto songB = albumB / "b.flac";
  for (const auto& path : {songA, songB}) {
    fs::create_directories(path.parent_path());
    writeText(path, "fake audio");
  }
  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(songA, makeMetadata("A"));
  reader->put(songB, makeMetadata("B"));
  const auto databasePath = temp.dbPath("library.sqlite");
  auto service = makeFileScannerService(FileScannerServiceDependencies{
      .metadataReader = reader,
      .watcherFactory = nullptr,
      .databasePath = databasePath,
      .coverExportDir = temp.path() / "covers",
      .folderThumbnailSeam = nullptr,
      .watcherDebounce = std::chrono::milliseconds{10},
      .reconcileInterval = std::chrono::milliseconds{60000}});
  ScanState scanState;
  service->setEventSink([&scanState](const ScannerEvent& event) { scanState.onEvent(event); });
  struct ServiceStopGuard {
    std::shared_ptr<FileScannerService> service;
    ~ServiceStopGuard() {
      service->stopWatching();
      service->stop();
      service->setEventSink(nullptr);
    }
  } stopGuard{service};

  service->scan({ScannerRoot{.path = rootA, .recursive = true}, ScannerRoot{.path = rootB, .recursive = true}},
                ScanMode::Full);
  REQUIRE(scanState.waitForScan(std::chrono::seconds{10}));
  service->startWatching({ScannerRoot{.path = rootA, .recursive = true}, ScannerRoot{.path = rootB, .recursive = true}});
  REQUIRE(snapshotHasSong(service->snapshot(), songA));
  REQUIRE(snapshotHasSong(service->snapshot(), songB));

  const bool removed = service->removeLocation(albumA);

  CHECK(removed);
  CHECK_FALSE(snapshotHasSong(service->snapshot(), songA));
  CHECK(snapshotHasSong(service->snapshot(), songB));
  CHECK_FALSE(cacheHasLocation(databasePath, rootA, songA));
  CHECK(cacheHasLocation(databasePath, rootB, songB));
  // 合并树 album 目录节点由 rootB 的存活条目重建（track 节点键仍是 rootB 的 rel）。
  const auto snapshot = service->snapshot();
  const auto trackNode = std::ranges::find_if(snapshot.nodes, [&](const PlaylistNode& node) {
    return node.song.has_value() && node.song->filePath == songB;
  });
  REQUIRE(trackNode != snapshot.nodes.end());
  CHECK(trackNode->nodeId == "track:album/b.flac");
}
