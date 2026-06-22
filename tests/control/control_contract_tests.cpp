#include "control_test_harness.h"

#include <doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;
namespace metadata = seriona::metadata;

namespace {

template <typename T>
void expectValueSemantics() {
  CHECK(std::is_default_constructible_v<T>);
  CHECK(std::is_copy_constructible_v<T>);
  CHECK(std::is_copy_assignable_v<T>);
  CHECK(std::is_move_constructible_v<T>);
  CHECK(std::is_move_assignable_v<T>);
}

std::filesystem::path repoRoot() {
  auto sourcePath = std::filesystem::path{__FILE__}.lexically_normal();
  return sourcePath.parent_path().parent_path().parent_path();
}

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

}

TEST_CASE("control public contracts remain value types") {
  expectValueSemantics<TrackIdentity>();
  expectValueSemantics<DisplayMetadata>();
  expectValueSemantics<ArtworkRef>();
  expectValueSemantics<PlaybackTimeline>();
  expectValueSemantics<PlaybackSnapshot>();
  expectValueSemantics<SnapshotFreshness>();
  expectValueSemantics<PlaybackCapabilities>();
  expectValueSemantics<PlayerStateSnapshot>();
  expectValueSemantics<LibraryStateSnapshot>();
  expectValueSemantics<ControlDomainNotification>();
  expectValueSemantics<MediaControllerCommandResult>();
  expectValueSemantics<MediaControllerOptions>();
  expectValueSemantics<MediaControlCommand>();
  expectValueSemantics<SubscriptionHandle>();
}

TEST_CASE("control test harness records fake playback scanner and metadata activity") {
  test::DeterministicClock clock{};
  test::FakeAudioPlaybackService fakeAudio{};
  test::FakeFileScannerService fakeScanner{};
  test::FakeMetadataSharingService fakeMetadata{};
  test::AudioBackendEventCollector audioEvents{};
  test::ScannerEventCollector scannerEvents{};
  test::PlayerStateSnapshotCollector playerSnapshots{};
  test::LibraryStateSnapshotCollector librarySnapshots{};

  fakeAudio.setEventSink([&](audio::BackendEvent event) { audioEvents.push(std::move(event)); });
  fakeScanner.setEventSink([&](scanner::ScannerEvent event) { scannerEvents.push(std::move(event)); });

  const audio::TrackPlaybackRequest request{.trackId = "track-01",
                                            .filePath = std::filesystem::path{"music/track-01.flac"},
                                            .title = "Track 01",
                                            .artist = "Artist",
                                            .offset = std::nullopt,
                                            .duration = std::nullopt,
                                            .sampleRate = std::nullopt,
                                            .bitDepth = std::nullopt,
                                            .channels = std::nullopt,
                                            .format = std::nullopt};
  fakeAudio.loadTrack(request);
  fakeAudio.play();
  fakeAudio.pause();
  fakeAudio.seek(std::chrono::milliseconds{1234});
  CHECK(fakeAudio.loadTrackCalls() == 1U);
  CHECK(fakeAudio.playCalls() == 1U);
  CHECK(fakeAudio.pauseCalls() == 1U);
  CHECK(fakeAudio.seekCalls() == 1U);
  CHECK(fakeAudio.lastLoadedTrack().has_value());
  CHECK(fakeAudio.lastSeekPosition().value() == std::chrono::milliseconds{1234});

  const std::vector<scanner::ScannerRoot> roots{{.path = std::filesystem::path{"music"}, .recursive = true}};
  fakeScanner.scan(roots, scanner::ScanMode::Full);
  fakeScanner.startWatching(roots);
  CHECK(fakeScanner.scanCalls() == 1U);
  CHECK(fakeScanner.startWatchingCalls() == 1U);
  CHECK(fakeScanner.lastScannedRoots().has_value());
  CHECK(fakeScanner.lastWatchingRoots().has_value());

  const auto playingSnapshot = test::makePlayerStateSnapshot(clock, 1U, PlaybackStatus::Playing, std::chrono::milliseconds{0});
  clock.advance(std::chrono::milliseconds{250});
  const auto pausedSnapshot = test::makePlayerStateSnapshot(clock, 2U, PlaybackStatus::Paused, std::chrono::milliseconds{250});
  const auto librarySnapshot = test::makeLibraryStateSnapshot(7U, LibraryScanStatus::Scanning);
  playerSnapshots.push(playingSnapshot);
  playerSnapshots.push(pausedSnapshot);
  librarySnapshots.push(librarySnapshot);

  CHECK(playerSnapshots.count() == 2U);
  CHECK(playerSnapshots.last().freshness.version == 2U);
  CHECK(playerSnapshots.last().playback.state == PlaybackStatus::Paused);
  CHECK(playerSnapshots.last().freshness.sampledAt == clock.now());
  CHECK(librarySnapshots.count() == 1U);
  CHECK(librarySnapshots.last().version == 7U);
  CHECK(librarySnapshots.last().scanStatus == LibraryScanStatus::Scanning);

  fakeMetadata.setBackendKind(metadata::MetadataBackendKind::Noop);
  fakeMetadata.setCapabilities(metadata::MetadataBackendCapabilities{.canPublishMetadata = false,
                                                                      .canPublishTimeline = false,
                                                                      .canReceiveCommands = true,
                                                                      .requiresPlatformExtension = false,
                                                                      .hasPlatformExtension = false});

  const metadata::PlatformMediaState platformState{.controlState = pausedSnapshot,
                                                    .timelineUpdateInterval = std::chrono::milliseconds{1000}};
  fakeMetadata.setStartResult(metadata::MetadataSyncResult{.accepted = true,
                                                           .changed = true,
                                                           .state = platformState,
                                                           .errorCode = std::nullopt,
                                                           .message = {}});
  fakeMetadata.setUpdateResult(metadata::MetadataSyncResult{.accepted = true,
                                                            .changed = false,
                                                            .state = platformState,
                                                            .errorCode = std::nullopt,
                                                            .message = {}});
  fakeMetadata.setStopResult(metadata::MetadataSyncResult{.accepted = true,
                                                          .changed = false,
                                                          .state = platformState,
                                                          .errorCode = std::nullopt,
                                                          .message = {}});

  CHECK(fakeMetadata.start(platformState).accepted);
  CHECK(fakeMetadata.update(platformState).accepted);
  CHECK(fakeMetadata.stop().accepted);
  CHECK(fakeMetadata.startCalls() == 1U);
  CHECK(fakeMetadata.updateCalls() == 1U);
  CHECK(fakeMetadata.stopCalls() == 1U);
  CHECK(fakeMetadata.lastStartedState().has_value());
  CHECK(fakeMetadata.lastUpdatedState().has_value());
  CHECK(fakeMetadata.lastStopResult().has_value());
}

