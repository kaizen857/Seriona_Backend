#pragma once

// 均衡器频段常量表（EQ B1.4 公共单点定义；任务 22 DSP 同源引用，禁双份拷贝）。
// 属于实现导向共享常量，非公共 API 面：不暴露第三方类型、无状态，纯 C++23 值常量。
// 与 audio_contracts.h 的 EqualizerBandMode/EqualizerStateSnapshot 组合使用：
//   - Band10 生效前缀 = kEqualizer10BandCenterHz.size()（10 段）
//   - Band31 生效前缀 = kEqualizer31BandCenterHz.size()（31 段）
// 曲线（181 点 20–20k）按“每档固定 Q”的 RBJ peaking 幅频公式计算，见 reducer 实现。

#include <array>

namespace seriona::audio {

// 10 段图示均衡（1-octave，ISO 标称中心频率）：20 Hz–16 kHz 档位。
inline constexpr std::array<double, 10> kEqualizer10BandCenterHz{
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

// 31 段图示均衡（1/3-octave，ISO 标称中心频率）：20 Hz–20 kHz 档位。
inline constexpr std::array<double, 31> kEqualizer31BandCenterHz{
    20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,   100.0,  125.0,  160.0,
    200.0,  250.0,  315.0,  400.0,  500.0,  630.0,  800.0,  1000.0, 1250.0, 1600.0,
    2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0,
    20000.0};

// 每档固定 Q（RBJ peaking 设计值，防 ma_peak2_config_init q==0 静默回退 0.707）：
//   - 10 段（1-octave）：Q ≈ 1.41
//   - 31 段（1/3-octave）：Q ≈ 4.32
inline constexpr double kEqualizer10BandQ = 1.41;
inline constexpr double kEqualizer31BandQ = 4.32;

}  // namespace seriona::audio
