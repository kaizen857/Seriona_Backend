// PipeWire 设备格式枚举真实环境测试（仅 Linux 注册，见 tests/CMakeLists.txt
// 的 if(UNIX AND NOT APPLE) 分支）。无 PipeWire 守护进程时 enumerate()
// 返回空列表（pw_context_connect 失败），测试 SUCCEED 跳过——平台能力
// 测试不依赖真实硬件（AGENTS.md 测试约束），守卫模式同
// metadata_mpris_tests.cpp:154-189。

#include "seriona/audio/device/audio_device_format_enumerator.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace seriona::audio {
namespace {

TEST_CASE("pipewire device format enumerator returns sink capabilities on a live daemon") {
  auto enumerator = makeDeviceFormatEnumerator();
  REQUIRE(enumerator != nullptr);

  const auto capabilities = enumerator->enumerate();
  if (capabilities.empty()) {
    // 无 PipeWire 守护进程（pw_context_connect 失败）或无可枚举 sink：
    // 视为跳过，测试通过（无断言即通过，doctest 2.5.2 无 SUCCEED 宏）。
    return;
  }

  // 有真实 sink：至少一个设备应带非空采样率能力（EnumFormat 的
  // Audio:rate 是 sink 必需字段）。
  const bool anyRates = std::any_of(capabilities.begin(), capabilities.end(),
                                    [](const DeviceFormatCapabilities& caps) {
                                      return !caps.supportedSampleRates.empty();
                                    });
  CHECK_MESSAGE(anyRates, "at least one pipewire sink must report sample rates");
}

}
}
