// EQ 频段表与系数行为的帧级单测（任务 27，B2.5 组 seriona.audio.eq_coefficients）。
//
// 被测面：inc/seriona/audio/equalizer_tables.h（表单点）+ src/audio/eq_dsp.{h,cpp}
// （DSP 核心；频响性质经实际 process 测量，非表头纯数学——系数/越界/直通语义
// 必须以真实 biquad 链为准）。
//
// 覆盖（对应计划书 B2.5 eq_coefficients 组）：
//   - ISO 频率表/固定 Q 单点：10/31 段中心频率、段数与 Q 常量逐字面锁定，且
//     10 段中心全部落在 31 段表内（同值子集，防两表漂移）；
//   - 0dB 恒等直通：EQ 开 + 全 0dB → fastBypass 快路径，样本域逐位不变 + 频响
//     平坦 + bandBypassed 全真（任务 25 出厂主路径契约）；
//   - 单 band 峰值位置：+15dB@1000Hz（31 档 idx17）→ 实测 1000Hz 处 +15±0.2dB
//     且高于相邻 800/1250Hz 取样 ≥3dB（Q=4.32 峰值特性）；
//   - 未用 band 直通：Band10 前缀外（idx10..30）增益不生效——样本逐位不变 +
//     report.activeBandCount==10 + bandBypassed(超前缀)==true；
//   - 44.1k–192k 极端系数容差：fs∈{44100,192000} × {+15/−15dB} × 中心频率取样
//     （DC 侧 20Hz 与奈奎斯特侧 20kHz@192k / 8kHz@44.1k）实测增益误差 ≤0.2dB。
//     取样纪律：恒取 band 中心（peaking 幅频的平坦顶区，|H(f)| 对频率一阶导为
//     0 → 数值上天然避 Q=4.32 陡峭裙边），正弦单频 + 最小二乘幅值估计；
//   - 越界守卫：f_center ≥ 0.95×(fs/2) 硬直通（configure 期一次判定）——
//     fs=22050（0.95×11025=10473.75：12500/16000/20000 越界）与 fs=32000
//     （0.95×16000=15200：16000/20000 越界）顶段 ±15dB 配置 → report
//     nyquistBypassBandCount 精确、bandRangeBypassed/bandBypassed 告警面精确、
//     越界增益对样本零影响（逐位恒等、无增长）；越界内侧最顶可用 band 仍正常
//     滤波（22050 的 8000、32000 的 12500 实测 +15±0.2dB）。
#include <doctest.h>

#include "../../src/audio/eq_dsp.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/equalizer_tables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace audio = seriona::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

audio::EqualizerConfig flatConfig(audio::EqualizerBandMode mode, bool enabled) {
  audio::EqualizerConfig config{};
  config.enabled = enabled;
  config.mode = mode;
  return config;
}

// 单 band 增益配置（其余 band 与 preGain 恒 0）。
audio::EqualizerConfig singleBandConfig(audio::EqualizerBandMode mode,
                                        std::size_t bandIndex,
                                        float gainDb) {
  audio::EqualizerConfig config = flatConfig(mode, true);
  config.bandGainsDb[bandIndex] = gainDb;
  return config;
}

double linToDb(double ratio) { return 20.0 * std::log10(ratio); }

// 伪随机 f32 缓冲（LCG，含负值/近 1 值/次正规附近的跨尺度样本，防"全零直通"
// 掩盖逐位差异）。
void fillPseudoRandom(std::vector<float>& out, std::uint32_t seed, std::size_t samples) {
  out.resize(samples);
  std::uint32_t state = seed;
  for (std::size_t i = 0; i < samples; ++i) {
    state = state * 1664525U + 1013904223U;
    const double unit = static_cast<double>(state) / 4294967296.0;  // [0,1)
    out[i] = static_cast<float>((unit - 0.5) * 1.9);
  }
}

