#include "control/playback_context_builder.h"

#include "seriona/scanner/playlist_tree_builder.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::control {
namespace {

[[nodiscard]] scanner::SongMetadata song(std::string trackId,
                                         std::string title,
                                         std::filesystem::path path,
                                         std::chrono::milliseconds duration = std::chrono::seconds{30}) {
  scanner::SongMetadata metadata{};
  metadata.trackId = std::move(trackId);
  metadata.title = std::move(title);
  metadata.filePath = path;
  metadata.sourceFilePath = path;
  metadata.duration = duration;
  metadata.logicalTrackId = metadata.trackId;
  return metadata;
}

[[nodiscard]] scanner::SongMetadata contextSong(std::string trackId,
                                                std::string title,
                                                std::string artist,
                                                std::string album,
                                                std::filesystem::path path,
                                                std::optional<std::uint32_t> trackNumber,
                                                std::optional<std::uint32_t> discNumber,
                                                std::optional<std::uint32_t> year,
                                                std::optional<std::chrono::milliseconds> duration,
                                                std::optional<std::filesystem::file_time_type> fileMtime) {
  scanner::SongMetadata metadata{};
  metadata.trackId = std::move(trackId);
  metadata.title = std::move(title);
  metadata.artist = std::move(artist);
  metadata.album = std::move(album);
  metadata.filePath = std::move(path);
  metadata.sourceFilePath = metadata.filePath;
  metadata.trackNumber = trackNumber;
  metadata.discNumber = discNumber;
  metadata.year = year;
  metadata.duration = duration;
  metadata.fileMtime = fileMtime;
  metadata.logicalTrackId = metadata.trackId;
  return metadata;
}

[[nodiscard]] const scanner::PlaylistNode& requireNode(const scanner::PlaylistTreeSnapshot& snapshot,
                                                       std::string_view nodeId) {
  const auto iterator = std::ranges::find(snapshot.nodes, nodeId, &scanner::PlaylistNode::nodeId);
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] scanner::PlaylistNode directoryNode(std::string nodeId,
                                                  std::optional<std::string> parentNodeId,
                                                  std::string displayName,
                                                  std::vector<std::string> children) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::move(parentNodeId),
                               .kind = scanner::PlaylistNodeKind::Directory,
                               .displayName = std::move(displayName),
                               .song = std::nullopt,
                               .childNodeIds = std::move(children)};
}

[[nodiscard]] scanner::PlaylistNode rootNode(std::vector<std::string> children) {
  auto root = directoryNode("root:.", std::nullopt, "Library", std::move(children));
  root.kind = scanner::PlaylistNodeKind::Root;
  return root;
}

[[nodiscard]] scanner::PlaylistNode trackNode(std::string nodeId,
                                              std::string parentNodeId,
                                              scanner::SongMetadata metadata) {
  return scanner::PlaylistNode{.nodeId = std::move(nodeId),
                               .parentNodeId = std::move(parentNodeId),
                               .kind = scanner::PlaylistNodeKind::Track,
                               .displayName = metadata.title,
                               .song = std::move(metadata),
                               .childNodeIds = {}};
}

[[nodiscard]] std::filesystem::file_time_type fileTimeAtSeconds(const int seconds) {
  return std::filesystem::file_time_type{} + std::chrono::seconds{seconds};
}

[[nodiscard]] scanner::PlaylistTreeSnapshot playbackTreeFixture() {
  scanner::PlaylistTreeSnapshot snapshot{};
  snapshot.version = 7;
  snapshot.rootNodeId = "root:.";
  snapshot.nodes = {rootNode({"dir:albums", "track:loose", "dir:empty"}),
                    directoryNode("dir:albums", "root:.", "albums", {"track:beta", "dir:albums/live", "track:alpha"}),
                    directoryNode("dir:albums/live", "dir:albums", "live", {"track:gamma"}),
                    directoryNode("dir:empty", "root:.", "empty", {}),
                    trackNode("track:beta",
                              "dir:albums",
                              contextSong("beta",
                                          "Beta",
                                          {},
                                          "Studio",
                                          "albums/beta.flac",
                                          2U,
                                          1U,
                                          2020U,
                                          std::chrono::seconds{180},
                                          fileTimeAtSeconds(50))),
                    trackNode("track:gamma",
                              "dir:albums/live",
                              contextSong("gamma",
                                          "Gamma",
                                          "Artist C",
                                          {},
                                          "albums/live/gamma.flac",
                                          std::nullopt,
                                          2U,
                                          2019U,
                                          std::chrono::seconds{60},
                                          fileTimeAtSeconds(10))),
                    trackNode("track:alpha",
                              "dir:albums",
                              contextSong("alpha",
                                          "Alpha",
                                          "Artist A",
                                          "Studio",
                                          "albums/alpha.flac",
                                          1U,
                                          std::nullopt,
                                          std::nullopt,
                                          std::chrono::seconds{120},
                                          fileTimeAtSeconds(30))),
                    trackNode("track:loose",
                              "root:.",
                              contextSong("loose",
                                          "Loose",
                                          "Artist B",
                                          "Singles",
                                          "loose.flac",
                                          3U,
                                          1U,
                                          2021U,
                                          std::nullopt,
                                          std::nullopt))};
  return snapshot;
}

