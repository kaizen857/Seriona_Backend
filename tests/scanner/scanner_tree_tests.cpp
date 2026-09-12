#include "seriona/scanner/playlist_tree_builder.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace seriona::scanner {
namespace {

[[nodiscard]] SongMetadata song(std::string title, std::filesystem::path path, std::chrono::milliseconds duration) {
  SongMetadata metadata{};
  metadata.title = std::move(title);
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.duration = duration;
  metadata.logicalTrackId = path.generic_string();
  return metadata;
}

[[nodiscard]] SongMetadata cueTrack(std::string title,
                                    std::filesystem::path cuePath,
                                    std::filesystem::path sourceAudioPath,
                                    std::uint32_t trackNumber) {
  SongMetadata metadata{};
  metadata.title = std::move(title);
  metadata.filePath = cuePath;
  metadata.sourceFilePath = std::move(sourceAudioPath);
  metadata.offset = std::chrono::seconds{static_cast<int>(trackNumber - 1U) * 60};
  metadata.duration = std::chrono::seconds{60};
  metadata.trackNumber = trackNumber;
  metadata.logicalTrackId = cuePath.generic_string() + "#track" + std::to_string(trackNumber - 1U);
  metadata.trackId = metadata.logicalTrackId;
  return metadata;
}

[[nodiscard]] const PlaylistNode& requireNode(const PlaylistTreeSnapshot& snapshot, const std::string_view nodeId) {
  const auto iterator = std::ranges::find(snapshot.nodes, nodeId, &PlaylistNode::nodeId);
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] const PlaylistNode& requireSong(const PlaylistTreeSnapshot& snapshot, const std::string_view title) {
  const auto iterator = std::ranges::find_if(snapshot.nodes, [&title](const PlaylistNode& node) {
    return node.song.has_value() && node.song->title == title;
  });
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

TEST_CASE("playlist tree builder publishes directory-first immutable snapshots with aggregate stats") {
  PlaylistTreeBuilder builder{"Music"};
  builder.addDirectory({.relativePath = "empty", .displayName = "empty"});
  builder.addSong({.relativePath = "artist/zeta.flac", .metadata = song("Zeta", "artist/zeta.flac", std::chrono::seconds{90})});
  builder.addSong({.relativePath = "artist/alpha.flac", .metadata = song("Alpha", "artist/alpha.flac", std::chrono::seconds{30})});
  builder.addSong({.relativePath = "loose.flac", .metadata = song("Loose", "loose.flac", std::chrono::seconds{10})});

  const auto first = builder.publish();
  const auto stats = builder.stats();

  REQUIRE(first.rootNodeId.has_value());
  const auto& root = requireNode(first, *first.rootNodeId);
  CHECK(root.kind == PlaylistNodeKind::Root);
  CHECK(root.displayName == "Music");
  CHECK(stats.songCount == 3U);
  CHECK(stats.totalDuration == std::chrono::seconds{130});
  CHECK(std::ranges::none_of(first.nodes, [](const PlaylistNode& node) { return node.displayName == "empty"; }));

  REQUIRE(root.childNodeIds.size() == 2U);
  CHECK(root.childNodeIds[0] == "dir:artist");
  CHECK(root.childNodeIds[1] == "track:loose.flac");

  const auto& artist = requireNode(first, "dir:artist");
  REQUIRE(artist.childNodeIds.size() == 2U);
  CHECK(artist.childNodeIds[0] == "track:artist/alpha.flac");
  CHECK(artist.childNodeIds[1] == "track:artist/zeta.flac");

  builder.addSong({.relativePath = "artist/beta.flac", .metadata = song("Beta", "artist/beta.flac", std::chrono::seconds{5})});
  const auto second = builder.publish();

  CHECK(second.version == first.version + 1U);
  CHECK(requireNode(first, "dir:artist").childNodeIds.size() == 2U);
  CHECK(requireNode(second, "dir:artist").childNodeIds.size() == 3U);
}

TEST_CASE("playlist tree builder attaches external lrc metadata to paired song without lrc nodes") {
  PlaylistTreeBuilder builder{"Music"};
  builder.addSong({.relativePath = "disc/song.flac", .metadata = song("Song", "disc/song.flac", std::chrono::seconds{42})});
  const auto lrcMtime = std::filesystem::file_time_type::clock::now();
  builder.attachExternalLyrics("disc/song.flac", "disc/song.lrc", "abc123", lrcMtime,
                               {LyricLine{std::chrono::seconds{1}, "line one"},
                                LyricLine{std::chrono::seconds{2}, "line two"}});

  const auto snapshot = builder.publish();
  const auto& node = requireSong(snapshot, "Song");

  REQUIRE(node.song.has_value());
  CHECK(node.song->effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(node.song->externalLyricsPath == std::filesystem::path{"disc/song.lrc"});
  CHECK(node.song->externalLyricsHash == "abc123");
  CHECK(node.song->externalLyricsMtime == lrcMtime);
  REQUIRE(node.song->effectiveLyrics.size() == 2U);
  CHECK(node.song->effectiveLyrics[0].text == "line one");
  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& candidate) {
    return candidate.nodeId.ends_with(".lrc") || candidate.displayName.ends_with(".lrc");
  }));
}