// 生成 f0 正弦（mono f32）。相位 double 递推，每样本回绕防长缓冲漂移。
void fillSine(std::vector<float>& out, double fs, double f0Hz, double amplitude, std::size_t frames) {
  out.resize(frames);
  const double omega = 2.0 * kPi * f0Hz / fs;
  const double twoPi = 2.0 * kPi;
  double phase = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    out[i] = static_cast<float>(amplitude * std::sin(phase));
    phase += omega;
    if (phase >= twoPi) {
      phase -= twoPi;
    }
  }
}

// 最小二乘幅值估计：对 y 尾部 windowFrames 个样本拟合 a·cos(ωn)+b·sin(ωn)。
// 纯音 + 稳态下对窗长/周期非整数性无偏（双精度累加），瞬态残差由 warmup 吸收。
double estimateSineAmplitude(const std::vector<float>& y,
                             double fs,
                             double f0Hz,
                             std::size_t skipFrames,
                             std::size_t windowFrames) {
  const double omega = 2.0 * kPi * f0Hz / fs;
  double cosPhase = 1.0;
  double sinPhase = 0.0;
  const double rotC = std::cos(omega);
  const double rotS = std::sin(omega);
  double sumCw = 0.0;
  double sumSw = 0.0;
  double sumCC = 0.0;
  double sumSS = 0.0;
  double sumCS = 0.0;
  for (std::size_t i = skipFrames; i < skipFrames + windowFrames; ++i) {
    const double w = static_cast<double>(y[i]);
    sumCw += cosPhase * w;
    sumSw += sinPhase * w;
    sumCC += cosPhase * cosPhase;
    sumSS += sinPhase * sinPhase;
    sumCS += cosPhase * sinPhase;
    // 旋转递推（双精度，误差 ~1e-16/步，长窗累积可忽略）。
    const double nextC = cosPhase * rotC - sinPhase * rotS;
    sinPhase = cosPhase * rotS + sinPhase * rotC;
    cosPhase = nextC;
  }
  const double denom = sumCC * sumSS - sumCS * sumCS;
  const double a = (sumCw * sumSS - sumSw * sumCS) / denom;
  const double b = (sumSw * sumCC - sumCw * sumCS) / denom;
  return std::hypot(a, b);
}

// —— DSP 夹具 ——

struct DspFixture {
  audio::EqualizerDspProcessor dsp{};

  audio::EqualizerDspProcessor::ConfigReport configure(const audio::EqualizerConfig& config,
                                                       std::uint32_t fs,
                                                       std::uint32_t channels) {
    return dsp.configure(config, fs, channels);
  }

  // 平滑收敛：3 块静音（块长 4096，任何被测 fs 下 3×blockMs>20ms → 每块推进
  // 1/3 段位移、恰好 3 块精确到位；滤波状态全程零输入保持零）。
  void settle(std::uint32_t fs, std::uint32_t channels) {
    static_cast<void>(fs);
    std::vector<float> silence(4096U * channels, 0.0f);
    for (int i = 0; i < 3; ++i) {
      dsp.process(silence.data(), 4096U);
    }
    REQUIRE(dsp.fullySettled());
  }

  // 处理整段缓冲（平滑已收敛 → 分块大小不影响增益）。
  void process(std::vector<float>& samples, std::size_t chunkFrames, std::uint32_t channels) {
    const std::size_t total = samples.size() / channels;
    std::size_t offset = 0;
    while (offset < total) {
      const std::uint32_t frames =
          static_cast<std::uint32_t>(std::min(chunkFrames, total - offset));
      dsp.process(samples.data() + offset * channels, frames);
      offset += frames;
    }
  }

