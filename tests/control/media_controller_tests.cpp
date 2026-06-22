#include "../../src/control/control_state_reducer.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace seriona::control;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;

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

scanner::PlaylistTreeSnapshot libraryTree(std::vector<scanner::SongMetadata> songs, std::uint64_t version = 10) {
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

scanner::ScannerEvent scannerSnapshotEvent(scanner::PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion = 1) {
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

TrackIdentity track(std::string id, std::string path) {
  return TrackIdentity{.trackId = std::move(id), .filePath = std::filesystem::path{std::move(path)}, .sourceId = {}, .libraryId = {}};
}

audio::BackendEvent audioStateEvent(audio::PlaybackState state, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackStateChanged,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackStateChanged{.state = state}};
}

audio::BackendEvent playbackEndedEvent(std::string id, std::string path, std::uint64_t version) {
  audio::TrackPlaybackRequest request{.trackId = std::move(id),
                                      .filePath = std::filesystem::path{std::move(path)},
                                      .title = {},
                                      .artist = {},
                                      .offset = std::nullopt,
                                      .duration = std::nullopt,
                                      .sampleRate = std::nullopt,
                                      .bitDepth = std::nullopt,
                                      .channels = std::nullopt,
                                      .format = std::nullopt};
  audio::PlaybackClockSnapshot clock{.trackId = request.trackId, .position = std::chrono::milliseconds{3000}, .sampledAt = {}, .version = version};
  return audio::BackendEvent{.type = audio::BackendEventType::PlaybackEnded,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::PlaybackEnded{.request = request, .finalClock = clock}};
}

void installLibrary(ControlStateReducer& reducer) {
  reducer.reduceScannerEvent(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")})));
}

std::string loadedTrackId(const ControlReduction& reduction) {
  for (const auto& intent : reduction.intents) {
    if (intent.kind == ControlIntentKind::LoadTrack) {
      REQUIRE(intent.track.has_value());
      return intent.track->trackId;
    }
  }
  return {};
}

}

TEST_CASE("state reducer rejects play without library and stays stopped") {
  ControlStateReducer reducer{};

  const auto reduction = reducer.reduceCommand(command(MediaControlCommandKind::Play));

  CHECK_FALSE(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::NoPlayableTrack);
  CHECK(reduction.intents.empty());
  REQUIRE(reduction.notifications.size() == 1U);
  CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Stopped);
}

