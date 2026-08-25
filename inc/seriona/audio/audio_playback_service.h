#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/device/audio_device_format_enumerator.h"
#include "seriona/audio/device/audio_output_device.h"

#include <memory>

namespace seriona::audio {

// backend 为播放后端（miniaudio）；formatEnumerator 可选，提供平台原生
// 设备采样率/位深能力（Linux PipeWire / Windows WASAPI），enumeratePlaybackDevices
// 会用它覆盖后端报告的 supportedSampleFormats/supportedSampleRates。
// 两参数均有默认值，不影响既有调用方。
[[nodiscard]] std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(
    std::unique_ptr<AudioOutputDeviceBackend> backend = nullptr,
    std::unique_ptr<DeviceFormatEnumerator> formatEnumerator = nullptr);

}
