#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

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
                               .logicalTrackId = {}};
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

scanner::ScannerEvent scanStartedEvent(std::uint64_t version) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanStarted,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = scanner::ScanProgress{}};
}

audio::BackendEvent audioTrackChangedEvent(std::string id, std::string path, std::uint64_t version) {
  auto request = audio::TrackPlaybackRequest{.trackId = std::move(id),
                                             .filePath = std::filesystem::path{std::move(path)},
                                             .title = {},
                                             .artist = {},
                                             .offset = std::nullopt,
                                             .duration = std::chrono::milliseconds{3000},
                                             .sampleRate = std::nullopt,
                                             .bitDepth = std::nullopt,
                                             .channels = std::nullopt,
                                             .format = std::nullopt};
  return audio::BackendEvent{.type = audio::BackendEventType::TrackChanged,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::TrackChanged{.request = std::move(request)}};
}

MediaControlCommand command(MediaControlCommandKind kind) {
  MediaControlCommand value{};
  value.kind = kind;
  return value;
}

TrackIdentity track(std::string id, std::string path) {
  return TrackIdentity{.trackId = std::move(id), .filePath = std::filesystem::path{std::move(path)}, .sourceId = {}, .libraryId = {}};
}

struct ControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  control_test::FakeMetadataSharingService* fakeMetadata{nullptr};
  std::unique_ptr<MediaController> controller{};

  ControllerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService)},
                                     MediaControllerOptions{.runInlineForTests = true});
  }
};

void installLibrary(ControllerFixture& fixture, std::uint64_t treeVersion = 20, std::uint64_t eventVersion = 1) {
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, treeVersion),
                                                eventVersion));
  fixture.controller->drainForTests();
}

}

TEST_CASE("media controller facade drives fake audio load before play for play and select") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(playResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.fakeAudio->playCalls() == 1U);

  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("b", "music/b.flac");
  const auto selectResult = fixture.controller->submitCommand(select);

  CHECK(selectResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 2U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");
  CHECK(fixture.fakeAudio->playCalls() == 2U);
}

TEST_CASE("media controller facade scans library and publishes committed library snapshots") {
  ControllerFixture fixture{};
  control_test::LibraryStateSnapshotCollector librarySnapshots{};
  fixture.controller->subscribeLibraryState([&](const LibraryStateSnapshot& snapshot) { librarySnapshots.push(snapshot); });
  fixture.controller->start();

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  const auto scanResult = fixture.controller->scanLibrary(roots, scanner::ScanMode::Full);

  CHECK(scanResult.accepted);
  CHECK(fixture.fakeScanner->scanCalls() == 1U);
  REQUIRE(fixture.fakeScanner->lastScannedRoots().has_value());
  CHECK(fixture.fakeScanner->lastScannedRoots()->front().path == std::filesystem::path{"music"});
  REQUIRE(fixture.fakeScanner->lastScanMode().has_value());
  CHECK(*fixture.fakeScanner->lastScanMode() == scanner::ScanMode::Full);

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 33), 2));
  CHECK(librarySnapshots.count() == 1U);
  fixture.controller->drainForTests();

  REQUIRE(librarySnapshots.count() >= 2U);
  CHECK(librarySnapshots.last().version == 33U);
  REQUIRE(librarySnapshots.last().libraryTree.has_value());
  CHECK(librarySnapshots.last().libraryTree->version == 33U);
}

TEST_CASE("media controller facade starts metadata and updates after committed player snapshot") {
  ControllerFixture fixture{};
  fixture.controller->start();

  CHECK(fixture.fakeMetadata->registerCommandCallbackCalls() == 1U);
  CHECK(fixture.fakeMetadata->startCalls() == 1U);
  REQUIRE(fixture.fakeMetadata->lastStartedState().has_value());
  CHECK(fixture.fakeMetadata->lastStartedState()->controlState.playback.state == PlaybackStatus::Stopped);

  installLibrary(fixture, 44, 1);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(fixture.fakeMetadata->updateCalls() >= 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState()->controlState.currentTrack.has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.currentTrack->trackId == "a");
}

TEST_CASE("media controller facade posts metadata commands onto the control executor") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Play));

  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  CHECK(fixture.fakeAudio->playCalls() == 1U);
}

TEST_CASE("media controller facade routes metadata pause like direct queued commands") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Pause));

  CHECK(fixture.fakeAudio->pauseCalls() == 0U);
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Paused);
}