TEST_CASE("state reducer play selects first playable track") {
  ControlStateReducer reducer{};
  installLibrary(reducer);

  const auto reduction = reducer.reduceCommand(command(MediaControlCommandKind::Play));

  CHECK(reduction.result.accepted);
  REQUIRE(reduction.intents.size() == 2U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::LoadTrack);
  CHECK(reduction.intents[0].track->trackId == "a");
  CHECK(reduction.intents[1].kind == ControlIntentKind::Play);
  REQUIRE(reducer.playerState().currentTrack.has_value());
  CHECK(reducer.playerState().currentTrack->trackId == "a");
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("state reducer validates selected track identity and file path") {
  ControlStateReducer reducer{};
  installLibrary(reducer);

  auto selectInvalid = command(MediaControlCommandKind::SelectTrack);
  selectInvalid.track = track("b", "music/not-b.flac");
  const auto invalid = reducer.reduceCommand(selectInvalid);
  CHECK_FALSE(invalid.result.accepted);
  CHECK(invalid.result.code == MediaControllerErrorCode::TrackNotInLibrary);

  auto selectValid = command(MediaControlCommandKind::SelectTrack);
  selectValid.track = track("b", "music/b.flac");
  const auto valid = reducer.reduceCommand(selectValid);
  CHECK(valid.result.accepted);
  CHECK(loadedTrackId(valid) == "b");
}

TEST_CASE("state reducer skips next and previous in flattened tree order") {
  ControlStateReducer reducer{};
  installLibrary(reducer);
  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("b", "music/b.flac");
  reducer.reduceCommand(select);

  const auto next = reducer.reduceCommand(command(MediaControlCommandKind::SkipNext));
  CHECK(loadedTrackId(next) == "c");
  const auto previous = reducer.reduceCommand(command(MediaControlCommandKind::SkipPrevious));
  CHECK(loadedTrackId(previous) == "b");
}

TEST_CASE("state reducer repeat modes control skip wrapping") {
  ControlStateReducer reducer{};
  installLibrary(reducer);
  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("c", "music/c.flac");
  reducer.reduceCommand(select);

  auto repeatOne = command(MediaControlCommandKind::SetRepeatMode);
  repeatOne.repeatMode = RepeatMode::One;
  reducer.reduceCommand(repeatOne);
  CHECK(loadedTrackId(reducer.reduceCommand(command(MediaControlCommandKind::SkipNext))) == "c");

  auto repeatAll = command(MediaControlCommandKind::SetRepeatMode);
  repeatAll.repeatMode = RepeatMode::All;
  reducer.reduceCommand(repeatAll);
  CHECK(loadedTrackId(reducer.reduceCommand(command(MediaControlCommandKind::SkipNext))) == "a");
}

TEST_CASE("state reducer shuffle uses deterministic seed") {
  ControlStateReducer first{MediaControllerOptions{.runInlineForTests = true, .shuffleSeed = 42}};
  ControlStateReducer second{MediaControllerOptions{.runInlineForTests = true, .shuffleSeed = 42}};
  installLibrary(first);
  installLibrary(second);

  for (auto* reducer : {&first, &second}) {
    auto select = command(MediaControlCommandKind::SelectTrack);
    select.track = track("a", "music/a.flac");
    reducer->reduceCommand(select);
    auto shuffle = command(MediaControlCommandKind::SetShuffle);
    shuffle.shuffle = true;
    reducer->reduceCommand(shuffle);
  }

  CHECK(loadedTrackId(first.reduceCommand(command(MediaControlCommandKind::SkipNext))) ==
        loadedTrackId(second.reduceCommand(command(MediaControlCommandKind::SkipNext))));
}

TEST_CASE("state reducer clamps seek and volume commands") {
  ControlStateReducer reducer{};
  installLibrary(reducer);
  reducer.reduceCommand(command(MediaControlCommandKind::Play));

  auto seekBack = command(MediaControlCommandKind::SeekBy);
  seekBack.delta = std::chrono::milliseconds{-5000};
  auto back = reducer.reduceCommand(seekBack);
  REQUIRE(back.intents.size() == 1U);
  CHECK(back.intents.front().position == std::chrono::milliseconds{0});

  auto seekForward = command(MediaControlCommandKind::SeekBy);
  seekForward.delta = std::chrono::milliseconds{5000};
  auto forward = reducer.reduceCommand(seekForward);
  REQUIRE(forward.intents.size() == 1U);
  CHECK(forward.intents.front().position == std::chrono::milliseconds{3000});

  auto volume = command(MediaControlCommandKind::SetVolume);
  volume.volume = 2.0F;
  auto loud = reducer.reduceCommand(volume);
  CHECK(loud.intents.front().volume == 1.0F);
  volume.volume = -1.0F;
  auto quiet = reducer.reduceCommand(volume);
  CHECK(quiet.intents.front().volume == 0.0F);
}

TEST_CASE("state reducer updates muted state and forwards audio intent") {
  ControlStateReducer reducer{};
  auto muted = command(MediaControlCommandKind::SetMuted);
  muted.muted = true;

  const auto reduction = reducer.reduceCommand(muted);

  CHECK(reducer.playerState().muted);
  REQUIRE(reduction.intents.size() == 1U);
  CHECK(reduction.intents.front().kind == ControlIntentKind::SetMuted);
  CHECK(reduction.intents.front().muted == true);
}

TEST_CASE("state reducer ignores stale audio and scanner event versions") {
  ControlStateReducer reducer{};

  const auto freshAudio = reducer.reduceAudioEvent(audioStateEvent(audio::PlaybackState::Playing, 2));
  const auto staleAudio = reducer.reduceAudioEvent(audioStateEvent(audio::PlaybackState::Paused, 1));
  CHECK(freshAudio.playerStateChanged);
  CHECK_FALSE(staleAudio.playerStateChanged);
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Playing);

  const auto freshScanner = reducer.reduceScannerEvent(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 10), 4));
  const auto staleScanner = reducer.reduceScannerEvent(scannerSnapshotEvent(libraryTree({song("b", "music/b.flac")}, 11), 3));
  CHECK(freshScanner.libraryStateChanged);
  CHECK_FALSE(staleScanner.libraryStateChanged);
  REQUIRE(reducer.libraryState().libraryTree.has_value());
  CHECK(reducer.libraryState().libraryTree->version == 10U);
}