[[nodiscard]] TrackIdentity track(std::string trackId, std::filesystem::path filePath) {
  return TrackIdentity{.trackId = std::move(trackId), .filePath = std::move(filePath), .sourceId = {}, .libraryId = {}};
}

[[nodiscard]] PlaybackContextDescriptor rootContext(TrackIdentity anchor,
                                                    std::vector<FolderSortRule> rules = {}) {
  return PlaybackContextDescriptor{.scope = PlaybackContextScope::Root,
                                   .rootPath = "/library-a",
                                   .folderNodeId = {},
                                   .anchorTrack = std::move(anchor),
                                   .sortRules = std::move(rules)};
}

[[nodiscard]] PlaybackContextDescriptor folderContext(std::string folderNodeId,
                                                      TrackIdentity anchor,
                                                      std::vector<FolderSortRule> rules = {},
                                                      std::filesystem::path rootPath = "/library-a") {
  return PlaybackContextDescriptor{.scope = PlaybackContextScope::Folder,
                                   .rootPath = std::move(rootPath),
                                   .folderNodeId = std::move(folderNodeId),
                                   .anchorTrack = std::move(anchor),
                                   .sortRules = std::move(rules)};
}

[[nodiscard]] std::vector<std::pair<std::string, std::vector<std::string>>> childOrders(
    const scanner::PlaylistTreeSnapshot& snapshot) {
  std::vector<std::pair<std::string, std::vector<std::string>>> orders;
  orders.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes) {
    orders.emplace_back(node.nodeId, node.childNodeIds);
  }
  std::ranges::sort(orders, {}, &std::pair<std::string, std::vector<std::string>>::first);
  return orders;
}

[[nodiscard]] std::vector<std::string> trackIds(const std::vector<PlaybackContextOrderItem>& order) {
  std::vector<std::string> ids;
  ids.reserve(order.size());
  for (const auto& item : order) {
    ids.push_back(item.identity.trackId);
  }
  return ids;
}

void requireReadyOrder(const PlaybackContextBuildResult& result,
                       const std::vector<std::string>& expectedTrackIds,
                       const std::size_t expectedAnchorIndex) {
  CHECK(result.status == PlaybackContextBuildStatus::Ready);
  CHECK(trackIds(result.order) == expectedTrackIds);
  REQUIRE(result.anchorIndex.has_value());
  CHECK(*result.anchorIndex == expectedAnchorIndex);
}

}

TEST_CASE("playback context builder baseline pins scanner tree child order facts") {
  scanner::PlaylistTreeBuilder builder{"Music"};
  builder.addDirectory({.relativePath = "empty", .displayName = "empty"});
  builder.addSong({.relativePath = "albums/zeta.flac", .metadata = song("zeta", "Zeta", "albums/zeta.flac")});
  builder.addSong({.relativePath = "loose.flac", .metadata = song("loose", "Loose", "loose.flac")});
  builder.addSong({.relativePath = "albums/live/coda.flac", .metadata = song("coda", "Coda", "albums/live/coda.flac")});
  builder.addSong({.relativePath = "albums/alpha.flac", .metadata = song("alpha", "Alpha", "albums/alpha.flac")});

  const auto snapshot = builder.publish();

  REQUIRE(snapshot.rootNodeId.has_value());
  const auto& root = requireNode(snapshot, *snapshot.rootNodeId);
  const std::vector<std::string> expectedRootOrder{"dir:albums", "track:loose.flac"};
  CHECK(root.childNodeIds == expectedRootOrder);

  const auto& albums = requireNode(snapshot, "dir:albums");
  const std::vector<std::string> expectedAlbumsOrder{"dir:albums/live", "track:albums/alpha.flac", "track:albums/zeta.flac"};
  CHECK(albums.childNodeIds == expectedAlbumsOrder);

  const auto& live = requireNode(snapshot, "dir:albums/live");
  const std::vector<std::string> expectedLiveOrder{"track:albums/live/coda.flac"};
  CHECK(live.childNodeIds == expectedLiveOrder);
  CHECK(std::ranges::none_of(snapshot.nodes, [](const scanner::PlaylistNode& node) { return node.nodeId == "dir:empty"; }));
}

