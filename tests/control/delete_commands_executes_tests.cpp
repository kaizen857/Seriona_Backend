// T8【后端】删除命令：DeleteTrack/DeleteFolder 控制层测试（TDD 先行）。
// 覆盖：命令字段校验；目标转发到 scanner（removeLocation）；删除在播/暂停曲目
// 先停止播放的竞态防护；scanner 失败 → BackendRejected + CommandRejected 通知；
// 文件不存在 → 幂等成功；真实 scanner 集成（文件消失 + 库快照更新 + 通知）。
#include <doctest.h>

#include "control_test_harness.h"

#include "seriona/control/media_controller.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include "file_scanner_service_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

// 测试专用临时根目录（控制测试不依赖 tests/scanner 的 scanner_test_harness）。
class TempRoot {
public:
  TempRoot() {
    const auto base = std::filesystem::temp_directory_path() / "seriona-control-delete-tests";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = base / ("case-" + std::to_string(stamp) + "-" + std::to_string(counter_++));
    std::filesystem::create_directories(path_, ec);
    if (ec) {
      throw std::runtime_error("failed to create temp root: " + ec.message());
    }
  }

  ~TempRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  static std::size_t counter_;
  std::filesystem::path path_{};
};

std::size_t TempRoot::counter_{0};

scanner::SongMetadata song(std::string id, std::string path, std::chrono::milliseconds duration = std::chrono::milliseconds{3000}) {
  return scanner::SongMetadata{.trackId = std::move(id),
                               .filePath = std::filesystem::path{std::move(path)},
                               .title = {},
                               .artist = {},
                               .album = {},
                               .albumArtist = {},
                               .genre = {},
                               .trackNumber = std::nullopt,
                               .discNumber = std::nullopt,
                               .year = std::nullopt,
                               .sampleRate = std::nullopt,
                               .bitDepth = std::nullopt,
                               .channels = std::nullopt,
                               .fileSizeBytes = std::nullopt,
                               .fileMtime = std::nullopt,
                               .contentHash = {},
                               .effectiveLyricsSource = scanner::LyricsSource::None,
                               .effectiveLyrics = {},
                               .externalLyricsPath = std::nullopt,
                               .externalLyricsHash = std::nullopt,
                               .externalLyricsMtime = std::nullopt,
                               .sourceFilePath = {},
                               .offset = std::nullopt,
                               .duration = duration,
                               .logicalTrackId = {},
                               .artworkPath = std::nullopt,
                               .thumbnailPath = std::nullopt};
}

scanner::PlaylistNode rootNode(std::vector<std::string> children) {
  return scanner::PlaylistNode{.nodeId = "root",
                               .parentNodeId = std::nullopt,
                               .kind = scanner::PlaylistNodeKind::Root,
                               .displayName = "Library",
                               .song = std::nullopt,
                               .childNodeIds = std::move(children)};
}

scanner::PlaylistNode trackNode(std::string nodeId, scanner::SongMetadata metadata) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::string{"root"},
                               .kind = scanner::PlaylistNodeKind::Track,
                               .displayName = metadata.trackId,
                               .song = std::move(metadata),
                               .childNodeIds = {}};
}

scanner::PlaylistTreeSnapshot libraryTree(std::vector<scanner::SongMetadata> songs, std::uint64_t version) {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = version;
  snapshot.rootNodeId = "root";
  std::vector<std::string> children;
  children.reserve(songs.size());
  for (std::size_t index = 0; index < songs.size(); ++index) {
    children.push_back("track-node-" + std::to_string(index));
  }
  snapshot.nodes.push_back(rootNode(children));
  for (std::size_t index = 0; index < songs.size(); ++index) {
    snapshot.nodes.push_back(trackNode(children[index], std::move(songs[index])));
  }
  return snapshot;
}

scanner::ScannerEvent scannerSnapshotEvent(scanner::PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::PlaylistSnapshotUpdated,
                               .monotonicVersion = eventVersion,
                               .timestamp = {},
                               .payload = std::move(snapshot)};
}

