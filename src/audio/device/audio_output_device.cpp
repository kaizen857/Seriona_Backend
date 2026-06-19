#include "seriona/audio/device/audio_output_device.h"

namespace seriona::audio {

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
  if (request.pcmQueue == nullptr || request.sampleRate == 0U || request.channelCount == 0U ||
      request.bufferFrames == 0U) {
    return false;
  }

  if (initialized_) {
    uninitialize();
  }

  auto backendRequest = request;
  backendRequest.callbackUserData = this;
  if (!backend_->initialize(backendRequest)) {
    return false;
  }

  pcmQueue_ = request.pcmQueue;
  callbackCount_.store(0U, std::memory_order_relaxed);
  requestedFrames_.store(0U, std::memory_order_relaxed);
  copiedFrames_.store(0U, std::memory_order_relaxed);
  silenceFrames_.store(0U, std::memory_order_relaxed);
  initialized_ = true;
  started_ = false;
  return true;
}

bool AudioOutputDevice::start() {
  if (!initialized_) {
    return false;
  }

  if (started_) {
    return true;
  }

  if (!backend_->start()) {
    return false;
  }

  started_ = true;
  return true;
}

bool AudioOutputDevice::stop() {
  if (!initialized_ || !started_) {
    return true;
  }

  if (!backend_->stop()) {
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
  initialized_ = false;
}

bool AudioOutputDevice::initialized() const noexcept { return initialized_; }

bool AudioOutputDevice::started() const noexcept { return started_; }

AudioDeviceFormat AudioOutputDevice::currentFormat() const { return backend_->currentFormat(); }

void AudioOutputDevice::renderCallback(void* userData, void* output, std::uint32_t frameCount) noexcept {
  auto* device = static_cast<AudioOutputDevice*>(userData);
  if (device == nullptr || device->pcmQueue_ == nullptr) {
    return;
  }

  const auto result = device->pcmQueue_->read(output, frameCount);
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
