// 仅 Windows 编译：WASAPI 原生设备采样率/位深格式枚举。
//
// WASAPI 没有"列出设备支持的格式"的 API，唯一途径是 common rate 表 ×
// format 表逐个 IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE) 探测：
// exclusive 模式反映驱动真实能力（shared 探测受引擎混音格式限制）。
// 每个 (rate, format) 用 WAVEFORMATEX 与 WAVEFORMATEXTENSIBLE 各试一次
// （1/2 声道场景），S_FALSE（closestMatch ≠ 精确支持）视为不支持。
//
// 本文件在 Linux 上不参与构建（CMake WIN32 分支），必须保持 MSVC 可编译：
// 不使用 designated initializers / gcc 扩展，全部用构造函数与显式赋值。

#ifdef _WIN32

#include "seriona/audio/device/audio_device_format_enumerator.h"

#include <windows.h>
#include <audioclient.h>
#include <comdef.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propkey.h>
#include <propvarutil.h>

#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace detail {
namespace {

// COM 指针 RAII 守卫：析构时 Release，operator& 供 CoCreateInstance 等
// 的 [out] 参数使用。不拷贝不移动，避免双释放。
template <typename T>
class ComPtrGuard {
public:
  ComPtrGuard() : ptr_(nullptr) {}
  explicit ComPtrGuard(T* ptr) : ptr_(ptr) {}
  ~ComPtrGuard() {
    if (ptr_ != nullptr) {
      ptr_->Release();
    }
  }

  ComPtrGuard(const ComPtrGuard&) = delete;
  ComPtrGuard& operator=(const ComPtrGuard&) = delete;

  T** operator&() { return &ptr_; }
  [[nodiscard]] T* get() const { return ptr_; }
  [[nodiscard]] T* operator->() const { return ptr_; }
  [[nodiscard]] explicit operator bool() const { return ptr_ != nullptr; }

private:
  T* ptr_;
};

// 探测矩阵：common rate 表（WASAPI 设备常用采样率全集）。
constexpr std::array<std::uint32_t, 13> kCommonRates = {
    8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 88200, 96000, 176400, 192000};

// 探测的格式规格。S24_32（24-in-32）在 Win10+ 必须用 WAVE_FORMAT_EXTENSIBLE
// + KSDATAFORMAT_SUBTYPE_PCM + wBitsPerSample=32 + 24 位有效位表达。
struct ProbeFormatSpec {
  AudioSampleFormat sampleFormat;
  WORD bitsPerSample;
  WORD validBitsPerSample;
  bool requiresExtensible;  // S24_32 强制 EXTENSIBLE（PCM tag 无有效位概念）
};

std::vector<ProbeFormatSpec> makeProbeFormats() {
  std::vector<ProbeFormatSpec> formats;
  formats.push_back(ProbeFormatSpec{AudioSampleFormat::Int16, 16, 16, false});
  formats.push_back(ProbeFormatSpec{AudioSampleFormat::Float32, 32, 32, false});
  formats.push_back(ProbeFormatSpec{AudioSampleFormat::Int32, 32, 32, false});
  formats.push_back(ProbeFormatSpec{AudioSampleFormat::Int24, 32, 24, true});
  return formats;
}

// 探测声道集合：mix format 声道数 <= 2 时 1/2 声道都试（多数硬件按
// 声道数区分能力），否则只试 mix format 的声道数。
std::vector<WORD> makeProbeChannels(WORD mixChannels) {
  std::vector<WORD> channels;
  if (mixChannels <= 2U) {
    channels.push_back(1);
    channels.push_back(2);
  } else {
    channels.push_back(mixChannels);
  }
  return channels;
}

// 构造 WAVEFORMATEX（non-extensible 版本）。subFormat 仅由调用方按格式
// 判定，此版本只用于 PCM / IEEE_FLOAT 平铺格式。
WAVEFORMATEX buildWaveFormatEx(const ProbeFormatSpec& spec, std::uint32_t rate, WORD channels) {
  WAVEFORMATEX wave{};
  if (spec.sampleFormat == AudioSampleFormat::Float32) {
    wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  } else {
    wave.wFormatTag = WAVE_FORMAT_PCM;
  }
  wave.nChannels = channels;
  wave.nSamplesPerSec = rate;
  wave.wBitsPerSample = spec.bitsPerSample;
  wave.nBlockAlign = static_cast<WORD>((static_cast<std::uint32_t>(channels) * spec.bitsPerSample) / 8U);
  wave.nAvgBytesPerSec = rate * wave.nBlockAlign;
  wave.cbSize = 0;
  return wave;
}

// 构造 WAVEFORMATEXTENSIBLE 版本（含有效位与 SubFormat）。
WAVEFORMATEXTENSIBLE buildWaveFormatExtensible(const ProbeFormatSpec& spec,
                                               std::uint32_t rate,
                                               WORD channels) {
  WAVEFORMATEXTENSIBLE wave{};
  wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wave.Format.nChannels = channels;
  wave.Format.nSamplesPerSec = rate;
  wave.Format.wBitsPerSample = spec.bitsPerSample;
  wave.Format.nBlockAlign = static_cast<WORD>((static_cast<std::uint32_t>(channels) * spec.bitsPerSample) / 8U);
  wave.Format.nAvgBytesPerSec = rate * wave.Format.nBlockAlign;
  wave.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
  wave.Samples.wValidBitsPerSample = spec.validBitsPerSample;
  wave.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                                     : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
  if (spec.sampleFormat == AudioSampleFormat::Float32) {
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  } else {
    wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  }
  return wave;
}

// 探测单个 (rate, format, channels) 组合：WAVEFORMATEX 与
// WAVEFORMATEXTENSIBLE 各试一次（S24_32 只试 EXTENSIBLE）。
// S_OK = 精确支持；S_FALSE / 错误 = 不支持（closestMatch 忽略并释放）。
bool probeFormatSupported(IAudioClient* client,
                          std::uint32_t rate,
                          const ProbeFormatSpec& spec,
                          WORD channels) {
  auto probeOnce = [&](const WAVEFORMATEX& wave) -> bool {
    WAVEFORMATEX* closestMatch = nullptr;
    const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wave, &closestMatch);
    if (closestMatch != nullptr) {
      CoTaskMemFree(closestMatch);
    }
    return hr == S_OK;
  };

