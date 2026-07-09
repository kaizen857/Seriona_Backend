#pragma once

#include "seriona/control/control_contracts.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace seriona::control {

enum class PlaybackContextBuildStatus {
  Ready,
  InvalidDescriptor,
  ContextNotFound,
  EmptyContext,
  AnchorNotFound,
};

struct PlaybackContextOrderItem {
  TrackIdentity identity;
  scanner::SongMetadata metadata;
  std::string nodeId;
  std::optional<std::string> parentNodeId;
  std::size_t treeOrderIndex{0};
};

struct PlaybackContextBuildResult {
  PlaybackContextBuildStatus status{PlaybackContextBuildStatus::InvalidDescriptor};
  PlaybackContextDescriptor context{};
  std::vector<PlaybackContextOrderItem> order{};
  std::optional<std::size_t> anchorIndex{};
};

[[nodiscard]] PlaybackContextBuildResult buildPlaybackContextOrder(const scanner::PlaylistTreeSnapshot& snapshot,
                                                                   PlaybackContextDescriptor descriptor);

}