TEST_CASE("playlist tree builder keeps parent access by node id without shared ownership cycles") {
  PlaylistTreeBuilder builder{"Music"};
  builder.addSong({.relativePath = "artist/song.flac", .metadata = song("Song", "artist/song.flac", std::chrono::seconds{1})});

  const auto snapshot = builder.publish();
  const auto& songNode = requireSong(snapshot, "Song");
  REQUIRE(songNode.parentNodeId.has_value());
  const auto& parent = requireNode(snapshot, *songNode.parentNodeId);

  CHECK(parent.kind == PlaylistNodeKind::Directory);
  CHECK(parent.nodeId == "dir:artist");
  CHECK(std::ranges::find(parent.childNodeIds, songNode.nodeId) != parent.childNodeIds.end());
}

TEST_CASE("playlist tree builder prunes CueContainer with zero tracks from published snapshot") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "sets/live.cue";
  cueContainer.logicalTrackId = "sets/live.cue";
  cueContainer.duration = std::chrono::seconds{0};

  builder.addSong({.relativePath = "sets/live.cue", .metadata = cueContainer});
  builder.addSong({.relativePath = "sets/normal.flac", .metadata = song("Normal", "sets/normal.flac", std::chrono::seconds{15})});

  const auto snapshot = builder.publish();
  const auto& sets = requireNode(snapshot, "dir:sets");
  const auto& normal = requireSong(snapshot, "Normal");

  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:sets/live.cue"; }));
  CHECK(std::ranges::none_of(sets.childNodeIds, [](const std::string& nodeId) { return nodeId == "dir:sets/live.cue"; }));

  CHECK(normal.kind == PlaylistNodeKind::Track);
  CHECK(normal.nodeId == "track:sets/normal.flac");
  REQUIRE(normal.parentNodeId.has_value());
  CHECK(*normal.parentNodeId == sets.nodeId);
}

TEST_CASE("playlist tree builder re-materializes CueContainer when a cue track is upserted later") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "sets/live.cue";
  cueContainer.logicalTrackId = "sets/live.cue";

  builder.addSong({.relativePath = "sets/live.cue", .metadata = cueContainer});
  const auto emptySnapshot = builder.publish();
  CHECK(std::ranges::none_of(emptySnapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:sets/live.cue"; }));

  builder.upsertSong({.relativePath = "sets/live.cue", .metadata = cueTrack("Intro", "sets/live.cue", "sets/live.flac", 1U)});
  const auto populatedSnapshot = builder.publish();
  const auto& cue = requireNode(populatedSnapshot, "dir:sets/live.cue");
  const auto& track = requireSong(populatedSnapshot, "Intro");

  CHECK(cue.kind == PlaylistNodeKind::Directory);
  CHECK(cue.displayName == "live.cue");
  CHECK_FALSE(cue.song.has_value());
  REQUIRE(cue.parentNodeId.has_value());
  CHECK(*cue.parentNodeId == "dir:sets");
  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == cue.nodeId);
  CHECK(std::ranges::find(cue.childNodeIds, track.nodeId) != cue.childNodeIds.end());
}