TEST_CASE("media controller facade shutdown unregisters callbacks and drops late events") {
  ControllerFixture fixture{};
  control_test::PlayerStateSnapshotCollector playerSnapshots{};
  control_test::LibraryStateSnapshotCollector librarySnapshots{};
  std::size_t notificationDeliveries{0};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](const PlayerStateSnapshot& snapshot) { playerSnapshots.push(snapshot); });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](const LibraryStateSnapshot& snapshot) { librarySnapshots.push(snapshot); });
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification&) { ++notificationDeliveries; });
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  const auto playerSnapshotBeforeShutdown = fixture.controller->playerStateSnapshot();
  const auto librarySnapshotBeforeShutdown = fixture.controller->libraryStateSnapshot();
  const auto playerDeliveriesBeforeShutdown = playerSnapshots.count();
  const auto libraryDeliveriesBeforeShutdown = librarySnapshots.count();
  const auto notificationDeliveriesBeforeShutdown = notificationDeliveries;
  const auto updateCallsBeforeShutdown = fixture.fakeMetadata->updateCalls();

  playerSubscription.unsubscribe();
  librarySubscription.unsubscribe();
  notificationSubscription.unsubscribe();
  fixture.controller->shutdown();

  CHECK(fixture.fakeAudio->setEventSinkCalls() == 2U);
  CHECK(fixture.fakeScanner->setEventSinkCalls() == 2U);
  CHECK(fixture.fakeMetadata->commandUnregistrations() == 1U);
  CHECK_FALSE(fixture.fakeMetadata->hasCommandCallback());
  CHECK(fixture.fakeMetadata->stopCalls() == 1U);

  fixture.fakeMetadata->emitCommand(command(MediaControlCommandKind::Pause));
  fixture.fakeAudio->emit(audioTrackChangedEvent("late-audio", "music/late.flac", 99));
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("late", "music/late.flac")}, 99), 99));
  fixture.controller->drainForTests();

  CHECK(fixture.fakeAudio->pauseCalls() == 0U);
  CHECK(fixture.fakeMetadata->updateCalls() == updateCallsBeforeShutdown);
  CHECK(playerSnapshots.count() == playerDeliveriesBeforeShutdown);
  CHECK(librarySnapshots.count() == libraryDeliveriesBeforeShutdown);
  CHECK(notificationDeliveries == notificationDeliveriesBeforeShutdown);
  CHECK(fixture.controller->playerStateSnapshot().freshness.version == playerSnapshotBeforeShutdown.freshness.version);
  CHECK(fixture.controller->libraryStateSnapshot().version == librarySnapshotBeforeShutdown.version);
}

TEST_CASE("media controller facade contains subscriber exceptions and updates metadata") {
  ControllerFixture fixture{};
  control_test::PlayerStateSnapshotCollector laterPlayerSnapshots{};
  control_test::LibraryStateSnapshotCollector laterLibrarySnapshots{};
  std::size_t laterNotifications{0};
  fixture.controller->subscribePlayerState([](const PlayerStateSnapshot&) { throw std::runtime_error{"player subscriber"}; });
  fixture.controller->subscribePlayerState([&](const PlayerStateSnapshot& snapshot) { laterPlayerSnapshots.push(snapshot); });
  fixture.controller->subscribeLibraryState([](const LibraryStateSnapshot&) { throw std::runtime_error{"library subscriber"}; });
  fixture.controller->subscribeLibraryState([&](const LibraryStateSnapshot& snapshot) { laterLibrarySnapshots.push(snapshot); });
  fixture.controller->subscribeDomainNotifications([](const ControlDomainNotification&) { throw std::runtime_error{"notification subscriber"}; });
  fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification&) { ++laterNotifications; });
  fixture.controller->start();

  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(laterLibrarySnapshots.count() >= 2U);
  CHECK(laterPlayerSnapshots.count() >= 2U);
  CHECK(laterNotifications >= 1U);
  CHECK(fixture.fakeMetadata->updateCalls() >= 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller facade subscribers receive committed snapshots not raw sink payloads") {
  ControllerFixture fixture{};
  control_test::PlayerStateSnapshotCollector playerSnapshots{};
  control_test::LibraryStateSnapshotCollector librarySnapshots{};
  fixture.controller->subscribePlayerState([&](const PlayerStateSnapshot& snapshot) { playerSnapshots.push(snapshot); });
  fixture.controller->subscribeLibraryState([&](const LibraryStateSnapshot& snapshot) { librarySnapshots.push(snapshot); });
  fixture.controller->start();

  fixture.fakeScanner->emit(scanStartedEvent(1));
  CHECK(librarySnapshots.count() == 1U);
  fixture.controller->drainForTests();

  fixture.fakeAudio->emit(audioTrackChangedEvent("sink-track", "music/sink.flac", 1));
  CHECK(playerSnapshots.count() == 1U);
  fixture.controller->drainForTests();

  REQUIRE(librarySnapshots.count() >= 2U);
  CHECK(librarySnapshots.last().scanStatus == LibraryScanStatus::Scanning);
  CHECK_FALSE(librarySnapshots.last().libraryTree.has_value());
  REQUIRE(playerSnapshots.count() >= 2U);
  REQUIRE(playerSnapshots.last().currentTrack.has_value());
  CHECK(playerSnapshots.last().currentTrack->trackId == "sink-track");
  CHECK(playerSnapshots.last().timeline.duration == std::chrono::milliseconds{3000});
}
