#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
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
                               .logicalTrackId = {},
                               .artworkPath = std::nullopt};
}

scanner::SongMetadata songWithArtwork(std::string id, std::string path, std::filesystem::path artworkPath) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.artworkPath = std::move(artworkPath);
  metadata.contentHash = "artwork-hash";
  return metadata;
}

scanner::SongMetadata songWithDisplayMetadata(std::string id,
                                              std::string path,
                                              std::string title,
                                              std::string artist,
                                              std::string album,
                                              std::string albumArtist,
                                              std::string genre) {
  auto metadata = song(std::move(id), std::move(path));
  metadata.title = std::move(title);
  metadata.artist = std::move(artist);
  metadata.album = std::move(album);
  metadata.albumArtist = std::move(albumArtist);
  metadata.genre = std::move(genre);
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

audio::BackendEvent audioPositionUpdatedEvent(std::string id, std::chrono::milliseconds position, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackPositionUpdated,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackPositionUpdated{.clock = audio::PlaybackClockSnapshot{.trackId = std::move(id),
                                                                                                               .position = position,
                                                                                                               .sampledAt = {},
                                                                                                               .version = version,
                                                                                                               .continuous = true}}};
}

audio::BackendEvent audioPlaybackStateChangedEvent(audio::PlaybackState state, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackStateChanged,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackStateChanged{.state = state}};
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

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

bool hasNotification(const std::vector<ControlDomainNotification>& notifications,
                     ControlDomainNotificationKind kind,
                     MediaControllerErrorCode errorCode) {
  return std::ranges::any_of(notifications, [kind, errorCode](const ControlDomainNotification& notification) {
    return notification.kind == kind && notification.errorCode == errorCode;
  });
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
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationsSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK_FALSE(playResult.accepted);
  CHECK(playResult.code == MediaControllerErrorCode::NoPlayableTrack);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::NoPlayableTrack);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::NoPlayableTrack));
  }

  installLibrary(fixture);
  auto invalidSelect = command(MediaControlCommandKind::SelectTrack);
  invalidSelect.track = track("missing", "music/missing.flac");
  const auto selectResult = fixture.controller->submitCommand(invalidSelect);

  CHECK_FALSE(selectResult.accepted);
  CHECK(selectResult.code == MediaControllerErrorCode::TrackNotInLibrary);
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::TrackNotInLibrary);
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(hasNotification(notifications, ControlDomainNotificationKind::CommandRejected, MediaControllerErrorCode::TrackNotInLibrary));
  }
  notificationsSubscription.unsubscribe();
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

TEST_CASE("media controller keeps visible playback state stable during seek while playing") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  auto seek = command(MediaControlCommandKind::SeekTo);
  seek.position = std::chrono::milliseconds{1500};
  REQUIRE(fixture.controller->submitCommand(seek).accepted);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);

  fixture.fakeAudio->emit(audioPlaybackStateChangedEvent(audio::PlaybackState::Loading, 40));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);

  fixture.fakeAudio->emit(audioPlaybackStateChangedEvent(audio::PlaybackState::Playing, 41));
  fixture.controller->drainForTests();
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
}

TEST_CASE("media controller facade applies skip repeat and playback-ended policies through fake audio") {
  ControllerFixture fixture{};
  std::atomic_size_t playbackEndedNotifications{0};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    if (notification.kind == ControlDomainNotificationKind::PlaybackEnded) {
      playbackEndedNotifications.fetch_add(1U);
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

  CHECK(waitUntil([&] { return playbackEndedNotifications.load() == 1U; }));
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  notificationSubscription.unsubscribe();
}

TEST_CASE("media controller propagates scanner artwork to player snapshot metadata") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithArtwork("a", "music/a.flac", "/tmp/seriona-cover-a.png")}, 21), 21));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.artwork.has_value());
  CHECK(snapshot.artwork->localPath == std::filesystem::path{"/tmp/seriona-cover-a.png"});
  CHECK(snapshot.artwork->contentHash == std::string{"artwork-hash"});
}

