#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace seriona::audio {
namespace {

using StereoFrame = std::array<std::uint8_t, 4>;

constexpr PcmBufferQueueConfig queueConfig(std::uint32_t frames) {
  return PcmBufferQueueConfig{frames, static_cast<std::uint32_t>(sizeof(StereoFrame))};
}

AudioOutputDeviceOpenRequest openRequest(PcmBufferQueue& queue) {
  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  return AudioOutputDeviceOpenRequest{.config = config,
                                      .sampleFormat = AudioSampleFormat::Int16,
                                      .sampleRate = 48000,
                                      .channelCount = 2,
                                      .bufferFrames = 4,
                                      .pcmQueue = &queue};
}

class FakeAudioOutputDeviceBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override {
    ++enumerateCalls;
    return {format};
  }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    CHECK_FALSE(inCallback);
    ++initializeCalls;
    lastCallbackUserData = request.callbackUserData;
    format.deviceId = request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    initialized = initializeResult;
    return initializeResult;
  }

  [[nodiscard]] bool start() override {
    CHECK_FALSE(inCallback);
    ++startCalls;
    started = startResult;
    return startResult;
  }

  [[nodiscard]] bool stop() override {
    CHECK_FALSE(inCallback);
    ++stopCalls;
    started = false;
    return stopResult;
  }

  void uninitialize() noexcept override {
    CHECK_FALSE(inCallback);
    ++uninitializeCalls;
    initialized = false;
  }

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Int16,
                           .channelCount = 2,
                           .bufferFrames = 4,
                           .actualMode = AudioOutputMode::Mixed};
  AudioOutputDevice* lastCallbackUserData{nullptr};
  int enumerateCalls{0};
  int initializeCalls{0};
  int startCalls{0};
  int stopCalls{0};
  int uninitializeCalls{0};
  bool initializeResult{true};
  bool startResult{true};
  bool stopResult{true};
  bool initialized{false};
  bool started{false};
  bool inCallback{false};
};

}

TEST_CASE("audio_output_device callback copies queued pcm and updates counters") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 2> input{StereoFrame{1, 2, 3, 4}, StereoFrame{5, 6, 7, 8}};
  std::array<StereoFrame, 2> output{};

  CHECK(queue.write(input.data(), static_cast<std::uint32_t>(input.size())));
  CHECK(device.initialize(openRequest(queue)));
  fake->inCallback = true;
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));
  fake->inCallback = false;

  CHECK(output == input);
  const auto deviceCounters = device.counters();
  CHECK(deviceCounters.callbackCount == 1U);
  CHECK(deviceCounters.requestedFrames == 2U);
  CHECK(deviceCounters.copiedFrames == 2U);
  CHECK(deviceCounters.silenceFrames == 0U);
  CHECK(queue.counters().consumedFrames == 2U);
  CHECK(fake->startCalls == 0);
  CHECK(fake->stopCalls == 0);
  CHECK(fake->uninitializeCalls == 0);
}

TEST_CASE("audio_output_device callback fills silence on underrun") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));
  std::array<StereoFrame, 3> output{StereoFrame{9, 9, 9, 9}, StereoFrame{8, 8, 8, 8}, StereoFrame{7, 7, 7, 7}};

  CHECK(device.initialize(openRequest(queue)));
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  for (const auto& frame : output) {
    CHECK(frame == StereoFrame{0, 0, 0, 0});
  }
  const auto deviceCounters = device.counters();
  CHECK(deviceCounters.callbackCount == 1U);
  CHECK(deviceCounters.requestedFrames == 3U);
  CHECK(deviceCounters.copiedFrames == 0U);
  CHECK(deviceCounters.silenceFrames == 3U);
  CHECK(queue.counters().underrunCount == 1U);
  CHECK(queue.counters().silenceFrames == 3U);
}

TEST_CASE("audio_output_device lifecycle runs through fake backend outside callback") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));

  const auto devices = device.enumeratePlaybackDevices();
  CHECK(devices.size() == 1U);
  CHECK(fake->enumerateCalls == 1);

  CHECK(device.initialize(openRequest(queue)));
  CHECK(device.initialized());
  CHECK(fake->lastCallbackUserData == &device);
  CHECK(fake->initializeCalls == 1);
  CHECK(device.currentFormat().deviceId == "fake-device");

  CHECK(device.start());
  CHECK(device.started());
  CHECK(fake->startCalls == 1);

  CHECK(device.stop());
  CHECK_FALSE(device.started());
  CHECK(fake->stopCalls == 1);

  device.uninitialize();
  CHECK_FALSE(device.initialized());
  CHECK(fake->uninitializeCalls == 1);
}

TEST_CASE("audio_output_device uninitialize stops started device first") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));

  CHECK(device.initialize(openRequest(queue)));
  CHECK(device.start());
  device.uninitialize();

  CHECK(fake->stopCalls == 1);
  CHECK(fake->uninitializeCalls == 1);
  CHECK_FALSE(device.initialized());
  CHECK_FALSE(device.started());
}

}
