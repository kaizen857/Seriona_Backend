// 设备格式能力合并逻辑测试：makeAudioPlaybackService(fakeBackend, fakeEnumerator)
// 的 enumeratePlaybackDevices 会用平台枚举器（此处为 fake）的能力数据覆盖
// 播放后端报告的 supportedSampleFormats/supportedSampleRates。
// 全部使用 fake，不依赖真实硬件（AGENTS.md 音频测试约束）。

#include "seriona/audio/audio_playback_service.h"
#include "seriona/audio/device/audio_device_format_enumerator.h"
#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
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

class FakeFormatEnumerator final : public DeviceFormatEnumerator {
public:
  explicit FakeFormatEnumerator(std::vector<DeviceFormatCapabilities> capabilities)
      : capabilities_(std::move(capabilities)) {}

  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override {
    ++enumerateCalls;
    return capabilities_;
  }

  int enumerateCalls{0};

private:
  std::vector<DeviceFormatCapabilities> capabilities_;
};

std::vector<AudioDeviceFormat> twoFakeDevices() {
  std::vector<AudioDeviceFormat> devices;
  devices.push_back(AudioDeviceFormat{.deviceId = "alsa_output.pci-0000_04_00.6.HiFi__Speaker__sink",
                                      .deviceName = "Ryzen HD Audio Controller Speaker",
                                      .backendName = "miniaudio",
                                      .sampleRate = 48000,
                                      .sampleFormat = AudioSampleFormat::Float32,
                                      .channelCount = 2,
                                      .bufferFrames = 512,
                                      .actualMode = AudioOutputMode::Mixed,
                                      .fallbackApplied = false,
                                      .supportedSampleFormats = {},
                                      .supportedSampleRates = {}});
  devices.push_back(AudioDeviceFormat{.deviceId = "dev-2",
                                      .deviceName = "Device Two",
                                      .backendName = "miniaudio",
                                      .sampleRate = 96000,
                                      .sampleFormat = AudioSampleFormat::Int16,
                                      .channelCount = 1,
                                      .bufferFrames = 256,
                                      .actualMode = AudioOutputMode::Direct,
                                      .fallbackApplied = false,
                                      .supportedSampleFormats = {},
                                      .supportedSampleRates = {}});
  return devices;
}

// 构造带能力数据的设备：默认带 48000 单格式，便于断言覆盖/保留。
AudioDeviceFormat deviceWithCapabilities(const AudioDeviceFormat& device) {
  auto copy = device;
  copy.supportedSampleFormats = {AudioSampleFormat::Int16};
  copy.supportedSampleRates = {48000};
  return copy;
}

std::shared_ptr<AudioPlaybackService> makeService(std::unique_ptr<AudioOutputDeviceBackend> backend,
                                                  std::unique_ptr<DeviceFormatEnumerator> enumerator) {
  return makeAudioPlaybackService(std::move(backend), std::move(enumerator));
}

}

TEST_CASE("device format enumerator caps override matched device by deviceId") {
  const auto devices = twoFakeDevices();
  auto backend = std::make_unique<FakeEnumerateBackend>(devices);
  DeviceFormatCapabilities caps{};
  caps.deviceId = devices[0].deviceId;
  caps.deviceName = "irrelevant name";
  caps.supportedSampleFormats = {AudioSampleFormat::Float32, AudioSampleFormat::Int32};
  caps.supportedSampleRates = {44100, 48000, 96000, 192000};
  auto enumerator = std::make_unique<FakeFormatEnumerator>(std::vector<DeviceFormatCapabilities>{caps});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 2U);
  CHECK(result[0].supportedSampleFormats == caps.supportedSampleFormats);
  CHECK(result[0].supportedSampleRates == caps.supportedSampleRates);
  // 未匹配设备保留原能力。
  CHECK(result[1].supportedSampleFormats.empty());
  CHECK(result[1].supportedSampleRates.empty());
  // 播放字段不被覆盖。
  CHECK(result[0].backendName == "miniaudio");
  CHECK(result[0].sampleRate == 48000U);
}

