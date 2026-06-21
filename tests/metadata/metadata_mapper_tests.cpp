#include <doctest/doctest.h>

#include "seriona/metadata/metadata_contracts.h"

namespace seriona::metadata {
MetadataBackendCapabilities metadataMapperCapabilities();
}

TEST_CASE("metadata mapper exposes a concrete capability baseline") {
  const auto capabilities = seriona::metadata::metadataMapperCapabilities();

  CHECK_FALSE(capabilities.canPublishMetadata);
  CHECK_FALSE(capabilities.canPublishTimeline);
  CHECK_FALSE(capabilities.canReceiveCommands);
}