TEST_CASE("state reducer reduces scanner lifecycle and snapshot events") {
  ControlStateReducer reducer{};

  reducer.reduceScannerEvent(scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanStarted, .monotonicVersion = 1});
  CHECK(reducer.libraryState().scanStatus == LibraryScanStatus::Scanning);

  scanner::ScanProgress progress{.filesDiscovered = 2,
                                 .filesScanned = 1,
                                 .filesSkipped = 0,
                                 .errors = 0,
                                 .elapsed = std::chrono::milliseconds{0},
                                 .currentPath = std::nullopt};
  reducer.reduceScannerEvent(scanner::ScannerEvent{.type = scanner::ScannerEventType::ProgressUpdated, .monotonicVersion = 2, .payload = progress});
  REQUIRE(reducer.libraryState().scanProgress.has_value());
  CHECK(reducer.libraryState().scanProgress->filesScanned == 1U);

  reducer.reduceScannerEvent(scannerSnapshotEvent(libraryTree({song("a", "music/a.flac")}, 25), 3));
  REQUIRE(reducer.libraryState().libraryTree.has_value());
  CHECK(reducer.libraryState().version == 25U);

  reducer.reduceScannerEvent(scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanCompleted, .monotonicVersion = 4});
  CHECK(reducer.libraryState().scanStatus == LibraryScanStatus::Completed);
  reducer.reduceScannerEvent(scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanStopped, .monotonicVersion = 5});
  CHECK(reducer.libraryState().scanStatus == LibraryScanStatus::Stopped);

  scanner::ScannerError error{.code = scanner::ScannerErrorCode::MetadataReadFailed,
                              .message = "bad tag",
                              .detail = {},
                              .path = std::nullopt};
  reducer.reduceScannerEvent(scanner::ScannerEvent{.type = scanner::ScannerEventType::ScanError, .monotonicVersion = 6, .payload = error});
  CHECK(reducer.libraryState().scanStatus == LibraryScanStatus::Error);
  REQUIRE(reducer.libraryState().lastError.has_value());
  CHECK(reducer.libraryState().lastError->message == "bad tag");
}

TEST_CASE("state reducer reduces audio playback events") {
  ControlStateReducer reducer{};

  auto request = audio::TrackPlaybackRequest{.trackId = "a",
                                             .filePath = std::filesystem::path{"music/a.flac"},
                                             .title = {},
                                             .artist = {},
                                             .offset = std::nullopt,
                                             .duration = std::chrono::milliseconds{3000},
                                             .sampleRate = std::nullopt,
                                             .bitDepth = std::nullopt,
                                             .channels = std::nullopt,
                                             .format = std::nullopt};
  reducer.reduceAudioEvent(audio::BackendEvent{.type = audio::BackendEventType::TrackChanged,
                                               .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                                               .monotonicVersion = 1,
                                               .payload = audio::TrackChanged{.request = request}});
  REQUIRE(reducer.playerState().currentTrack.has_value());
  CHECK(reducer.playerState().currentTrack->trackId == "a");

  reducer.reduceAudioEvent(audioStateEvent(audio::PlaybackState::Paused, 2));
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Paused);

  audio::PlaybackClockSnapshot clock{.trackId = "a", .position = std::chrono::milliseconds{2000}, .version = 3};
  reducer.reduceAudioEvent(audio::BackendEvent{.type = audio::BackendEventType::PlaybackPositionUpdated,
                                               .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                                               .monotonicVersion = 3,
                                               .payload = audio::PlaybackPositionUpdated{.clock = clock}});
  CHECK(reducer.playerState().timeline.position == std::chrono::milliseconds{2000});

  auto fallback = reducer.reduceAudioEvent(audio::BackendEvent{.type = audio::BackendEventType::OutputModeFallback,
                                                               .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                                                               .monotonicVersion = 4,
                                                               .payload = audio::OutputModeFallback{.reason = "direct unavailable"}});
  REQUIRE(fallback.notifications.size() == 1U);
  CHECK(fallback.notifications.front().kind == ControlDomainNotificationKind::OutputModeFallback);

  auto error = reducer.reduceAudioEvent(audio::BackendEvent{.type = audio::BackendEventType::PlaybackError,
                                                            .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                                                            .monotonicVersion = 5,
                                                            .payload = audio::PlaybackError{.code = audio::PlaybackErrorCode::DecodeFailed,
                                                                                             .message = "decode failed",
                                                                                             .detail = {},
                                                                                             .clock = std::nullopt}});
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Error);
  REQUIRE(error.notifications.size() == 1U);
  CHECK(error.notifications.front().kind == ControlDomainNotificationKind::PlaybackError);
}

TEST_CASE("state reducer handles playback ended with next track or stopped state") {
  ControlStateReducer reducer{};
  installLibrary(reducer);
  auto select = command(MediaControlCommandKind::SelectTrack);
  select.track = track("a", "music/a.flac");
  reducer.reduceCommand(select);

  const auto next = reducer.reduceAudioEvent(playbackEndedEvent("a", "music/a.flac", 1));
  CHECK(loadedTrackId(next) == "b");

  select.track = track("c", "music/c.flac");
  reducer.reduceCommand(select);
  const auto stopped = reducer.reduceAudioEvent(playbackEndedEvent("c", "music/c.flac", 2));
  CHECK(stopped.playerStateChanged);
  CHECK(reducer.playerState().playback.state == PlaybackStatus::Stopped);
}