TEST_CASE("device format enumerator caps override matched device by deviceName") {
  const auto devices = twoFakeDevices();
  auto backend = std::make_unique<FakeEnumerateBackend>(devices);
  DeviceFormatCapabilities caps{};
  caps.deviceId = "unrelated";
  caps.deviceName = devices[1].deviceName;
  caps.supportedSampleFormats = {AudioSampleFormat::Int16, AudioSampleFormat::Int24};
  caps.supportedSampleRates = {32000, 44100};
  auto enumerator = std::make_unique<FakeFormatEnumerator>(std::vector<DeviceFormatCapabilities>{caps});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 2U);
  CHECK(result[1].supportedSampleFormats == caps.supportedSampleFormats);
  CHECK(result[1].supportedSampleRates == caps.supportedSampleRates);
  CHECK(result[0].supportedSampleFormats.empty());
  CHECK(result[0].supportedSampleRates.empty());
}

TEST_CASE("device format enumerator empty caps do not overwrite existing capabilities") {
  auto device = twoFakeDevices()[0];
  device.supportedSampleFormats = {AudioSampleFormat::Float32};
  device.supportedSampleRates = {48000, 96000};
  auto backend = std::make_unique<FakeEnumerateBackend>(std::vector<AudioDeviceFormat>{device});

  // 全部空列表：空=未枚举或全支持，不得覆盖。
  DeviceFormatCapabilities emptyCaps{};
  emptyCaps.deviceId = device.deviceId;
  auto enumerator = std::make_unique<FakeFormatEnumerator>(std::vector<DeviceFormatCapabilities>{emptyCaps});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 1U);
  CHECK(result[0].supportedSampleFormats == device.supportedSampleFormats);
  CHECK(result[0].supportedSampleRates == device.supportedSampleRates);
}

TEST_CASE("device format enumerator partial caps override only non-empty list") {
  auto device = twoFakeDevices()[0];
  device.supportedSampleFormats = {AudioSampleFormat::Float32};
  device.supportedSampleRates = {48000, 96000};
  auto backend = std::make_unique<FakeEnumerateBackend>(std::vector<AudioDeviceFormat>{device});

  DeviceFormatCapabilities caps{};
  caps.deviceId = device.deviceId;
  caps.supportedSampleRates = {44100, 192000};  // 只提供采样率
  auto enumerator = std::make_unique<FakeFormatEnumerator>(std::vector<DeviceFormatCapabilities>{caps});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 1U);
  CHECK(result[0].supportedSampleRates == caps.supportedSampleRates);
  CHECK(result[0].supportedSampleFormats == device.supportedSampleFormats);
}

TEST_CASE("device format enumerator unmatched device keeps backend-reported capabilities") {
  auto device = twoFakeDevices()[0];
  device.supportedSampleFormats = {AudioSampleFormat::Float32};
  device.supportedSampleRates = {48000};
  auto backend = std::make_unique<FakeEnumerateBackend>(std::vector<AudioDeviceFormat>{device});

  DeviceFormatCapabilities caps{};
  caps.deviceId = "other-device";
  caps.deviceName = "Other Device";
  caps.supportedSampleFormats = {AudioSampleFormat::Int16};
  caps.supportedSampleRates = {44100};
  auto enumerator = std::make_unique<FakeFormatEnumerator>(std::vector<DeviceFormatCapabilities>{caps});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 1U);
  CHECK(result[0].supportedSampleFormats == device.supportedSampleFormats);
  CHECK(result[0].supportedSampleRates == device.supportedSampleRates);
}

TEST_CASE("device format enumerator matches each device with its own caps") {
  const auto devices = twoFakeDevices();
  auto backend = std::make_unique<FakeEnumerateBackend>(devices);

  DeviceFormatCapabilities capsOne{};
  capsOne.deviceId = devices[0].deviceId;
  capsOne.supportedSampleFormats = {AudioSampleFormat::Float32};
  capsOne.supportedSampleRates = {48000, 192000};
  DeviceFormatCapabilities capsTwo{};
  capsTwo.deviceName = devices[1].deviceName;
  capsTwo.supportedSampleFormats = {AudioSampleFormat::Int16};
  capsTwo.supportedSampleRates = {44100};
  auto enumerator = std::make_unique<FakeFormatEnumerator>(
      std::vector<DeviceFormatCapabilities>{capsOne, capsTwo});

  auto service = makeService(std::move(backend), std::move(enumerator));
  REQUIRE(service != nullptr);
  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 2U);
  CHECK(result[0].supportedSampleFormats == capsOne.supportedSampleFormats);
  CHECK(result[0].supportedSampleRates == capsOne.supportedSampleRates);
  CHECK(result[1].supportedSampleFormats == capsTwo.supportedSampleFormats);
  CHECK(result[1].supportedSampleRates == capsTwo.supportedSampleRates);
}

