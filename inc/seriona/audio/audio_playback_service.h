#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/device/audio_output_device.h"

#include <memory>

namespace seriona::audio {

[[nodiscard]] std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(
    std::unique_ptr<AudioOutputDeviceBackend> backend = nullptr);

}
