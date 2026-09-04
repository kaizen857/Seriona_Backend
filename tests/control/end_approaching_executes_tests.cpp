// MediaController 控制层接线测试（T8：EndApproaching → PrepareNext）。
// 仿 set_transition_config_executes_tests 的 rig：FakeAudioPlaybackService 直接
// emit 后端事件，controller（runInlineForTests）把音频事件送进 reducer；断言
// executeIntents 对 PrepareNext intent 的转发（payload 一致：track + kind/
// isGaplessGroup 决策表行），以及 prepare 不推进播放索引（真实 PlaybackEnded
// 提交的 LoadTrack 目标 == 预解码 peek 目标）。
#include <doctest.h>

#include "control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
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

// CUE 派生曲：filePath= .cue 路径（identity 归属），sourceFilePath=实际音频；
// offset 有值 → requestFromSong 标记 boundedSegment（无间隙组判定前提）。
scanner::SongMetadata cueSong(std::string id, std::string cuePath, std::string audioPath,
                              std::chrono::milliseconds offset) {
  auto metadata = song(std::move(id), std::move(cuePath));
  metadata.sourceFilePath = std::move(audioPath);
  metadata.offset = offset;
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

// 与 set_transition_config_executes_tests 同名助手保持语义一致：安装根级曲库并推进。
void installLibrary(ControllerFixture& fixture, std::vector<scanner::SongMetadata> songs, std::uint64_t treeVersion = 20,
                    std::uint64_t eventVersion = 1) {
  fixture.fakeScanner->emit(scannerSnapshotEvent(libraryTree(std::move(songs), treeVersion), eventVersion));
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

MediaControlCommand selectTrackCommand(std::string trackId, std::string path) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::SelectTrack;
  value.track = TrackIdentity{.trackId = std::move(trackId), .filePath = std::move(path)};
  return value;
}

MediaControlCommand setRepeatModeCommand(RepeatMode mode) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::SetRepeatMode;
  value.repeatMode = mode;
  return value;
}

MediaControlCommand setShuffleCommand(bool shuffle) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::SetShuffle;
  value.shuffle = shuffle;
  return value;
}

MediaControlCommand playNextTrackCommand(std::string trackId, std::string path) {
  MediaControlCommand value{};
  value.kind = MediaControlCommandKind::PlayNextTrack;
  value.track = TrackIdentity{.trackId = std::move(trackId), .filePath = std::move(path)};
  return value;
}

// 只设本任务关心的过渡字段：自动档位（决策表输入）+ 预加载/交叉长度（供参考，
// 服务侧武装条件；reducer 行只看 autoAdvanceFadeMode）。
audio::TransitionConfig fadeConfig(audio::AutoAdvanceFadeMode mode,
                                   std::chrono::milliseconds crossfadeMs = 4000ms,
                                   std::chrono::milliseconds preloadMs = 800ms) {
  audio::TransitionConfig config{};
  config.autoAdvanceFadeMode = mode;
  config.crossfadeMs = crossfadeMs;
  config.gaplessPreloadMs = preloadMs;
  return config;
}

audio::BackendEvent endApproachingEvent(std::uint64_t version,
                                        std::chrono::milliseconds remainingMs = 1500ms) {
  return audio::BackendEvent{.type = audio::BackendEventType::EndApproaching,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::EndApproaching{.remainingMs = remainingMs}};
}

audio::BackendEvent playbackEndedEvent(std::string id, std::string path, std::uint64_t version) {
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
                             .payload = audio::PlaybackEnded{.request = std::move(request)}};
}

// T10：接管提交事件（服务在无缝直切/重叠 handoff 完成、新曲已加载输出时发出）。
audio::BackendEvent advanceCompletedEvent(std::string trackId, std::uint64_t version) {
  return audio::BackendEvent{.type = audio::BackendEventType::AdvanceCompleted,
                             .sourceModule = audio::BackendSourceModule::AudioPlaybackService,
                             .monotonicVersion = version,
                             .timestamp = {},
                             .payload = audio::AdvanceCompleted{.trackId = std::move(trackId)}};
}

// 断言单次 PrepareNext：目标曲 + 交接方式（决策表行校验）。
void requirePrepareNext(ControllerFixture& fixture,
                        std::size_t expectedCalls,
                        const char* expectedTrackId,
                        audio::PrepareNextKind expectedKind,
                        bool expectedGaplessGroup) {
  REQUIRE(fixture.fakeAudio->prepareNextCalls() == expectedCalls);
  REQUIRE(fixture.fakeAudio->lastPreparedTrack().has_value());
  CHECK(fixture.fakeAudio->lastPreparedTrack()->trackId == expectedTrackId);
  REQUIRE(fixture.fakeAudio->lastPrepareNextMeta().has_value());
  CHECK(fixture.fakeAudio->lastPrepareNextMeta()->kind == expectedKind);
  CHECK(fixture.fakeAudio->lastPrepareNextMeta()->isGaplessGroup == expectedGaplessGroup);
}

}  // namespace

