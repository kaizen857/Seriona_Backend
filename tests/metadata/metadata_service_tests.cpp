#include <doctest/doctest.h>

#include "seriona/metadata/metadata_contracts.h"

namespace seriona::metadata {
MetadataSyncResult metadataServiceDefaultResult();
}

TEST_CASE("metadata service exposes a concrete default sync result") {
  const auto result = seriona::metadata::metadataServiceDefaultResult();

  CHECK_FALSE(result.accepted);
  CHECK_FALSE(result.changed);
  CHECK(result.errorCode == std::nullopt);
}
