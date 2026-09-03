#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

// --- S24（Int24，3 字节小端打包）测试辅助 --------------------------------------------
// 被测路径黑盒验证：队列内容布局与 ffmpeg_filter_pipeline.cpp packS32ToS24 产物一致
// （24 位内容小端 3 字节，第 3 字节高位为符号位），经 renderCallback 读取后由
// AudioOutputDevice 内部增益。下述打包/解包是测试侧独立参考实现，不依赖被测内部函数。

constexpr PcmBufferQueueConfig queueConfigWithFrameBytes(std::uint32_t frames, std::uint32_t bytesPerFrame) {
  return PcmBufferQueueConfig{frames, bytesPerFrame};
}

AudioOutputDeviceOpenRequest openRequestForFormat(PcmBufferQueue& queue, AudioSampleFormat format) {
  AudioOutputConfig config{};
  config.preferredDeviceId = "fake-device";
  return AudioOutputDeviceOpenRequest{.config = config,
                                      .sampleFormat = format,
                                      .sampleRate = 48000,
                                      .channelCount = 2,
                                      .bufferFrames = 4,
                                      .pcmQueue = &queue};
}

void packS24Reference(std::int32_t value24, std::uint8_t* bytes) noexcept {
  const auto raw = static_cast<std::uint32_t>(value24) & 0xFFFFFFU;
  bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
}

void packS32LeftAlignedReference(std::int32_t value24, std::uint8_t* bytes) noexcept {
  // Int32 参考路径输入：24 位内容左对齐到 S32 域（packS32ToS24 的逆布局）。
  const auto raw = static_cast<std::uint32_t>(value24) << 8U;
  bytes[0] = static_cast<std::uint8_t>(raw & 0xFFU);
  bytes[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  bytes[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
  bytes[3] = static_cast<std::uint8_t>((raw >> 24U) & 0xFFU);
}

std::int32_t unpackS24Reference(const std::uint8_t* bytes) noexcept {
  const auto raw24 = static_cast<std::uint32_t>(bytes[0]) |
                     (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16U);
  return raw24 >= 0x800000U ? static_cast<std::int32_t>(raw24) - 0x1000000
                            : static_cast<std::int32_t>(raw24);
}

// 与设备内实现同构的参考数学：24 位值左对齐（×256）到 S32 域，double 增益 + llround，
// 再取高 24 位（算术右移 8，C++23 保证）。llround 半程远离零，与 applyInt32Gain 一致。
std::int32_t expectedS24Gain(std::int32_t value24, float gain) noexcept {
  const auto scaled = std::llround(static_cast<double>(value24) * 256.0 * static_cast<double>(gain));
  return static_cast<std::int32_t>(scaled >> 8);
}

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

TEST_CASE("audio_output_device callback applies volume and mute without backend calls") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 2> input{StereoFrame{100, 0, 200, 0}, StereoFrame{50, 0, 150, 0}};
  std::array<StereoFrame, 2> output{};

  CHECK(queue.write(input.data(), static_cast<std::uint32_t>(input.size())));
  CHECK(device.initialize(openRequest(queue)));
  device.setVolume(0.5F);
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(output == std::array<StereoFrame, 2>{StereoFrame{50, 0, 100, 0}, StereoFrame{25, 0, 75, 0}});
  CHECK(queue.write(input.data(), static_cast<std::uint32_t>(input.size())));
  device.setMuted(true);
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));
  for (const auto& frame : output) {
    CHECK(frame == StereoFrame{0, 0, 0, 0});
  }
  CHECK(fake->startCalls == 0);
  CHECK(fake->stopCalls == 0);
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

TEST_CASE("audio_output_device callback after stop observes inactive generation and fills silence") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(4));
  const std::array<StereoFrame, 2> stale{StereoFrame{9, 9, 9, 9}, StereoFrame{8, 8, 8, 8}};
  std::array<StereoFrame, 2> output{StereoFrame{1, 1, 1, 1}, StereoFrame{1, 1, 1, 1}};

  CHECK(queue.write(stale.data(), static_cast<std::uint32_t>(stale.size())));
  CHECK(device.initialize(openRequest(queue)));
  CHECK(device.start());
  CHECK(device.stop());
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  for (const auto& frame : output) {
    CHECK(frame == StereoFrame{0, 0, 0, 0});
  }
  const auto counters = device.counters();
  CHECK(counters.callbackCount == 1U);
  CHECK(counters.requestedFrames == 2U);
  CHECK(counters.copiedFrames == 0U);
  CHECK(counters.silenceFrames == 2U);
  CHECK(queue.counters().consumedFrames == 0U);
}

TEST_CASE("audio_output_device resume republishes callback queue after stop") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue queue(queueConfig(8));
  const std::array<StereoFrame, 2> input{StereoFrame{1, 2, 3, 4}, StereoFrame{5, 6, 7, 8}};
  std::array<StereoFrame, 2> output{};

  CHECK(device.initialize(openRequest(queue)));
  CHECK(device.start());
  CHECK(device.stop());
  CHECK(queue.write(input.data(), static_cast<std::uint32_t>(input.size())));
  CHECK(device.start());
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(output == input);
  CHECK(fake->startCalls == 2);
  CHECK(fake->stopCalls == 1);
  CHECK(queue.counters().consumedFrames == 2U);
}