TEST_CASE("media controller preserves scanner display metadata for platform snapshots") {
  ControllerFixture fixture{};
  fixture.controller->start();
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("riot",
                                                                                      "music/R・I・O・T.flac",
                                                                                      "R·I·O·T",
                                                                                      "RAISE A SUILEN",
                                                                                      "R・I・O・T",
                                                                                      "RAISE A SUILEN",
                                                                                      "Rock")},
                                                       22),
                                               22));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->title == "R·I·O·T");
  CHECK(snapshot.display->artist == "RAISE A SUILEN");
  CHECK(snapshot.display->album == "R・I・O・T");
  CHECK(snapshot.display->albumArtist == "RAISE A SUILEN");
  CHECK(snapshot.display->genre == "Rock");

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "R・I・O・T");
}

TEST_CASE("media controller does not replace scanner album with parent directory name") {
  ControllerFixture fixture{};
  fixture.controller->start();
  constexpr auto kFolderAlbum = "[M3-44] ARForest - The Unfinished [FLAC]";
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("arforest-01",
                                                                                      std::string{"music/"} + kFolderAlbum + "/01 - Abandoned Creation.flac",
                                                                                      "Abandoned Creation",
                                                                                      "ARForest",
                                                                                      "The Unfinished",
                                                                                      "ARForest",
                                                                                      "Soundtrack")},
                                                       23),
                                               23));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  const auto snapshot = fixture.controller->playerStateSnapshot();

  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->album == "The Unfinished");
  CHECK(snapshot.display->album != kFolderAlbum);

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "The Unfinished");
  CHECK(publishedSnapshot.display->album != kFolderAlbum);
}

TEST_CASE("media controller preserves scanner display metadata after audio track changed event") {
  ControllerFixture fixture{};
  fixture.controller->start();
  constexpr auto kFolderAlbum = "[M3-44] ARForest - The Unfinished [FLAC]";
  const auto path = std::string{"music/"} + kFolderAlbum + "/01 - Abandoned Creation.flac";
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({songWithDisplayMetadata("arforest-01",
                                                                                      path,
                                                                                      "Abandoned Creation",
                                                                                      "ARForest",
                                                                                      "The Unfinished",
                                                                                      "ARForest",
                                                                                      "Soundtrack")},
                                                       24),
                                               24));
  fixture.controller->drainForTests();

  CHECK(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  fixture.fakeAudio->emit(audioTrackChangedEvent("arforest-01", path, 25));
  fixture.controller->drainForTests();

  const auto snapshot = fixture.controller->playerStateSnapshot();
  REQUIRE(snapshot.display.has_value());
  CHECK(snapshot.display->title == "Abandoned Creation");
  CHECK(snapshot.display->artist == "ARForest");
  CHECK(snapshot.display->album == "The Unfinished");
  CHECK(snapshot.display->album != kFolderAlbum);

  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  const auto& publishedSnapshot = fixture.fakeMetadata->lastUpdatedState()->controlState;
  REQUIRE(publishedSnapshot.display.has_value());
  CHECK(publishedSnapshot.display->album == "The Unfinished");
  CHECK(publishedSnapshot.display->album != kFolderAlbum);
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
  std::mutex librarySnapshotMutex{};
  std::vector<LibraryStateSnapshot> librarySnapshots{};
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot snapshot) {
    std::lock_guard lock{librarySnapshotMutex};
    librarySnapshots.push_back(std::move(snapshot));
  });
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
  fixture.controller->drainForTests();

  REQUIRE(waitUntil([&] {
    std::lock_guard lock{librarySnapshotMutex};
    return librarySnapshots.size() >= 2U;
  }));
  {
    std::lock_guard lock{librarySnapshotMutex};
    CHECK(librarySnapshots.back().version == 33U);
    REQUIRE(librarySnapshots.back().libraryTree.has_value());
    CHECK(librarySnapshots.back().libraryTree->version == 33U);
  }
  librarySubscription.unsubscribe();
}

