#include "seriona/metadata/metadata_contracts.h"

#include <chrono>
#include <memory>

namespace seriona::metadata {

MetadataSyncResult metadataServiceDefaultResult() {
  return MetadataSyncResult{};
}

MetadataSyncResult metadataServiceSynchronize(const control::PlayerStateSnapshot& snapshot) {
  return MetadataSyncResult{.accepted = true,
                            .changed = snapshot.freshness.version > 0U,
                            .state = PlatformMediaState{.controlState = snapshot,
                                                        .timelineUpdateInterval = std::chrono::milliseconds{1000}},
                            .errorCode = std::nullopt,
                            .message = {}};
}

std::unique_ptr<MetadataSharingService> makeMetadataSharingService(const MetadataSharingOptions& options);

}
