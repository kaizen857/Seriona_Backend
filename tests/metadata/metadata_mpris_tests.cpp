#include <doctest/doctest.h>

#include "seriona/metadata/metadata_contracts.h"

namespace seriona::metadata {
MetadataSyncResult metadataMprisSmokeResult();
}

TEST_CASE("metadata mpris smoke path returns a stable accepted result") {
  const auto result = seriona::metadata::metadataMprisSmokeResult();

  CHECK(result.accepted);
  CHECK_FALSE(result.changed);
}
