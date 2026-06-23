#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
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

scanner::ScannerEvent scannerErrorEvent(std::string message, std::uint64_t version) {
  return scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanError,
                               .monotonicVersion = version,
                               .timestamp = {},
                               .payload = scanner::ScannerError{.code = scanner::ScannerErrorCode::MetadataReadFailed,
                                                                .message = std::move(message),
                                                                .detail = {},
                                                                .path = std::nullopt}};
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

audio::BackendEvent audioPlaybackEndedEvent(std::string id, std::string path, std::uint64_t version,
                                            std::chrono::milliseconds position = std::chrono::milliseconds{3000}) {
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
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackEnded,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackEnded{.request = request,
                                                             .finalClock = audio::PlaybackClockSnapshot{.trackId = request.trackId,
                                                                                                         .position = position,
                                                                                                         .sampledAt = {},
                                                                                                         .version = version,
                                                                                                         .continuous = false}}};
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

  explicit ControllerFixture(MediaControllerOptions options) {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    fakeMetadata = metadataService.get();
    controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                 .scanner = fakeScanner,
                                                                 .metadata = std::move(metadataService)},
                                     options);
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

TEST_CASE("media controller facade rejects unplayable commands and publishes command notifications") {
  ControllerFixture fixture{};
  std::vector<ControlDomainNotification> notifications{};
  fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification& notification) {
    notifications.push_back(notification);
  });
  fixture.controller->start();

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK_FALSE(playResult.accepted);
  CHECK(playResult.code == MediaControllerErrorCode::NoPlayableTrack);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  REQUIRE_FALSE(notifications.empty());
  CHECK(notifications.back().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(notifications.back().errorCode == MediaControllerErrorCode::NoPlayableTrack);

  installLibrary(fixture);
  auto invalidSelect = command(MediaControlCommandKind::SelectTrack);
  invalidSelect.track = track("missing", "music/missing.flac");
  const auto selectResult = fixture.controller->submitCommand(invalidSelect);

  CHECK_FALSE(selectResult.accepted);
  CHECK(selectResult.code == MediaControllerErrorCode::TrackNotInLibrary);
  REQUIRE_FALSE(notifications.empty());
  CHECK(notifications.back().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(notifications.back().errorCode == MediaControllerErrorCode::TrackNotInLibrary);
}

TEST_CASE("media controller facade forwards seek volume mute and toggle commands through fake audio") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto seekPastDuration = command(MediaControlCommandKind::SeekTo);
  seekPastDuration.position = std::chrono::milliseconds{4500};
  const auto seekPastResult = fixture.controller->submitCommand(seekPastDuration);
  CHECK(seekPastResult.accepted);
  CHECK(fixture.fakeAudio->seekCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
  CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{3000});
  CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{3000});

  auto seekBeforeStart = command(MediaControlCommandKind::SeekBy);
  seekBeforeStart.delta = std::chrono::milliseconds{-5000};
  const auto seekBeforeResult = fixture.controller->submitCommand(seekBeforeStart);
  CHECK(seekBeforeResult.accepted);
  CHECK(fixture.fakeAudio->seekCalls() == 2U);
  REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
  CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{0});

  auto setVolume = command(MediaControlCommandKind::SetVolume);
  setVolume.volume = 1.75F;
  const auto volumeResult = fixture.controller->submitCommand(setVolume);
  CHECK(volumeResult.accepted);
  CHECK(fixture.fakeAudio->setVolumeCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastVolume().has_value());
  CHECK(*fixture.fakeAudio->lastVolume() == 1.0F);
  CHECK(fixture.controller->playerStateSnapshot().volume == 1.0F);

  auto setMuted = command(MediaControlCommandKind::SetMuted);
  setMuted.muted = true;
  const auto mutedResult = fixture.controller->submitCommand(setMuted);
  CHECK(mutedResult.accepted);
  CHECK(fixture.fakeAudio->setMutedCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastMuted().has_value());
  CHECK(*fixture.fakeAudio->lastMuted());
  CHECK(fixture.controller->playerStateSnapshot().muted);

  const auto toggleResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));
  CHECK(toggleResult.accepted);
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);

  const auto resumeResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));
  CHECK(resumeResult.accepted);
  CHECK(fixture.fakeAudio->resumeCalls() == 1U);
  CHECK(fixture.fakeAudio->playCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller facade applies skip repeat and playback-ended policies through fake audio") {
  ControllerFixture fixture{};
  std::size_t playbackEndedNotifications{0};
  fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification& notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      ++playbackEndedNotifications;
    }
  });
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  const auto skipNextResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(skipNextResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");

  auto repeatAll = command(MediaControlCommandKind::SetRepeatMode);
  repeatAll.repeatMode = RepeatMode::All;
  CHECK(fixture.controller->submitCommand(repeatAll).accepted);
  const auto wrapNextResult = fixture.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(wrapNextResult.accepted);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");

  auto repeatOne = command(MediaControlCommandKind::SetRepeatMode);
  repeatOne.repeatMode = RepeatMode::One;
  CHECK(fixture.controller->submitCommand(repeatOne).accepted);
  fixture.fakeAudio->emit(audioPlaybackEndedEvent("a", "music/a.flac", 10));
  fixture.controller->drainForTests();

  CHECK(playbackEndedNotifications == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller facade shuffle produces deterministic selected tracks") {
  ControllerFixture first{};
  ControllerFixture second{};
  first.controller->start();
  second.controller->start();
  installLibrary(first, 20, 1);
  installLibrary(second, 20, 1);
  first.controller->submitCommand(command(MediaControlCommandKind::Play));
  second.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto enableShuffle = command(MediaControlCommandKind::SetShuffle);
  enableShuffle.shuffle = true;
  CHECK(first.controller->submitCommand(enableShuffle).accepted);
  CHECK(second.controller->submitCommand(enableShuffle).accepted);
  const auto firstSkip = first.controller->submitCommand(command(MediaControlCommandKind::SkipNext));
  const auto secondSkip = second.controller->submitCommand(command(MediaControlCommandKind::SkipNext));

  CHECK(firstSkip.accepted);
  CHECK(secondSkip.accepted);
  REQUIRE(first.fakeAudio->lastLoadedTrack().has_value());
  REQUIRE(second.fakeAudio->lastLoadedTrack().has_value());
  CHECK(first.fakeAudio->lastLoadedTrack()->trackId == second.fakeAudio->lastLoadedTrack()->trackId);
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

TEST_CASE("media controller facade exposes first scanned track while stopped") {
  ControllerFixture fixture{};
  control_test::PlayerStateSnapshotCollector playerSnapshots{};
  fixture.controller->subscribePlayerState([&](const PlayerStateSnapshot& snapshot) { playerSnapshots.push(snapshot); });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, 33), 2));
  fixture.controller->drainForTests();

  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  REQUIRE(playerSnapshots.count() >= 2U);
  REQUIRE(playerSnapshots.last().currentTrack.has_value());
  CHECK(playerSnapshots.last().currentTrack->trackId == "a");

  const auto toggleResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));

  CHECK(toggleResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.fakeAudio->playCalls() == 1U);
}

