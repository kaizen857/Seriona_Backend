#include "seriona/audio/audio_playback_service.h"
#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace {

class FakeEnumerateBackend final : public AudioOutputDeviceBackend {
public:
  explicit FakeEnumerateBackend(std::vector<AudioDeviceFormat> devices) : devices_(std::move(devices)) {}

  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override {
    ++enumerateCalls;
    return devices_;
  }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest&) override { return false; }
  [[nodiscard]] bool start() override { return false; }
  [[nodiscard]] bool stop() override { return true; }
  void uninitialize() noexcept override {}
  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return AudioDeviceFormat{}; }

  int enumerateCalls{0};

private:
  std::vector<AudioDeviceFormat> devices_;
};

// 模拟 NoopAudioPlaybackService（src/control/media_controller_module.cpp:21-40）：
// 全部方法空实现，不覆写 enumeratePlaybackDevices——基类非纯虚默认返回空列表
// 正是 Noop 的机制；后端控制层 FakeAudioPlaybackService（tests/control/
// control_test_harness.h:61）与前端 FakeAudioPlaybackService 同机制。
class NoopStylePlaybackService final : public AudioPlaybackService {
public:
  void setEventSink(BackendEventSink) override {}
  void configureOutput(const AudioOutputConfig&) override {}
  void loadTrack(const TrackPlaybackRequest&) override {}
  void prepareNext(const TrackPlaybackRequest&) override {}
  void play() override {}
  void pause() override {}
  void resume() override {}
  void stop() override {}
  void seek(std::chrono::milliseconds) override {}
  void setVolume(float) override {}
  void setMuted(bool) override {}
  void selectOutputDevice(const std::string&) override {}
  [[nodiscard]] PlaybackClockSnapshot queryPlaybackClock() const override { return {}; }
};

std::vector<AudioDeviceFormat> twoFakeDevices() {
  std::vector<AudioDeviceFormat> devices;
  devices.push_back(AudioDeviceFormat{.deviceId = "dev-1",
                                      .deviceName = "Device One",
                                      .backendName = "fake",
                                      .sampleRate = 48000,
                                      .sampleFormat = AudioSampleFormat::Float32,
                                      .channelCount = 2,
                                      .bufferFrames = 512,
                                      .actualMode = AudioOutputMode::Mixed,
                                      .fallbackApplied = false});
  devices.push_back(AudioDeviceFormat{.deviceId = "dev-2",
                                      .deviceName = "Device Two",
                                      .backendName = "fake",
                                      .sampleRate = 96000,
                                      .sampleFormat = AudioSampleFormat::Int16,
                                      .channelCount = 1,
                                      .bufferFrames = 256,
                                      .actualMode = AudioOutputMode::Direct,
                                      .fallbackApplied = false});
  return devices;
}

}

TEST_CASE("audio_playback_service enumerate_playback_devices returns fake backend devices with full fields") {
  const auto expected = twoFakeDevices();
  auto backend = std::make_unique<FakeEnumerateBackend>(expected);
  auto* fake = backend.get();

  auto service = makeAudioPlaybackService(std::move(backend));
  REQUIRE(service != nullptr);

  const auto devices = service->enumeratePlaybackDevices();

  REQUIRE(devices.size() == 2U);
  CHECK(fake->enumerateCalls == 1);

  CHECK(devices[0].deviceId == "dev-1");
  CHECK(devices[0].deviceName == "Device One");
  CHECK(devices[0].sampleRate == 48000U);
  CHECK(devices[0].channelCount == 2U);
  CHECK(devices[0].sampleFormat == AudioSampleFormat::Float32);
  CHECK(devices[0].actualMode == AudioOutputMode::Mixed);

  CHECK(devices[1].deviceId == "dev-2");
  CHECK(devices[1].deviceName == "Device Two");
  CHECK(devices[1].sampleRate == 96000U);
  CHECK(devices[1].channelCount == 1U);
  CHECK(devices[1].sampleFormat == AudioSampleFormat::Int16);
  CHECK(devices[1].actualMode == AudioOutputMode::Direct);
}

TEST_CASE("audio_playback_service noop style and base default return empty device list") {
  NoopStylePlaybackService service;

  // 派生类视图：不覆写枚举方法 → 继承基类默认实现，返回空列表（Noop 同机制）。
  CHECK(service.enumeratePlaybackDevices().empty());

  // 基类视图：AudioPlaybackService 默认实现返回空列表——必须是默认实现而非纯虚，
  // 否则 4 个实现者（SingleTrack/Noop/后端 Fake/前端 Fake）全部编译失败。
  auto* base = static_cast<const AudioPlaybackService*>(&service);
  CHECK(base->enumeratePlaybackDevices().empty());
}

}
