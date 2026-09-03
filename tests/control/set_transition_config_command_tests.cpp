// SetTransitionConfig 命令/TransitionConfig 契约测试（T1）。
// 覆盖：TransitionConfig 默认构造 == 裁定默认（9 字段）；相等运算符；两个过渡
// 档位枚举的 0/1/2 序数；MediaControlCommandKind::SetTransitionConfig 追加在
// 枚举末尾（序列化兼容）；命令可经 designated initialization 携带载荷。
#include "../../inc/seriona/control/control_contracts.h"

#include <doctest.h>

#include <chrono>
#include <type_traits>

using namespace seriona::control;
namespace audio = seriona::audio;

namespace {

template <typename T>
void expectValueSemantics() {
  CHECK(std::is_default_constructible_v<T>);
  CHECK(std::is_copy_constructible_v<T>);
  CHECK(std::is_copy_assignable_v<T>);
  CHECK(std::is_move_constructible_v<T>);
  CHECK(std::is_move_assignable_v<T>);
}

}

TEST_CASE("transition config default construction equals the adjudicated defaults") {
  expectValueSemantics<audio::TransitionConfig>();
  expectValueSemantics<MediaControlCommand>();

  // 默认构造 == 裁定默认 == "旧行为等价"：9 项设置全部取裁定表默认值。
  const audio::TransitionConfig defaults{};
  CHECK(defaults.autoAdvanceFadeMode == audio::AutoAdvanceFadeMode::Off);
  CHECK_FALSE(defaults.fadeOnTransport);
  CHECK_FALSE(defaults.fadeOnSeek);
  CHECK(defaults.gaplessPreloadMs == std::chrono::milliseconds{0});
  CHECK(defaults.crossfadeMs == std::chrono::milliseconds{3000});
  CHECK(defaults.transportFadeMs == std::chrono::milliseconds{300});
  CHECK(defaults.seekFadeMs == std::chrono::milliseconds{300});
  CHECK(defaults.manualAdvanceFadeMode == audio::ManualAdvanceFadeMode::Off);
  CHECK(defaults.manualShortCrossfadeMs == std::chrono::milliseconds{500});

  // 等价的显式值对象与默认构造相等（旧行为等价判据）。
  const audio::TransitionConfig adjudicated{};
  CHECK(defaults == adjudicated);
  CHECK_FALSE(defaults != adjudicated);
}

TEST_CASE("transition fade mode enums keep adjudicated ordinal values") {
  CHECK(static_cast<int>(audio::AutoAdvanceFadeMode::Off) == 0);
  CHECK(static_cast<int>(audio::AutoAdvanceFadeMode::ExceptGaplessGroup) == 1);
  CHECK(static_cast<int>(audio::AutoAdvanceFadeMode::All) == 2);

  CHECK(static_cast<int>(audio::ManualAdvanceFadeMode::Off) == 0);
  CHECK(static_cast<int>(audio::ManualAdvanceFadeMode::ShortDip) == 1);
  CHECK(static_cast<int>(audio::ManualAdvanceFadeMode::FullCrossfade) == 2);
}