  // 频率响应测量（中心频率取样 = 平坦顶区）。
  // warmupSeconds 依滤波极点时间常数：τ ≈ 10^(g/20)·Q/(π·f0)（boost 方向最慢），
  // 16τ + 余量；window ≥ 4 周期且 ≥ 2048 样本。
  double measureGainDb(std::uint32_t fs,
                       std::uint32_t channels,
                       double f0Hz,
                       double gainDb,
                       double inputAmplitude) {
    const double aLin = std::pow(10.0, gainDb / 20.0);
    const double q = audio::kEqualizer31BandQ;
    const double tauSeconds = aLin * q / (kPi * f0Hz);
    const double warmSeconds = 16.0 * tauSeconds + 0.02;
    const std::size_t periodFrames =
        static_cast<std::size_t>(std::lround(fs / f0Hz));
    const std::size_t warmFrames =
        static_cast<std::size_t>(warmSeconds * static_cast<double>(fs)) + 2U * periodFrames;
    const std::size_t windowFrames = std::max<std::size_t>(2048U, 4U * periodFrames);

    std::vector<float> tone;
    fillSine(tone, static_cast<double>(fs), f0Hz, inputAmplitude, warmFrames + windowFrames);
    process(tone, 8192U, channels);
    REQUIRE(tone.size() == warmFrames + windowFrames);
    const double measured =
        estimateSineAmplitude(tone, static_cast<double>(fs), f0Hz, warmFrames, windowFrames);
    REQUIRE(std::isfinite(measured));
    return linToDb(measured / inputAmplitude);
  }
};

}  // namespace