TEST_CASE("media controller facade does not run scanner work on the control executor") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  fixture.fakeScanner->blockScansUntilReleased();
  fixture.controller->start();
  installLibrary(fixture);
  for (auto attempts = 0; attempts < 100 && !fixture.controller->libraryStateSnapshot().libraryTree.has_value(); ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.controller->libraryStateSnapshot().libraryTree.has_value());
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  for (auto attempts = 0; attempts < 100 && fixture.fakeAudio->playCalls() == 0U; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(fixture.fakeAudio->playCalls() == 1U);

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  auto scanResult = std::async(std::launch::async, [&] {
    return fixture.controller->scanLibrary(roots, scanner::ScanMode::Full);
  });
  REQUIRE(fixture.fakeScanner->waitForBlockedScan(std::chrono::seconds{1}));

  fixture.fakeAudio->emit(audioPositionUpdatedEvent("a", std::chrono::milliseconds{1250}, 8));
  auto pauseResult = std::async(std::launch::async, [&] {
    return fixture.controller->submitCommand(command(MediaControlCommandKind::Pause));
  });

  REQUIRE(scanResult.wait_for(std::chrono::seconds{1}) == std::future_status::timeout);
  REQUIRE(pauseResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(pauseResult.get().accepted);
  for (auto attempts = 0; attempts < 100 && fixture.controller->playerStateSnapshot().timeline.position != std::chrono::milliseconds{1250}; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK(fixture.fakeAudio->pauseCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);
  CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{1250});

  fixture.fakeScanner->releaseBlockedScans();
  REQUIRE(scanResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(scanResult.get().accepted);
}

TEST_CASE("media controller facade exposes first scanned track while stopped") {
  ControllerFixture fixture{};
  std::promise<PlayerStateSnapshot> publishedTrackSnapshot{};
  auto trackSnapshot = publishedTrackSnapshot.get_future();
  std::atomic_bool trackSnapshotCaptured{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot snapshot) {
    if (snapshot.currentTrack.has_value() && !trackSnapshotCaptured.exchange(true)) {
      publishedTrackSnapshot.set_value(std::move(snapshot));
    }
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac")}, 33), 2));
  fixture.controller->drainForTests();

  const auto player = fixture.controller->playerStateSnapshot();
  REQUIRE(player.currentTrack.has_value());
  CHECK(player.currentTrack->trackId == "a");
  CHECK(player.playback.state == PlaybackStatus::Stopped);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
  CHECK(fixture.fakeAudio->playCalls() == 0U);
  REQUIRE(trackSnapshot.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto publishedSnapshot = trackSnapshot.get();
  REQUIRE(publishedSnapshot.currentTrack.has_value());
  CHECK(publishedSnapshot.currentTrack->trackId == "a");

  const auto toggleResult = fixture.controller->submitCommand(command(MediaControlCommandKind::TogglePlayPause));

  CHECK(toggleResult.accepted);
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  CHECK(fixture.fakeAudio->playCalls() == 1U);
  playerSubscription.unsubscribe();
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
  std::mutex notificationMutex{};
  std::vector<ControlDomainNotification> notifications{};
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification notification) {
    std::lock_guard lock{notificationMutex};
    notifications.push_back(std::move(notification));
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scannerErrorEvent("metadata read failed", 5));
  fixture.controller->drainForTests();

  const auto librarySnapshot = fixture.controller->libraryStateSnapshot();
  CHECK(librarySnapshot.version == 5U);
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Error);
  REQUIRE(librarySnapshot.lastError.has_value());
  CHECK(librarySnapshot.lastError->message == "metadata read failed");
  REQUIRE(waitUntil([&] {
    std::lock_guard lock{notificationMutex};
    return !notifications.empty();
  }));
  {
    std::lock_guard lock{notificationMutex};
    CHECK(notifications.back().kind == ControlDomainNotificationKind::LibraryScanError);
    CHECK(notifications.back().errorCode == MediaControllerErrorCode::BackendRejected);
    CHECK(notifications.back().scanStatus == LibraryScanStatus::Error);
  }
  notificationSubscription.unsubscribe();
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
  std::atomic_size_t laterPlayerSnapshots{0};
  std::atomic_size_t laterLibrarySnapshots{0};
  std::atomic_size_t laterNotifications{0};
  auto throwingPlayerSubscription = fixture.controller->subscribePlayerState([](PlayerStateSnapshot) { throw std::runtime_error{"player subscriber"}; });
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot) { laterPlayerSnapshots.fetch_add(1U); });
  auto throwingLibrarySubscription = fixture.controller->subscribeLibraryState([](LibraryStateSnapshot) { throw std::runtime_error{"library subscriber"}; });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot) { laterLibrarySnapshots.fetch_add(1U); });
  auto throwingNotificationSubscription = fixture.controller->subscribeDomainNotifications([](ControlDomainNotification) { throw std::runtime_error{"notification subscriber"}; });
  auto notificationSubscription = fixture.controller->subscribeDomainNotifications([&](ControlDomainNotification) { laterNotifications.fetch_add(1U); });
  fixture.controller->start();

  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));

  CHECK(waitUntil([&] { return laterLibrarySnapshots.load() >= 2U; }));
  CHECK(waitUntil([&] { return laterPlayerSnapshots.load() >= 2U; }));
  CHECK(waitUntil([&] { return laterNotifications.load() >= 1U; }));
  CHECK(fixture.fakeMetadata->updateCalls() >= 1U);
  REQUIRE(fixture.fakeMetadata->lastUpdatedState().has_value());
  CHECK(fixture.fakeMetadata->lastUpdatedState()->controlState.playback.state == PlaybackStatus::Playing);
  throwingPlayerSubscription.unsubscribe();
  playerSubscription.unsubscribe();
  throwingLibrarySubscription.unsubscribe();
  librarySubscription.unsubscribe();
  throwingNotificationSubscription.unsubscribe();
  notificationSubscription.unsubscribe();
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

