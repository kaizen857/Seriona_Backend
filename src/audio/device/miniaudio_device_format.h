#pragma once

#include "seriona/audio/audio_contracts.h"

#include <miniaudio.h>

#include <string>

namespace seriona::audio {

// 由 ma_context_get_devices 的基础枚举信息构造 AudioDeviceFormat 基础字段
// （deviceId/deviceName/backendName 等；能力字段由 fillDeviceCapabilitiesFromQuery 填充）。
AudioDeviceFormat makeAudioDeviceFormat(const ma_device_info& info, std::string deviceId);

// 按 ma_context_get_device_info 的查询结果填充能力字段：
// - queryResult == MA_SUCCESS：从 info.nativeDataFormats 提取 supportedSampleFormats /
//   supportedSampleRates（按出现顺序去重）。ma_format_unknown 与 sampleRate==0 是 miniaudio
//   "全支持"语义，不产生条目——列表为空即"全支持/未枚举"。
// - 其它结果（MA_SHARE_MODE_NOT_SUPPORTED 等）：能力列表保持空并置 fallbackApplied=true。
void fillDeviceCapabilitiesFromQuery(AudioDeviceFormat& device, const ma_device_info& info, ma_result queryResult);

}
