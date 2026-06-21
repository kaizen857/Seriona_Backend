#include <doctest/doctest.h>

#include <cstring>

#include "metadata_module.h"

TEST_CASE("metadata module exposes a stable scaffold name") {
  const auto* name = seriona::metadata::moduleName();

  REQUIRE(name != nullptr);
  CHECK(std::strcmp(name, "seriona_metadata") == 0);
}
