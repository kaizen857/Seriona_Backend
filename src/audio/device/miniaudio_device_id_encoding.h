#pragma once

// miniaudio 设备 id 的稳定文本编码（跨平台）。
//
// 背景：ma_device_id 是无标签 union（miniaudio.h "ma_device_id" 注释），同一个
// ma_device_info.id 必须结合 ma_context.backend 才能知道读哪个成员；直接 hex 整块
// union 会把未使用成员（多为零填充）也编码进去，且无法在日志/设置中辨认。本编码
// 把"当前后端对应的那一个成员"文本化，作为 AudioDeviceFormat.deviceId 的契约值：
//   - 字符串后端（ALSA/PulseAudio/CoreAudio/sndio/audio4/OSS）取 NUL 结尾串本体；
//   - WASAPI 取 wchar_t 串转 UTF-8；
//   - GUID 类（DirectSound）固定 16 字节小写 hex；
//   - 整数后端（WinMM/JACK/AAudio/OpenSL/Null/Custom）数字成员转十进制串；
// 字符串成员都可能以纯数字开头/结尾，故整数类统一加 "<backend>:" 前缀防与旧版
// "枚举索引字符串"（纯数字）撞车；字符串类不加前缀。
//
// 幂等性：同一系统、同一后端、同一物理设备两次枚举编码结果一致（后端填充规则
// 确定），这是持久化 deviceId 的前提。本头只依赖 miniaudio.h 的类型声明，
// 不触发 MINIAUDIO_IMPLEMENTATION，可被任意测试翻译单元安全包含。

#include <miniaudio.h>

#include <cstdint>
#include <string>

namespace seriona::audio {

inline constexpr const char* miniaudioBackendTag(ma_backend backend) noexcept {
  switch (backend) {
  case ma_backend_wasapi:
    return "wasapi";
  case ma_backend_dsound:
    return "dsound";
  case ma_backend_winmm:
    return "winmm";
  case ma_backend_coreaudio:
    return "coreaudio";
  case ma_backend_alsa:
    return "alsa";
  case ma_backend_pulseaudio:
    return "pulse";
  case ma_backend_jack:
    return "jack";
  case ma_backend_sndio:
    return "sndio";
  case ma_backend_audio4:
    return "audio4";
  case ma_backend_oss:
    return "oss";
  case ma_backend_aaudio:
    return "aaudio";
  case ma_backend_opensl:
    return "opensl";
  case ma_backend_webaudio:
    return "webaudio";
  case ma_backend_custom:
    return "custom";
  case ma_backend_null:
    return "null";
  default:
    return "unknown";
  }
}

inline std::string encodeMiniaudioDeviceId(ma_backend backend, const ma_device_id& id) {
  switch (backend) {
  case ma_backend_alsa:
    return std::string(id.alsa);
  case ma_backend_pulseaudio:
    return std::string(id.pulse);
  case ma_backend_coreaudio:
    return std::string(id.coreaudio);
  case ma_backend_sndio:
    return std::string(id.sndio);
  case ma_backend_audio4:
    return std::string(id.audio4);
  case ma_backend_oss:
    return std::string(id.oss);
  case ma_backend_wasapi: {
    // wchar_t 宽串转 UTF-8。miniaudio 的 ma_wchar_win32 即 wchar_t（Windows 上
    // UTF-16）；按字节序无关的最小实现逐码点收敛。
    std::string utf8;
    const ma_wchar_win32* src = id.wasapi;
    while (src != nullptr && *src != 0) {
      const std::uint32_t codepoint = static_cast<std::uint32_t>(*src++);
      if (codepoint < 0x80U) {
        utf8.push_back(static_cast<char>(codepoint));
      } else if (codepoint < 0x800U) {
        utf8.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        utf8.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
      } else {
        utf8.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        utf8.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        utf8.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
      }
    }
    return utf8;
  }
  case ma_backend_dsound: {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(32);
    for (const ma_uint8 byte : id.dsound) {
      hex.push_back(kHex[byte >> 4U]);
      hex.push_back(kHex[byte & 0x0FU]);
    }
    return hex;
  }
  default: {
    // 整数/特化后端：与旧版"枚举索引"（纯数字）区分，加后端标签前缀。
    const int value = [&]() {
      switch (backend) {
      case ma_backend_winmm:
        return static_cast<int>(id.winmm);
      case ma_backend_jack:
        return id.jack;
      case ma_backend_aaudio:
        return static_cast<int>(id.aaudio);
      case ma_backend_opensl:
        return static_cast<int>(id.opensl);
      case ma_backend_null:
        return id.nullbackend;
      default:
        return id.custom.i;
      }
    }();
    return std::string(miniaudioBackendTag(backend)) + ":" + std::to_string(value);
  }
  }
}

} // namespace seriona::audio
