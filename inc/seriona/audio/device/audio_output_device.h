#pragma once

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/buffer/pcm_buffer_queue.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace seriona::audio {

class AudioOutputDevice;

struct AudioOutputDeviceCounters {
  std::uint64_t callbackCount{0};
  std::uint64_t requestedFrames{0};
  std::uint64_t copiedFrames{0};
  std::uint64_t silenceFrames{0};
};

struct AudioOutputDeviceOpenRequest {
  AudioOutputConfig config{};
  AudioSampleFormat sampleFormat{AudioSampleFormat::Float32};
  std::uint32_t sampleRate{48000};
  std::uint16_t channelCount{2};
  std::uint32_t bufferFrames{512};
  PcmBufferQueue* pcmQueue{nullptr};
  AudioOutputDevice* callbackUserData{nullptr};
};

struct AudioOutputDeviceError {
  PlaybackErrorCode code{PlaybackErrorCode::DeviceUnavailable};
  std::string message;
  std::string detail;
};

class AudioOutputDeviceBackend {
public:
  virtual ~AudioOutputDeviceBackend() = default;

  [[nodiscard]] virtual std::vector<AudioDeviceFormat> enumeratePlaybackDevices() = 0;
  [[nodiscard]] virtual bool initialize(const AudioOutputDeviceOpenRequest& request) = 0;
  [[nodiscard]] virtual bool start() = 0;
  [[nodiscard]] virtual bool stop() = 0;
  virtual void uninitialize() noexcept = 0;
  [[nodiscard]] virtual AudioDeviceFormat currentFormat() const = 0;
  [[nodiscard]] virtual std::optional<AudioOutputDeviceError> lastError() const { return std::nullopt; }
};

class AudioOutputDevice {
public:
  explicit AudioOutputDevice(std::unique_ptr<AudioOutputDeviceBackend> backend = nullptr);
  ~AudioOutputDevice();

  AudioOutputDevice(const AudioOutputDevice&) = delete;
  AudioOutputDevice& operator=(const AudioOutputDevice&) = delete;
  AudioOutputDevice(AudioOutputDevice&&) = delete;
  AudioOutputDevice& operator=(AudioOutputDevice&&) = delete;

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices();
  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request);
  [[nodiscard]] bool start();
  [[nodiscard]] bool stop();
  void uninitialize() noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] AudioDeviceFormat currentFormat() const;
  [[nodiscard]] std::optional<AudioOutputDeviceError> lastError() const;

  static void renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept;
  [[nodiscard]] AudioOutputDeviceCounters counters() const noexcept;

private:
  std::unique_ptr<AudioOutputDeviceBackend> backend_;
  PcmBufferQueue* pcmQueue_{nullptr};
  std::atomic<std::uint64_t> callbackCount_{0};
  std::atomic<std::uint64_t> requestedFrames_{0};
  std::atomic<std::uint64_t> copiedFrames_{0};
  std::atomic<std::uint64_t> silenceFrames_{0};
  std::optional<AudioOutputDeviceError> lastError_{};
  bool initialized_{false};
  bool started_{false};
};

std::unique_ptr<AudioOutputDeviceBackend> makeMiniaudioOutputDeviceBackend();

}