TEST_CASE("transition config equality reflects every adjudicated field") {
  audio::TransitionConfig config{};
  config.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::ExceptGaplessGroup;
  config.fadeOnTransport = true;
  config.fadeOnSeek = true;
  config.gaplessPreloadMs = std::chrono::milliseconds{800};
  config.crossfadeMs = std::chrono::milliseconds{4000};
  config.transportFadeMs = std::chrono::milliseconds{600};
  config.seekFadeMs = std::chrono::milliseconds{250};
  config.manualAdvanceFadeMode = audio::ManualAdvanceFadeMode::ShortDip;
  config.manualShortCrossfadeMs = std::chrono::milliseconds{1000};

  audio::TransitionConfig same{};
  same.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::ExceptGaplessGroup;
  same.fadeOnTransport = true;
  same.fadeOnSeek = true;
  same.gaplessPreloadMs = std::chrono::milliseconds{800};
  same.crossfadeMs = std::chrono::milliseconds{4000};
  same.transportFadeMs = std::chrono::milliseconds{600};
  same.seekFadeMs = std::chrono::milliseconds{250};
  same.manualAdvanceFadeMode = audio::ManualAdvanceFadeMode::ShortDip;
  same.manualShortCrossfadeMs = std::chrono::milliseconds{1000};
  CHECK(config == same);

  SUBCASE("auto advance mode participates") {
    auto other = config;
    other.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::All;
    CHECK(config != other);
  }
  SUBCASE("transport switch participates") {
    auto other = config;
    other.fadeOnTransport = false;
    CHECK(config != other);
  }
  SUBCASE("seek switch participates") {
    auto other = config;
    other.fadeOnSeek = false;
    CHECK(config != other);
  }
  SUBCASE("gapless preload participates") {
    auto other = config;
    other.gaplessPreloadMs = std::chrono::milliseconds{801};
    CHECK(config != other);
  }
  SUBCASE("crossfade length participates") {
    auto other = config;
    other.crossfadeMs = std::chrono::milliseconds{4001};
    CHECK(config != other);
  }
  SUBCASE("transport fade length participates") {
    auto other = config;
    other.transportFadeMs = std::chrono::milliseconds{601};
    CHECK(config != other);
  }
  SUBCASE("seek fade length participates") {
    auto other = config;
    other.seekFadeMs = std::chrono::milliseconds{251};
    CHECK(config != other);
  }
  SUBCASE("manual advance mode participates") {
    auto other = config;
    other.manualAdvanceFadeMode = audio::ManualAdvanceFadeMode::FullCrossfade;
    CHECK(config != other);
  }
  SUBCASE("manual short crossfade participates") {
    auto other = config;
    other.manualShortCrossfadeMs = std::chrono::milliseconds{1001};
    CHECK(config != other);
  }
}

TEST_CASE("set transition config command contract exposes transition config payload") {
  // SetTransitionConfig is appended at the end of the kind enum: existing values
  // keep their ordinal positions, so serialized commands stay compatible.
  CHECK(static_cast<int>(MediaControlCommandKind::SetTransitionConfig) >
        static_cast<int>(MediaControlCommandKind::RemoveFromQueue));
  CHECK(static_cast<int>(MediaControlCommandKind::SetTransitionConfig) ==
        static_cast<int>(MediaControlCommandKind::RemoveFromQueue) + 1);

  // Designated initialization of kind + transitionConfig must work; skipped
  // members fall back to their defaults.
  const MediaControlCommand command{.kind = MediaControlCommandKind::SetTransitionConfig,
                                    .transitionConfig = audio::TransitionConfig{
                                        .crossfadeMs = std::chrono::milliseconds{2500}}};

  CHECK(command.kind == MediaControlCommandKind::SetTransitionConfig);
  REQUIRE(command.transitionConfig.has_value());
  CHECK(command.transitionConfig->crossfadeMs == std::chrono::milliseconds{2500});
  // 未指定的中间字段仍取裁定默认。
  CHECK(command.transitionConfig->autoAdvanceFadeMode == audio::AutoAdvanceFadeMode::Off);
  CHECK(command.transitionConfig->manualShortCrossfadeMs == std::chrono::milliseconds{500});

  // Default-constructed command keeps transitionConfig empty and kind = Play.
  const MediaControlCommand empty{};
  CHECK(empty.kind == MediaControlCommandKind::Play);
  CHECK(empty.transitionConfig == std::nullopt);
}

TEST_CASE("set transition config command payload is readable and writable") {
  MediaControlCommand command{};
  command.kind = MediaControlCommandKind::SetTransitionConfig;
  command.transitionConfig = audio::TransitionConfig{};
  CHECK(command.kind == MediaControlCommandKind::SetTransitionConfig);
  REQUIRE(command.transitionConfig.has_value());

  command.transitionConfig->fadeOnTransport = true;
  command.transitionConfig->transportFadeMs = std::chrono::milliseconds{900};

  CHECK(command.transitionConfig->fadeOnTransport);
  CHECK(command.transitionConfig->transportFadeMs == std::chrono::milliseconds{900});

  // Assignment replaces the payload entirely.
  const audio::TransitionConfig custom{.autoAdvanceFadeMode = audio::AutoAdvanceFadeMode::All,
                                       .gaplessPreloadMs = std::chrono::milliseconds{1200}};
  command.transitionConfig = custom;
  CHECK(command.transitionConfig->autoAdvanceFadeMode == audio::AutoAdvanceFadeMode::All);
  CHECK(command.transitionConfig->gaplessPreloadMs == std::chrono::milliseconds{1200});
  CHECK_FALSE(command.transitionConfig->fadeOnTransport);
}
