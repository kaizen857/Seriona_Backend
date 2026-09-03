// MediaController 控制层接线测试（TDD 先行）。
// 覆盖：executeIntents 对 ConfigureOutput intent 的转发（payload 一致、无曲目时
// 仅配置不重载、播放中重载序列 ConfigureOutput → LoadTrack → Seek → Play 且顺序
// 正确）；MediaController::enumeratePlaybackDevices() 对 fake 设备列表的透传。
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

MediaControlCommand configureOutputCommand(audio::AudioOutputConfig config) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::ConfigureOutput;
  value.outputConfig = std::move(config);
  return value;
}

MediaControlCommand seekToCommand(std::chrono::milliseconds position) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::SeekTo;
  value.position = position;
  return value;
}

}  // namespace

TEST_CASE("controller forwards configure_output intent to audio service") {
  SUBCASE("applies configuration without reload when no track is selected") {
    ControllerFixture fixture{};
    fixture.controller->start();

    audio::AudioOutputConfig config{};
    config.outputMode = audio::AudioOutputMode::Mixed;
    config.targetSampleRate = 48000U;
    config.targetSampleFormat = audio::AudioSampleFormat::Int16;
    config.bufferDuration = std::chrono::milliseconds{150};

    const auto result = fixture.controller->submitCommand(configureOutputCommand(config));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    CHECK(fixture.fakeAudio->configureOutputCalls() == 1U);
    REQUIRE(fixture.fakeAudio->lastConfiguredOutput().has_value());
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->outputMode == audio::AudioOutputMode::Mixed);
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->targetSampleRate == 48000U);
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->targetSampleFormat == audio::AudioSampleFormat::Int16);
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->bufferDuration == std::chrono::milliseconds{150});
    // 无曲目：仅配置、不重载、不播放（首个条目为构造期 installSinks 的 setEventSink）
    CHECK(fixture.fakeAudio->loadTrackCalls() == 0U);
    CHECK(fixture.fakeAudio->seekCalls() == 0U);
    CHECK(fixture.fakeAudio->playCalls() == 0U);
    CHECK(fixture.fakeAudio->callLog() == std::vector<std::string>{"setEventSink", "configureOutput"});
  }

  SUBCASE("reloads the current track while playing with seek and play after configuration") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.fakeAudio->playCalls() == 1U);

    audio::AudioOutputConfig config{};
    config.outputMode = audio::AudioOutputMode::Direct;
    config.bufferDuration = std::chrono::milliseconds{250};

    const auto result = fixture.controller->submitCommand(configureOutputCommand(config));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    // 顺序断言：ConfigureOutput 必须先于 LoadTrack/Seek/Play（reducer 保证顺序，executeIntents 顺序执行）
    const std::vector<std::string> expected{"setEventSink", "loadTrack", "play", "configureOutput", "loadTrack", "seek", "play"};
    CHECK(fixture.fakeAudio->callLog() == expected);
    CHECK(fixture.fakeAudio->configureOutputCalls() == 1U);
    REQUIRE(fixture.fakeAudio->lastConfiguredOutput().has_value());
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->outputMode == audio::AudioOutputMode::Direct);
    CHECK(fixture.fakeAudio->lastConfiguredOutput()->bufferDuration == std::chrono::milliseconds{250});
    // 重载当前曲目并保持位置与播放状态
    CHECK(fixture.fakeAudio->loadTrackCalls() == 2U);
    REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
    CHECK(fixture.fakeAudio->seekCalls() == 1U);
    REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
    CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{0});
    CHECK(fixture.fakeAudio->playCalls() == 2U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  }

  SUBCASE("reloads the current track while paused with seek and pause after configuration") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture);

    const auto playResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Play));
    REQUIRE(playResult.accepted);
    const auto seekResult = fixture.controller->submitCommand(seekToCommand(std::chrono::milliseconds{1234}));
    REQUIRE(seekResult.accepted);
    const auto pauseResult = fixture.controller->submitCommand(command(MediaControlCommandKind::Pause));
    REQUIRE(pauseResult.accepted);
    REQUIRE(fixture.fakeAudio->callLog() ==
            std::vector<std::string>{"setEventSink", "loadTrack", "play", "seek", "pause"});

    audio::AudioOutputConfig config{};
    config.outputMode = audio::AudioOutputMode::Mixed;
    config.bufferDuration = std::chrono::milliseconds{250};

    const auto result = fixture.controller->submitCommand(configureOutputCommand(config));

    CHECK(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::None);
    // 暂停中重载序列：configureOutput → loadTrack → seek → pause（audio 状态机在
    // 重载完成后的 Ready 上接受 pause 转 Paused，见 playback_state_machine_tests）
    const std::vector<std::string> expected{"setEventSink", "loadTrack", "play", "seek", "pause",
                                            "configureOutput", "loadTrack", "seek", "pause"};
    CHECK(fixture.fakeAudio->callLog() == expected);
    CHECK(fixture.fakeAudio->configureOutputCalls() == 1U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 2U);
    REQUIRE(fixture.fakeAudio->lastLoadedTrack().has_value());
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
    CHECK(fixture.fakeAudio->seekCalls() == 2U);
    REQUIRE(fixture.fakeAudio->lastSeekPosition().has_value());
    CHECK(*fixture.fakeAudio->lastSeekPosition() == std::chrono::milliseconds{1234});
    CHECK(fixture.fakeAudio->pauseCalls() == 2U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);
  }

  SUBCASE("rejects ConfigureOutput missing config before touching the audio service") {
    ControllerFixture fixture{};
    fixture.controller->start();

    const auto result = fixture.controller->submitCommand(command(MediaControlCommandKind::ConfigureOutput));

    CHECK_FALSE(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(fixture.fakeAudio->configureOutputCalls() == 0U);
    CHECK(fixture.fakeAudio->callLog() == std::vector<std::string>{"setEventSink"});
  }
}

