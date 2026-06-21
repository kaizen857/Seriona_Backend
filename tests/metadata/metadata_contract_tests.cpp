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
  const seriona::control::PlayerStateSnapshot snapshot{};

  CHECK(snapshot.freshness.version == 0U);
  CHECK(snapshot.freshness.sampledAt == std::chrono::steady_clock::time_point{});
  CHECK(snapshot.currentTrack == std::nullopt);
  CHECK(snapshot.playback.state == seriona::control::PlaybackStatus::Stopped);
  CHECK(snapshot.repeatMode == seriona::control::RepeatMode::Off);
  CHECK(snapshot.shuffle == false);
  CHECK(snapshot.capabilities.canPlay == false);
  CHECK(snapshot.capabilities.canSetShuffle == false);
  CHECK(snapshot.timeline.position == std::chrono::milliseconds{0});
}

TEST_CASE("control command surface exposes shuffle and repeat semantics") {
  const seriona::control::MediaControlCommand command{
      .kind = seriona::control::MediaControlCommandKind::SetShuffle,
      .repeatMode = seriona::control::RepeatMode::All,
      .shuffle = true,
  };

  CHECK(command.kind == seriona::control::MediaControlCommandKind::SetShuffle);
  CHECK(command.shuffle == std::optional<bool>{true});
  CHECK(command.repeatMode == std::optional<seriona::control::RepeatMode>{seriona::control::RepeatMode::All});
}
