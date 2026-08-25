#include "seriona/audio/device/audio_device_format_enumerator.h"

#include "spdlog/spdlog.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::audio {

namespace {

// 无平台实现时的空枚举器：enumerate() 恒返回空列表。
// 调用方（AudioPlaybackService::enumeratePlaybackDevices）在能力列表为空
// 时保留播放后端设备自身报告的字段，因此空结果即"保持现状"。
class EmptyDeviceFormatEnumerator final : public DeviceFormatEnumerator {
public:
  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override { return {}; }
};

}

#if defined(__linux__) && !defined(__APPLE__)
namespace detail {
// 由 pipewire_device_format_enumerator.cpp 实现：PipeWire 原生 SPA
// EnumFormat 枚举（节点真实格式能力）。
std::unique_ptr<DeviceFormatEnumerator> makePipeWireDeviceFormatEnumerator();
}
#endif

#ifdef _WIN32
namespace detail {
// 由 windows_device_format_enumerator.cpp 实现：WASAPI 原生格式矩阵探测。
std::unique_ptr<DeviceFormatEnumerator> makeWindowsDeviceFormatEnumerator();
}
#endif

// 平台工厂：结构与 metadata 模块的 makeMetadataServiceBackendFromOptions
// （metadata_service_backend.cpp:285-318）一致——Linux 返回 PipeWire 实现、
// Windows 返回 WASAPI 实现、其余平台返回空枚举器并记录日志。
// 平台实现统一经 CachingDeviceFormatEnumerator 包装：真实枚举在后台
// 线程执行，enumerate() 只读缓存，避免在 audio worker 线程上同步执行
// 数百毫秒的平台探测（会暂停 fillQueue 导致 buffer underrun）。
std::unique_ptr<DeviceFormatEnumerator> makeDeviceFormatEnumerator() {
  std::unique_ptr<DeviceFormatEnumerator> platform;
#if defined(__linux__) && !defined(__APPLE__)
  spdlog::info("device format enumerator: linux/pipewire (SPA EnumFormat, cached)");
  platform = detail::makePipeWireDeviceFormatEnumerator();
#elif defined(_WIN32)
  spdlog::info("device format enumerator: windows/wasapi (format matrix probe, cached)");
  platform = detail::makeWindowsDeviceFormatEnumerator();
#else
  spdlog::info("device format enumerator: noop (unsupported platform)");
  platform = std::make_unique<EmptyDeviceFormatEnumerator>();
#endif
  return std::make_unique<CachingDeviceFormatEnumerator>(std::move(platform));
}

}
