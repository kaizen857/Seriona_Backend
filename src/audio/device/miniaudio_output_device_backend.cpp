#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "seriona/audio/device/audio_output_device.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

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

std::string miniaudioDetail(ma_result result) {
  return "miniaudio result " + std::to_string(static_cast<int>(result));
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
    lastError_.reset();
    if (initialized_) {
      uninitialize();
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = toMiniaudioFormat(request.sampleFormat);
    config.playback.channels = request.channelCount;
    config.sampleRate = request.sampleRate;
    config.periodSizeInFrames = request.bufferFrames;
    // Explicit device buffering (plan §二 P1-③): target ~100ms total device buffer
    // (96-150ms band) via `periods`; this miniaudio exposes only periodSizeInFrames +
    // periods (total = periodSize * periods), so derive the period count from the
    // 100ms target while keeping periodSizeInFrames as-is (service caps it at 512).
    const auto explicitBufferFrames = request.sampleRate / 10U;
    config.periods =
        std::max(3U, (explicitBufferFrames + request.bufferFrames - 1U) / std::max(request.bufferFrames, 1U));
    config.dataCallback = miniaudioDataCallback;
    config.pUserData = request.callbackUserData;

    auto result = ma_device_init(nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
      spdlog::warn("miniaudio backend rejected explicit buffer config (periodSizeInFrames={}, periods={}); "
                   "retrying with backend defaults",
                   config.periodSizeInFrames, config.periods);
      config.periods = 0;
      result = ma_device_init(nullptr, &config, &device_);
    }
    if (result != MA_SUCCESS) {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "failed to initialize audio output device",
                                          miniaudioDetail(result)};
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

  [[nodiscard]] bool start() override {
    lastError_.reset();
    if (!initialized_) {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "audio output device is not initialized",
                                          "miniaudio start requested before initialize"};
      return false;
    }

    const auto result = ma_device_start(&device_);
    if (result != MA_SUCCESS) {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "failed to start audio output device",
                                          miniaudioDetail(result)};
      return false;
    }

    return true;
  }

  [[nodiscard]] bool stop() override {
    lastError_.reset();
    if (!initialized_) {
      return true;
    }

    const auto result = ma_device_stop(&device_);
    if (result != MA_SUCCESS) {
      lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                          "failed to stop audio output device",
                                          miniaudioDetail(result)};
      return false;
    }

    return true;
  }

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

  [[nodiscard]] std::optional<AudioOutputDeviceError> lastError() const override { return lastError_; }

private:
  ma_device device_{};
  AudioDeviceFormat currentFormat_{};
  std::optional<AudioOutputDeviceError> lastError_{};
  bool initialized_{false};
};

}

std::unique_ptr<AudioOutputDeviceBackend> makeMiniaudioOutputDeviceBackend() {
  return std::make_unique<MiniaudioOutputDeviceBackend>();
}

}
