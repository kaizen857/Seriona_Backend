#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>

#include "seriona/control/control_contracts.h"
#include "metadata_module.h"

TEST_CASE("metadata module exposes a stable scaffold name") {
  const auto* name = seriona::metadata::moduleName();

  REQUIRE(name != nullptr);
  CHECK(std::strcmp(name, "seriona_metadata") == 0);
}

TEST_CASE("control snapshot defaults to no current track") {
  const seriona::control::PlayerSnapshot snapshot{};

  CHECK(snapshot.freshness.version == 0U);
  CHECK(snapshot.freshness.sampledAt == std::chrono::steady_clock::time_point{});
  CHECK(snapshot.currentTrack == std::nullopt);
  CHECK(snapshot.playback.state == seriona::control::PlaybackStatus::Stopped);
  CHECK(snapshot.repeatMode == seriona::control::RepeatMode::Off);
  CHECK(snapshot.capabilities.bits == 0U);
}
