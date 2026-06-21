#pragma once

#include "metadata_mapper.h"

#include <chrono>
#include <optional>

namespace seriona::metadata {

struct MetadataSyncPlan {
  bool emitMetadata{false};
  bool emitTimeline{false};
  MetadataPlatformSnapshotDto snapshot{};
};

class MetadataSynchronizer {
public:
  [[nodiscard]] MetadataSyncPlan synchronize(const control::PlayerStateSnapshot& snapshot);

private:
  [[nodiscard]] static bool isFreshEnough(const control::PlayerStateSnapshot& snapshot,
                                          const std::optional<control::SnapshotFreshness>& lastFreshness);
  [[nodiscard]] static bool staticMetadataChanged(const MetadataPlatformSnapshotDto& current,
                                                  const MetadataPlatformSnapshotDto& previous);
  [[nodiscard]] static bool timelineChanged(const MetadataPlatformSnapshotDto& current,
                                            const MetadataPlatformSnapshotDto& previous);
  [[nodiscard]] static bool shouldEmitTimelineImmediately(const control::PlayerStateSnapshot& current,
                                                          const std::optional<control::PlayerStateSnapshot>& previous);

  std::optional<control::SnapshotFreshness> lastFreshness_{};
  std::optional<control::PlayerStateSnapshot> lastSnapshot_{};
  std::optional<MetadataPlatformSnapshotDto> lastStaticSnapshot_{};
  std::optional<MetadataPlatformSnapshotDto> lastTimelineSnapshot_{};
  std::chrono::steady_clock::time_point lastTimelineEmitAt_{};
  bool hasTimelineEmitAt_{false};
};

}