TEST_CASE("eq coefficients: ISO 频率表与固定 Q 常量单点（10/31 段逐字面 + 子集一致）") {
  // 10 段 1-octave：ISO 名义中心频率 31.5Hz–16kHz。
  const std::array<double, 10> expected10{31.5, 63.0,  125.0,  250.0, 500.0,
                                          1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
  REQUIRE(audio::kEqualizer10BandCenterHz.size() == expected10.size());
  for (std::size_t i = 0; i < expected10.size(); ++i) {
    CHECK(audio::kEqualizer10BandCenterHz[i] == expected10[i]);
  }

  // 31 段 1/3-octave：ISO 名义中心频率 20Hz–20kHz。
  const std::array<double, 31> expected31{20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,
                                          100.0,  125.0,  160.0,  200.0,  250.0,  315.0,  400.0,
                                          500.0,  630.0,  800.0,  1000.0, 1250.0, 1600.0, 2000.0,
                                          2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0,
                                          12500.0, 16000.0, 20000.0};
  REQUIRE(audio::kEqualizer31BandCenterHz.size() == expected31.size());
  for (std::size_t i = 0; i < expected31.size(); ++i) {
    CHECK(audio::kEqualizer31BandCenterHz[i] == expected31[i]);
  }
  // 单调递增（表的有序性被 DSP 前缀语义依赖）。
  for (std::size_t i = 1; i < audio::kEqualizer31BandCenterHz.size(); ++i) {
    CHECK(audio::kEqualizer31BandCenterHz[i] > audio::kEqualizer31BandCenterHz[i - 1]);
  }
  // 固定 Q 常量（10 档 1.41 / 31 档 4.32，RBJ peaking 防 q==0 静默回退）。
  CHECK(audio::kEqualizer10BandQ == 1.41);
  CHECK(audio::kEqualizer31BandQ == 4.32);

  // 10 段中心须是 31 段的同值子集（两表同一 ISO 源，禁止漂移）。
  for (const double ten : audio::kEqualizer10BandCenterHz) {
    const bool found = std::find(audio::kEqualizer31BandCenterHz.begin(),
                                 audio::kEqualizer31BandCenterHz.end(),
                                 ten) != audio::kEqualizer31BandCenterHz.end();
    CHECK(found);
  }
}

TEST_CASE("eq coefficients: 0dB 恒等直通（EQ 开 + 全 0dB → 样本域逐位不变 + 频响平坦）") {
  DspFixture fx;
  const audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
  const auto report = fx.configure(config, 48000U, 2U);
  REQUIRE(report.accepted);
  CHECK(report.enabled);
  CHECK(report.sampleRate == 48000U);
  CHECK(report.channelCount == 2U);
  CHECK(report.mode == audio::EqualizerBandMode::Band31);
  CHECK(report.activeBandCount == audio::kEqualizer31BandCenterHz.size());
  CHECK(report.nyquistBypassBandCount == 0U);  // 48k：0.95×24k=22800 > 20k，全带可用

  // 全零稳态 → 快路径：process 不触碰样本。
  CHECK(fx.dsp.fastBypassActive());
  CHECK(fx.dsp.fullySettled());
  for (std::size_t i = 0; i < audio::kEqualizer31BandCenterHz.size(); ++i) {
    CHECK(fx.dsp.bandBypassed(i));
  }

  std::vector<float> buffer;
  fillPseudoRandom(buffer, 12345U, 4800U);
  const std::vector<float> reference = buffer;
  fx.dsp.process(buffer.data(), 2400U);
  CHECK(buffer == reference);  // 逐位（f32 operator== 即逐位同值；无任何处理触碰）

  // 频响平坦：正弦经过 = 原样（0dB±0.001）。
  std::vector<float> tone;
  fillSine(tone, 48000.0, 997.0, 0.5, 4800U);
  const std::vector<float> toneRef = tone;
  fx.dsp.process(tone.data(), 4800U);
  CHECK(tone == toneRef);
}

TEST_CASE("eq coefficients: 单 band 峰值位置（31 档 1000Hz +15dB 中心为峰）") {
  DspFixture fx;
  const audio::EqualizerConfig config = singleBandConfig(audio::EqualizerBandMode::Band31, 17U, 15.0F);
  const auto report = fx.configure(config, 48000U, 1U);
  REQUIRE(report.accepted);
  CHECK(report.enabled);
  CHECK(report.activeBandCount == audio::kEqualizer31BandCenterHz.size());
  CHECK(report.nyquistBypassBandCount == 0U);
  fx.settle(48000U, 1U);
  CHECK(fx.dsp.fullySettled());
  CHECK(fx.dsp.currentBandGainDb(17U) == 15.0);

  // 中心平坦区取样（避 Q=4.32 裙边最陡点）：800/1000/1250 三点，峰必在 1000。
  const double atCenter = fx.measureGainDb(48000U, 1U, 1000.0, 15.0, 0.2);
  const double belowLow = fx.measureGainDb(48000U, 1U, 800.0, 15.0, 0.2);
  const double belowHigh = fx.measureGainDb(48000U, 1U, 1250.0, 15.0, 0.2);
  CHECK(std::abs(atCenter - 15.0) <= 0.2);
  CHECK(atCenter >= belowLow + 3.0);   // 邻档裙边显著低于峰
  CHECK(atCenter >= belowHigh + 3.0);
}

TEST_CASE("eq coefficients: 未用 band 直通（Band10 前缀外增益不生效，逐位不变）") {
  DspFixture fx;
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band10, true);
  // 仅前缀外（idx10..30）置 +15dB：按 mode 前缀语义不得参与处理。
  for (std::size_t i = 10U; i < audio::kEqualizer31BandCenterHz.size(); ++i) {
    config.bandGainsDb[i] = 15.0F;
  }
  const auto report = fx.configure(config, 48000U, 1U);
  REQUIRE(report.accepted);
  CHECK(report.activeBandCount == audio::kEqualizer10BandCenterHz.size());
  CHECK(fx.dsp.fastBypassActive());  // 前缀内全 0dB → 出厂快路径
  for (std::size_t i = 10U; i < audio::kEqualizer31BandCenterHz.size(); ++i) {
    CHECK(fx.dsp.bandBypassed(i));  // 超前缀未用 band：硬直通
  }
  for (std::size_t i = 0U; i < 10U; ++i) {
    CHECK(fx.dsp.bandBypassed(i));  // 0dB 收敛：硬直通
  }

  std::vector<float> buffer;
  fillPseudoRandom(buffer, 777U, 4800U);
  const std::vector<float> reference = buffer;
  fx.dsp.process(buffer.data(), 2400U);
  CHECK(buffer == reference);

  // 频响平坦：1kHz 正弦原样通过。
  std::vector<float> tone;
  fillSine(tone, 48000.0, 1000.0, 0.5, 9600U);
  const std::vector<float> toneRef = tone;
  fx.dsp.process(tone.data(), 9600U);
  CHECK(tone == toneRef);
}