TEST_CASE("media controller facade ignores stale audio and scanner events") {
  ControllerFixture fixture{};
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("new", "music/new.flac")}, 30), 30));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  CHECK(fixture.controller->libraryStateSnapshot().libraryTree->nodes.size() == 2U);
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("old", "music/old.flac")}, 20), 20));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  CHECK(fixture.controller->libraryStateSnapshot().libraryTree->version == 30U);

  fixture.fakeAudio->emit(audioTrackChangedEvent("fresh", "music/fresh.flac", 8));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "fresh");
  fixture.fakeAudio->emit(audioTrackChangedEvent("stale", "music/stale.flac", 7));
  fixture.controller->drainForTests();
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "fresh");
}

TEST_CASE("media controller facade publishes scanner errors as library state and domain notifications") {
  ControllerFixture fixture{};
  std::vector<ControlDomainNotification> notifications{};
  fixture.controller->subscribeDomainNotifications([&](const ControlDomainNotification& notification) {
    notifications.push_back(notification);
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerErrorEvent("metadata read failed", 5));
  fixture.controller->drainForTests();

  const auto librarySnapshot = fixture.controller->libraryStateSnapshot();
  CHECK(librarySnapshot.version == 5U);
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Error);
  REQUIRE(librarySnapshot.lastError.has_value());
  CHECK(librarySnapshot.lastError->message == "metadata read failed");
  REQUIRE_FALSE(notifications.empty());
  CHECK(notifications.back().kind == ControlDomainNotificationKind::LibraryScanError);
  CHECK(notifications.back().errorCode == MediaControllerErrorCode::BackendRejected);
  CHECK(notifications.back().scanStatus == LibraryScanStatus::Error);
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

TEST_CASE("media controller facade clears constructor-installed sinks when destroyed before start") {
  auto fakeAudio = std::make_shared<control_test::FakeAudioPlaybackService>();
  auto fakeScanner = std::make_shared<control_test::FakeFileScannerService>();
  auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
  {
    auto controller = makeMediaController(MediaControllerDependencies{.audio = fakeAudio,
                                                                      .scanner = fakeScanner,
                                                                      .metadata = std::move(metadataService)},
                                         MediaControllerOptions{.runInlineForTests = true});
    CHECK(fakeAudio->setEventSinkCalls() == 1U);
    CHECK(fakeScanner->setEventSinkCalls() == 1U);
  }

  CHECK(fakeAudio->setEventSinkCalls() == 2U);
  CHECK(fakeScanner->setEventSinkCalls() == 2U);

  fakeAudio->emit(audioTrackChangedEvent("late-audio", "music/late.flac", 1));
  fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("late", "music/late.flac")}, 1), 1));
  CHECK(fakeAudio->emitEventCalls() == 1U);
  CHECK(fakeScanner->emitEventCalls() == 1U);
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

TEST_CASE("media controller facade completes dispatch future when queued work throws") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 20), 1));
  for (auto attempts = 0; attempts < 100 && !fixture.controller->libraryStateSnapshot().libraryTree.has_value(); ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  fixture.fakeAudio->loadTrackThrows(std::runtime_error{"load failed"});

  auto commandResult = std::async(std::launch::async, [&] { return fixture.controller->submitCommand(command(MediaControlCommandKind::Play)); });

  REQUIRE(commandResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK_THROWS_WITH_AS(static_cast<void>(commandResult.get()), "load failed", std::runtime_error);
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