TEST_CASE("playback context builder derives root order from scanner tree without mutating child ids") {
  auto snapshot = playbackTreeFixture();
  const auto before = childOrders(snapshot);

  const auto result = buildPlaybackContextOrder(snapshot, rootContext(track("gamma", "albums/live/gamma.flac")));

  requireReadyOrder(result, {"beta", "gamma", "alpha", "loose"}, 1U);
  CHECK(result.context.rootPath == std::filesystem::path{"/library-a"});
  CHECK(result.context.scope == PlaybackContextScope::Root);
  CHECK(childOrders(snapshot) == before);
}

TEST_CASE("playback context builder derives folder and nested folder contexts") {
  const auto snapshot = playbackTreeFixture();

  const auto folder = buildPlaybackContextOrder(
      snapshot, folderContext("dir:albums", track("alpha", "albums/alpha.flac"), {}, "/library-a"));
  requireReadyOrder(folder, {"beta", "gamma", "alpha"}, 2U);
  CHECK(folder.context.rootPath == std::filesystem::path{"/library-a"});
  CHECK(folder.context.folderNodeId == "dir:albums");

  const auto sameNodeDifferentRoot = buildPlaybackContextOrder(
      snapshot, folderContext("dir:albums", track("beta", "albums/beta.flac"), {}, "/library-b"));
  requireReadyOrder(sameNodeDifferentRoot, {"beta", "gamma", "alpha"}, 0U);
  CHECK(sameNodeDifferentRoot.context.rootPath == std::filesystem::path{"/library-b"});
  CHECK(sameNodeDifferentRoot.context.folderNodeId == "dir:albums");

  const auto nested = buildPlaybackContextOrder(
      snapshot, folderContext("dir:albums/live", track("gamma", "albums/live/gamma.flac")));
  requireReadyOrder(nested, {"gamma"}, 0U);
}

TEST_CASE("playback context builder returns typed empty or missing context outcomes without root fallback") {
  const auto snapshot = playbackTreeFixture();

  auto missingAnchor = rootContext(track("loose", "loose.flac"));
  missingAnchor.anchorTrack = std::nullopt;
  const auto invalidMissingAnchor = buildPlaybackContextOrder(snapshot, missingAnchor);
  CHECK(invalidMissingAnchor.status == PlaybackContextBuildStatus::InvalidDescriptor);
  CHECK(invalidMissingAnchor.order.empty());

  auto missingRootPath = rootContext(track("loose", "loose.flac"));
  missingRootPath.rootPath.clear();
  const auto invalidMissingRootPath = buildPlaybackContextOrder(snapshot, missingRootPath);
  CHECK(invalidMissingRootPath.status == PlaybackContextBuildStatus::InvalidDescriptor);
  CHECK(invalidMissingRootPath.order.empty());

  const auto empty = buildPlaybackContextOrder(snapshot, folderContext("dir:empty", track("loose", "loose.flac")));
  CHECK(empty.status == PlaybackContextBuildStatus::EmptyContext);
  CHECK(empty.order.empty());
  CHECK_FALSE(empty.anchorIndex.has_value());

  const auto missingFolder = buildPlaybackContextOrder(snapshot, folderContext("dir:missing", track("loose", "loose.flac")));
  CHECK(missingFolder.status == PlaybackContextBuildStatus::ContextNotFound);
  CHECK(missingFolder.order.empty());
  CHECK_FALSE(missingFolder.anchorIndex.has_value());

  auto noRoot = snapshot;
  noRoot.rootNodeId = std::nullopt;
  const auto missingRoot = buildPlaybackContextOrder(noRoot, rootContext(track("loose", "loose.flac")));
  CHECK(missingRoot.status == PlaybackContextBuildStatus::ContextNotFound);
  CHECK(missingRoot.order.empty());
}

