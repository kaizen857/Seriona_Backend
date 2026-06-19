#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "seriona/audio/device/audio_output_device.h"

#include <memory>

namespace seriona::audio {
namespace {

ma_format toMiniaudioFormat(AudioSampleFormat format) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16:
    return ma_format_s16;
  case AudioSampleFormat::Int24:
    return ma_format_s24;
  case AudioSampleFormat::Int32:
    return ma_format_s32;
  case AudioSampleFormat::Float32:
    return ma_format_f32;
  case AudioSampleFormat::Unknown:
    return ma_format_unknown;
  }

  return ma_format_unknown;
}

AudioSampleFormat fromMiniaudioFormat(ma_format format) noexcept {
  switch (format) {
  case ma_format_s16:
    return AudioSampleFormat::Int16;
  case ma_format_s24:
    return AudioSampleFormat::Int24;
  case ma_format_s32:
    return AudioSampleFormat::Int32;
  case ma_format_f32:
    return AudioSampleFormat::Float32;
  default:
    return AudioSampleFormat::Unknown;
  }
}

void miniaudioDataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) noexcept {
  AudioOutputDevice::renderCallback(device != nullptr ? device->pUserData : nullptr, output, frameCount);
}

class MiniaudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  MiniaudioOutputDeviceBackend() = default;
  ~MiniaudioOutputDeviceBackend() override { uninitialize(); }

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override {
    ma_context context{};
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
      return {};
    }

    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    std::vector<AudioDeviceFormat> devices;
    if (ma_context_get_devices(&context, &playbackInfos, &playbackCount, nullptr, nullptr) == MA_SUCCESS) {
      devices.reserve(playbackCount);
      for (ma_uint32 index = 0; index < playbackCount; ++index) {
        const auto& info = playbackInfos[index];
        devices.push_back(AudioDeviceFormat{.deviceId = std::to_string(index),
                                            .deviceName = info.name,
                                            .backendName = "miniaudio",
                                            .sampleFormat = AudioSampleFormat::Unknown,
                                            .actualMode = AudioOutputMode::Mixed});
      }
    }

    ma_context_uninit(&context);
    return devices;
  }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    if (initialized_) {
      uninitialize();
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = toMiniaudioFormat(request.sampleFormat);
    config.playback.channels = request.channelCount;
    config.sampleRate = request.sampleRate;
    config.periodSizeInFrames = request.bufferFrames;
    config.dataCallback = miniaudioDataCallback;
    config.pUserData = request.callbackUserData;

    const auto result = ma_device_init(nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
      return false;
    }
    currentFormat_ = AudioDeviceFormat{.deviceId = request.config.preferredDeviceId,
                                       .deviceName = device_.playback.name,
                                       .backendName = "miniaudio",
                                       .sampleRate = device_.sampleRate,
                                       .sampleFormat = fromMiniaudioFormat(device_.playback.format),
                                       .channelCount = static_cast<std::uint16_t>(device_.playback.channels),
                                       .bufferFrames = device_.playback.internalPeriodSizeInFrames,
                                       .actualMode = request.config.outputMode,
                                       .fallbackApplied = false};
    initialized_ = true;
    return true;
  }

  [[nodiscard]] bool start() override { return initialized_ && ma_device_start(&device_) == MA_SUCCESS; }

  [[nodiscard]] bool stop() override { return !initialized_ || ma_device_stop(&device_) == MA_SUCCESS; }

  void uninitialize() noexcept override {
    if (!initialized_) {
      return;
    }

    ma_device_uninit(&device_);
    device_ = {};
    currentFormat_ = {};
    initialized_ = false;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return currentFormat_; }

private:
  ma_device device_{};
  AudioDeviceFormat currentFormat_{};
  bool initialized_{false};
};

}

std::unique_ptr<AudioOutputDeviceBackend> makeMiniaudioOutputDeviceBackend() {
  return std::make_unique<MiniaudioOutputDeviceBackend>();
}

}
