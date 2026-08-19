// ControlStateReducer 对 ConfigureOutput 命令的校验与立即重载测试。
// 覆盖：校验失败（CommandRejected + 状态零变化）、校验通过（ConfigureOutput→
// LoadTrack→Seek→[Play|Pause] 的顺序与 payload）、CUE 偏移、状态保留矩阵、
// 无选中曲目时仅应用配置不重载。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "control/control_state_reducer.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

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
  tree.nodes.push_back(rootNode({"song-a", "song-b"}));
  tree.nodes.push_back(trackNode("song-a", makeSong("a", "/music/a.flac")));
  tree.nodes.push_back(trackNode("song-b", makeSong("b", "/music/b.flac")));
  return tree;
}

// CUE 派生曲目：filePath 指向 .cue，sourceFilePath 指向实际音频，offset 非空。
PlaylistTreeSnapshot makeCueLibrary() {
  PlaylistTreeSnapshot tree{};
  tree.version = 1;
  tree.rootNodeId = "root";
  auto cueSong = makeSong("cue1", "/music/album.cue");
  cueSong.sourceFilePath = std::filesystem::path{"/music/album.flac"};
  cueSong.offset = std::chrono::milliseconds{2000};
  tree.nodes.push_back(rootNode({"cue1"}));
  tree.nodes.push_back(trackNode("cue1", std::move(cueSong)));
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

  ControlReduction configureOutput(audio::AudioOutputConfig config) {
    return reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::ConfigureOutput,
        .outputConfig = std::move(config),
    });
  }

  // 通过后端事件把播放状态驱动到 Loading（reducer 唯一可达该状态的路径）。
  void driveToLoading() {
    audio::BackendEvent event{};
    event.type = audio::BackendEventType::PlaybackStateChanged;
    event.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    event.monotonicVersion = 10;
    event.payload = audio::PlaybackStateChanged{.state = audio::PlaybackState::Loading};
    reducer.reduceAudioEvent(event);
  }
};

audio::AudioOutputConfig defaultConfig() {
  audio::AudioOutputConfig config{};
  config.outputMode = audio::AudioOutputMode::Direct;
  return config;
}

MediaControlCommand configureOutputCommand(audio::AudioOutputConfig config) {
  return MediaControlCommand{.kind = MediaControlCommandKind::ConfigureOutput, .outputConfig = std::move(config)};
}

}  // namespace

TEST_CASE("reducer rejects ConfigureOutput missing config with zero intents and no state change") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");

  const auto before = fixture.reducer.playerState();

  const auto reduction = fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::ConfigureOutput});

  CHECK_FALSE(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(reduction.intents.empty());
  REQUIRE(reduction.notifications.size() == 1U);
  CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);

  const auto after = fixture.reducer.playerState();
  CHECK(after.currentTrack.has_value() == before.currentTrack.has_value());
  if (before.currentTrack.has_value()) {
    REQUIRE(after.currentTrack.has_value());
    CHECK(after.currentTrack->trackId == before.currentTrack->trackId);
    CHECK(after.currentTrack->filePath == before.currentTrack->filePath);
  }
  CHECK(after.timeline.position == before.timeline.position);
  CHECK(after.timeline.duration == before.timeline.duration);
  CHECK(after.volume == before.volume);
  CHECK(after.muted == before.muted);
  CHECK(after.playback.state == before.playback.state);
}

