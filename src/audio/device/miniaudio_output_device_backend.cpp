#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "seriona/audio/device/audio_output_device.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace seriona::audio {

AudioDeviceFormat makeAudioDeviceFormat(const ma_device_info& info, std::string deviceId) {
  return AudioDeviceFormat{.deviceId = std::move(deviceId),
                          .deviceName = info.name,
                          .backendName = "miniaudio",
                          .sampleFormat = AudioSampleFormat::Unknown,
                          .actualMode = AudioOutputMode::Mixed};
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

void fillDeviceCapabilitiesFromQuery(AudioDeviceFormat& device, const ma_device_info& info, ma_result queryResult) {
  if (queryResult != MA_SUCCESS) {
    // 能力查询失败（MA_SHARE_MODE_NOT_SUPPORTED 等）：列表保持空（=未枚举）并标记，
    // 前端按"全支持"显示并允许协商回退。
    device.fallbackApplied = true;
    return;
  }

  device.supportedSampleFormats.clear();
  device.supportedSampleRates.clear();
  const ma_uint32 count =
      std::min(info.nativeDataFormatCount, static_cast<ma_uint32>(ma_countof(info.nativeDataFormats)));
  for (ma_uint32 i = 0; i < count; ++i) {
    const auto& native = info.nativeDataFormats[i];
    // ma_format_unknown 与 sampleRate==0 是 miniaudio 的"全支持"语义：不产生条目，
    // 列表为空即全支持，与"未枚举"的默认空列表行为一致。
    if (native.format != ma_format_unknown) {
      const AudioSampleFormat format = fromMiniaudioFormat(native.format);
      if (format != AudioSampleFormat::Unknown &&
          std::find(device.supportedSampleFormats.begin(), device.supportedSampleFormats.end(), format) ==
              device.supportedSampleFormats.end()) {
        device.supportedSampleFormats.push_back(format);
      }
    }
    if (native.sampleRate != 0 &&
        std::find(device.supportedSampleRates.begin(), device.supportedSampleRates.end(), native.sampleRate) ==
            device.supportedSampleRates.end()) {
      device.supportedSampleRates.push_back(native.sampleRate);
    }
  }
}

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
        AudioDeviceFormat device = makeAudioDeviceFormat(info, std::to_string(index));
        // 逐设备查询详细能力（nativeDataFormats）；失败时能力留空并标记 fallbackApplied
        // （空列表=未枚举=前端按全支持显示，见 fillDeviceCapabilitiesFromQuery）。
        ma_device_info detailed{};
        const ma_result result = ma_context_get_device_info(&context, ma_device_type_playback, &info.id, &detailed);
        fillDeviceCapabilitiesFromQuery(device, detailed, result);
        devices.push_back(std::move(device));
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

    // preferredDeviceId 非空时解析（枚举索引字符串，见 enumeratePlaybackDevices）并绑定对应
    // 播放设备；解析失败（非数字/越界/枚举失败）回退默认设备——仅记日志，不视为失败。
    const bool boundToPreferredDevice =
        !request.config.preferredDeviceId.empty() && resolvePreferredDevice(request.config.preferredDeviceId, config);

    auto result = ma_device_init(boundToPreferredDevice ? &context_ : nullptr, &config, &device_);
    if (result != MA_SUCCESS) {
      spdlog::warn("miniaudio backend rejected explicit buffer config (periodSizeInFrames={}, periods={}); "
                   "retrying with backend defaults",
                   config.periodSizeInFrames, config.periods);
      config.periods = 0;
      result = ma_device_init(boundToPreferredDevice ? &context_ : nullptr, &config, &device_);
    }
    if (result != MA_SUCCESS) {
      releaseContext();
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

    // context 必须比 device 活得久（ma_device_init 将 pContext 存进 pDevice->pContext），
    // 因此先 uninit device，再释放 context。
    ma_device_uninit(&device_);
    device_ = {};
    currentFormat_ = {};
    initialized_ = false;
    releaseContext();
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return currentFormat_; }

  [[nodiscard]] std::optional<AudioOutputDeviceError> lastError() const override { return lastError_; }

private:
  // 将 preferredDeviceId（枚举索引字符串，见 enumeratePlaybackDevices：deviceId =
  // std::to_string(index)）解析回 ma_device_id 并写入 config.playback.pDeviceID。
  // miniaudio 在 ma_device_init 内部拷贝 pDeviceID（MA_COPY_MEMORY 进 pDevice->playback.id），
  // 因此 playbackInfos[index].id 只需在 init 调用期间有效；但显式 context 会被 device
  // 长期持有（pDevice->pContext），必须作为成员保存并在 ma_device_uninit 之后释放。
  // 解析/枚举失败时释放本次 context 并返回 false——调用方回退默认设备（仅日志，不视为失败）。
  bool resolvePreferredDevice(const std::string& preferredDeviceId, ma_device_config& config) {
    std::size_t index = 0;
    {
      const char* const begin = preferredDeviceId.data();
      const char* const end = begin + preferredDeviceId.size();
      const auto parsed = std::from_chars(begin, end, index);
      if (parsed.ec != std::errc{} || parsed.ptr != end) {
        spdlog::warn("miniaudio backend: preferredDeviceId '{}' is not a numeric device index; "
                     "falling back to the default output device",
                     preferredDeviceId);
        return false;
      }
    }

    if (ma_context_init(nullptr, 0, nullptr, &context_) != MA_SUCCESS) {
      spdlog::warn("miniaudio backend: failed to initialize context for preferred device index {}; "
                   "falling back to the default output device",
                   index);
      return false;
    }
    contextInitialized_ = true;

    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&context_, &playbackInfos, &playbackCount, nullptr, nullptr) != MA_SUCCESS) {
      spdlog::warn("miniaudio backend: failed to enumerate playback devices for preferred device index {}; "
                   "falling back to the default output device",
                   index);
      releaseContext();
      return false;
    }
    if (index >= playbackCount) {
      spdlog::warn("miniaudio backend: preferred device index {} is out of range ({} playback devices available); "
                   "falling back to the default output device",
                   index, playbackCount);
      releaseContext();
      return false;
    }

    config.playback.pDeviceID = &playbackInfos[index].id;
    return true;
  }

  void releaseContext() noexcept {
    if (!contextInitialized_) {
      return;
    }
    ma_context_uninit(&context_);
    context_ = {};
    contextInitialized_ = false;
  }

  ma_device device_{};
  ma_context context_{};
  bool contextInitialized_{false};
  AudioDeviceFormat currentFormat_{};
  std::optional<AudioOutputDeviceError> lastError_{};
  bool initialized_{false};
};

}

std::unique_ptr<AudioOutputDeviceBackend> makeMiniaudioOutputDeviceBackend() {
  return std::make_unique<MiniaudioOutputDeviceBackend>();
}

}