TEST_CASE("audio_output_device can rebind callback queue before restart") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  auto* fake = backend.get();
  AudioOutputDevice device(std::move(backend));
  PcmBufferQueue staleQueue(queueConfig(8));
  PcmBufferQueue freshQueue(queueConfig(8));
  const std::array<StereoFrame, 1> stale{StereoFrame{9, 9, 9, 9}};
  const std::array<StereoFrame, 1> fresh{StereoFrame{1, 2, 3, 4}};
  std::array<StereoFrame, 1> output{};

  CHECK(staleQueue.write(stale.data(), static_cast<std::uint32_t>(stale.size())));
  CHECK(freshQueue.write(fresh.data(), static_cast<std::uint32_t>(fresh.size())));
  CHECK(device.initialize(openRequest(staleQueue)));
  CHECK(device.start());
  CHECK(device.stop());

  device.rebindQueue(freshQueue);
  CHECK(device.start());
  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(output == fresh);
  CHECK(fake->startCalls == 2);
  CHECK(fake->stopCalls == 1);
  CHECK(staleQueue.counters().consumedFrames == 0U);
  CHECK(freshQueue.counters().consumedFrames == 1U);
}

TEST_CASE("audio_output_device callback after uninitialize has no queue lifetime dependency") {
  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioOutputDevice device(std::move(backend));
  std::array<StereoFrame, 1> output{StereoFrame{3, 3, 3, 3}};

  {
    PcmBufferQueue queue(queueConfig(4));
    const std::array<StereoFrame, 1> stale{StereoFrame{4, 4, 4, 4}};
    CHECK(queue.write(stale.data(), static_cast<std::uint32_t>(stale.size())));
    CHECK(device.initialize(openRequest(queue)));
    device.uninitialize();
  }

  AudioOutputDevice::renderCallback(&device, output.data(), static_cast<std::uint32_t>(output.size()));

  CHECK(output[0] == StereoFrame{0, 0, 0, 0});
  const auto counters = device.counters();
  CHECK(counters.callbackCount == 1U);
  CHECK(counters.requestedFrames == 1U);
  CHECK(counters.copiedFrames == 0U);
  CHECK(counters.silenceFrames == 1U);
}

TEST_CASE("audio_output_device s24 gain scales boundary samples with correct sign extension and endianness") {
  // 覆盖任务 4 样本矩阵：±(2^23-1)、-2^23、0 及符号扩展/字节序判别样本。
  // 期望值由与 Int32 参考同构的数学独立计算（见 expectedS24Gain）。
  constexpr auto kPosMax = (std::int32_t{1} << 23) - 1;   //  8388607 = 2^23-1
  constexpr auto kNegMax = -(std::int32_t{1} << 23);      // -8388608 = -2^23
  const std::vector<std::int32_t> values{0,   1,         -1,        kPosMax,     -kPosMax, kNegMax,
                                         2,   -2,         3,         -3,         0x010203, -0x010203,
                                         100000,         -654321,   4194303,     -4194304};
  REQUIRE(values.size() % 2U == 0U);
  const auto frameCount = static_cast<std::uint32_t>(values.size() / 2U);
  REQUIRE(frameCount > 0U);

  PcmBufferQueue queue(queueConfigWithFrameBytes(16, 6U));
  std::vector<std::uint8_t> inputBytes(values.size() * 3U);
  for (std::size_t index = 0; index < values.size(); ++index) {
    packS24Reference(values[index], inputBytes.data() + index * 3U);
  }
  CHECK(queue.write(inputBytes.data(), frameCount));

  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioOutputDevice device(std::move(backend));
  CHECK(device.initialize(openRequestForFormat(queue, AudioSampleFormat::Int24)));

  for (const float gain : {0.5F, 0.7F}) {
    std::vector<std::uint8_t> outputBytes(values.size() * 3U, 0xAA);
    device.setVolume(gain);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), frameCount);
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto actual = unpackS24Reference(outputBytes.data() + index * 3U);
      CAPTURE(values[index]);
      CAPTURE(gain);
      CHECK(actual == expectedS24Gain(values[index], gain));
    }
    CHECK(queue.write(inputBytes.data(), frameCount));  // 队列已消费，回填后测下一增益
  }

  // 增益 1.0：早退路径，输出字节与输入逐位一致（不重打包）。
  {
    std::vector<std::uint8_t> outputBytes(values.size() * 3U, 0xAA);
    device.setVolume(1.0F);
    AudioOutputDevice::renderCallback(&device, outputBytes.data(), frameCount);
    CHECK(outputBytes == inputBytes);
  }
}