TEST_CASE("controller end approaching prepares next with fade mode decision rows") {
  SUBCASE("All mode prepares the sequential next track with Crossfade") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);

    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::Crossfade, false);
    // 预解码不推进播放索引：无额外 loadTrack/play。
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.fakeAudio->playCalls() == 1U);
  }

  SUBCASE("Off mode prepares the sequential next track with SeamlessDirect") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::Off)))
                .accepted);

    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::SeamlessDirect, false);
  }

  SUBCASE("ExceptGaplessGroup crosses out-of-cue pairs and seams in-cue pairs") {
    ControllerFixture fixture{};
    fixture.controller->start();
    // 顺序：cue-a-1 / cue-a-2 同属 side-a.cue（无间隙组），随后是 side-b.cue 的邻曲。
    installLibrary(fixture,
                   {cueSong("cue-a-1", "music/side-a.cue", "music/side-a.flac", 0ms),
                    cueSong("cue-a-2", "music/side-a.cue", "music/side-a.flac", 100000ms),
                    cueSong("cue-b-1", "music/side-b.cue", "music/side-b.flac", 0ms)});
    REQUIRE(fixture.controller->submitCommand(
                setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::ExceptGaplessGroup)))
                .accepted);

    REQUIRE(fixture.controller->submitCommand(selectTrackCommand("cue-a-1", "music/side-a.cue")).accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    // 组内邻曲（同一 .cue 文件）：中间档豁免交叉，尽力无缝。
    requirePrepareNext(fixture, 1U, "cue-a-2", audio::PrepareNextKind::SeamlessDirect, true);

    // 组内也仍只 prepare、不推进：随后真实 PlaybackEnded 才提交 cue-a-2。
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    fixture.fakeAudio->emit(playbackEndedEvent("cue-a-1", "music/side-a.flac", 2));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "cue-a-2");

    // 组间邻曲（cue-a-2 → cue-b-1，不同 .cue 文件）：交叉。
    fixture.fakeAudio->emit(endApproachingEvent(3));
    fixture.controller->drainForTests();
    requirePrepareNext(fixture, 2U, "cue-b-1", audio::PrepareNextKind::Crossfade, false);
  }

  SUBCASE("RepeatOne prepares the current track itself with SeamlessDirect even under All") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(setRepeatModeCommand(RepeatMode::One)).accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    // RepeatOne 自身重播为无缝直切（裁定基线①），不受 All 交叉档影响。
    requirePrepareNext(fixture, 1U, "a", audio::PrepareNextKind::SeamlessDirect, false);
    // 真实自然结束提交 = 自身重播（LoadTrack 同曲）。
    fixture.fakeAudio->emit(playbackEndedEvent("a", "music/a.flac", 2));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "a");
  }

  SUBCASE("temporary queue front is the prepare target and stays queued until the real end") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.controller->submitCommand(playNextTrackCommand("c", "music/c.flac")).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    // 临时队列队首 c 为预解码目标（All → Crossfade）；队列未被 peek 消费。
    requirePrepareNext(fixture, 1U, "c", audio::PrepareNextKind::Crossfade, false);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    const auto& queueSnapshot = fixture.controller->playerStateSnapshot().queueEntries;
    REQUIRE(queueSnapshot.size() == 1U);
    CHECK(queueSnapshot[0].trackId == "c");
    // 真实自然结束提交 = 消费队首 c（而非顺序下一曲 b）。
    fixture.fakeAudio->emit(playbackEndedEvent("a", "music/a.flac", 2));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "c");
  }

  SUBCASE("queue front under Off mode prepares with SeamlessDirect") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::Off)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.controller->submitCommand(playNextTrackCommand("c", "music/c.flac")).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    requirePrepareNext(fixture, 1U, "c", audio::PrepareNextKind::SeamlessDirect, false);
  }

  SUBCASE("end of order without repeat mode prepares nothing") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(selectTrackCommand("b", "music/b.flac")).accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    // 列表末端且无 repeat：peek 无下一曲 → 零 PrepareNext（自然硬切）。
    CHECK(fixture.fakeAudio->prepareNextCalls() == 0U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  }

  SUBCASE("shuffle activation prepares nothing (random next cannot be predecoded)") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(setShuffleCommand(true)).accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    CHECK(fixture.fakeAudio->prepareNextCalls() == 0U);
  }
}

