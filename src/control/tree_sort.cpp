#include "tree_sort.h"

#include "text_collation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace seriona::control {
namespace {

struct TextValue {
  bool missing{true};
  std::string value{};
};

struct NumberValue {
  bool missing{true};
  std::int64_t value{0};
};

using SortValue = std::variant<TextValue, NumberValue>;

[[nodiscard]] bool isBlank(const std::string& value) {
  return std::ranges::all_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
}

[[nodiscard]] TextValue textValue(std::string value) {
  if (isBlank(value)) {
    return TextValue{};
  }
  return TextValue{.missing = false, .value = std::move(value)};
}

[[nodiscard]] NumberValue numberValue(std::optional<std::int64_t> value) {
  if (!value.has_value()) {
    return NumberValue{};
  }
  return NumberValue{.missing = false, .value = *value};
}

[[nodiscard]] std::string filenameOf(const std::filesystem::path& path) {
  const auto filename = path.filename().generic_u8string();
  return {filename.begin(), filename.end()};
}

// 字段语义与前端一致（前端为当前可见行为的事实来源）：曲目标题缺标签时回退显示名，
// 文件夹的 title/filename 用 displayName，其余字段视作缺失。
[[nodiscard]] std::string titleOf(const scanner::PlaylistNode& node) {
  if (node.song.has_value() && !node.song->title.empty()) {
    return node.song->title;
  }
  return node.displayName;
}

[[nodiscard]] std::string filenameOfNode(const scanner::PlaylistNode& node) {
  if (!node.song.has_value()) {
    return node.displayName;
  }
  auto filename = filenameOf(node.song->filePath);
  if (filename.empty()) {
    return node.displayName;
  }
  return filename;
}

[[nodiscard]] SortValue sortValueFor(const scanner::PlaylistNode& node, FolderSortField field) {
  switch (field) {
    case FolderSortField::Title:
      return textValue(titleOf(node));
    case FolderSortField::Artist:
      return node.song.has_value() ? textValue(node.song->artist) : TextValue{};
    case FolderSortField::Album:
      return node.song.has_value() ? textValue(node.song->album) : TextValue{};
    case FolderSortField::Filename:
      return textValue(filenameOfNode(node));
    case FolderSortField::Year:
      return numberValue(node.song.has_value() && node.song->year.has_value()
                             ? std::optional<std::int64_t>{static_cast<std::int64_t>(*node.song->year)}
                             : std::nullopt);
    case FolderSortField::Duration:
      return numberValue(node.song.has_value() && node.song->duration.has_value()
                             ? std::optional<std::int64_t>{node.song->duration->count()}
                             : std::nullopt);
    case FolderSortField::CreatedDate:
      return numberValue(node.song.has_value() && node.song->fileMtime.has_value()
                             ? std::optional<std::int64_t>{static_cast<std::int64_t>(
                                   node.song->fileMtime->time_since_epoch().count())}
                             : std::nullopt);
    case FolderSortField::DiscNumber:
      return numberValue(node.song.has_value() && node.song->discNumber.has_value()
                             ? std::optional<std::int64_t>{static_cast<std::int64_t>(*node.song->discNumber)}
                             : std::nullopt);
    case FolderSortField::TrackNumber:
      return numberValue(node.song.has_value() && node.song->trackNumber.has_value()
                             ? std::optional<std::int64_t>{static_cast<std::int64_t>(*node.song->trackNumber)}
                             : std::nullopt);
  }
  return TextValue{};
}

[[nodiscard]] int compareMissing(bool leftMissing,
                                 bool rightMissing,
                                 FolderSortMissingValuePolicy policy) {
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
          const auto missingComparison =
              compareMissing(leftValue.missing, rightValue.missing, rule.missingValuePolicy);
          if (missingComparison != 0 || leftValue.missing || rightValue.missing) {
            return missingComparison;
          }

          int comparison = 0;
          if constexpr (std::is_same_v<LeftValue, TextValue>) {
            comparison = compareCollatedText(leftValue.value, rightValue.value);
          } else {
            if (leftValue.value < rightValue.value) {
              comparison = -1;
            } else if (rightValue.value < leftValue.value) {
              comparison = 1;
            }
          }
          if (rule.direction == FolderSortDirection::Descending) {
            comparison = -comparison;
          }
          return comparison;
        }
      },
      left,
      right);
}

[[nodiscard]] int compareByRule(const scanner::PlaylistNode& left,
                                const scanner::PlaylistNode& right,
                                const FolderSortRule& rule) {
  return compareSortValue(sortValueFor(left, rule.field), sortValueFor(right, rule.field), rule);
}

}

void sortTreeChildOrderByRules(scanner::PlaylistTreeSnapshot& tree,
                               const std::map<std::string, std::vector<FolderSortRule>>& rulesByFolderNodeId) {
  if (rulesByFolderNodeId.empty() || tree.nodes.empty()) {
    return;
  }

  std::unordered_map<std::string, const scanner::PlaylistNode*> nodes;
  nodes.reserve(tree.nodes.size());
  for (const auto& node : tree.nodes) {
    nodes.emplace(node.nodeId, &node);
  }

  // 根级规则与文件夹规则共用一张表，根级的键是空串（前端约定），此处归一到根节点 id。
  const auto rulesFor = [&](const std::string& nodeId) -> const std::vector<FolderSortRule>* {
    if (const auto it = rulesByFolderNodeId.find(nodeId); it != rulesByFolderNodeId.end()) {
      return &it->second;
    }
    if (tree.rootNodeId.has_value() && nodeId == *tree.rootNodeId) {
      if (const auto it = rulesByFolderNodeId.find(std::string{}); it != rulesByFolderNodeId.end()) {
        return &it->second;
      }
    }
    return nullptr;
  };

  for (auto& node : tree.nodes) {
    const auto* rules = rulesFor(node.nodeId);
    if (rules == nullptr || rules->empty() || node.childNodeIds.size() < 2) {
      continue;
    }

    std::stable_sort(node.childNodeIds.begin(),
                     node.childNodeIds.end(),
                     [&](const std::string& leftId, const std::string& rightId) {
                       const auto leftIt = nodes.find(leftId);
                       const auto rightIt = nodes.find(rightId);
                       if (leftIt == nodes.end() || rightIt == nodes.end()) {
                         return false;
                       }
                       for (const auto& rule : *rules) {
                         const auto comparison = compareByRule(*leftIt->second, *rightIt->second, rule);
                         if (comparison != 0) {
                           return comparison < 0;
                         }
                       }
                       return false;
                     });
  }
}

}
