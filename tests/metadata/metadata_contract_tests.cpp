#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <type_traits>
#include <utility>

#include "seriona/control/control_contracts.h"
#include "seriona/metadata/metadata_contracts.h"
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

TEST_CASE("metadata contract exposes stable service seams") {
  using MetadataService = seriona::metadata::MetadataSharingService;

  static_assert(std::is_same_v<decltype(seriona::metadata::makeMetadataSharingService(
                        std::declval<const seriona::metadata::MetadataSharingOptions&>())),
                    std::unique_ptr<MetadataService>>);
  static_assert(std::is_same_v<decltype(std::declval<MetadataService&>().registerCommandCallback(
                        std::declval<seriona::control::MediaControlCommandSink>())),
                    seriona::control::SubscriptionHandle>);
  static_assert(std::is_same_v<decltype(std::declval<MetadataService&>().start(
                        std::declval<const seriona::metadata::PlatformMediaState&>())),
                    seriona::metadata::MetadataSyncResult>);
  static_assert(std::is_same_v<decltype(std::declval<MetadataService&>().update(
                        std::declval<const seriona::metadata::PlatformMediaState&>())),
                    seriona::metadata::MetadataSyncResult>);
  static_assert(std::is_same_v<decltype(std::declval<MetadataService&>().stop()),
                    seriona::metadata::MetadataSyncResult>);

  const seriona::metadata::PlatformMediaState state{};

  CHECK(state.controlState.currentTrack == std::nullopt);
  CHECK(state.timelineUpdateInterval == std::chrono::milliseconds{1000});
}

TEST_CASE("metadata contract models Linux Windows and Noop options without platform-only signatures") {
  const seriona::metadata::MetadataSharingOptions noopOptions{
      .backendKind = seriona::metadata::MetadataBackendKind::Noop,
  };
  const seriona::metadata::MetadataSharingOptions linuxOptions{
      .backendKind = seriona::metadata::MetadataBackendKind::Linux,
  };
  const seriona::metadata::MetadataSharingOptions windowsOptions{
      .backendKind = seriona::metadata::MetadataBackendKind::Windows,
  };

  CHECK(noopOptions.backendKind == seriona::metadata::MetadataBackendKind::Noop);
  CHECK(linuxOptions.backendKind == seriona::metadata::MetadataBackendKind::Linux);
  CHECK(windowsOptions.backendKind == seriona::metadata::MetadataBackendKind::Windows);
  CHECK(windowsOptions.platformExtension == nullptr);
  CHECK(noopOptions.timelineUpdateInterval == std::chrono::milliseconds{1000});
}

TEST_CASE("metadata contract models missing Windows host handles as capability degradation") {
  const seriona::metadata::MetadataBackendCapabilities degradedWindowsCapabilities{
      .canPublishMetadata = false,
      .canPublishTimeline = false,
      .canReceiveCommands = false,
      .requiresPlatformExtension = true,
      .hasPlatformExtension = false,
  };
  const seriona::metadata::MetadataSyncResult result{};

  CHECK(degradedWindowsCapabilities.requiresPlatformExtension);
  CHECK_FALSE(degradedWindowsCapabilities.hasPlatformExtension);
  CHECK_FALSE(degradedWindowsCapabilities.canPublishMetadata);
  CHECK_FALSE(result.accepted);
  CHECK_FALSE(result.changed);
  CHECK(result.errorCode == std::nullopt);
}
