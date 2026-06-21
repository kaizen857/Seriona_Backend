#pragma once

#include "seriona/metadata/metadata_contracts.h"

#include <memory>
#include <vector>

namespace seriona::metadata {

struct MetadataServiceTestHooks {
  bool failStart{false};
  bool failStop{false};
  bool failUpdate{false};
  bool hasPlatformHost{false};
  std::size_t startCalls{0};
  std::size_t updateCalls{0};
  std::size_t stopCalls{0};
  std::size_t commandRegistrations{0};
  std::size_t commandUnregistrations{0};
  std::vector<MetadataSyncResult> results{};
};

[[nodiscard]] std::shared_ptr<MetadataServiceTestHooks> makeMetadataServiceTestHooks();
[[nodiscard]] std::unique_ptr<MetadataSharingService> makeRecordingMetadataSharingService(
    const MetadataSharingOptions& options, const std::shared_ptr<MetadataServiceTestHooks>& hooks);

}
