#include "seriona/audio/device/audio_output_device.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace seriona::audio {
namespace {

std::uint32_t bytesPerSample(AudioSampleFormat format) noexcept {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U;
  case AudioSampleFormat::Int24:
    return 3U;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U;
  case AudioSampleFormat::Unknown:
    return 0U;
  }
  return 0U;
}

void applyInt16Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<std::int16_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const auto scaled = std::lround(static_cast<float>(samples[index]) * volume);
    samples[index] = static_cast<std::int16_t>(std::clamp<long>(scaled,
                                                               std::numeric_limits<std::int16_t>::min(),
                                                               std::numeric_limits<std::int16_t>::max()));
  }
}

void applyInt32Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<std::int32_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const auto scaled = std::llround(static_cast<double>(samples[index]) * static_cast<double>(volume));
    samples[index] = static_cast<std::int32_t>(std::clamp<long long>(scaled,
                                                                    std::numeric_limits<std::int32_t>::min(),
                                                                    std::numeric_limits<std::int32_t>::max()));
  }
}

void applyFloat32Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* samples = static_cast<float*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    samples[index] *= volume;
  }
}

void applyInt24Gain(void* output, std::uint32_t frameCount, std::uint16_t channelCount, float volume) noexcept {
  auto* bytes = static_cast<std::uint8_t*>(output);
  const auto sampleCount = static_cast<std::size_t>(frameCount) * channelCount;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    auto* sample = bytes + index * 3U;
    std::int32_t value = static_cast<std::int32_t>(sample[0]) | (static_cast<std::int32_t>(sample[1]) << 8U) |
                         (static_cast<std::int32_t>(sample[2]) << 16U);
    if ((value & 0x00800000) != 0) {
      value |= ~0x00FFFFFF;
    }
    const auto scaled = std::lround(static_cast<float>(value) * volume);
    const auto clamped = std::clamp<long>(scaled, -8'388'608L, 8'388'607L);
    const auto encoded = static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped)) & 0x00FFFFFFU;
    sample[0] = static_cast<std::uint8_t>(encoded & 0xFFU);
    sample[1] = static_cast<std::uint8_t>((encoded >> 8U) & 0xFFU);
    sample[2] = static_cast<std::uint8_t>((encoded >> 16U) & 0xFFU);
  }
}

void applyGain(void* output,
               std::uint32_t frameCount,
               std::uint16_t channelCount,
               AudioSampleFormat sampleFormat,
               float volume,
               bool muted) noexcept {
  if (output == nullptr || frameCount == 0U || channelCount == 0U) {
    return;
  }
  if (muted || volume <= 0.0F) {
    std::memset(output, 0, static_cast<std::size_t>(frameCount) * channelCount * bytesPerSample(sampleFormat));
    return;
  }
  if (volume == 1.0F) {
    return;
  }
  switch (sampleFormat) {
  case AudioSampleFormat::Int16:
    applyInt16Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Int24:
    applyInt24Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Int32:
    applyInt32Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Float32:
    applyFloat32Gain(output, frameCount, channelCount, volume);
    return;
  case AudioSampleFormat::Unknown:
    return;
  }
}

}

AudioOutputDevice::AudioOutputDevice(std::unique_ptr<AudioOutputDeviceBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_) {
    backend_ = makeMiniaudioOutputDeviceBackend();
  }
}

AudioOutputDevice::~AudioOutputDevice() { uninitialize(); }

std::vector<AudioDeviceFormat> AudioOutputDevice::enumeratePlaybackDevices() {
  return backend_->enumeratePlaybackDevices();
}

