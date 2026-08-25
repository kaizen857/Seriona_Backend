#pragma once

// 设备采样率/位深格式能力枚举接口（平台隔离层）。
//
// miniaudio 所有后端的 ma_context_get_device_info 都无法完整枚举设备
// 支持的采样率/位深组合（pulse 后端只填 1 个 nativeDataFormat，pipewire
// 后端只报 f32 + core clockRate）。本模块用平台原生 API 补齐该信息：
//   - Linux  ：PipeWire 原生 SPA EnumFormat（节点真实格式能力）
//   - Windows：WASAPI GetMixFormat + common rate × format 矩阵
//              IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE) 探测
// 其余平台返回空枚举器（调用方保留设备自身报告）。
//
// 本头是纯标准库契约，不包含任何平台 API 类型；平台实现隔离在
// src/audio/device/ 下的独立源文件（见工厂 makeDeviceFormatEnumerator 的
// #if 分派，结构与 metadata 模块的 makeMetadataServiceBackendFromOptions
// 一致）。

#include "seriona/audio/audio_contracts.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace seriona::audio {

// 单个输出设备的格式能力枚举结果。
// 语义与 AudioDeviceFormat.supportedSampleFormats/supportedSampleRates
// （audio_contracts.h）对齐：空列表表示"未枚举或全支持"，不产生覆盖。
// deviceId/deviceName 用于与播放后端（miniaudio）枚举结果做精确匹配：
// 优先 deviceId（Linux 为 PipeWire node.name，Windows 为设备实例 ID），
// 不匹配再试 deviceName（Linux 为 node.description，Windows 为友好名称）。
struct DeviceFormatCapabilities {
  std::string deviceId;
  std::string deviceName;
  // 设备支持的输出位深格式（已按滤镜链能力求交：只保留
  // Int16/Int24/Int32/Float32 四类，其余映射 Unknown 后跳过）。
  std::vector<AudioSampleFormat> supportedSampleFormats;
  // 设备支持的采样率（去重排序，单位 Hz）。
  std::vector<std::uint32_t> supportedSampleRates;
};

// 平台设备格式枚举器抽象。每次 enumerate() 新建平台会话、不持有全局
// 连接，保证可在任意工作线程调用（enumeratePlaybackDevices 运行在
// audio worker 线程）。
class DeviceFormatEnumerator {
public:
  virtual ~DeviceFormatEnumerator() = default;

  // 枚举所有可用输出设备（sink）的格式能力。
  // 返回空列表表示平台不支持或枚举失败/超时——调用方保留播放后端
  // 设备自身报告的字段，仅在有实际能力数据时覆盖格式/采样率列表。
  [[nodiscard]] virtual std::vector<DeviceFormatCapabilities> enumerate() = 0;
};

// 平台工厂：Linux 返回 PipeWire 实现，Windows 返回 WASAPI 实现，
// 其余平台返回空枚举器（enumerate() 恒返回空列表）并记录日志。
// 注意：返回的枚举器经过 CachingDeviceFormatEnumerator 包装——真实
// 平台枚举在后台线程执行，enumerate() 只读缓存（微秒级），不会阻塞
// 调用线程（audio worker 线程上的长阻塞会暂停 fillQueue 导致 buffer
// underrun；详见 CachingDeviceFormatEnumerator 注释）。
[[nodiscard]] std::unique_ptr<DeviceFormatEnumerator> makeDeviceFormatEnumerator();

// 带缓存的枚举器装饰器：包装平台枚举器（PipeWire/WASAPI 探测耗时
// 数百毫秒），把真实枚举移到常驻后台线程，enumerate() 同步返回缓存。
//
// 背景：AudioPlaybackService::enumeratePlaybackDevices 在 audio worker
// 线程执行（与 fillQueue 同线程），若在这里同步跑平台枚举，数百毫秒
// 的阻塞会暂停 PCM 队列填充，miniaudio 设备线程拉空队列即触发
// buffer underrun（打开设置窗口触发枚举时偶现）。本装饰器保证
// enumerate() 永不阻塞超过锁拷贝时间，并让后台线程持续刷新缓存：
//   - 构造后立即后台预热（启动后首次打开设置即能看到完整能力列表）
//   - 缓存 TTL 过期（30 秒）后，下一次 enumerate() 触发后台刷新并
//     返回旧缓存（可短暂过期，设备能力极少秒级变化）
//   - 枚举失败返回空缓存时按 5 秒退避重试，避免打开设置空转
// 线程安全：平台枚举器只被后台线程调用（无并发），缓存读写均在
// 内部互斥锁保护下进行。
class CachingDeviceFormatEnumerator final : public DeviceFormatEnumerator {
public:
  // 构造即启动后台预热线程；析构 join 等待退出。
  explicit CachingDeviceFormatEnumerator(std::unique_ptr<DeviceFormatEnumerator> platform);
  ~CachingDeviceFormatEnumerator() override;
  CachingDeviceFormatEnumerator(const CachingDeviceFormatEnumerator&) = delete;
  CachingDeviceFormatEnumerator& operator=(const CachingDeviceFormatEnumerator&) = delete;

  // 同步返回当前缓存（可能为空/短暂过期），不执行平台枚举。
  [[nodiscard]] std::vector<DeviceFormatCapabilities> enumerate() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
