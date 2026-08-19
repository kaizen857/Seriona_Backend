#include "../../src/audio/device/miniaudio_device_format.h"

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace seriona::audio {
namespace {

ma_device_info deviceInfoWithFormats() {
  // mock ma_context_get_device_info 输出：WASAPI 风格支持多种格式/采样率的设备。
  ma_device_info info{};
  info.nativeDataFormatCount = 5;
  info.nativeDataFormats[0] = {ma_format_s24, 2, 48000, 0};
  info.nativeDataFormats[1] = {ma_format_s32, 2, 48000, 0};
  info.nativeDataFormats[2] = {ma_format_s32, 2, 96000, 0};
  info.nativeDataFormats[3] = {ma_format_f32, 2, 96000, 0};
  info.nativeDataFormats[4] = {ma_format_s16, 1, 44100, 0};
  return info;
}

}

TEST_CASE("miniaudio device info successful query fills supported formats and rates (deduplicated)") {
  const ma_device_info info = deviceInfoWithFormats();

  AudioDeviceFormat device = makeAudioDeviceFormat(info, "3");
  fillDeviceCapabilitiesFromQuery(device, info, MA_SUCCESS);

  CHECK(device.deviceId == "3");
  CHECK(device.deviceName == info.name);
  CHECK(device.backendName == "miniaudio");

  REQUIRE(device.supportedSampleFormats.size() == 4U);
  CHECK(device.supportedSampleFormats[0] == AudioSampleFormat::Int24);
  CHECK(device.supportedSampleFormats[1] == AudioSampleFormat::Int32);
  CHECK(device.supportedSampleFormats[2] == AudioSampleFormat::Float32);
  CHECK(device.supportedSampleFormats[3] == AudioSampleFormat::Int16);

  REQUIRE(device.supportedSampleRates.size() == 3U);
  CHECK(device.supportedSampleRates[0] == 48000U);
  CHECK(device.supportedSampleRates[1] == 96000U);
  CHECK(device.supportedSampleRates[2] == 44100U);

  CHECK_FALSE(device.fallbackApplied);
}

TEST_CASE("miniaudio device info unknown format and zero sample rate mean full support (empty lists)") {
  ma_device_info info{};
  info.nativeDataFormatCount = 1;
  info.nativeDataFormats[0] = {ma_format_unknown, 0, 0, 0};

  AudioDeviceFormat device = makeAudioDeviceFormat(info, "0");
  fillDeviceCapabilitiesFromQuery(device, info, MA_SUCCESS);

  CHECK(device.supportedSampleFormats.empty());
  CHECK(device.supportedSampleRates.empty());
  CHECK_FALSE(device.fallbackApplied);
}

TEST_CASE("miniaudio device info empty native data formats yields empty capability lists") {
  ma_device_info info{};
  info.nativeDataFormatCount = 0;

  AudioDeviceFormat device = makeAudioDeviceFormat(info, "0");
  fillDeviceCapabilitiesFromQuery(device, info, MA_SUCCESS);

  CHECK(device.supportedSampleFormats.empty());
  CHECK(device.supportedSampleRates.empty());
  CHECK_FALSE(device.fallbackApplied);
}

TEST_CASE("miniaudio device info unsupported formats are skipped") {
  ma_device_info info{};
  info.nativeDataFormatCount = 3;
  info.nativeDataFormats[0] = {ma_format_u8, 2, 48000, 0};
  info.nativeDataFormats[1] = {ma_format_f32, 2, 48000, 0};
  info.nativeDataFormats[2] = {ma_format_u8, 1, 8000, 0};

  AudioDeviceFormat device = makeAudioDeviceFormat(info, "0");
  fillDeviceCapabilitiesFromQuery(device, info, MA_SUCCESS);

  REQUIRE(device.supportedSampleFormats.size() == 1U);
  CHECK(device.supportedSampleFormats[0] == AudioSampleFormat::Float32);
  REQUIRE(device.supportedSampleRates.size() == 2U);
  CHECK(device.supportedSampleRates[0] == 48000U);
  CHECK(device.supportedSampleRates[1] == 8000U);
}

TEST_CASE("miniaudio device info failed query leaves capabilities empty and marks fallback") {
  ma_device_info info{};
  info.nativeDataFormatCount = 0;

  AudioDeviceFormat device = makeAudioDeviceFormat(info, "0");
  fillDeviceCapabilitiesFromQuery(device, info, MA_SHARE_MODE_NOT_SUPPORTED);

  CHECK(device.supportedSampleFormats.empty());
  CHECK(device.supportedSampleRates.empty());
  CHECK(device.fallbackApplied);
}

TEST_CASE("miniaudio device info no device result leaves capabilities empty and marks fallback") {
  AudioDeviceFormat device = makeAudioDeviceFormat(ma_device_info{}, "0");
  fillDeviceCapabilitiesFromQuery(device, ma_device_info{}, MA_NO_DEVICE);

  CHECK(device.supportedSampleFormats.empty());
  CHECK(device.supportedSampleRates.empty());
  CHECK(device.fallbackApplied);
}

}
