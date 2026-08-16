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

TEST_CASE("configure output command contract exposes output config payload") {
  // The payload type must stay a plain value type usable in commands.
  expectValueSemantics<audio::AudioOutputConfig>();
  expectValueSemantics<MediaControlCommand>();

  // ConfigureOutput is appended at the end of the kind enum: existing values
  // keep their ordinal positions, so serialized commands stay compatible.
  CHECK(static_cast<int>(MediaControlCommandKind::ConfigureOutput) >
        static_cast<int>(MediaControlCommandKind::ApplyFolderSortRules));
  CHECK(static_cast<int>(MediaControlCommandKind::ConfigureOutput) ==
        static_cast<int>(MediaControlCommandKind::ApplyFolderSortRules) + 1);

  // Designated initialization of kind + outputConfig must work; skipped
  // members fall back to their defaults.
  const MediaControlCommand command{.kind = MediaControlCommandKind::ConfigureOutput,
                                    .outputConfig = audio::AudioOutputConfig{
                                        .outputMode = audio::AudioOutputMode::Direct}};

  CHECK(command.kind == MediaControlCommandKind::ConfigureOutput);
  REQUIRE(command.outputConfig.has_value());
  CHECK(command.outputConfig->outputMode == audio::AudioOutputMode::Direct);

  // Default-constructed command keeps outputConfig empty and kind = Play.
  const MediaControlCommand empty{};
  CHECK(empty.kind == MediaControlCommandKind::Play);
  CHECK(empty.outputConfig == std::nullopt);
}

TEST_CASE("configure output command output config fields are readable and writable") {
  MediaControlCommand command{};
  command.kind = MediaControlCommandKind::ConfigureOutput;
  command.outputConfig = audio::AudioOutputConfig{};
  CHECK(command.kind == MediaControlCommandKind::ConfigureOutput);
  REQUIRE(command.outputConfig.has_value());

  // Writable through the optional payload.
  command.outputConfig->outputMode = audio::AudioOutputMode::Mixed;
  command.outputConfig->bufferDuration = std::chrono::milliseconds{150};
  command.outputConfig->keepDeviceOpen = true;

  // Read back the written values.
  CHECK(command.outputConfig->outputMode == audio::AudioOutputMode::Mixed);
  CHECK(command.outputConfig->bufferDuration == std::chrono::milliseconds{150});
  CHECK(command.outputConfig->keepDeviceOpen);

  // Assignment replaces the payload entirely.
  const audio::AudioOutputConfig direct{.outputMode = audio::AudioOutputMode::Direct,
                                        .allowFallback = false};
  command.outputConfig = direct;
  CHECK(command.outputConfig->outputMode == audio::AudioOutputMode::Direct);
  CHECK_FALSE(command.outputConfig->allowFallback);
  CHECK(command.outputConfig->targetSampleRate == std::nullopt);
}
