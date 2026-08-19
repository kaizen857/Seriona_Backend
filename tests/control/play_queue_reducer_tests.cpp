// T7【后端】临时播放队列：PlayNextTrack/ClearPlayQueue/RemoveFromQueue 与
// index 冻结语义测试（TDD 先行）。
//
// 覆盖：
// 1. PlayNextTrack 目标入队首，快照 queueEntries: [{trackId, nodeId}] 契约；
// 2. 当前曲播完（PlaybackEnded）先消费队列头部 —— 播放上下文 index 冻结；
//    队列空后从原 index 的下一曲继续（A 歌曲1 → 插播 B 歌曲2 → B 播完 → A 歌曲3）；
// 3. SkipNext（next 命令）同样先消费队列头部；
// 4. ClearPlayQueue/RemoveFromQueue 行为与幂等（空队列/越界均成功）；
// 5. PlayNextTrack 对不存在的 trackId 返回命令失败（TrackNotInLibrary）；
// 6. 队列不持久化（新 reducer 实例 = 重启，队列为空）；
// 7. 旧快照（无 queueEntries 字段的初始化）解析不崩，默认为空 —— 序列化兼容；
// 8. 入队后库变化导致队列条目不可解析时，消费时静默跳过。
#include <doctest.h>

#include "control/control_state_reducer.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace seriona::control;
using namespace seriona::scanner;
namespace audio = seriona::audio;

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

PlaylistNode trackNode(std::string nodeId, std::string parentNodeId, SongMetadata metadata) {
  return PlaylistNode{
      .nodeId = std::move(nodeId),
      .parentNodeId = std::move(parentNodeId),
      .kind = PlaylistNodeKind::Track,
      .displayName = metadata.trackId,
      .song = std::move(metadata),
      .childNodeIds = {},
  };
}

PlaylistNode containerNode(std::string nodeId, PlaylistNodeKind kind, std::vector<std::string> children) {
  return PlaylistNode{
      .nodeId = std::move(nodeId),
      .parentNodeId = std::string{"root"},
      .kind = kind,
      .displayName = nodeId,
      .song = std::nullopt,
      .childNodeIds = std::move(children),
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

// A 文件夹（folder-a）：a1/a2/a3；B 文件夹（folder-b）：b1/b2。
PlaylistTreeSnapshot makeLibrary() {
  PlaylistTreeSnapshot tree{};
  tree.version = 1;
  tree.rootNodeId = "root";
  tree.nodes.push_back(rootNode({"folder-a", "folder-b"}));
  tree.nodes.push_back(containerNode("folder-a", PlaylistNodeKind::Directory, {"song-a1", "song-a2", "song-a3"}));
  tree.nodes.push_back(containerNode("folder-b", PlaylistNodeKind::Directory, {"song-b1", "song-b2"}));
  tree.nodes.push_back(trackNode("song-a1", "folder-a", makeSong("a1", "/music/a/a1.flac")));
  tree.nodes.push_back(trackNode("song-a2", "folder-a", makeSong("a2", "/music/a/a2.flac")));
  tree.nodes.push_back(trackNode("song-a3", "folder-a", makeSong("a3", "/music/a/a3.flac")));
  tree.nodes.push_back(trackNode("song-b1", "folder-b", makeSong("b1", "/music/b/b1.flac")));
  tree.nodes.push_back(trackNode("song-b2", "folder-b", makeSong("b2", "/music/b/b2.flac")));
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

  ControlReduction playNextTrack(const char* trackId, const char* filePath) {
    return reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::PlayNextTrack,
        .track = TrackIdentity{.trackId = trackId, .filePath = filePath},
    });
  }

  ControlReduction clearQueue() {
    return reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::ClearPlayQueue});
  }

  ControlReduction removeFromQueue(std::size_t index) {
    return reducer.reduceCommand(MediaControlCommand{
        .kind = MediaControlCommandKind::RemoveFromQueue,
        .queueIndex = index,
    });
  }

  ControlReduction skipNext() {
    return reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
  }

  // 模拟音频后端确认当前曲目播放结束。
  audio::BackendEvent endedEvent(const char* trackId, const char* filePath) {
    audio::TrackPlaybackRequest request{};
    request.trackId = trackId;
    request.filePath = filePath;
    request.title = "Title " + std::string{trackId};
    request.artist = "Artist";
    request.duration = std::chrono::milliseconds{600000};

    audio::BackendEvent event{};
    event.type = audio::BackendEventType::PlaybackEnded;
    event.sourceModule = audio::BackendSourceModule::AudioPlaybackService;
    event.monotonicVersion = nextEventVersion++;
    event.payload = audio::PlaybackEnded{
        .request = request,
        .finalClock = audio::PlaybackClockSnapshot{.trackId = trackId,
                                                   .position = std::chrono::milliseconds{600000},
                                                   .sampledAt = {},
                                                   .version = 1,
                                                   .continuous = false},
    };
    return event;
  }
};

}  // namespace

