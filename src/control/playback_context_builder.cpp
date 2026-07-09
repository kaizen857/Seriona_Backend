#include "playback_context_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace seriona::control {
namespace {

using NodeIndex = std::unordered_map<std::string, const scanner::PlaylistNode*>;

struct StringSortValue {
  bool missing{true};
  std::string value{};
};

struct NumberSortValue {
  bool missing{true};
  std::int64_t value{0};
};

using SortValue = std::variant<StringSortValue, NumberSortValue>;

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
    const auto treeOrderIndex = order.size();
    order.push_back(PlaybackContextOrderItem{.identity = identityFromSong(*node->song),
                                             .metadata = *node->song,
                                             .nodeId = node->nodeId,
                                             .parentNodeId = node->parentNodeId,
                                             .treeOrderIndex = treeOrderIndex});
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

[[nodiscard]] bool sameTrack(const TrackIdentity& left, const TrackIdentity& right) {
  return !left.trackId.empty() && left.trackId == right.trackId && (right.filePath.empty() || left.filePath == right.filePath);
}

[[nodiscard]] StringSortValue textValue(std::string value) {
  const auto missing = value.empty();
  return StringSortValue{.missing = missing, .value = std::move(value)};
}

[[nodiscard]] NumberSortValue numberValue(std::optional<std::int64_t> value) {
  if (!value.has_value()) {
    return NumberSortValue{};
  }
  return NumberSortValue{.missing = false, .value = *value};
}

[[nodiscard]] std::string filenameOf(const std::filesystem::path& path) {
  const auto filename = path.filename().generic_u8string();
  return {filename.begin(), filename.end()};
}

[[nodiscard]] SortValue sortValueFor(const PlaybackContextOrderItem& item, const FolderSortField field) {
  const auto& song = item.metadata;
  switch (field) {
    case FolderSortField::Title:
      return textValue(song.title);
    case FolderSortField::Artist:
      return textValue(song.artist);
    case FolderSortField::Album:
      return textValue(song.album);
    case FolderSortField::Filename:
      return textValue(filenameOf(song.filePath));
    case FolderSortField::Year:
      return numberValue(song.year.transform([](const std::uint32_t value) { return static_cast<std::int64_t>(value); }));
    case FolderSortField::Duration:
      return numberValue(song.duration.transform([](const std::chrono::milliseconds value) { return value.count(); }));
    case FolderSortField::CreatedDate:
      return numberValue(song.fileMtime.transform([](const std::filesystem::file_time_type value) {
        return static_cast<std::int64_t>(value.time_since_epoch().count());
      }));
    case FolderSortField::DiscNumber:
      return numberValue(song.discNumber.transform([](const std::uint32_t value) { return static_cast<std::int64_t>(value); }));
    case FolderSortField::TrackNumber:
      return numberValue(song.trackNumber.transform([](const std::uint32_t value) { return static_cast<std::int64_t>(value); }));
  }
  return textValue({});
}

template <typename T>
[[nodiscard]] int comparePresentValues(const T& left, const T& right) {
  if (left < right) {
    return -1;
  }
  if (right < left) {
    return 1;
  }
  return 0;
}

[[nodiscard]] int compareMissing(const bool leftMissing,
                                 const bool rightMissing,
                                 const FolderSortMissingValuePolicy policy) {
  if (leftMissing == rightMissing) {
    return 0;
  }
  const auto leftBefore = policy == FolderSortMissingValuePolicy::First;
  return leftMissing == leftBefore ? -1 : 1;
}

[[nodiscard]] int compareSortValue(const SortValue& left,
                                   const SortValue& right,
                                   const FolderSortRule& rule) {
  return std::visit(
      [&](const auto& leftValue, const auto& rightValue) {
        using LeftValue = std::decay_t<decltype(leftValue)>;
        using RightValue = std::decay_t<decltype(rightValue)>;
        if constexpr (!std::is_same_v<LeftValue, RightValue>) {
          return 0;
        } else {
          const auto missingComparison = compareMissing(leftValue.missing, rightValue.missing, rule.missingValuePolicy);
          if (missingComparison != 0 || leftValue.missing || rightValue.missing) {
            return missingComparison;
          }

          auto comparison = comparePresentValues(leftValue.value, rightValue.value);
          if (rule.direction == FolderSortDirection::Descending) {
            comparison = -comparison;
          }
          return comparison;
        }
      },
      left,
      right);
}

[[nodiscard]] int compareByRule(const PlaybackContextOrderItem& left,
                                const PlaybackContextOrderItem& right,
                                const FolderSortRule& rule) {
  return compareSortValue(sortValueFor(left, rule.field), sortValueFor(right, rule.field), rule);
}

void sortOrder(std::vector<PlaybackContextOrderItem>& order, const std::vector<FolderSortRule>& rules) {
  if (rules.empty()) {
    return;
  }
  std::ranges::sort(order, [&](const PlaybackContextOrderItem& left, const PlaybackContextOrderItem& right) {
    for (const auto& rule : rules) {
      const auto comparison = compareByRule(left, right, rule);
      if (comparison != 0) {
        return comparison < 0;
      }
    }
    return left.treeOrderIndex < right.treeOrderIndex;
  });
}

void setAnchorStatus(PlaybackContextBuildResult& result) {
  const auto anchor = result.context.anchorTrack;
  if (!anchor.has_value()) {
    result.status = PlaybackContextBuildStatus::InvalidDescriptor;
    return;
  }

  const auto anchorIterator = std::ranges::find_if(result.order, [&](const PlaybackContextOrderItem& item) {
    return sameTrack(item.identity, *anchor);
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

  appendTracksDepthFirst(nodes, contextNodeId, result.order);
  if (result.order.empty()) {
    result.status = PlaybackContextBuildStatus::EmptyContext;
    return result;
  }

  sortOrder(result.order, result.context.sortRules);
  setAnchorStatus(result);
  return result;
}

}