MediaControlCommand deleteCommand(MediaControlCommandKind kind, std::filesystem::path target) {
  MediaControlCommand value{};
  value.kind = kind;
  value.targetPath = std::move(target);
  return value;
}

MediaControlCommand command(MediaControlCommandKind kind) {
  MediaControlCommand value{};
  value.kind = kind;
  return value;
}

// --- 假 scanner 控制层接线 fixture（复用 configure_output_executes_tests 模式） ---

struct FakeScannerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  std::unique_ptr<MediaController> controller{};

  FakeScannerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService)},
                                     MediaControllerOptions{.runInlineForTests = true});
    controller->subscribeDomainNotifications([this](const ControlDomainNotification& notification) {
      std::lock_guard lock{notificationMutex};
      notifications.push_back(notification);
    });
  }

  [[nodiscard]] bool waitForNotification(ControlDomainNotificationKind kind, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock{notificationMutex};
        if (std::ranges::any_of(notifications, [kind](const ControlDomainNotification& notification) {
              return notification.kind == kind;
            })) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
  }
};

void installLibrary(FakeScannerFixture& fixture, std::uint64_t treeVersion = 20, std::uint64_t eventVersion = 1) {
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, treeVersion),
                                                 eventVersion));
  fixture.controller->drainForTests();
}

// --- 真实 scanner 集成 fixture（真实文件 + 真实 SQLite 缓存 + 假音频） ---

class FakeMetadataReader final : public scanner::TagMetadataReader {
public:
  void put(std::filesystem::path path, scanner::RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] scanner::RawTagMetadata read(const scanner::TagReadRequest& request) override {
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + request.path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;
  }

  [[nodiscard]] std::vector<scanner::RawTagMetadata> readCueSheet(const scanner::TagReadRequest&) override { return {}; }

private:
  std::map<std::filesystem::path, scanner::RawTagMetadata> metadataByPath_;
};

[[nodiscard]] scanner::RawTagMetadata makeMetadata(std::string title) {
  scanner::RawTagMetadata metadata{};
  metadata.title = std::move(title);
  metadata.artist = "Test Artist";
  metadata.album = "Test Album";
  metadata.duration = std::chrono::milliseconds{180000};
  metadata.sampleRate = 48000;
  metadata.bitDepth = 24;
  metadata.channels = 2;
  return metadata;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

struct RealScannerFixture {
  TempRoot temp{};
  std::filesystem::path root{temp.path() / "music"};
  std::shared_ptr<FakeMetadataReader> reader{std::make_shared<FakeMetadataReader>()};
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  std::unique_ptr<MediaController> controller{};

  RealScannerFixture() {
    std::filesystem::create_directories(root);
    auto realScanner = scanner::makeFileScannerService(scanner::FileScannerServiceDependencies{
        .metadataReader = reader,
        .watcherFactory = nullptr,
        .databasePath = temp.path() / "library.sqlite",
        .coverExportDir = temp.path() / "covers",
        .watcherDebounce = std::chrono::milliseconds{10},
        .reconcileInterval = std::chrono::milliseconds{60000}});
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = std::move(realScanner),
                                                                 .metadata = std::move(metadataService)},
                                     MediaControllerOptions{.runInlineForTests = true});
    controller->subscribeDomainNotifications([this](const ControlDomainNotification& notification) {
      std::lock_guard lock{notificationMutex};
      notifications.push_back(notification);
    });
  }

  void putSong(const std::filesystem::path& relative, std::string title) {
    const auto absolute = root / relative;
    std::filesystem::create_directories(absolute.parent_path());
    writeText(absolute, "fake audio");
    reader->put(absolute, makeMetadata(std::move(title)));
  }

  void scanLibrary() {
    const auto result = controller->scanLibrary({scanner::ScannerRoot{.path = root, .recursive = true}}, scanner::ScanMode::Full);
    REQUIRE(result.accepted);
  }

  // 轮询处理内联事件循环直至谓词成立（扫描/删除均经 scanner worker + 控制事件循环）。
  [[nodiscard]] bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      controller->drainForTests();
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    controller->drainForTests();
    return predicate();
  }

  [[nodiscard]] bool libraryTreeReady(std::size_t expectedTracks) {
    return waitFor([this, expectedTracks] {
      const auto tree = controller->libraryStateSnapshot().libraryTree;
      return tree.has_value() && tree->nodes.size() >= expectedTracks;
    }, std::chrono::seconds{10});
  }

  [[nodiscard]] bool libraryTreeMissing(const std::filesystem::path& absolute, std::size_t expectedRemaining) {
    return waitFor([this, absolute, expectedRemaining] {
      const auto tree = controller->libraryStateSnapshot().libraryTree;
      if (!tree.has_value()) {
        return false;
      }
      const auto stillPresent = std::ranges::any_of(tree->nodes, [&](const scanner::PlaylistNode& node) {
        return node.song.has_value() && node.song->filePath == absolute;
      });
      return !stillPresent && tree->nodes.size() >= expectedRemaining;
    }, std::chrono::seconds{5});
  }

  [[nodiscard]] bool waitForNotificationKind(ControlDomainNotificationKind kind, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock{notificationMutex};
        if (std::ranges::any_of(notifications, [kind](const ControlDomainNotification& notification) {
              return notification.kind == kind;
            })) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
  }
};

}  // namespace