TEST_CASE("controller end approaching prepare does not advance the playback index") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
  REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
              .accepted);
  REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

  fixture.fakeAudio->emit(endApproachingEvent(1));
  fixture.controller->drainForTests();
  requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::Crossfade, false);
  // prepare 本身零副作用：播放索引/当前曲不变。
  CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
  CHECK(fixture.fakeAudio->playCalls() == 1U);
  CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a");

  // 真实 PlaybackEnded 才提交：LoadTrack 目标 = 预解码 peek 目标（b）。
  fixture.fakeAudio->emit(playbackEndedEvent("a", "music/a.flac", 2));
  fixture.controller->drainForTests();
  REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");
}

TEST_CASE("controller ignores stale and out-of-state EndApproaching events") {
  SUBCASE("monotonic version gate drops duplicates and older events") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(5));
    fixture.controller->drainForTests();
    requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::Crossfade, false);

    // 同一版本重复（陈旧/乱序在途事件）：版本门控直接丢弃，不重复 PrepareNext。
    fixture.fakeAudio->emit(endApproachingEvent(5));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->prepareNextCalls() == 1U);
    // 更旧版本回放同样被丢弃。
    fixture.fakeAudio->emit(endApproachingEvent(4));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->prepareNextCalls() == 1U);
  }

  SUBCASE("EndApproaching outside Playing/Paused is dropped") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Stop)).accepted);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();

    // 逻辑 Stopped 下的事件 = 陈旧预告（状态机 Stop 后无播放上下文）。
    CHECK(fixture.fakeAudio->prepareNextCalls() == 0U);
  }
}

// ============ T10：AdvanceCompleted 提交域（Metis 缺口 1b 控制器侧） ============

TEST_CASE("controller advance completed commits the takeover target without reloading") {
  SUBCASE("sequential next track commits and stays Playing with zero reload") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::Crossfade, false);

    // 服务完成接管（交叉/直切 handoff）→ AC(b)：提交 = 切目标曲，但绝不重发
    // LoadTrack/Play（曲已在服务侧加载输出——控制器不发音频意图，裁定⑦ 提交语义）。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 2));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.fakeAudio->playCalls() == 1U);
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
    CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{0});
  }

  SUBCASE("temporary queue front commit consumes the queue entry") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.controller->submitCommand(playNextTrackCommand("c", "music/c.flac")).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    requirePrepareNext(fixture, 1U, "c", audio::PrepareNextKind::Crossfade, false);

    fixture.fakeAudio->emit(advanceCompletedEvent("c", 2));
    fixture.controller->drainForTests();
    // 队首 c 被提交消费：无重载、队列清空、当前曲 = c。
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.controller->playerStateSnapshot().queueEntries.empty());
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "c");
  }

  SUBCASE("RepeatOne self replay commits in place without reload") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(setRepeatModeCommand(RepeatMode::One)).accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    requirePrepareNext(fixture, 1U, "a", audio::PrepareNextKind::SeamlessDirect, false);

    // 服务无缝重播同曲 → AC(自身)：原地续播提交（位置归零、无 LoadTrack）。
    fixture.fakeAudio->emit(advanceCompletedEvent("a", 2));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a");
    CHECK(fixture.controller->playerStateSnapshot().timeline.position == std::chrono::milliseconds{0});
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Playing);
  }

  SUBCASE("advance completed without pending ledger is ignored (stale or unsolicited takeover)") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

    // 无 EndApproaching 账本直接 AC：陈旧/双提交/未经批准的接管 → 丢弃，零副作用。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 1));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a");
  }

  SUBCASE("second advance completed after a commit is deduplicated") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);

    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 2));
    fixture.controller->drainForTests();
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");

    // 同目标重复 AC（服务重复发/乱序在途）：账本已消费 → 丢弃（双提交防重）。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 3));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");
  }
}

TEST_CASE("controller aborts and reschedules when takeover target mismatches the ledger") {
  ControllerFixture fixture{};
  fixture.controller->start();
  installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
  REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
              .accepted);
  REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
  REQUIRE(fixture.fakeAudio->loadTrackCalls() == 1U);

  fixture.fakeAudio->emit(endApproachingEvent(1));
  fixture.controller->drainForTests();
  requirePrepareNext(fixture, 1U, "b", audio::PrepareNextKind::Crossfade, false);

  // 服务接管了账本未批准的目标（x 不在推进级联中）→ 校验失败：abort（撤第二源 +
  // 重新武装）并按控制器当前决策重发普通 LoadTrack（提交语义永远由控制器裁决）。
  fixture.fakeAudio->emit(advanceCompletedEvent("x", 2));
  fixture.controller->drainForTests();
  CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
  REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
  CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "b");
  REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
  CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");
}