TEST_CASE("playlist tree builder rejects remove and rename of a pruned CueContainer without touching siblings") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "sets/live.cue";
  cueContainer.logicalTrackId = "sets/live.cue";

  builder.addSong({.relativePath = "sets/live.cue", .metadata = cueContainer});
  builder.addSong({.relativePath = "sets/normal.flac", .metadata = song("Normal", "sets/normal.flac", std::chrono::seconds{15})});

  const auto prunedSnapshot = builder.publish();
  CHECK(std::ranges::none_of(prunedSnapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:sets/live.cue"; }));

  CHECK_FALSE(builder.removeSubtree("sets/live.cue"));
  CHECK_FALSE(builder.renameSubtree("sets/live.cue", "sets/moved.cue"));

  const auto snapshot = builder.publish();
  const auto& sets = requireNode(snapshot, "dir:sets");
  const auto& normal = requireSong(snapshot, "Normal");

  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:sets/live.cue"; }));
  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:sets/moved.cue"; }));
  CHECK(normal.nodeId == "track:sets/normal.flac");
  REQUIRE(normal.parentNodeId.has_value());
  CHECK(*normal.parentNodeId == sets.nodeId);
  REQUIRE(sets.childNodeIds.size() == 1U);
  CHECK(sets.childNodeIds[0] == normal.nodeId);
}

TEST_CASE("playlist tree builder cascades prune from a zero-track CueContainer to its empty parent directory") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "solo/live.cue";
  cueContainer.logicalTrackId = "solo/live.cue";

  builder.addSong({.relativePath = "solo/live.cue", .metadata = cueContainer});

  const auto snapshot = builder.publish();
  REQUIRE(snapshot.rootNodeId.has_value());
  const auto& root = requireNode(snapshot, *snapshot.rootNodeId);

  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:solo/live.cue"; }));
  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId == "dir:solo"; }));
  CHECK(std::ranges::none_of(root.childNodeIds, [](const std::string& nodeId) { return nodeId == "dir:solo"; }));
}

TEST_CASE("playlist tree builder nests CueTrack nodes under their CueContainer directory") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "sets/live.cue";
  cueContainer.logicalTrackId = "sets/live.cue";

  builder.addSong({.relativePath = "sets/live.cue", .metadata = cueContainer});
  builder.addSong({.relativePath = "sets/live.cue", .metadata = cueTrack("Intro", "sets/live.cue", "sets/live.flac", 1U)});
  builder.addSong({.relativePath = "sets/normal.flac", .metadata = song("Normal", "sets/normal.flac", std::chrono::seconds{15})});

  const auto snapshot = builder.publish();
  const auto& cue = requireNode(snapshot, "dir:sets/live.cue");
  const auto& track = requireSong(snapshot, "Intro");
  const auto& normal = requireSong(snapshot, "Normal");

  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == cue.nodeId);
  CHECK(std::ranges::find(cue.childNodeIds, track.nodeId) != cue.childNodeIds.end());

  REQUIRE(normal.parentNodeId.has_value());
  CHECK(*normal.parentNodeId == "dir:sets");
}

TEST_CASE("playlist tree builder keeps nested CueTrack nodes under nested CueContainer directory") {
  PlaylistTreeBuilder builder{"Music"};
  SongMetadata cueContainer{};
  cueContainer.filePath = "box/disc/live.cue";
  cueContainer.logicalTrackId = "box/disc/live.cue";

  builder.addSong({.relativePath = "box/disc/live.cue", .metadata = cueContainer});
  builder.addSong({.relativePath = "box/disc/live.cue", .metadata = cueTrack("Deep Cut", "box/disc/live.cue", "box/disc/live.flac", 2U)});

  const auto snapshot = builder.publish();
  const auto& cue = requireNode(snapshot, "dir:box/disc/live.cue");
  const auto& track = requireSong(snapshot, "Deep Cut");

  REQUIRE(cue.parentNodeId.has_value());
  CHECK(*cue.parentNodeId == "dir:box/disc");
  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == cue.nodeId);
  CHECK(std::ranges::find(cue.childNodeIds, track.nodeId) != cue.childNodeIds.end());
}

}
}