TEST_CASE("controller DeleteTrack plumbing") {
  SUBCASE("forwards target path to scanner and returns accepted") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/a.flac"));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
    REQUIRE(!fixture.fakeScanner->removeLocationPaths().empty());
    CHECK(fixture.fakeScanner->removeLocationPaths().back() == std::filesystem::path{"music/a.flac"});
  }

  SUBCASE("forwards DeleteFolder folder path to scanner") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteFolder, "music/album"));

    CHECK(result.accepted);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
    CHECK(fixture.fakeScanner->removeLocationPaths().back() == std::filesystem::path{"music/album"});
  }

  SUBCASE("rejects commands without a target path") {
    FakeScannerFixture fixture{};
    fixture.controller->start();

    const auto result = fixture.controller->submitCommand(command(MediaControlCommandKind::DeleteTrack));

    CHECK_FALSE(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(fixture.fakeScanner->removeLocationCalls() == 0U);
    CHECK(fixture.waitForNotification(ControlDomainNotificationKind::CommandRejected));
  }

  SUBCASE("stops playback first when the target is the playing track") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/a.flac"));

    CHECK(result.accepted);
    // 先停止当前播放（audio->stop 经 executeIntents 已调用），再删除
    CHECK(fixture.fakeAudio->stopCalls() == 1U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
  }

  SUBCASE("stops playback when the target is loading") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);
    // 模拟 audio worker 加载中途：状态机发布 Loading（loadTrackOnWorker 进行中）。
    // T6 切轨抑制：selectTrack 后的 Loading 被压回乐观 Playing（前端按钮保持
    // 播放态），可见状态为 Playing —— 删除路径按 Playing 处理同样先停播。
    audio::BackendEvent loadingEvent{};
    loadingEvent.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    loadingEvent.monotonicVersion = 10;
    loadingEvent.timestamp = std::chrono::steady_clock::now();
    loadingEvent.payload = audio::PlaybackStateChanged{audio::PlaybackState::Loading};
    fixture.fakeAudio->emit(loadingEvent);
    fixture.controller->drainForTests();
    REQUIRE(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/a.flac"));

    CHECK(result.accepted);
    CHECK(fixture.fakeAudio->stopCalls() == 1U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
  }

  SUBCASE("keeps playing when deleting a non-current track") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/b.flac"));

    CHECK(result.accepted);
    CHECK(fixture.fakeAudio->stopCalls() == 0U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
  }

  SUBCASE("maps scanner rejection to BackendRejected with CommandRejected notification") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);
    fixture.fakeScanner->setRemoveLocationResult(false);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/a.flac"));

    CHECK_FALSE(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::BackendRejected);
    CHECK_FALSE(result.message.empty());
    CHECK(fixture.waitForNotification(ControlDomainNotificationKind::CommandRejected));
  }

  SUBCASE("returns accepted for already-missing files (idempotent via scanner)") {
    FakeScannerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, "music/missing.flac"));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    REQUIRE(fixture.fakeScanner->removeLocationCalls() == 1U);
  }
}

