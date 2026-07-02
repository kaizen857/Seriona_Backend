#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seriona::audio {

struct WaveformConfig {
  float dbFloor{-55.0F};
  float dbCeiling{0.0F};
  bool enableSIMD{true};
  int threadCount{0};
};

[[nodiscard]] std::vector<int> buildAudioWaveform(const std::string& filepath,
                                                  int barCount,
                                                  int totalWidth,
                                                  int& barWidth,
                                                  int maxHeight,
                                                  std::int64_t startTimeUS = 0,
                                                  std::int64_t endTimeUS = 0,
                                                  const WaveformConfig& config = WaveformConfig{});

}