  if (!spec.requiresExtensible) {
    const auto waveEx = buildWaveFormatEx(spec, rate, channels);
    if (probeOnce(waveEx)) {
      return true;
    }
  }
  const auto waveExt = buildWaveFormatExtensible(spec, rate, channels);
  return probeOnce(waveExt.Format);
}

// UTF-16 → UTF-8（设备名/实例 ID 均为宽字符串属性）。
std::string wideToUtf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == L'\0') {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) {
    return {};
  }
  std::string utf8(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
  return utf8;
}

// 读取设备属性字符串，失败/空返回空串。
std::string readDeviceProperty(IPropertyStore* store, REFPROPERTYKEY key) {
  if (store == nullptr) {
    return {};
  }
  PROPVARIANT value{};
  PropVariantInit(&value);
  std::string result;
  if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
    result = wideToUtf8(value.pwszVal);
  }
  PropVariantClear(&value);
  return result;
}

std::vector<DeviceFormatCapabilities> enumerateWindowsDevices() {
  // COM 初始化：RPC_E_CHANGED_MODE 表示本线程已被其他组件以不同模式初始化
  // ——不视为失败也不 Uninitialize；仅 S_OK（本线程首次初始化）才配
  // CoUninitialize。
  const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
    spdlog::warn("wasapi format enumerator: CoInitializeEx failed: 0x{:08X}", static_cast<unsigned int>(initHr));
    return {};
  }
  const bool ownsInitialization = initHr == S_OK;

  std::vector<DeviceFormatCapabilities> result;
  do {
    ComPtrGuard<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
      spdlog::warn("wasapi format enumerator: CoCreateInstance(MMDeviceEnumerator) failed: 0x{:08X}",
                   static_cast<unsigned int>(hr));
      break;
    }

    ComPtrGuard<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
      spdlog::warn("wasapi format enumerator: EnumAudioEndpoints failed: 0x{:08X}",
                   static_cast<unsigned int>(hr));
      break;
    }

    UINT deviceCount = 0;
    if (FAILED(collection->GetCount(&deviceCount)) || deviceCount == 0U) {
      break;
    }

    for (UINT index = 0; index < deviceCount; ++index) {
      ComPtrGuard<IMMDevice> device;
      if (FAILED(collection->Item(index, &device)) || !device) {
        continue;
      }

      // 设备标识：deviceId 用实例 ID（唯一），deviceName 用友好名称，
      // 兜底接口友好名称。
      ComPtrGuard<IPropertyStore> store;
      if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) {
        continue;
      }
      std::string deviceName = readDeviceProperty(store.get(), PKEY_Device_FriendlyName);
      if (deviceName.empty()) {
        deviceName = readDeviceProperty(store.get(), PKEY_DeviceInterface_FriendlyName);
      }
      std::string instanceId;
      LPWSTR endpointId = nullptr;
      if (SUCCEEDED(device->GetId(&endpointId)) && endpointId != nullptr) {
        instanceId = wideToUtf8(endpointId);
      }
      if (endpointId != nullptr) {
        CoTaskMemFree(endpointId);
      }
      if (instanceId.empty()) {
        instanceId = deviceName;  // 属性读取失败时退化为友好名称匹配
      }

      ComPtrGuard<IAudioClient> client;
      hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
      if (FAILED(hr) || !client) {
        spdlog::debug("wasapi format enumerator: activate audio client failed for '{}': 0x{:08X}",
                      deviceName, static_cast<unsigned int>(hr));
        continue;
      }

      // GetMixFormat：shared 模式首选格式，提供声道数（探测矩阵的基准）。
      WAVEFORMATEX* mixFormat = nullptr;
      if (FAILED(client->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
        spdlog::debug("wasapi format enumerator: GetMixFormat failed for '{}'", deviceName);
        continue;
      }
      const WORD mixChannels = mixFormat->nChannels;
      CoTaskMemFree(mixFormat);
      mixFormat = nullptr;

      // 探测矩阵：rate × format × 声道（WAVEFORMATEX + EXTENSIBLE 各试）。
      // format 集 = 至少一个 rate 支持的 format；rate 集 = 至少一个 format
      // 支持的 rate（与 Linux 侧语义对齐，AudioDeviceFormat 是扁平列表）。
      std::vector<std::uint32_t> supportedRates;
      std::vector<AudioSampleFormat> supportedFormats;
      const auto probeFormats = makeProbeFormats();
      const auto probeChannels = makeProbeChannels(mixChannels);
      for (const auto rate : kCommonRates) {
        bool rateSupported = false;
        for (const auto& spec : probeFormats) {
          bool formatSupported = false;
          for (const auto channels : probeChannels) {
            if (probeFormatSupported(client.get(), rate, spec, channels)) {
              formatSupported = true;
              break;
            }
          }
          if (formatSupported) {
            rateSupported = true;
            supportedFormats.push_back(spec.sampleFormat);
          }
        }
        if (rateSupported) {
          supportedRates.push_back(rate);
        }
      }

      if (supportedRates.empty() && supportedFormats.empty()) {
        continue;
      }
      std::sort(supportedFormats.begin(), supportedFormats.end());
      supportedFormats.erase(std::unique(supportedFormats.begin(), supportedFormats.end()), supportedFormats.end());

      DeviceFormatCapabilities caps{};
      caps.deviceId = instanceId;
      caps.deviceName = deviceName;
      caps.supportedSampleFormats = std::move(supportedFormats);
      caps.supportedSampleRates = std::move(supportedRates);
      result.push_back(std::move(caps));
      spdlog::debug("wasapi format enumerator: '{}': {} rate(s), {} format(s)",
                    deviceName, caps.supportedSampleRates.size(), caps.supportedSampleFormats.size());
    }
  } while (false);

  if (ownsInitialization) {
    CoUninitialize();
  }
  return result;
}

}

// WASAPI 枚举器实现（工厂声明的函数，定义在此）。
class WindowsDeviceFormatEnumerator final : public DeviceFormatEnumerator {
public:
  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override { return enumerateWindowsDevices(); }
};

std::unique_ptr<DeviceFormatEnumerator> makeWindowsDeviceFormatEnumerator() {
  return std::make_unique<WindowsDeviceFormatEnumerator>();
}

}
}

#endif  // _WIN32