TEST_CASE("subscription handle unsubscribe detaches callback exactly once") {
  test::FakeMetadataSharingService fakeMetadata{};
  test::MediaControlCommandCollector commands{};

  const auto handle = fakeMetadata.registerCommandCallback([&](const MediaControlCommand& command) { commands.push(command); });
  REQUIRE(handle.unsubscribe);
  CHECK(handle.subscriptionId != 0U);
  CHECK(fakeMetadata.registerCommandCallbackCalls() == 1U);
  CHECK(fakeMetadata.commandRegistrations() == 1U);
  CHECK(fakeMetadata.hasCommandCallback());

  fakeMetadata.emitCommand(test::makePlayCommand());
  CHECK(commands.count() == 1U);

  handle.unsubscribe();
  handle.unsubscribe();
  CHECK(fakeMetadata.commandUnregistrations() == 1U);
  CHECK_FALSE(fakeMetadata.hasCommandCallback());

  fakeMetadata.emitCommand(test::makePauseCommand());
  CHECK(commands.count() == 1U);
}

TEST_CASE("public control headers avoid platform-only tokens") {
  const auto root = repoRoot();
  const auto controlHeader = readTextFile(root / "inc/seriona/control/control_contracts.h");
  const auto metadataHeader = readTextFile(root / "inc/seriona/metadata/metadata_contracts.h");
  const std::array<std::string, 8> forbiddenTokens{"DBus", "SMTC", "MPRIS", "miniaudio", "SQLite", "TagReader", "Qt", "QML"};

  for (const auto& token : forbiddenTokens) {
    CHECK(controlHeader.find(token) == std::string::npos);
    CHECK(metadataHeader.find(token) == std::string::npos);
  }
}