TEST_CASE("audio_output_device s24 gain zero and mute silence the packed samples") {
  PcmBufferQueue queue(queueConfigWithFrameBytes(8, 6U));
  const std::vector<std::int32_t> values{8388607, -8388608, 123, -456};
  std::vector<std::uint8_t> inputBytes(values.size() * 3U);
  for (std::size_t index = 0; index < values.size(); ++index) {
    packS24Reference(values[index], inputBytes.data() + index * 3U);
  }
  const auto frameCount = static_cast<std::uint32_t>(values.size() / 2U);
  CHECK(queue.write(inputBytes.data(), frameCount));

  auto backend = std::make_unique<FakeAudioOutputDeviceBackend>();
  AudioOutputDevice device(std::move(backend));
  CHECK(device.initialize(openRequestForFormat(queue, AudioSampleFormat::Int24)));

  device.setVolume(0.0F);
  std::vector<std::uint8_t> outputBytes(values.size() * 3U, 0x55);
  AudioOutputDevice::renderCallback(&device, outputBytes.data(), frameCount);
  for (std::size_t index = 0; index < values.size(); ++index) {
    CHECK(unpackS24Reference(outputBytes.data() + index * 3U) == 0);
  }
  const auto counters = device.counters();
  CHECK(counters.copiedFrames == frameCount);  // 静音来自增益路径而非补零

  CHECK(queue.write(inputBytes.data(), frameCount));
  device.setMuted(true);
  AudioOutputDevice::renderCallback(&device, outputBytes.data(), frameCount);
  for (std::size_t index = 0; index < values.size(); ++index) {
    CHECK(unpackS24Reference(outputBytes.data() + index * 3U) == 0);
  }
}

TEST_CASE("audio_output_device s24 gain path matches int32 reference path per sample") {
  // 等价验收（任务 4）：同 24 位内容左对齐输入、同增益下，S24 3 字节输出解包值
  // 与 Int32 参考路径输出取高 24 位逐样本一致（容差 ±1 LSB；0.5/1.0 下逐位相等）。
  constexpr auto kPosMax = (std::int32_t{1} << 23) - 1;
  constexpr auto kNegMax = -(std::int32_t{1} << 23);
  const std::vector<std::int32_t> values{0,   1,          -1,   kPosMax, -kPosMax, kNegMax,
                                         5,   -5,          7,    -7,      123456,   -987654,
                                         8388606,          -8388606, 262143,   -262144};
  REQUIRE(values.size() % 2U == 0U);
  const auto frameCount = static_cast<std::uint32_t>(values.size() / 2U);

  for (const float gain : {0.5F, 0.7F, 1.0F}) {
    PcmBufferQueue s24Queue(queueConfigWithFrameBytes(16, 6U));
    std::vector<std::uint8_t> s24Input(values.size() * 3U);
    for (std::size_t index = 0; index < values.size(); ++index) {
      packS24Reference(values[index], s24Input.data() + index * 3U);
    }
    CHECK(s24Queue.write(s24Input.data(), frameCount));
    auto s24Backend = std::make_unique<FakeAudioOutputDeviceBackend>();
    AudioOutputDevice s24Device(std::move(s24Backend));
    CHECK(s24Device.initialize(openRequestForFormat(s24Queue, AudioSampleFormat::Int24)));
    s24Device.setVolume(gain);
    std::vector<std::uint8_t> s24Output(values.size() * 3U, 0xAA);
    AudioOutputDevice::renderCallback(&s24Device, s24Output.data(), frameCount);

    PcmBufferQueue s32Queue(queueConfigWithFrameBytes(16, 8U));
    std::vector<std::uint8_t> s32Input(values.size() * 4U);
    for (std::size_t index = 0; index < values.size(); ++index) {
      packS32LeftAlignedReference(values[index], s32Input.data() + index * 4U);
    }
    CHECK(s32Queue.write(s32Input.data(), frameCount));
    auto s32Backend = std::make_unique<FakeAudioOutputDeviceBackend>();
    AudioOutputDevice s32Device(std::move(s32Backend));
    CHECK(s32Device.initialize(openRequestForFormat(s32Queue, AudioSampleFormat::Int32)));
    s32Device.setVolume(gain);
    std::vector<std::uint8_t> s32Output(values.size() * 4U, 0xAA);
    AudioOutputDevice::renderCallback(&s32Device, s32Output.data(), frameCount);

    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto s24Value = unpackS24Reference(s24Output.data() + index * 3U);
      const auto* s32Bytes = s32Output.data() + index * 4U;
      const auto s32Value = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(s32Bytes[0]) | (static_cast<std::uint32_t>(s32Bytes[1]) << 8U) |
          (static_cast<std::uint32_t>(s32Bytes[2]) << 16U) | (static_cast<std::uint32_t>(s32Bytes[3]) << 24U));
      const auto int32Reference24 = s32Value >> 8;  // 取高 24 位（算术移位）
      const auto delta = std::abs(s24Value - int32Reference24);
      CAPTURE(values[index]);
      CAPTURE(gain);
      CAPTURE(s24Value);
      CAPTURE(int32Reference24);
      CHECK(delta <= 1);
      if (gain == 0.5F || gain == 1.0F) {
        CHECK(delta == 0);  // 2 的幂增益下逐位相等
      }
    }
  }
}

}
