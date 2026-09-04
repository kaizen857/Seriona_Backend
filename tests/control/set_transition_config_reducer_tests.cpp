// ControlStateReducer 对 SetTransitionConfig 命令的校验与意图生成测试（T1）。
// 覆盖：校验失败（CommandRejected + 状态零变化）、校验边界（0 下界合法 = 即时完
// 成语义；各上限合法；上限+1 拒绝；非法枚举拒绝）、与 ConfigureOutput 语义隔离
// （校验通过仅生成单意图，绝不触发重载尾意图/快照变化）。
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
namespace audio = seriona::audio;
namespace scanner = seriona::scanner;

namespace {

scanner::SongMetadata makeSong(std::string id, std::string path) {
  scanner::SongMetadata song{};
  song.trackId = std::move(id);
  song.filePath = std::filesystem::path{std::move(path)};
  song.title = "Title " + song.trackId;
  song.artist = "Artist";
  song.contentHash = song.trackId;
  song.duration = std::chrono::milliseconds{600000};
  return song;
}

scanner::PlaylistNode trackNode(std::string nodeId, scanner::SongMetadata metadata) {
  return scanner::PlaylistNode{
      .nodeId = std::move(nodeId),
      .parentNodeId = std::string{"root"},
      .kind = scanner::PlaylistNodeKind::Track,
      .displayName = metadata.trackId,
      .song = std::move(metadata),
      .childNodeIds = {},
  };
}

scanner::PlaylistNode rootNode(std::vector<std::string> children) {
  return scanner::PlaylistNode{
      .nodeId = "root",
      .parentNodeId = std::nullopt,
      .kind = scanner::PlaylistNodeKind::Root,
      .displayName = "Library",
      .song = std::nullopt,
      .childNodeIds = std::move(children),
  };
}

scanner::PlaylistTreeSnapshot makeLibrary() {
  scanner::PlaylistTreeSnapshot tree{};
  tree.version = 1;
  tree.rootNodeId = "root";
  tree.nodes.push_back(rootNode({"song-a", "song-b"}));
  tree.nodes.push_back(trackNode("song-a", makeSong("a", "/music/a.flac")));
  tree.nodes.push_back(trackNode("song-b", makeSong("b", "/music/b.flac")));
  return tree;
}

scanner::ScannerEvent scannerSnapshotEvent(scanner::PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion) {
  return scanner::ScannerEvent{
      .type = scanner::ScannerEventType::PlaylistSnapshotUpdated,
      .monotonicVersion = eventVersion,
      .timestamp = {},
      .payload = std::move(snapshot),
  };
}

struct ReducerFixture {
  ControlStateReducer reducer{};

  void installLibrary() {
    reducer.reduceScannerEvent(scannerSnapshotEvent(makeLibrary(), 1));
  }

  void selectTrack() {
    reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::SelectTrack,
        .track = TrackIdentity{.trackId = "a", .filePath = "/music/a.flac"},
    });
  }

  ControlReduction setTransitionConfig(audio::TransitionConfig config) {
    return reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::SetTransitionConfig,
        .transitionConfig = std::move(config),
    });
  }
};

audio::TransitionConfig customConfig() {
  audio::TransitionConfig config{};
  config.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::ExceptGaplessGroup;
  config.fadeOnTransport = true;
  config.fadeOnSeek = true;
  config.gaplessPreloadMs = std::chrono::milliseconds{800};
  config.crossfadeMs = std::chrono::milliseconds{4000};
  config.transportFadeMs = std::chrono::milliseconds{600};
  config.seekFadeMs = std::chrono::milliseconds{250};
  config.manualAdvanceFadeMode = audio::ManualAdvanceFadeMode::ShortDip;
  config.manualShortCrossfadeMs = std::chrono::milliseconds{1000};
  return config;
}

// 断言拒绝路径：CommandRejected + InvalidCommand + 零意图 + 播放快照零变化。
void expectRejected(ReducerFixture& fixture, const ControlReduction& reduction) {
  CHECK_FALSE(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(reduction.intents.empty());
  REQUIRE(reduction.notifications.size() == 1U);
  CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);

  const auto& after = fixture.reducer.playerState();
  CHECK(after.currentTrack.has_value());
  CHECK(after.currentTrack->trackId == "a");
  CHECK(after.timeline.position == std::chrono::milliseconds{1500});
  CHECK(after.timeline.duration == std::chrono::milliseconds{600000});
  CHECK(after.volume == 0.5F);
  CHECK(after.playback.state == PlaybackStatus::Playing);
}

}  // namespace

TEST_CASE("reducer rejects SetTransitionConfig missing config with zero intents and no state change") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack();

  const auto reduction = fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SetTransitionConfig});

  CHECK_FALSE(reduction.result.accepted);
  CHECK(reduction.result.code == MediaControllerErrorCode::InvalidCommand);
  CHECK(reduction.intents.empty());
  REQUIRE(reduction.notifications.size() == 1U);
  CHECK(reduction.notifications.front().kind == ControlDomainNotificationKind::CommandRejected);
  CHECK(reduction.notifications.front().errorCode == MediaControllerErrorCode::InvalidCommand);
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Playing);
}

