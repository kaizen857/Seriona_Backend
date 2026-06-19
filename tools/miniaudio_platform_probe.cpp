#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

enum class ArgumentResult {
  Run,
  Help,
  Error,
};

void noPlaybackCallback(ma_device*, void*, const void*, ma_uint32) noexcept {}

const char* resultDescription(ma_result result) noexcept {
  const char* description = ma_result_description(result);
  return description != nullptr ? description : "unknown miniaudio result";
}

const char* formatName(ma_format format) noexcept {
  const char* name = ma_get_format_name(format);
  return name != nullptr ? name : "unknown";
}

const char* yesNo(ma_bool32 value) noexcept { return value ? "yes" : "no"; }

void printUsage(const char* programName) {
  std::cout << "Usage: " << programName << " [--list-only]\n"
            << "Lists miniaudio playback devices and probes the default playback format without starting playback.\n";
}

void printNativeFormats(const ma_device_info& info) {
  std::cout << "    native_format_count: " << info.nativeDataFormatCount << '\n';
  const auto count = std::min<std::uint32_t>(info.nativeDataFormatCount, 8U);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto& format = info.nativeDataFormats[index];
    std::cout << "    native_format[" << index << "]: format=" << formatName(format.format)
              << ", channels=" << format.channels << ", sample_rate=" << format.sampleRate;
    if ((format.flags & MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE) != 0U) {
      std::cout << ", exclusive=yes";
    }
    std::cout << '\n';
  }
  if (info.nativeDataFormatCount > count) {
    std::cout << "    native_format_truncated: yes\n";
  }
}

ArgumentResult parseArguments(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return ArgumentResult::Help;
    }
    if (argument == "--list-only") {
      continue;
    }

    std::cerr << "unsupported argument: " << argument << '\n';
    printUsage(argv[0]);
    return ArgumentResult::Error;
  }

  return ArgumentResult::Run;
}

int probeDefaultFormat(ma_context& context) {
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_unknown;
  config.playback.channels = 0;
  config.sampleRate = 0;
  config.dataCallback = noPlaybackCallback;

  ma_device device{};
  const ma_result result = ma_device_init(&context, &config, &device);
  if (result != MA_SUCCESS) {
    std::cout << "default_format_skip_reason: ma_device_init failed: " << resultDescription(result)
              << " (" << static_cast<int>(result) << ")\n";
    return 0;
  }

  std::cout << "default_format_probe: initialized_stopped_device=yes\n"
            << "  playback_name: " << device.playback.name << '\n'
            << "  client_format: " << formatName(device.playback.format) << '\n'
            << "  client_channels: " << device.playback.channels << '\n'
            << "  device_sample_rate: " << device.sampleRate << '\n'
            << "  internal_format: " << formatName(device.playback.internalFormat) << '\n'
            << "  internal_channels: " << device.playback.internalChannels << '\n'
            << "  internal_sample_rate: " << device.playback.internalSampleRate << '\n'
            << "  internal_period_frames: " << device.playback.internalPeriodSizeInFrames << '\n'
            << "  internal_periods: " << device.playback.internalPeriods << '\n'
            << "  playback_started: no\n";

  ma_device_uninit(&device);
  return 0;
}

}

int main(int argc, char** argv) {
  const ArgumentResult argumentResult = parseArguments(argc, argv);
  if (argumentResult == ArgumentResult::Help) {
    return 0;
  }
  if (argumentResult == ArgumentResult::Error) {
    return 2;
  }

  std::cout << "seriona_miniaudio_platform_probe: list-only, no playback is started\n";

  ma_context context{};
  const ma_result initResult = ma_context_init(nullptr, 0, nullptr, &context);
  if (initResult != MA_SUCCESS) {
    std::cout << "skip_reason: ma_context_init failed: " << resultDescription(initResult)
              << " (" << static_cast<int>(initResult) << ")\n";
    return 0;
  }

  ma_device_info* playbackInfos = nullptr;
  ma_uint32 playbackCount = 0;
  const ma_result devicesResult = ma_context_get_devices(&context, &playbackInfos, &playbackCount, nullptr, nullptr);
  if (devicesResult != MA_SUCCESS) {
    std::cout << "skip_reason: ma_context_get_devices failed: " << resultDescription(devicesResult)
              << " (" << static_cast<int>(devicesResult) << ")\n";
    ma_context_uninit(&context);
    return 0;
  }

  std::cout << "playback_device_count: " << playbackCount << '\n';
  for (ma_uint32 index = 0; index < playbackCount; ++index) {
    const auto& info = playbackInfos[index];
    std::cout << "  device[" << index << "]: name=" << info.name << ", default=" << yesNo(info.isDefault) << '\n';
    printNativeFormats(info);
  }

  if (playbackCount == 0) {
    std::cout << "skip_reason: no playback devices reported by miniaudio\n";
    ma_context_uninit(&context);
    return 0;
  }

  const int result = probeDefaultFormat(context);
  ma_context_uninit(&context);
  return result;
}
