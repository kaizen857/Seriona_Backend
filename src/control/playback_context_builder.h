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
};

struct PlaybackContextBuildResult {
  PlaybackContextBuildStatus status{PlaybackContextBuildStatus::InvalidDescriptor};
  PlaybackContextDescriptor context{};
  std::vector<PlaybackContextOrderItem> order{};
  std::optional<std::size_t> anchorIndex{};
};

// 曲目身份判等的唯一实现（reducer 与 builder 共用）。以 trackId 为主键。
// 刻意不比对 filePath：CUE 派生曲的 TrackIdentity.filePath 是 .cue 路径，而
// audio::TrackPlaybackRequest.filePath 是实际音频路径，两者语义不同，纳入比对会把
// 同一曲目判为不同曲目（破坏 CUE 播放与曲目定位）。
[[nodiscard]] bool tracksMatch(const TrackIdentity& lhs, const TrackIdentity& rhs) noexcept;

[[nodiscard]] PlaybackContextBuildResult buildPlaybackContextOrder(const scanner::PlaylistTreeSnapshot& snapshot,
                                                                   PlaybackContextDescriptor descriptor);

}
