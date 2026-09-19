#include "playback_context_builder.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace seriona::control {
namespace {

using NodeIndex = std::unordered_map<std::string, const scanner::PlaylistNode*>;

[[nodiscard]] NodeIndex indexNodes(const scanner::PlaylistTreeSnapshot& snapshot) {
  NodeIndex nodes;
  nodes.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes) {
    nodes.emplace(node.nodeId, &node);
  }
  return nodes;
}

[[nodiscard]] const scanner::PlaylistNode* findNode(const NodeIndex& nodes, const std::string& nodeId) {
  const auto iterator = nodes.find(nodeId);
  if (iterator == nodes.end()) {
    return nullptr;
  }
  return iterator->second;
}

[[nodiscard]] bool isContainerNode(const scanner::PlaylistNode& node) noexcept {
  return node.kind == scanner::PlaylistNodeKind::Root || node.kind == scanner::PlaylistNodeKind::Directory ||
         node.kind == scanner::PlaylistNodeKind::Album || node.kind == scanner::PlaylistNodeKind::Disc;
}

[[nodiscard]] bool isTrackNode(const scanner::PlaylistNode& node) noexcept {
  return node.kind == scanner::PlaylistNodeKind::Track && node.song.has_value();
}

[[nodiscard]] TrackIdentity identityFromSong(const scanner::SongMetadata& song) {
  return TrackIdentity{.trackId = song.trackId, .filePath = song.filePath, .sourceId = {}, .libraryId = {}};
}

void appendTracksDepthFirst(const NodeIndex& nodes,
                            const std::string& nodeId,
                            std::vector<PlaybackContextOrderItem>& order) {
  const auto* node = findNode(nodes, nodeId);
  if (node == nullptr) {
    return;
  }

  if (isTrackNode(*node)) {
    order.push_back(PlaybackContextOrderItem{.identity = identityFromSong(*node->song),
                                             .metadata = *node->song,
                                             .nodeId = node->nodeId,
                                             .parentNodeId = node->parentNodeId});
    return;
  }

  for (const auto& childNodeId : node->childNodeIds) {
    appendTracksDepthFirst(nodes, childNodeId, order);
  }
}

[[nodiscard]] bool descriptorIsValid(const PlaybackContextDescriptor& descriptor) {
  if (descriptor.rootPath.empty() || !descriptor.anchorTrack.has_value() || descriptor.anchorTrack->trackId.empty()) {
    return false;
  }
  if (descriptor.scope == PlaybackContextScope::Folder) {
    return !descriptor.folderNodeId.empty();
  }
  return descriptor.scope == PlaybackContextScope::Root;
}

void setAnchorStatus(PlaybackContextBuildResult& result) {
  const auto anchor = result.context.anchorTrack;
  if (!anchor.has_value()) {
    result.status = PlaybackContextBuildStatus::InvalidDescriptor;
    return;
  }

  const auto anchorIterator = std::ranges::find_if(result.order, [&](const PlaybackContextOrderItem& item) {
    return tracksMatch(item.identity, *anchor);
  });
  if (anchorIterator == result.order.end()) {
    result.status = PlaybackContextBuildStatus::AnchorNotFound;
    result.anchorIndex = std::nullopt;
    return;
  }

  result.status = PlaybackContextBuildStatus::Ready;
  result.anchorIndex = static_cast<std::size_t>(std::distance(result.order.begin(), anchorIterator));
}

}

bool tracksMatch(const TrackIdentity& lhs, const TrackIdentity& rhs) noexcept {
  return !lhs.trackId.empty() && lhs.trackId == rhs.trackId;
}

PlaybackContextBuildResult buildPlaybackContextOrder(const scanner::PlaylistTreeSnapshot& snapshot,
                                                     PlaybackContextDescriptor descriptor) {
  PlaybackContextBuildResult result{};
  result.context = std::move(descriptor);
  if (!descriptorIsValid(result.context)) {
    result.status = PlaybackContextBuildStatus::InvalidDescriptor;
    return result;
  }

  const auto nodes = indexNodes(snapshot);
  std::string contextNodeId;
  if (result.context.scope == PlaybackContextScope::Root) {
    if (!snapshot.rootNodeId.has_value()) {
      result.status = PlaybackContextBuildStatus::ContextNotFound;
      return result;
    }
    contextNodeId = *snapshot.rootNodeId;
  } else {
    contextNodeId = result.context.folderNodeId;
  }

  const auto* contextNode = findNode(nodes, contextNodeId);
  if (contextNode == nullptr || !isContainerNode(*contextNode)) {
    result.status = PlaybackContextBuildStatus::ContextNotFound;
    return result;
  }

  // 顺序即树序：文件夹内子节点顺序已由控制层按生效规则排序（决策⑦），纯 DFS 即得
  // “可见序”。此处绝不能再对平铺结果排序——那正是 R5 的结构性分叉（全局平铺排序 ≠
  // 逐层可见序），且会把已正确的树序再次打乱。
  appendTracksDepthFirst(nodes, contextNodeId, result.order);
  if (result.order.empty()) {
    result.status = PlaybackContextBuildStatus::EmptyContext;
    return result;
  }

  setAnchorStatus(result);
  return result;
}

}