TEST_CASE("PlayNextTrack enqueues at front and snapshot exposes queueEntries contract") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  const auto first = fixture.playNextTrack("b1", "/music/b/b1.flac");
  REQUIRE(first.result.accepted);
  CHECK(first.result.code == MediaControllerErrorCode::None);
  CHECK(first.playerStateChanged);
  {
    const auto& entries = fixture.reducer.playerState().queueEntries;
    REQUIRE(entries.size() == 1U);
    CHECK(entries[0].trackId == "b1");
    CHECK(entries[0].nodeId == "folder-b");
  }

  // 第二次 PlayNextTrack 入队首（后入先消费）。
  const auto second = fixture.playNextTrack("b2", "/music/b/b2.flac");
  REQUIRE(second.result.accepted);
  CHECK(second.playerStateChanged);
  {
    const auto& entries = fixture.reducer.playerState().queueEntries;
    REQUIRE(entries.size() == 2U);
    CHECK(entries[0].trackId == "b2");
    CHECK(entries[0].nodeId == "folder-b");
    CHECK(entries[1].trackId == "b1");
    CHECK(entries[1].nodeId == "folder-b");
  }
}

TEST_CASE("PlayNextTrack rejects unknown target and missing identity") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  const auto missing = fixture.reducer.reduceCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::PlayNextTrack,
      .track = TrackIdentity{.trackId = "ghost", .filePath = "/music/ghost.flac"},
  });
  CHECK_FALSE(missing.result.accepted);
  CHECK(missing.result.code == MediaControllerErrorCode::TrackNotInLibrary);

  const auto noIdentity = fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::PlayNextTrack});
  CHECK_FALSE(noIdentity.result.accepted);
  CHECK(noIdentity.result.code == MediaControllerErrorCode::InvalidCommand);

  CHECK(fixture.reducer.playerState().queueEntries.empty());
}

TEST_CASE("queue track plays after current ends then folder sequence resumes from frozen index") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  fixture.playNextTrack("b1", "/music/b/b1.flac");
  CHECK(fixture.reducer.playerState().queueEntries.size() == 1U);

  // A 文件夹歌曲1 播完 → 消费队列：播放 B 歌曲1，文件夹序列 index 冻结（仍指向 a1）。
  fixture.reducer.reduceAudioEvent(fixture.endedEvent("a1", "/music/a/a1.flac"));
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "b1");
  CHECK(fixture.reducer.playerState().queueEntries.empty());

  // 队列曲播完 → 队列空 → 从原 index（a1=0）的下一曲继续：a2（而非 a3）。
  fixture.reducer.reduceAudioEvent(fixture.endedEvent("b1", "/music/b/b1.flac"));
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "a2");
}

TEST_CASE("SkipNext consumes queue first then resumes from frozen index") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  fixture.playNextTrack("b1", "/music/b/b1.flac");

  // next 命令：先消费队列头部（index 冻结），不推进文件夹序列。
  const auto skip = fixture.skipNext();
  REQUIRE(skip.result.accepted);
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "b1");
  CHECK(fixture.reducer.playerState().queueEntries.empty());

  // 队列已空：队列曲播完后由文件夹序列从冻结 index（a1=0）接续：a2。
  fixture.reducer.reduceAudioEvent(fixture.endedEvent("b1", "/music/b/b1.flac"));
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "a2");
}