TEST_CASE("controller delete commands with real scanner: file disappears and library snapshot updates") {
  RealScannerFixture fixture{};
  const auto trackA = fixture.root / "a.flac";
  const auto trackB = fixture.root / "b.flac";
  fixture.putSong("a.flac", "Alpha");
  fixture.putSong("b.flac", "Beta");
  fixture.controller->start();
  fixture.scanLibrary();
  REQUIRE(fixture.libraryTreeReady(3U));  // root + 2 tracks

  const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, trackA));

  CHECK(result.accepted);
  CHECK_FALSE(std::filesystem::exists(trackA));
  CHECK(std::filesystem::exists(trackB));
  // 库快照更新：树中不再含 trackA（PlaylistSnapshotUpdated → LibraryStateSnapshot 发布）
  REQUIRE(fixture.libraryTreeMissing(trackA, 2U));
  CHECK(fixture.waitForNotificationKind(ControlDomainNotificationKind::LibrarySnapshotUpdated));
}

TEST_CASE("controller DeleteFolder with real scanner: recursive removal and snapshot update") {
  RealScannerFixture fixture{};
  const auto folder = fixture.root / "folder";
  const auto nested = folder / "sub" / "nested.flac";
  const auto keep = fixture.root / "keep.flac";
  fixture.putSong("folder/x.flac", "Folder One");
  fixture.putSong("folder/sub/nested.flac", "Folder Nested");
  fixture.putSong("keep.flac", "Keep");
  fixture.controller->start();
  fixture.scanLibrary();
  REQUIRE(fixture.libraryTreeReady(4U));  // root + 3 tracks

  const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteFolder, folder));

  CHECK(result.accepted);
  CHECK_FALSE(std::filesystem::exists(folder));  // 递归全删
  CHECK(std::filesystem::exists(keep));
  REQUIRE(fixture.libraryTreeMissing(nested, 2U));
}

TEST_CASE("controller delete commands with real scanner: deleting the playing track stops playback") {
  RealScannerFixture fixture{};
  const auto trackA = fixture.root / "a.flac";
  fixture.putSong("a.flac", "Alpha");
  fixture.putSong("b.flac", "Beta");
  fixture.controller->start();
  fixture.scanLibrary();
  REQUIRE(fixture.libraryTreeReady(3U));

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  REQUIRE(playResult.accepted);
  REQUIRE(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

  const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, trackA));

  CHECK(result.accepted);
  CHECK(fixture.fakeAudio->stopCalls() >= 1U);  // 播放停止（先停再删）
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  CHECK_FALSE(std::filesystem::exists(trackA));
  REQUIRE(fixture.libraryTreeMissing(trackA, 2U));
  CHECK(fixture.waitForNotificationKind(ControlDomainNotificationKind::LibrarySnapshotUpdated));
}

TEST_CASE("controller delete commands with real scanner: missing file is idempotent success") {
  RealScannerFixture fixture{};
  const auto trackA = fixture.root / "a.flac";
  fixture.putSong("a.flac", "Alpha");
  fixture.controller->start();
  fixture.scanLibrary();
  REQUIRE(fixture.libraryTreeReady(2U));

  const auto missing = fixture.root / "never-existed.flac";
  CHECK_FALSE(std::filesystem::exists(missing));

  const auto result = fixture.controller->submitCommand(deleteCommand(MediaControlCommandKind::DeleteTrack, missing));

  CHECK(result.accepted);
  CHECK(result.code == MediaControllerErrorCode::None);
  CHECK(std::filesystem::exists(trackA));  // 库内曲目不受影响
}
