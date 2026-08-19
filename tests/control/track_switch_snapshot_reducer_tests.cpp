// ControlStateReducer 切轨（selectTrack）期间的 Loading 快照抑制测试（T6）。
// 需求 4 按钮锁定：selectTrack 后立即发布乐观 Playing 快照，音频层发布
// Loading 时被抑制（快照序列无 Loading 中间态），直到真实 Playing/Stopped/Error
// 解除；真实错误（PlaybackError）必须原样发布，不被抑制。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "control/control_state_reducer.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace seriona::control;
using namespace seriona::scanner;
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;

namespace {

SongMetadata makeSong(std::string id, std::string path) {
  SongMetadata song{};
  song.trackId = std::move(id);
  song.filePath = std::filesystem::path{std::move(path)};
  song.title = "Title " + song.trackId;
  song.artist = "Artist";
  song.contentHash = song.trackId;
  song.duration = std::chrono::milliseconds{600000};
  return song;
}

PlaylistNode trackNode(std::string nodeId, SongMetadata metadata) {
  return PlaylistNode{
      .nodeId = std::move(nodeId),
      .parentNodeId = std::string{"root"},
      .kind = PlaylistNodeKind::Track,
      .displayName = metadata.trackId,
      .song = std::move(metadata),
      .childNodeIds = {},
  };
}

PlaylistNode rootNode(std::vector<std::string> children) {
  return PlaylistNode{
      .nodeId = "root",
      .parentNodeId = std::nullopt,
      .kind = PlaylistNodeKind::Root,
      .displayName = "Library",
      .song = std::nullopt,
      .childNodeIds = std::move(children),
  };
}

PlaylistTreeSnapshot makeLibrary() {
  PlaylistTreeSnapshot tree{};
  tree.version = 1;
  tree.rootNodeId = "root";
  tree.nodes.push_back(rootNode({"song-a", "song-b", "song-c"}));
  tree.nodes.push_back(trackNode("song-a", makeSong("a", "/music/a.flac")));
  tree.nodes.push_back(trackNode("song-b", makeSong("b", "/music/b.flac")));
  tree.nodes.push_back(trackNode("song-c", makeSong("c", "/music/c.flac")));
  return tree;
}

ScannerEvent scannerSnapshotEvent(PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion) {
  return ScannerEvent{
      .type = ScannerEventType::PlaylistSnapshotUpdated,
      .monotonicVersion = eventVersion,
      .timestamp = {},
      .payload = std::move(snapshot),
  };
}

struct ReducerFixture {
  ControlStateReducer reducer{};
  std::uint64_t nextEventVersion{10};

  void installLibrary(PlaylistTreeSnapshot tree = makeLibrary()) {
    reducer.reduceScannerEvent(scannerSnapshotEvent(std::move(tree), 1));
  }

  ControlReduction selectTrack(const char* trackId, const char* filePath) {
    return reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::SelectTrack,
        .track = TrackIdentity{.trackId = trackId, .filePath = filePath},
    });
  }

  ControlReduction reduce(MediaControlCommandKind kind) {
    return reducer.reduceCommand(MediaControlCommand{.kind = kind});
  }

  // 模拟音频后端发布状态事件（reducer 唯一 Loading 来源）。
  audio::BackendEvent stateEvent(audio::PlaybackState state) {
    audio::BackendEvent event{};
    event.type = audio::BackendEventType::PlaybackStateChanged;
    event.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    event.monotonicVersion = nextEventVersion++;
    event.payload = audio::PlaybackStateChanged{.state = state};
    return event;
  }

  // 模拟音频后端加载失败（真实错误路径）。
  audio::BackendEvent errorEvent(audio::PlaybackErrorCode code, const char* message) {
    audio::BackendEvent event{};
    event.type = audio::BackendEventType::PlaybackError;
    event.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    event.monotonicVersion = nextEventVersion++;
    event.payload = audio::PlaybackError{.code = code, .message = message, .detail = "detail", .clock = std::nullopt};
    return event;
  }
};

// 记录每条快照的可见播放状态，断言整条序列不含 Loading。

}  // namespace

TEST_CASE("track switch suppresses Loading snapshot (optimistic Playing until backend confirms)") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));

  // SkipNext → selectTrack(b, startPlayback=true)：乐观 Playing 立即可见。
  const auto switchReduction = fixture.reduce(MediaControlCommandKind::SkipNext);
  REQUIRE(switchReduction.result.accepted);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "b");

  std::vector<PlaybackStatus> sequence{};
  sequence.push_back(fixture.reducer.playerState().playback.state);  // 乐观 Playing

  // 音频层加载新曲目：Loading 必须被抑制（保持 Playing），快照序列无中间态。
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Loading));
  sequence.push_back(fixture.reducer.playerState().playback.state);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  // 音频层确认加载完成：真实 Playing，抑制解除。
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));
  sequence.push_back(fixture.reducer.playerState().playback.state);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  CHECK_FALSE(std::find(sequence.begin(), sequence.end(), PlaybackStatus::Loading) != sequence.end());
  CHECK(sequence.size() == 3U);
}

TEST_CASE("track switch load failure publishes Error snapshot (never suppressed)") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));

  fixture.reduce(MediaControlCommandKind::SkipNext);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  // 加载中：Loading 被抑制，保持乐观 Playing。
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Loading));
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  // 加载失败：Error 必须原样可见（不被抑制），错误信息可读。
  fixture.reducer.reduceAudioEvent(fixture.errorEvent(audio::PlaybackErrorCode::OpenFailed, "failed to open track b"));
  const auto& state = fixture.reducer.playerState().playback;
  CHECK(state.state == PlaybackStatus::Error);
  REQUIRE(state.errorCode.has_value());
  CHECK(*state.errorCode == "OpenFailed");
  REQUIRE(state.errorMessage.has_value());
  CHECK(*state.errorMessage == "failed to open track b");
}

TEST_CASE("initial play from empty selection suppresses Loading snapshot") {
  ReducerFixture fixture{};
  fixture.installLibrary();

  const auto playReduction = fixture.reduce(MediaControlCommandKind::Play);
  REQUIRE(playReduction.result.accepted);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());

  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Loading));
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("seek Loading suppression regression: seek keeps visible state while backend seeks") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));

  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{1500}});
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  // 后端 seek 过程发布 Loading：依旧被 visibleStateDuringSeek_ 抑制。
  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Loading));
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);

  fixture.reducer.reduceAudioEvent(fixture.stateEvent(audio::PlaybackState::Playing));
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
  CHECK(fixture.reducer.playerState().timeline.position == std::chrono::milliseconds{1500});
}