TEST_CASE("ClearPlayQueue empties queue and skips to folder next") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  fixture.playNextTrack("b1", "/music/b/b1.flac");
  fixture.playNextTrack("b2", "/music/b/b2.flac");
  CHECK(fixture.reducer.playerState().queueEntries.size() == 2U);

  const auto clear = fixture.clearQueue();
  REQUIRE(clear.result.accepted);
  CHECK(fixture.reducer.playerState().queueEntries.empty());

  // 幂等：空队列再次 Clear 成功。
  const auto clearAgain = fixture.clearQueue();
  REQUIRE(clearAgain.result.accepted);

  // 队列已清空：当前曲播完直接进入文件夹序列下一曲。
  fixture.reducer.reduceAudioEvent(fixture.endedEvent("a1", "/music/a/a1.flac"));
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "a2");
}

TEST_CASE("RemoveFromQueue removes by index and is idempotent") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");

  fixture.playNextTrack("b1", "/music/b/b1.flac");  // [b1]
  fixture.playNextTrack("b2", "/music/b/b2.flac");  // [b2, b1]

  // 移除索引 1（b1），队列剩 [b2]。
  const auto remove1 = fixture.removeFromQueue(1);
  REQUIRE(remove1.result.accepted);
  {
    const auto& entries = fixture.reducer.playerState().queueEntries;
    REQUIRE(entries.size() == 1U);
    CHECK(entries[0].trackId == "b2");
  }

  // 移除索引 0（b2）→ 空。
  REQUIRE(fixture.removeFromQueue(0).result.accepted);
  CHECK(fixture.reducer.playerState().queueEntries.empty());

  // 幂等：空队列 / 越界索引均成功（no-op）。
  REQUIRE(fixture.removeFromQueue(0).result.accepted);
  REQUIRE(fixture.removeFromQueue(7).result.accepted);

  // 缺少索引 → InvalidCommand。
  const auto noIndex = fixture.reducer.reduceCommand(MediaControlCommand{.kind = MediaControlCommandKind::RemoveFromQueue});
  CHECK_FALSE(noIndex.result.accepted);
  CHECK(noIndex.result.code == MediaControllerErrorCode::InvalidCommand);
}

TEST_CASE("snapshot queueEntries stays empty on legacy snapshots and across restart") {
  // 旧快照（无 queueEntries 字段的初始化）解析不崩，默认为空 —— 序列化兼容。
  const PlayerStateSnapshot legacySnapshot{
      .freshness = {},
      .currentTrack = std::nullopt,
      .display = std::nullopt,
      .artwork = std::nullopt,
      .playback = {},
      .repeatMode = RepeatMode::Off,
      .shuffle = false,
      .capabilities = {},
      .timeline = {},
      .volume = 1.0F,
      .muted = false,
  };
  CHECK(legacySnapshot.queueEntries.empty());

  // 队列不持久化：新 reducer 实例（模拟重启）队列为空。
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");
  fixture.playNextTrack("b1", "/music/b/b1.flac");
  CHECK(fixture.reducer.playerState().queueEntries.size() == 1U);

  ReducerFixture restarted{};
  restarted.installLibrary();
  CHECK(restarted.reducer.playerState().queueEntries.empty());
}

TEST_CASE("unresolvable queue entry is skipped when library changed after enqueue") {
  ReducerFixture fixture{};
  fixture.installLibrary();
  fixture.selectTrack("a1", "/music/a/a1.flac");
  fixture.playNextTrack("b1", "/music/b/b1.flac");

  // 库更新：b1 被移除（版本 2）。reconcile 重建上下文（a1 仍在，index 不变），
  // 队列条目保留。
  auto treeWithoutB1 = makeLibrary();
  treeWithoutB1.version = 2;
  treeWithoutB1.nodes.erase(
      std::remove_if(treeWithoutB1.nodes.begin(), treeWithoutB1.nodes.end(),
                     [](const PlaylistNode& node) { return node.song.has_value() && node.song->trackId == "b1"; }),
      treeWithoutB1.nodes.end());
  fixture.reducer.reduceScannerEvent(scannerSnapshotEvent(std::move(treeWithoutB1), 2));

  // a1 播完 → 队列条目 b1 已不可解析 → 静默跳过 → 队列空 → 文件夹下一曲 a2。
  fixture.reducer.reduceAudioEvent(fixture.endedEvent("a1", "/music/a/a1.flac"));
  REQUIRE(fixture.reducer.playerState().currentTrack.has_value());
  CHECK(fixture.reducer.playerState().currentTrack->trackId == "a2");
  CHECK(fixture.reducer.playerState().queueEntries.empty());
}