TEST_CASE("playback context builder reports missing anchors within the requested scope only") {
  const auto snapshot = playbackTreeFixture();

  const auto result = buildPlaybackContextOrder(snapshot, folderContext("dir:albums", track("deleted", "deleted.flac")));

  CHECK(result.status == PlaybackContextBuildStatus::AnchorNotFound);
  CHECK(trackIds(result.order) == std::vector<std::string>{"beta", "gamma", "alpha"});
  CHECK_FALSE(result.anchorIndex.has_value());
  CHECK(std::ranges::none_of(result.order, [](const PlaybackContextOrderItem& item) { return item.identity.trackId == "loose"; }));
}

TEST_CASE("playback context builder applies sort fields directions and missing value policies") {
  const auto snapshot = playbackTreeFixture();

  const auto sortedFolderIds = [&](FolderSortRule rule) {
    const auto result = buildPlaybackContextOrder(
        snapshot, folderContext("dir:albums", track("alpha", "albums/alpha.flac"), {rule}));
    CHECK(result.status == PlaybackContextBuildStatus::Ready);
    return trackIds(result.order);
  };

  CHECK(sortedFolderIds({.field = FolderSortField::Title,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"alpha", "beta", "gamma"});
  CHECK(sortedFolderIds({.field = FolderSortField::Title,
                         .direction = FolderSortDirection::Descending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"gamma", "beta", "alpha"});
  CHECK(sortedFolderIds({.field = FolderSortField::Artist,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::First}) ==
        std::vector<std::string>{"beta", "alpha", "gamma"});
  CHECK(sortedFolderIds({.field = FolderSortField::Album,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"beta", "alpha", "gamma"});
  CHECK(sortedFolderIds({.field = FolderSortField::Filename,
                         .direction = FolderSortDirection::Descending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"gamma", "beta", "alpha"});
  CHECK(sortedFolderIds({.field = FolderSortField::Year,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::First}) ==
        std::vector<std::string>{"alpha", "gamma", "beta"});
  CHECK(sortedFolderIds({.field = FolderSortField::Duration,
                         .direction = FolderSortDirection::Descending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"beta", "alpha", "gamma"});
  CHECK(sortedFolderIds({.field = FolderSortField::CreatedDate,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"gamma", "alpha", "beta"});
  CHECK(sortedFolderIds({.field = FolderSortField::DiscNumber,
                         .direction = FolderSortDirection::Descending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"gamma", "beta", "alpha"});
  CHECK(sortedFolderIds({.field = FolderSortField::TrackNumber,
                         .direction = FolderSortDirection::Ascending,
                         .missingValuePolicy = FolderSortMissingValuePolicy::Last}) ==
        std::vector<std::string>{"alpha", "beta", "gamma"});

  const auto rootDuration = buildPlaybackContextOrder(
      snapshot,
      rootContext(track("beta", "albums/beta.flac"),
                  {{.field = FolderSortField::Duration,
                    .direction = FolderSortDirection::Descending,
                    .missingValuePolicy = FolderSortMissingValuePolicy::Last}}));
  requireReadyOrder(rootDuration, {"beta", "alpha", "gamma", "loose"}, 0U);
}

TEST_CASE("playback context builder uses unsorted tree order as tie fallback for multiple sort rules") {
  const auto snapshot = playbackTreeFixture();

  const auto result = buildPlaybackContextOrder(
      snapshot,
      folderContext("dir:albums",
                    track("beta", "albums/beta.flac"),
                    {{.field = FolderSortField::Album,
                      .direction = FolderSortDirection::Ascending,
                      .missingValuePolicy = FolderSortMissingValuePolicy::Last},
                     {.field = FolderSortField::TrackNumber,
                      .direction = FolderSortDirection::Descending,
                      .missingValuePolicy = FolderSortMissingValuePolicy::Last}}));

  requireReadyOrder(result, {"beta", "alpha", "gamma"}, 0U);
}

}