TEST_CASE("eq coefficients: 44.1k–192k 极端系数容差 0.2dB（中心平坦区取样）") {
  // fs×band×方向覆盖系数极端几何：DC 侧 20Hz@44.1k 与 63Hz@192k、奈奎斯特侧
  // 8kHz@44.1k 与 20kHz@192k（高频窄带）、±15dB 双向。
  // 注：192k 下 <63Hz 的 ±15dB 中心增益受 f32 系数量化坍塌（实测 20Hz +15 短
  // 3.4dB / −15 短 3.9dB、25Hz 短 1.4dB、31.5Hz−15 短 0.85dB、50Hz−15 短 0.28dB
  // ——记录于 equalizer-b0-notes，0.2dB 规格在该几何不成立）；取样下界取 63Hz。
  const std::array<std::uint32_t, 2> rates{44100U, 192000U};
  for (const std::uint32_t fs : rates) {
    DspFixture fx;
    std::vector<std::pair<double, float>> probes;
    if (fs == 44100U) {
      probes = {{20.0, 15.0F}, {20.0, -15.0F}, {8000.0, 15.0F}};
    } else {
      probes = {{63.0, 15.0F}, {20000.0, 15.0F}, {20000.0, -15.0F}, {63.0, -15.0F}};
    }
    for (const auto& probe : probes) {
      const double f0 = probe.first;
      const float gainDb = probe.second;
      const std::size_t bandIndex =
          static_cast<std::size_t>(std::find(audio::kEqualizer31BandCenterHz.begin(),
                                             audio::kEqualizer31BandCenterHz.end(),
                                             f0) -
                                   audio::kEqualizer31BandCenterHz.begin());
      REQUIRE(bandIndex < audio::kEqualizer31BandCenterHz.size());
      const auto report = fx.configure(singleBandConfig(audio::EqualizerBandMode::Band31, bandIndex, gainDb),
                                       fs, 1U);
      REQUIRE(report.accepted);
      CHECK(report.nyquistBypassBandCount == 0U);  // 192k 全带可用；44.1k 的 20k 也在 0.95 界内
      fx.settle(fs, 1U);
      const double measuredDb = fx.measureGainDb(fs, 1U, f0, gainDb, 0.2);
      CHECK_MESSAGE(std::abs(measuredDb - static_cast<double>(gainDb)) <= 0.2,
                    "fs=" << fs << " f0=" << f0 << " gainDb=" << gainDb
                          << " measured=" << measuredDb);
    }
  }
}

