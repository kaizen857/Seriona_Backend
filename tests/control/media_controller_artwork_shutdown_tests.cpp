#include "../../src/control/artwork_resolver.h"
#include "../../src/control/media_controller_module.h"
#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

scanner::SongMetadata song(std::string id, std::string path) {
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
                               .duration = std::chrono::milliseconds{3000},
                               .logicalTrackId = {},
                               .artworkPath = std::nullopt,
                               .thumbnailPath = std::nullopt};
}

scanner::SongMetadata songWithThumbnail(std::string id, std::string path, std::filesystem::path thumbnailPath) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.thumbnailPath = std::move(thumbnailPath);
  return metadata;
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

MediaControlCommand command(MediaControlCommandKind kind) {
  MediaControlCommand value{};
  value.kind = kind;
  return value;
}

// Loader that blocks until released, then returns a no-art outcome. Mirrors
// the blocked-loader pattern of the resolver tests.
class BlockingArtworkLoader {
public:
  ArtworkResolveOutcome operator()(const std::filesystem::path&, const std::filesystem::path&) {
    {
      std::lock_guard lock{mutex_};
      entered_ = true;
    }
    enteredCv_.notify_all();
    std::unique_lock lock{mutex_};
    releaseCv_.wait(lock, [this] { return released_; });
    return ArtworkResolveOutcome{};
  }

  bool waitForEnter(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return enteredCv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() {
    {
      std::lock_guard lock{mutex_};
      released_ = true;
    }
    releaseCv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable enteredCv_;
  std::condition_variable releaseCv_;
  bool entered_{false};
  bool released_{false};
};

struct ArtworkShutdownFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::shared_ptr<ArtworkResolver> resolver;
  std::unique_ptr<MediaController> controller;

  ArtworkShutdownFixture(std::filesystem::path coverExportDir, bool inlineMode, ArtworkLoader loader = ArtworkResolver::productionLoader()) {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    resolver = std::make_shared<ArtworkResolver>(std::move(coverExportDir), ArtworkResolverCompletion{}, std::move(loader));
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService),
                                                                 .folderSortSettingsStore = {},
                                                                 .artworkResolver = resolver},
                                     MediaControllerOptions{.runInlineForTests = inlineMode});
  }
};

}  // namespace

TEST_CASE("production media controller dependencies wire the configured artwork directory into the resolver") {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto databasePath = std::filesystem::temp_directory_path() / ("seriona-artwork-shutdown-" + suffix + ".sqlite");
  const auto coverDir = std::filesystem::temp_directory_path() / ("seriona-artwork-shutdown-" + suffix + "-covers");
  std::error_code cleanupError{};
  std::filesystem::remove(databasePath, cleanupError);
  std::filesystem::remove_all(coverDir, cleanupError);

  auto dependencies = makeProductionMediaControllerDependencies(databasePath, coverDir);

  REQUIRE(dependencies.artworkResolver != nullptr);
  const auto* resolver = dynamic_cast<const ArtworkResolver*>(dependencies.artworkResolver.get());
  REQUIRE(resolver != nullptr);
  CHECK(resolver->coverExportDir() == coverDir);

  dependencies.artworkResolver.reset();
  dependencies.folderSortSettingsStore.reset();
  std::filesystem::remove(databasePath, cleanupError);
}

TEST_CASE("production media controller dependencies omit the artwork resolver without an export directory") {
  auto dependencies = makeProductionMediaControllerDependencies();

  CHECK(dependencies.artworkResolver == nullptr);
}

TEST_CASE("media controller shutdown before start stops the artwork resolver") {
  SUBCASE("inline mode") {
    ArtworkShutdownFixture fixture{std::filesystem::temp_directory_path(), true};
    CHECK_FALSE(fixture.resolver->stopped());
    fixture.controller->shutdown();
    CHECK(fixture.resolver->stopped());
  }
  SUBCASE("threaded mode") {
    ArtworkShutdownFixture fixture{std::filesystem::temp_directory_path(), false};
    CHECK_FALSE(fixture.resolver->stopped());
    fixture.controller->shutdown();
    CHECK(fixture.resolver->stopped());
  }
}

TEST_CASE("media controller repeated shutdown is idempotent") {
  ArtworkShutdownFixture fixture{std::filesystem::temp_directory_path(), true};
  fixture.controller->start();
  fixture.controller->shutdown();
  const auto metadataStopsAfterFirst = fixture.fakeMetadata->stopCalls();
  CHECK(metadataStopsAfterFirst == 1U);

  fixture.controller->shutdown();

  CHECK(fixture.resolver->stopped());
  CHECK(fixture.fakeMetadata->stopCalls() == metadataStopsAfterFirst);
}

TEST_CASE("media controller shutdown waits for in-flight artwork resolution and drops the result") {
  BlockingArtworkLoader loader;
  ArtworkShutdownFixture fixture{std::filesystem::temp_directory_path(), false, std::ref(loader)};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(
      libraryTree({songWithThumbnail("a", "music/a.flac", "/thumbs/a.png")}, 21), 21));
  REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  REQUIRE(loader.waitForEnter(std::chrono::seconds{1}));

  auto shutdownResult = std::async(std::launch::async, [&] { fixture.controller->shutdown(); });
  CHECK(shutdownResult.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready);

  loader.release();
  REQUIRE(shutdownResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(fixture.resolver->stopped());

  const auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/thumbs/a.png"});
  CHECK(snapshot.artwork->thumbnailPath == std::filesystem::path{"/thumbs/a.png"});
}