TEST_CASE("device format enumerator service without enumerator keeps backend devices") {
  const auto expected = twoFakeDevices();
  auto backend = std::make_unique<FakeEnumerateBackend>(expected);
  auto service = makeAudioPlaybackService(std::move(backend));
  REQUIRE(service != nullptr);

  const auto result = service->enumeratePlaybackDevices();

  REQUIRE(result.size() == 2U);
  CHECK(result[0].supportedSampleFormats.empty());
  CHECK(result[0].supportedSampleRates.empty());
}

}

namespace seriona::audio {
// ---------------------------------------------------------------------------
// CachingDeviceFormatEnumerator 缓存行为：enumerate() 只读缓存，真实平台
// 枚举在后台线程执行（构造即预热），不阻塞调用线程（worker 线程上的
// 长阻塞会暂停 fillQueue 导致 buffer underrun——见类注释）。
// ---------------------------------------------------------------------------

namespace {

class DelayedFakeFormatEnumerator final : public DeviceFormatEnumerator {
public:
  explicit DelayedFakeFormatEnumerator(std::chrono::milliseconds delay)
      : delay_(delay),
        capabilities_({[] {
          DeviceFormatCapabilities caps{};
          caps.deviceId = "dev-cached";
          caps.supportedSampleFormats = {AudioSampleFormat::Int32};
          caps.supportedSampleRates = {48000, 96000};
          return caps;
        }()}) {}

  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override {
    enumerateCalls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(delay_);
    return capabilities_;
  }

  std::atomic<int> enumerateCalls{0};

private:
  std::chrono::milliseconds delay_;
  std::vector<DeviceFormatCapabilities> capabilities_;
};

}

TEST_CASE("caching enumerator warms up in background and serves cached capabilities") {
  auto platform = std::make_unique<DelayedFakeFormatEnumerator>(std::chrono::milliseconds{10});
  auto* rawPlatform = platform.get();
  auto caching = std::make_unique<CachingDeviceFormatEnumerator>(std::move(platform));

  // 构造即后台预热；等待后台线程完成（10ms 延迟 + 调度余量）。
  std::this_thread::sleep_for(std::chrono::milliseconds{100});

  const auto result = caching->enumerate();
  REQUIRE(result.size() == 1U);
  CHECK(result[0].deviceId == "dev-cached");
  CHECK(result[0].supportedSampleFormats == std::vector<AudioSampleFormat>{AudioSampleFormat::Int32});
  CHECK(result[0].supportedSampleRates == std::vector<std::uint32_t>{48000, 96000});
  // 预热只跑一次平台枚举，enumerate() 本身不再触发。
  CHECK(rawPlatform->enumerateCalls == 1);
}

TEST_CASE("caching enumerator returns immediately before background refresh completes") {
  auto platform = std::make_unique<DelayedFakeFormatEnumerator>(std::chrono::milliseconds{200});
  auto* rawPlatform = platform.get();
  auto caching = std::make_unique<CachingDeviceFormatEnumerator>(std::move(platform));

  // 预热未完成时 enumerate() 立即返回空缓存，等待时间远小于平台延迟。
  const auto started = std::chrono::steady_clock::now();
  const auto result = caching->enumerate();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(result.empty());
  CHECK(elapsed < std::chrono::milliseconds{100});
  // 不检查 enumerateCalls：后台线程是否已调度到属于竞态，本用例只验证不阻塞。
}

TEST_CASE("caching enumerator serves cache without re-invoking platform within ttl") {
  auto platform = std::make_unique<DelayedFakeFormatEnumerator>(std::chrono::milliseconds{1});
  auto* rawPlatform = platform.get();
  auto caching = std::make_unique<CachingDeviceFormatEnumerator>(std::move(platform));

  std::this_thread::sleep_for(std::chrono::milliseconds{50});  // 等预热完成

  for (int i = 0; i < 3; ++i) {
    const auto result = caching->enumerate();
    REQUIRE(result.size() == 1U);
  }
  CHECK(rawPlatform->enumerateCalls.load() == 1);  // TTL 内不重复平台枚举
}

TEST_CASE("caching enumerator destruction joins background thread safely") {
  auto platform = std::make_unique<DelayedFakeFormatEnumerator>(std::chrono::milliseconds{50});
  {
    auto caching = std::make_unique<CachingDeviceFormatEnumerator>(std::move(platform));
    // 预热尚未完成时立即析构：析构必须 join 等待后台线程退出，不崩溃。
  }
  CHECK(true);
}

}