bool AudioOutputDevice::initialize(const AudioOutputDeviceOpenRequest& request) {
  lastError_.reset();
  if (request.pcmQueue == nullptr || request.sampleRate == 0U || request.channelCount == 0U ||
      request.bufferFrames == 0U) {
    lastError_ = AudioOutputDeviceError{PlaybackErrorCode::FormatNegotiationFailed,
                                        "audio output device request is invalid",
                                        "pcm queue, sample rate, channel count, and buffer frames must be set"};
    return false;
  }

  if (initialized_) {
    uninitialize();
  }

  auto backendRequest = request;
  backendRequest.callbackUserData = this;
  if (!backend_->initialize(backendRequest)) {
    lastError_ = backend_->lastError();
    return false;
  }

  pcmQueue_ = request.pcmQueue;
  currentFormat_ = backend_->currentFormat();
  callbackCount_.store(0U, std::memory_order_relaxed);
  requestedFrames_.store(0U, std::memory_order_relaxed);
  copiedFrames_.store(0U, std::memory_order_relaxed);
  silenceFrames_.store(0U, std::memory_order_relaxed);
  initialized_ = true;
  started_ = false;
  return true;
}

bool AudioOutputDevice::start() {
  lastError_.reset();
  if (!initialized_) {
    lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                        "audio output device is not initialized",
                                        "start requires a successful initialize call"};
    return false;
  }

  if (started_) {
    return true;
  }

  if (!backend_->start()) {
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                       "failed to start audio output device",
                                                                       "AudioOutputDeviceBackend::start returned false"});
    return false;
  }

  started_ = true;
  return true;
}

bool AudioOutputDevice::stop() {
  lastError_.reset();
  if (!initialized_ || !started_) {
    return true;
  }

  if (!backend_->stop()) {
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                       "failed to stop audio output device",
                                                                       "AudioOutputDeviceBackend::stop returned false"});
    return false;
  }

  started_ = false;
  return true;
}

void AudioOutputDevice::uninitialize() noexcept {
  if (!initialized_) {
    return;
  }

  if (started_) {
    static_cast<void>(backend_->stop());
    started_ = false;
  }

  backend_->uninitialize();
  pcmQueue_ = nullptr;
  currentFormat_ = {};
  lastError_.reset();
  initialized_ = false;
}

bool AudioOutputDevice::initialized() const noexcept { return initialized_; }

bool AudioOutputDevice::started() const noexcept { return started_; }

AudioDeviceFormat AudioOutputDevice::currentFormat() const { return currentFormat_; }

std::optional<AudioOutputDeviceError> AudioOutputDevice::lastError() const { return lastError_; }

void AudioOutputDevice::setVolume(float linearGain) noexcept {
  if (std::isnan(linearGain)) {
    return;
  }
  volume_.store(std::clamp(linearGain, 0.0F, 1.0F), std::memory_order_release);
}

void AudioOutputDevice::setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_release); }

void AudioOutputDevice::renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept {
  auto* device = static_cast<AudioOutputDevice*>(userData);
  if (device == nullptr || device->pcmQueue_ == nullptr) {
    return;
  }

  const auto result = device->pcmQueue_->read(output, frameCount);
  applyGain(output,
            result.copiedFrames,
            device->currentFormat_.channelCount,
            device->currentFormat_.sampleFormat,
            device->volume_.load(std::memory_order_acquire),
            device->muted_.load(std::memory_order_acquire));
  device->callbackCount_.fetch_add(1U, std::memory_order_relaxed);
  device->requestedFrames_.fetch_add(result.requestedFrames, std::memory_order_relaxed);
  device->copiedFrames_.fetch_add(result.copiedFrames, std::memory_order_relaxed);
  device->silenceFrames_.fetch_add(result.silenceFrames, std::memory_order_relaxed);
}

AudioOutputDeviceCounters AudioOutputDevice::counters() const noexcept {
  return AudioOutputDeviceCounters{callbackCount_.load(std::memory_order_relaxed),
                                   requestedFrames_.load(std::memory_order_relaxed),
                                   copiedFrames_.load(std::memory_order_relaxed),
                                   silenceFrames_.load(std::memory_order_relaxed)};
}

}