TEST_CASE("media controller facade slow snapshot subscribers do not starve control work") {
  ControllerFixture fixture{MediaControllerOptions{.runInlineForTests = false}};
  std::promise<void> subscriberEntered{};
  auto subscriberIsBlocked = subscriberEntered.get_future();
  std::promise<void> releaseSubscriber{};
  auto releaseSignal = releaseSubscriber.get_future().share();
  std::promise<void> subscriberExited{};
  auto subscriberIsReleased = subscriberExited.get_future();
  std::atomic_bool enteredOnce{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot) mutable {
    if (enteredOnce.exchange(true)) {
      return;
    }
    subscriberEntered.set_value();
    releaseSignal.wait();
    subscriberExited.set_value();
  });
  fixture.controller->start();

  installLibrary(fixture);
  fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
  REQUIRE(subscriberIsBlocked.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

  auto pauseResult = std::async(std::launch::async, [&] {
    return fixture.controller->submitCommand(command(MediaControlCommandKind::Pause));
  });

  REQUIRE(pauseResult.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  CHECK(pauseResult.get().accepted);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);

  releaseSubscriber.set_value();
  REQUIRE(subscriberIsReleased.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  playerSubscription.unsubscribe();
}

TEST_CASE("media controller facade subscribers receive committed snapshots not raw sink payloads") {
  ControllerFixture fixture{};
  std::promise<PlayerStateSnapshot> committedPlayerSnapshot{};
  auto committedPlayer = committedPlayerSnapshot.get_future();
  std::atomic_bool playerSnapshotCaptured{false};
  std::promise<LibraryStateSnapshot> committedLibrarySnapshot{};
  auto committedLibrary = committedLibrarySnapshot.get_future();
  std::atomic_bool librarySnapshotCaptured{false};
  auto playerSubscription = fixture.controller->subscribePlayerState([&](PlayerStateSnapshot snapshot) {
    if (snapshot.currentTrack.has_value() && !playerSnapshotCaptured.exchange(true)) {
      committedPlayerSnapshot.set_value(std::move(snapshot));
    }
  });
  auto librarySubscription = fixture.controller->subscribeLibraryState([&](LibraryStateSnapshot snapshot) {
    if (snapshot.scanStatus == LibraryScanStatus::Scanning && !librarySnapshotCaptured.exchange(true)) {
      committedLibrarySnapshot.set_value(std::move(snapshot));
    }
  });
  fixture.controller->start();

  fixture.fakeScanner->emit(scanStartedEvent(1));
  fixture.controller->drainForTests();

  fixture.fakeAudio->emit(audioTrackChangedEvent("sink-track", "music/sink.flac", 1));
  fixture.controller->drainForTests();

  REQUIRE(committedLibrary.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto librarySnapshot = committedLibrary.get();
  CHECK(librarySnapshot.scanStatus == LibraryScanStatus::Scanning);
  CHECK_FALSE(librarySnapshot.libraryTree.has_value());
  REQUIRE(committedPlayer.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  const auto playerSnapshot = committedPlayer.get();
  REQUIRE(playerSnapshot.currentTrack.has_value());
  CHECK(playerSnapshot.currentTrack->trackId == "sink-track");
  CHECK(playerSnapshot.timeline.duration == std::chrono::milliseconds{3000});
  playerSubscription.unsubscribe();
  librarySubscription.unsubscribe();
}