TEST_CASE("controller advance window invalidators abort the in-flight transition") {
  SUBCASE("select track in window aborts pending advance then plays the new selection") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    // 窗内 SelectTrack（失效操作，裁定基线⑦）→ 先 AbortTransition 再执行切轨。
    REQUIRE(fixture.controller->submitCommand(selectTrackCommand("c", "music/c.flac")).accepted);
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    REQUIRE(fixture.fakeAudio->loadTrackCalls() == 2U);
    CHECK(fixture.fakeAudio->lastLoadedTrack()->trackId == "c");

    // 迟到 AC（被中止臂的残留事件）：账本已清 → 忽略，不干扰新选中曲目。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 2));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 2U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "c");
  }

  SUBCASE("stop in window aborts pending advance then stops") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Stop)).accepted);
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Stopped);
  }

  SUBCASE("play next in window aborts pending advance then queues the new target") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    REQUIRE(fixture.controller->submitCommand(playNextTrackCommand("c", "music/c.flac")).accepted);
    fixture.controller->drainForTests();
    // PlayNextTrack = 插队不立即切歌：abort 在途过渡后目标 c 进入队首，待本曲自然
    // 结束时提交（队列版本已递增 → 后续 AC 校验失败路径也被账本清空覆盖）。
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    const auto& queueSnapshot = fixture.controller->playerStateSnapshot().queueEntries;
    REQUIRE(queueSnapshot.size() == 1U);
    CHECK(queueSnapshot[0].trackId == "c");
  }

  SUBCASE("pause in window aborts pending advance then pauses") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Pause)).accepted);
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.controller->playerStateSnapshot().playback.state == PlaybackStatus::Paused);
  }

  SUBCASE("non invalidator command in window leaves the pending advance intact") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    // SetVolume 不在失效操作类（裁定基线⑦ 列表外）→ 不 abort；账本保留到提交。
    REQUIRE(fixture.controller
                ->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SetVolume, .volume = 0.5F})
                .accepted);
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);

    // 账本仍在：随后的 AC(b) 正常提交（无重载）。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 2));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "b");
  }

  SUBCASE("commands outside the window never abort") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac"), song("c", "music/c.flac")});
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    // 无 EndApproaching（无窗口）时任何操作都零 abort。
    REQUIRE(fixture.controller->submitCommand(selectTrackCommand("b", "music/b.flac")).accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Pause)).accepted);
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 0U);
  }
}

// T10 N1 加固回归：窗内载荷非法的失效命令（reject 路径）必须仍撤服务侧过渡——
// 门控 abort 压入的 AbortTransition 意图不得随 reject() 的全新 reduction 丢弃。
TEST_CASE("controller reject in advance window still aborts the in-flight transition") {
  SUBCASE("invalid seek in window aborts pending advance and still rejects the command") {
    ControllerFixture fixture{};
    fixture.controller->start();
    installLibrary(fixture, {song("a", "music/a.flac"), song("b", "music/b.flac")});
    REQUIRE(fixture.controller->submitCommand(setTransitionConfigCommand(fadeConfig(audio::AutoAdvanceFadeMode::All)))
                .accepted);
    REQUIRE(fixture.controller->submitCommand(command(MediaControlCommandKind::Play)).accepted);
    fixture.fakeAudio->emit(endApproachingEvent(1));
    fixture.controller->drainForTests();
    REQUIRE(fixture.fakeAudio->prepareNextCalls() == 1U);

    // 窗内缺 position 的 SeekTo：reject（错误通知照发）但 abort 意图必须送达服务侧。
    const auto result = fixture.controller->submitCommand(command(MediaControlCommandKind::SeekTo));
    fixture.controller->drainForTests();
    CHECK_FALSE(result.accepted);
    CHECK(result.code == MediaControllerErrorCode::InvalidCommand);
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);

    // 账本已清：迟到 AC 落 stale-drop（零新 abort、零重载），无曲目切换。
    fixture.fakeAudio->emit(advanceCompletedEvent("b", 2));
    fixture.controller->drainForTests();
    CHECK(fixture.fakeAudio->abortTransitionCalls() == 1U);
    CHECK(fixture.fakeAudio->loadTrackCalls() == 1U);
    REQUIRE(fixture.controller->playerStateSnapshot().currentTrack.has_value());
    CHECK(fixture.controller->playerStateSnapshot().currentTrack->trackId == "a");
  }
}