TEST_CASE("reducer rejects invalid ConfigureOutput values and keeps state untouched") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{1500}});
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SetVolume, .volume = 0.35F});

  const auto before = fixture.reducer.playerState();

  auto expectRejected = [&](const ControlReduction& reduction) {
    CHECK_FALSE(reduction.result.accepted);
    CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(reduction.intents.empty());
    REQUIRE(reduction.notifications.size() == 1U);
    CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
    CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);
    // 校验失败路径必须状态零变化（track/position/volume 不变）
    const auto after = fixture.reducer.playerState();
    CHECK(after.currentTrack.has_value() == before.currentTrack.has_value());
    if (before.currentTrack.has_value()) {
      REQUIRE(after.currentTrack.has_value());
      CHECK(after.currentTrack->trackId == before.currentTrack->trackId);
      CHECK(after.currentTrack->filePath == before.currentTrack->filePath);
    }
    CHECK(after.timeline.position == before.timeline.position);
    CHECK(after.timeline.duration == before.timeline.duration);
    CHECK(after.volume == before.volume);
    CHECK(after.playback.state == before.playback.state);
  };

  SUBCASE("buffer duration below range") {
    auto config = defaultConfig();
    config.bufferDuration = std::chrono::milliseconds{5};
    expectRejected(fixture.configureOutput(config));
  }
  SUBCASE("buffer duration above range") {
    auto config = defaultConfig();
    config.bufferDuration = std::chrono::milliseconds{1001};
    expectRejected(fixture.configureOutput(config));
  }
  SUBCASE("sample rate zero") {
    auto config = defaultConfig();
    config.targetSampleRate = 0U;
    expectRejected(fixture.configureOutput(config));
  }
  SUBCASE("sample rate below range") {
    auto config = defaultConfig();
    config.targetSampleRate = 7999U;
    expectRejected(fixture.configureOutput(config));
  }
  SUBCASE("sample rate above range") {
    auto config = defaultConfig();
    config.targetSampleRate = 768001U;
    expectRejected(fixture.configureOutput(config));
  }
  SUBCASE("output mode outside enum") {
    auto config = defaultConfig();
    config.outputMode = static_cast<audio::AudioOutputMode>(7);
    expectRejected(fixture.configureOutput(config));
  }
}

TEST_CASE("reducer applies ConfigureOutput without reload when no track is selected") {
  // 全新 reducer：无库、无选中曲目 —— 配置仍应用（1 个 ConfigureOutput intent），不重载。
  ControlStateReducer reducer{};
  auto config = defaultConfig();
  config.outputMode = audio::AudioOutputMode::Mixed;
  config.targetSampleRate = 48000U;
  config.bufferDuration = std::chrono::milliseconds{150};

  const auto reduction = reducer.reduceCommand(configureOutputCommand(config));

  CHECK(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::None);
  REQUIRE(reduction.intents.size() == 1U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
  REQUIRE(reduction.intents[0].outputConfig.has_value());
  CHECK(reduction.intents[0].outputConfig->outputMode == audio::AudioOutputMode::Mixed);
  CHECK(reduction.intents[0].outputConfig->targetSampleRate == 48000U);
  CHECK(reduction.intents[0].outputConfig->bufferDuration == std::chrono::milliseconds{150});
}

TEST_CASE("reducer reloads current track with seek and play preserving position while playing") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{1500}});

  auto config = defaultConfig();
  config.bufferDuration = std::chrono::milliseconds{250};

  const auto reduction = fixture.configureOutput(config);

  CHECK(reduction.result.accepted);
  // intent 顺序必须为 ConfigureOutput → LoadTrack → Seek → [Play|Pause]
  REQUIRE(reduction.intents.size() == 4U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
  CHECK(reduction.intents[1].kind == ControlIntentKind::LoadTrack);
  CHECK(reduction.intents[2].kind == ControlIntentKind::Seek);
  CHECK(reduction.intents[3].kind == ControlIntentKind::Play);

  // LoadTrack payload 必须来自选中曲目解析路径（PlayableTrack::request）
  REQUIRE(reduction.intents[1].track.has_value());
  CHECK(reduction.intents[1].track->trackId == "a");
  CHECK(reduction.intents[1].track->filePath == std::filesystem::path{"/music/a.flac"});
  CHECK_FALSE(reduction.intents[1].track->offset.has_value());

  // 非 CUE：Seek 位置 == timeline.position（1500）+ offset.value_or(0)
  REQUIRE(reduction.intents[2].position.has_value());
  CHECK(*reduction.intents[2].position == std::chrono::milliseconds{1500});

  // 状态保留：Playing 保持 Playing
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
  CHECK(fixture.reducer.playerState().timeline.position == std::chrono::milliseconds{1500});
}