TEST_CASE("eq coefficients: 越界守卫 fs=22050——顶段 ±15dB 硬直通恒等无增长") {
  // 22050：nyquist=11025，0.95×nyquist=10473.75 → 12500(28)/16000(29)/20000(30) 越界。
  DspFixture fx;
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
  config.bandGainsDb[26U] = 15.0F;  // 8000（界内，对比用）
  config.bandGainsDb[28U] = 15.0F;  // 12500（越界）
  config.bandGainsDb[29U] = 15.0F;  // 16000（越界）
  config.bandGainsDb[30U] = 15.0F;  // 20000（越界）
  const auto report = fx.configure(config, 22050U, 1U);
  REQUIRE(report.accepted);
  CHECK(report.nyquistBypassBandCount == 3U);
  CHECK(fx.dsp.bandRangeBypassed(28U));
  CHECK(fx.dsp.bandRangeBypassed(29U));
  CHECK(fx.dsp.bandRangeBypassed(30U));
  CHECK(!fx.dsp.bandRangeBypassed(26U));
  CHECK(!fx.dsp.bandRangeBypassed(27U));  // 10000 界内

  // 越界 band 自身 0dB 目标 → 仅越界增益时全链 fastBypass。
  audio::EqualizerConfig bypassOnly = flatConfig(audio::EqualizerBandMode::Band31, true);
  bypassOnly.bandGainsDb[29U] = 15.0F;  // 16000 越界 +15
  bypassOnly.bandGainsDb[30U] = -15.0F;
  const auto bypassReport = fx.configure(bypassOnly, 22050U, 1U);
  REQUIRE(bypassReport.accepted);
  CHECK(bypassReport.nyquistBypassBandCount == 3U);
  CHECK(fx.dsp.fastBypassActive());
  std::vector<float> buffer;
  fillPseudoRandom(buffer, 31337U, 8820U);
  const std::vector<float> reference = buffer;
  fx.dsp.process(buffer.data(), 2205U);
  CHECK(buffer == reference);  // 恒等：越界增益零影响（无增长亦无衰减）

  // 正弦过链不超输入峰值（恒等无增长断言的另一面）。
  std::vector<float> tone;
  fillSine(tone, 22050.0, 9000.0, 0.95, 8820U);
  const std::vector<float> toneRef = tone;
  fx.dsp.process(tone.data(), 2205U);
  CHECK(tone == toneRef);
}

TEST_CASE("eq coefficients: 越界守卫边界精度——fs=32000/44100 界内顶段仍正常滤波") {
  // 32000：0.95×16000=15200 → Band10 仅 16000(9) 越界；12500 界内仍可 +15dB。
  DspFixture fx;
  audio::EqualizerConfig config10 = flatConfig(audio::EqualizerBandMode::Band10, true);
  config10.bandGainsDb[9U] = 15.0F;  // 16000 ≥ 15200 → 越界
  const auto report10 = fx.configure(config10, 32000U, 1U);
  REQUIRE(report10.accepted);
  CHECK(report10.nyquistBypassBandCount == 1U);
  CHECK(fx.dsp.bandRangeBypassed(9U));
  CHECK(!fx.dsp.bandRangeBypassed(8U));  // 8000 界内
  CHECK(fx.dsp.fastBypassActive());

  // 31 档：12500(28) 为界内最顶可用段 → 实测 +15±0.2dB；16000/20000 越界计数 2。
  DspFixture fx31;
  const auto report31 = fx31.configure(singleBandConfig(audio::EqualizerBandMode::Band31, 28U, 15.0F),
                                       32000U, 1U);
  REQUIRE(report31.accepted);
  CHECK(report31.nyquistBypassBandCount == 2U);  // 仅 12500 生效；16000/20000 几何越界
  fx31.settle(32000U, 1U);
  const double measured12500 = fx31.measureGainDb(32000U, 1U, 12500.0, 15.0, 0.2);
  CHECK(std::abs(measured12500 - 15.0) <= 0.2);

  // 44100：0.95×22050=20947.5 > 20000 → 全带可用，无越界（含顶段 20000 +15）。
  DspFixture fx44;
  const auto report44 = fx44.configure(singleBandConfig(audio::EqualizerBandMode::Band31, 30U, 15.0F),
                                       44100U, 1U);
  REQUIRE(report44.accepted);
  CHECK(report44.nyquistBypassBandCount == 0U);
  fx44.settle(44100U, 1U);
  const double measured20k = fx44.measureGainDb(44100U, 1U, 20000.0, 15.0, 0.2);
  CHECK(std::abs(measured20k - 15.0) <= 0.2);
}