TEST_CASE("reducer rejects invalid SetTransitionConfig values and keeps state untouched") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack();
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{1500}});
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SetVolume, .volume = 0.5F});

  SUBCASE("auto advance mode outside enum (3)") {
    auto config = customConfig();
    config.autoAdvanceFadeMode = static_cast<audio::AutoAdvanceFadeMode>(3);
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("auto advance mode below enum (-1)") {
    auto config = customConfig();
    config.autoAdvanceFadeMode = static_cast<audio::AutoAdvanceFadeMode>(-1);
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("manual advance mode outside enum (3)") {
    auto config = customConfig();
    config.manualAdvanceFadeMode = static_cast<audio::ManualAdvanceFadeMode>(3);
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("crossfade above range (10001)") {
    auto config = customConfig();
    config.crossfadeMs = std::chrono::milliseconds{10001};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("crossfade below zero (-1)") {
    auto config = customConfig();
    config.crossfadeMs = std::chrono::milliseconds{-1};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("transport fade above range (3001)") {
    auto config = customConfig();
    config.transportFadeMs = std::chrono::milliseconds{3001};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("seek fade above range (3001)") {
    auto config = customConfig();
    config.seekFadeMs = std::chrono::milliseconds{3001};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("manual short crossfade above range (3001)") {
    auto config = customConfig();
    config.manualShortCrossfadeMs = std::chrono::milliseconds{3001};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
  SUBCASE("gapless preload above range (5001)") {
    auto config = customConfig();
    config.gaplessPreloadMs = std::chrono::milliseconds{5001};
    expectRejected(fixture, fixture.setTransitionConfig(config));
  }
}

TEST_CASE("reducer accepts SetTransitionConfig boundary values as a single intent") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack();

  auto expectAccepted = [&](const audio::TransitionConfig& sent) {
    const auto reduction = fixture.setTransitionConfig(sent);
    CHECK(reduction.result.accepted);
    CHECK(reduction.result.code == MediaControllerErrorCode::None);
    // 语义隔离：仅单意图，无任何重载尾意图（LoadTrack/Seek/Play/Pause）。
    REQUIRE(reduction.intents.size() == 1U);
    CHECK(reduction.intents[0].kind == ControlIntentKind::SetTransitionConfig);
    REQUIRE(reduction.intents[0].transitionConfig.has_value());
    CHECK(*reduction.intents[0].transitionConfig == sent);
  };

  SUBCASE("zero lengths are legal (instant-completion semantics, no lower clamp)") {
    audio::TransitionConfig zero{};
    expectAccepted(zero);
  }
  SUBCASE("crossfade upper boundary 10000") {
    auto config = customConfig();
    config.crossfadeMs = std::chrono::milliseconds{10000};
    expectAccepted(config);
  }
  SUBCASE("transport fade upper boundary 3000") {
    auto config = customConfig();
    config.transportFadeMs = std::chrono::milliseconds{3000};
    expectAccepted(config);
  }
  SUBCASE("seek fade upper boundary 3000") {
    auto config = customConfig();
    config.seekFadeMs = std::chrono::milliseconds{3000};
    expectAccepted(config);
  }
  SUBCASE("manual short crossfade upper boundary 3000") {
    auto config = customConfig();
    config.manualShortCrossfadeMs = std::chrono::milliseconds{3000};
    expectAccepted(config);
  }
  SUBCASE("gapless preload upper boundary 5000") {
    auto config = customConfig();
    config.gaplessPreloadMs = std::chrono::milliseconds{5000};
    expectAccepted(config);
  }
  SUBCASE("top enum modes accepted") {
    auto config = customConfig();
    config.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::All;
    config.manualAdvanceFadeMode = audio::ManualAdvanceFadeMode::FullCrossfade;
    expectAccepted(config);
  }
  SUBCASE("full custom config round-trips payload verbatim") {
    expectAccepted(customConfig());
  }
}

TEST_CASE("reducer keeps playback state untouched while a track is playing") {
  // 与 ConfigureOutput 的整轨重载语义隔离：SetTransitionConfig 不改任何快照字段。
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack();
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SeekTo,
                                                    .position = std::chrono::milliseconds{1500}});
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SetVolume, .volume = 0.5F});
  fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::Pause});
  CHECK(fixture.reducer.playerState().playback.state == PlaybackStatus::Paused);

  const auto reduction = fixture.setTransitionConfig(customConfig());

  CHECK(reduction.result.accepted);
  CHECK_FALSE(reduction.playerStateChanged);
  REQUIRE(reduction.intents.size() == 1U);
  CHECK(reduction.intents[0].kind == ControlIntentKind::SetTransitionConfig);

  const auto& state = fixture.reducer.playerState();
  CHECK(state.currentTrack.has_value());
  CHECK(state.currentTrack->trackId == "a");
  CHECK(state.timeline.position == std::chrono::milliseconds{1500});
  CHECK(state.timeline.duration == std::chrono::milliseconds{600000});
  CHECK(state.volume == 0.5F);
  CHECK(state.playback.state == PlaybackStatus::Paused);
}