TEST_CASE("reducer reload seek position includes cue offset") {
  ReducerFixture fixture{};
  fixture.installLibrary(makeCueLibrary());
  fixture.selectTrack("cue1", "/music/album.cue");
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{500}});

  const auto reduction = fixture.configureOutput(defaultConfig());

  CHECK(reduction.result.accepted);
  REQUIRE(reduction.intents.size() == 4U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
  CHECK(reduction.intents[1].kind == ControlIntentKind::LoadTrack);
  REQUIRE(reduction.intents[1].track.has_value());
  CHECK(reduction.intents[1].track->trackId == "cue1");
  CHECK(reduction.intents[1].track->filePath == std::filesystem::path{"/music/album.flac"});
  CHECK(reduction.intents[1].track->offset == std::chrono::milliseconds{2000});
  CHECK(reduction.intents[2].kind == ControlIntentKind::Seek);
  REQUIRE(reduction.intents[2].position.has_value());
  // CUE 偏移：Seek 位置 == timeline.position(500) + currentTrackOffset_(2000)
  CHECK(*reduction.intents[2].position == std::chrono::milliseconds{2500});
  CHECK(reduction.intents[3].kind == ControlIntentKind::Play);
}

TEST_CASE("reducer preserves playback state across reload") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a", "/music/a.flac");

  SUBCASE("paused keeps pause tail intent") {
    fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                      .position = std::chrono::milliseconds{1200}});
    fixture.reduce(MediaControlCommandKind::Pause);
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Paused);

    const auto reduction = fixture.configureOutput(defaultConfig());

    CHECK(reduction.result.accepted);
    REQUIRE(reduction.intents.size() == 4U);
    CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
    CHECK(reduction.intents[1].kind == ControlIntentKind::LoadTrack);
    CHECK(reduction.intents[2].kind == ControlIntentKind::Seek);
    REQUIRE(reduction.intents[2].position.has_value());
    CHECK(*reduction.intents[2].position == std::chrono::milliseconds{1200});
    CHECK(reduction.intents[3].kind == ControlIntentKind::Pause);
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Paused);
  }
  SUBCASE("loading resumes with play tail intent") {
    // T6 切轨抑制：selectTrack 后的 Loading 会被压回乐观 Playing。先确认真实
    // Playing（清除切轨抑制），再发布 Loading —— 此时无抑制，Loading 可见，
    // 验证 ConfigureOutput 重载走 Loading 分支（Play 尾意图）。
    audio::BackendEvent confirmedPlaying{};
    confirmedPlaying.type = audio::BackendEventType::PlaybackStateChanged;
    confirmedPlaying.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    confirmedPlaying.monotonicVersion = 5;
    confirmedPlaying.payload = audio::PlaybackStateChanged{.state = audio::PlaybackState::Playing};
    fixture.reducer.reduceAudioEvent(confirmedPlaying);
    fixture.driveToLoading();
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Loading);

    const auto reduction = fixture.configureOutput(defaultConfig());

    CHECK(reduction.result.accepted);
    REQUIRE(reduction.intents.size() == 4U);
    CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
    CHECK(reduction.intents[1].kind == ControlIntentKind::LoadTrack);
    CHECK(reduction.intents[2].kind == ControlIntentKind::Seek);
    CHECK(reduction.intents[3].kind == ControlIntentKind::Play);
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Loading);
  }
  SUBCASE("stopped reloads without play or pause tail intent") {
    fixture.reduce(MediaControlCommandKind::Stop);
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Stopped);

    const auto reduction = fixture.configureOutput(defaultConfig());

    CHECK(reduction.result.accepted);
    REQUIRE(reduction.intents.size() == 3U);
    CHECK(reduction.intents[0].kind == ControlIntentKind::ConfigureOutput);
    CHECK(reduction.intents[1].kind == ControlIntentKind::LoadTrack);
    CHECK(reduction.intents[2].kind == ControlIntentKind::Seek);
    CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Stopped);
  }
}
