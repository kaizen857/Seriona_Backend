// MediaController 控制层接线测试（TDD 先行，仿 configure_output_executes_tests）。
// 覆盖：executeIntents 对 SetTransitionConfig intent 的转发（payload 一致）；
// 与 ConfigureOutput 语义隔离——无论有无选中曲目/播放状态，均不触发 loadTrack/
// seek/play/pause/stop 等任何设备操作；缺配置命令在触碰 audio 前即被拒绝。
#include <doctest.h>

#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
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

MediaControlCommand command(MediaControlCommandKind kind) {
  MediaControlCommand value{};
  value.kind = kind;
  return value;
}

MediaControlCommand setTransitionConfigCommand(audio::TransitionConfig config) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::SetTransitionConfig;
  value.transitionConfig = std::move(config);
  return value;
}

audio::TransitionConfig transitionConfig() {
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

}  // namespace

TEST_CASE("controller forwards set_transition_config intent to audio service") {
  SUBCASE("applies configuration without reload or device operations when no track is selected") {
    ControllerFixture fixture{};
    fixture.controller->start();

    const auto result = fixture.controller->submitCommand(setTransitionConfigCommand(transitionConfig()));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    CHECK(fixture.fakeAudio->configureTransitionCalls() == 1U);
    REQUIRE(fixture.fakeAudio->lastConfiguredTransition().has_value());
    CHECK(*fixture.fakeAudio->lastConfiguredTransition() == transitionConfig());
    // 语义隔离：仅配置，不触发任何设备操作/重载（首个条目为构造期 installSinks 的 setEventSink）
    CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
    CHECK(fixture.fakeAudio->prepareNextCalls() == 0U);
    CHECK(fixture.fakeAudio->seekCalls() == 0U);
    CHECK(fixture.fakeAudio->playCalls() == 0U);
    CHECK(fixture.fakeAudio->pauseCalls() == 0U);
    CHECK(fixture.fakeAudio->resumeCalls() == 0U);
    CHECK(fixture.fakeAudio->stopCalls() == 0U);
    CHECK(fixture.fakeAudio->configureOutputCalls() == 0U);
    CHECK(fixture.fakeAudio->callLog() == std::vector<std::string>{"setEventSink", "configureTransition"});
  }

  SUBCASE("applies configuration without reload while a track is playing") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.fakeAudio->playCalls() == 1U);

    const auto result = fixture.controller->submitCommand(setTransitionConfigCommand(transitionConfig()));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    CHECK(fixture.fakeAudio->configureTransitionCalls() == 1U);
    REQUIRE(fixture.fakeAudio->lastConfiguredTransition().has_value());
    CHECK(*fixture.fakeAudio->lastConfiguredTransition() == transitionConfig());
    // 播放中 SetTransitionConfig 也不触发整轨重载：无额外 loadTrack/seek/play
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.fakeAudio->seekCalls() == 0U);
    CHECK(fixture.fakeAudio->playCalls() == 1U);
    CHECK(fixture.fakeAudio->pauseCalls() == 0U);
    CHECK(fixture.fakeAudio->callLog() ==
          std::vector<std::string>{"setEventSink", "loadTrack", "play", "configureTransition"});
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  }

  SUBCASE("rejects SetTransitionConfig missing config before touching the audio service") {
    ControllerFixture fixture{};
    fixture.controller->start();

    const auto result = fixture.controller->submitCommand(command(MediaControlCommandKind::SetTransitionConfig));

    CHECK_FALSE(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(fixture.fakeAudio->configureTransitionCalls() == 0U);
    CHECK(fixture.fakeAudio->callLog() == std::vector<std::string>{"setEventSink"});
  }
}