TEST_CASE("controller forwards enumerate_devices to audio service") {
  ControllerFixture fixture{};
  fixture.controller->start();

  audio::AudioDeviceFormat deviceA{};
  deviceA.deviceId = "device-a";
  deviceA.deviceName = "Fake Output A";
  deviceA.backendName = "fake";
  deviceA.sampleRate = 48000U;
  deviceA.sampleFormat = audio::AudioSampleFormat::Int16;
  deviceA.channelCount = 2U;
  deviceA.bufferFrames = 512U;
  deviceA.actualMode = audio::AudioOutputMode::Direct;
  deviceA.fallbackApplied = false;

  audio::AudioDeviceFormat deviceB{};
  deviceB.deviceId = "device-b";
  deviceB.deviceName = "Fake Output B";
  deviceB.backendName = "fake";
  deviceB.sampleRate = 44100U;
  deviceB.sampleFormat = audio::AudioSampleFormat::Float32;
  deviceB.channelCount = 2U;
  deviceB.bufferFrames = 256U;
  deviceB.actualMode = audio::AudioOutputMode::Mixed;
  deviceB.fallbackApplied = true;

  fixture.fakeAudio->setPlaybackDevices({deviceA, deviceB});

  const auto devices = fixture.controller->enumeratePlaybackDevices();

  REQUIRE(devices.size() == 2U);
  CHECK(devices[0].deviceId == "device-a");
  CHECK(devices[0].deviceName == "Fake Output A");
  CHECK(devices[0].sampleRate == 48000U);
  CHECK(devices[0].sampleFormat == audio::AudioSampleFormat::Int16);
  CHECK(devices[0].channelCount == 2U);
  CHECK(devices[1].deviceId == "device-b");
  CHECK(devices[1].deviceName == "Fake Output B");
  CHECK(devices[1].sampleRate == 44100U);
  CHECK(devices[1].sampleFormat == audio::AudioSampleFormat::Float32);
  CHECK(devices[1].channelCount == 2U);
  CHECK(devices[1].actualMode == audio::AudioOutputMode::Mixed);

  // 未设置设备时按基类默认返回空列表
  fixture.fakeAudio->setPlaybackDevices({});
  CHECK(fixture.controller->enumeratePlaybackDevices().empty());
}
