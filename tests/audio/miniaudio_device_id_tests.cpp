// miniaudio 设备 id 稳定文本编码的单测（miniaudio_device_id_encoding.h）。
// 编码是"后端感知 union 成员 → 文本"的纯函数：验证各后端的文本形态、字符串后端
// 与整数后端的防撞车前缀、WASAPI 宽串 UTF-8 收敛、GUID hex 形态、幂等性。

#include <doctest/doctest.h>

#include <miniaudio.h>
#include <miniaudio_device_id_encoding.h>

#include <cstring>
#include <string>

namespace {

ma_device_id zeroedId() {
  ma_device_id id{};
  std::memset(&id, 0, sizeof(id));
  return id;
}

} // namespace

TEST_CASE("miniaudio device id encoding: string backends pass through verbatim") {
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.alsa, "default", sizeof(id.alsa) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_alsa, id) == "default");
  }
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.alsa, ":0,0", sizeof(id.alsa) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_alsa, id) == ":0,0");
  }
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.pulse, "alsa_output.pci-0000_0a_00.4.analog-stereo", sizeof(id.pulse) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_pulseaudio, id) ==
          "alsa_output.pci-0000_0a_00.4.analog-stereo");
  }
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.coreaudio, "BuiltInSpeakerDevice", sizeof(id.coreaudio) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_coreaudio, id) == "BuiltInSpeakerDevice");
  }
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.sndio, "snd/0", sizeof(id.sndio) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_sndio, id) == "snd/0");
  }
  {
    ma_device_id id = zeroedId();
    std::strncpy(id.oss, "dev/dsp0", sizeof(id.oss) - 1);
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_oss, id) == "dev/dsp0");
  }
}

TEST_CASE("miniaudio device id encoding: wasapi wide string becomes utf-8") {
  ma_device_id id = zeroedId();
  const ma_wchar_win32 source[] = {L'{', L'0', L'.', L'0', L'.', L'0', L'.', L'0', L'0', L'0', L'0', L'0', L'0', L'0',
                                   L'0', L'0', L'}', L'.', L'{', 0x4E2D, 0x6587, 0};
  ma_uint32 index = 0;
  for (const ma_wchar_win32 c : source) {
    id.wasapi[index++] = c;
  }
  const std::string encoded = seriona::audio::encodeMiniaudioDeviceId(ma_backend_wasapi, id);
  CHECK(encoded == "{0.0.0.000000000}.{中文");
  // 宽串 UTF-8 往返稳定（幂等）。
  CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_wasapi, id) == encoded);
}

TEST_CASE("miniaudio device id encoding: dsound guid becomes lowercase hex") {
  ma_device_id id = zeroedId();
  for (ma_uint8 i = 0; i < 16; ++i) {
    id.dsound[i] = static_cast<ma_uint8>(i * 17U); // 0, 17, 34, ..., 255
  }
  CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_dsound, id) ==
        "00112233445566778899aabbccddeeff");
}

TEST_CASE("miniaudio device id encoding: integer backends get backend tag prefix") {
  {
    ma_device_id id = zeroedId();
    id.winmm = 3;
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_winmm, id) == "winmm:3");
  }
  {
    ma_device_id id = zeroedId();
    id.aaudio = 5;
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_aaudio, id) == "aaudio:5");
  }
  {
    ma_device_id id = zeroedId();
    id.opensl = 2;
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_opensl, id) == "opensl:2");
  }
  {
    ma_device_id id = zeroedId();
    id.nullbackend = 0;
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_null, id) == "null:0");
  }
  {
    ma_device_id id = zeroedId();
    id.jack = 0;
    CHECK(seriona::audio::encodeMiniaudioDeviceId(ma_backend_jack, id) == "jack:0");
  }
}

TEST_CASE("miniaudio device id encoding: prefixed integers never collide with legacy index format") {
  // 历史版本 deviceId 是纯十进制枚举索引；整数后端编码带 "backend:" 前缀，
  // 保证稳定 id 永不与旧格式（纯数字）混淆，旧值迁移可安全按纯数字判定。
  ma_device_id id = zeroedId();
  id.winmm = 0;
  const std::string encoded = seriona::audio::encodeMiniaudioDeviceId(ma_backend_winmm, id);
  CHECK(encoded == "winmm:0");
  CHECK(encoded.find_first_not_of("0123456789") != std::string::npos);
}

TEST_CASE("miniaudio device id encoding: backend tag names are stable") {
  CHECK(std::string(seriona::audio::miniaudioBackendTag(ma_backend_alsa)) == "alsa");
  CHECK(std::string(seriona::audio::miniaudioBackendTag(ma_backend_pulseaudio)) == "pulse");
  CHECK(std::string(seriona::audio::miniaudioBackendTag(ma_backend_wasapi)) == "wasapi");
  CHECK(std::string(seriona::audio::miniaudioBackendTag(ma_backend_coreaudio)) == "coreaudio");
  CHECK(std::string(seriona::audio::miniaudioBackendTag(ma_backend_null)) == "null");
}
