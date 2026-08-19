#include "seriona/audio/device/audio_output_device.h"

#include "spdlog/spdlog.h"

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
    // 音量由 miniaudio 设备侧控制：s24 为 3 字节打包格式，实时路径不实现增益分支
    // （避免在 renderCallback 中解包/重打包 3 字节样本），静音与音量 0 已在上方 memset 分支覆盖。
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

void fillSilence(void* output, std::uint32_t frameCount, std::uint32_t bytesPerFrame) noexcept {
  if (output == nullptr || frameCount == 0U || bytesPerFrame == 0U) {
    return;
  }

  std::memset(output, 0, static_cast<std::size_t>(frameCount) * bytesPerFrame);
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
    spdlog::error("device init failed: invalid request (rate={} ch={} bufFrames={} queue={})",
                  request.sampleRate, request.channelCount, request.bufferFrames,
                  static_cast<bool>(request.pcmQueue));
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
    spdlog::error("device init failed: backend init returned false");
    lastError_ = backend_->lastError();
    return false;
  }

  currentFormat_ = backend_->currentFormat();
  currentQueue_ = request.pcmQueue;
  publishCallbackQueue(*request.pcmQueue, currentFormat_);
  callbackCount_.store(0U, std::memory_order_relaxed);
  requestedFrames_.store(0U, std::memory_order_relaxed);
  copiedFrames_.store(0U, std::memory_order_relaxed);
  silenceFrames_.store(0U, std::memory_order_relaxed);
  initialized_ = true;
  started_ = false;
  spdlog::info("device initialized ({}Hz {}ch fmt={})", currentFormat_.sampleRate,
               currentFormat_.channelCount,
               static_cast<int>(currentFormat_.sampleFormat));
  return true;
}

bool AudioOutputDevice::start() {
  lastError_.reset();
  if (!initialized_) {
    spdlog::error("device start failed: not initialized");
    lastError_ = AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                        "audio output device is not initialized",
                                        "start requires a successful initialize call"};
    return false;
  }

  if (started_) {
    return true;
  }

  if (currentQueue_ != nullptr) {
    publishCallbackQueue(*currentQueue_, currentFormat_);
  }

  if (!backend_->start()) {
    spdlog::error("device start failed: backend start returned false");
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                        "failed to start audio output device",
                                                                        "AudioOutputDeviceBackend::start returned false"});
    return false;
  }

  started_ = true;
  spdlog::info("device started");
  return true;
}

bool AudioOutputDevice::stop() {
  lastError_.reset();
  if (!initialized_ || !started_) {
    return true;
  }

  if (!backend_->stop()) {
    spdlog::error("device stop failed: backend stop returned false");
    lastError_ = backend_->lastError().value_or(AudioOutputDeviceError{PlaybackErrorCode::DeviceUnavailable,
                                                                        "failed to stop audio output device",
                                                                        "AudioOutputDeviceBackend::stop returned false"});
    return false;
  }

  deactivateCallbackQueue();
  started_ = false;
  spdlog::info("device stopped");
  return true;
}

void AudioOutputDevice::rebindQueue(PcmBufferQueue& queue) noexcept {
  spdlog::debug("device rebind queue (generation={})", queue.generation());
  currentQueue_ = &queue;
  if (initialized_) {
    publishCallbackQueue(queue, currentFormat_);
  }
}

void AudioOutputDevice::uninitialize() noexcept {
  if (!initialized_) {
    return;
  }

  if (started_) {
    static_cast<void>(backend_->stop());
    started_ = false;
  }

  deactivateCallbackQueue();
  backend_->uninitialize();
  currentFormat_ = {};
  currentQueue_ = nullptr;
  lastError_.reset();
  initialized_ = false;
  spdlog::info("device uninitialized");
}

bool AudioOutputDevice::initialized() const noexcept { return initialized_; }

bool AudioOutputDevice::started() const noexcept { return started_; }

AudioDeviceFormat AudioOutputDevice::currentFormat() const { return currentFormat_; }

std::optional<AudioOutputDeviceError> AudioOutputDevice::lastError() const { return lastError_; }

void AudioOutputDevice::setVolume(float linearGain) noexcept {
  if (std::isnan(linearGain)) {
    return;
  }
  const auto clamped = std::clamp(linearGain, 0.0F, 1.0F);
        spdlog::debug("device volume set to {:.2f}", clamped);
  volume_.store(clamped, std::memory_order_release);
}

void AudioOutputDevice::setMuted(bool muted) noexcept {
    spdlog::debug("device mute set to {}", muted);
  muted_.store(muted, std::memory_order_release);
}

void AudioOutputDevice::renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept {
  auto* device = static_cast<AudioOutputDevice*>(userData);
  if (device == nullptr) {
    return;
  }

  auto& state = device->callbackState_;
  const auto bytesPerFrame = state.bytesPerFrame.load(std::memory_order_acquire);
  const auto channelCount = state.channelCount.load(std::memory_order_acquire);
  const auto sampleFormat = state.sampleFormat.load(std::memory_order_acquire);
  const auto generation = state.queueGeneration.load(std::memory_order_acquire);
  auto* queue = state.pcmQueue.load(std::memory_order_acquire);

  PcmBufferReadResult result{};
  result.requestedFrames = frameCount;
  if (queue != nullptr && state.active.load(std::memory_order_acquire)) {
    result = queue->readIfGeneration(output, frameCount, generation);
  } else {
    result.silenceFrames = frameCount;
    fillSilence(output, frameCount, bytesPerFrame);
  }

  applyGain(output,
            result.copiedFrames,
            channelCount,
            sampleFormat,
            device->volume_.load(std::memory_order_acquire),
            device->muted_.load(std::memory_order_acquire));
  device->callbackCount_.fetch_add(1U, std::memory_order_relaxed);
  device->requestedFrames_.fetch_add(result.requestedFrames, std::memory_order_relaxed);
  device->copiedFrames_.fetch_add(result.copiedFrames, std::memory_order_relaxed);
  device->silenceFrames_.fetch_add(result.silenceFrames, std::memory_order_relaxed);
}

void AudioOutputDevice::publishCallbackQueue(PcmBufferQueue& queue, const AudioDeviceFormat& format) noexcept {
  callbackState_.active.store(false, std::memory_order_release);
  callbackState_.bytesPerFrame.store(bytesPerSample(format.sampleFormat) * format.channelCount, std::memory_order_release);
  callbackState_.channelCount.store(format.channelCount, std::memory_order_release);
  callbackState_.sampleFormat.store(format.sampleFormat, std::memory_order_release);
  callbackState_.queueGeneration.store(queue.generation(), std::memory_order_release);
  callbackState_.pcmQueue.store(&queue, std::memory_order_release);
  callbackState_.active.store(true, std::memory_order_release);
}

void AudioOutputDevice::deactivateCallbackQueue() noexcept {
  callbackState_.active.store(false, std::memory_order_release);
  callbackState_.queueGeneration.fetch_add(1U, std::memory_order_acq_rel);
  callbackState_.pcmQueue.store(nullptr, std::memory_order_release);
}

AudioOutputDeviceCounters AudioOutputDevice::counters() const noexcept {
  return AudioOutputDeviceCounters{callbackCount_.load(std::memory_order_relaxed),
                                   requestedFrames_.load(std::memory_order_relaxed),
                                   copiedFrames_.load(std::memory_order_relaxed),
                                   silenceFrames_.load(std::memory_order_relaxed)};
}

}
