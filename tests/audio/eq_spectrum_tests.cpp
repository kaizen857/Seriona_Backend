// 频谱分析纯核心 + 摘录下混联测（任务 31，B3.3 组 seriona.audio.eq_spectrum）。
//
// 被测面：src/audio/spectrum_analyzer.{h,cpp}（任务 30 纯组件：feed(SpectrumFeedFrame)
// → optional<SpectrumAnalysis>，120 对数桶 binsDb；头注释即行为契约）+ 任务 29 设备摘录
// 下混（captureDownmix* 位于 audio_output_device.cpp 匿名空间，经 renderCallback 真实
// 路径 + latestCaptureFrame 断言单声道均值——本文件自带最小 fake backend 装置，独立
// 编写，不依赖 eq_render_integration_tests.cpp 内部符号；两态中链关闭态输出域逐格式
// int→f32 冻结尺度下混覆盖 captureDownmixIntToMono（s16/s32）/captureDownmixS24ToMono/
// captureDownmixFloatToMono（f32）三分支 × N=1 直取 / N>1 均值）。
//
// 覆盖（计划书 B3.3 五项 + dB 标定数值锚，120 桶/比例分摊语义，2026-09 重锚）：
//   1. 已知正弦落桶（对数轴率无关：logidx(i) = 120·log10(f/20)/3）：
//      - 22050 3kHz（logidx 87.04，主瓣尾越 87/88 桶界）→ 87 主体 + 86 分片按范围
//        重叠比例分摊（实测 b86≈−7.49 / b87≈−0.85，合计 ≈0dB）——比例分摊回归锁；
//      - 192k 10kHz（logidx 107.96，主瓣整落桶 107）→ 单桶读数 ≈0dB（实测 −0.004），
//        邻桶 108 仅主瓣外泄漏（−29.9dB 级）；
//      - 44.1k 1kHz 跨 67/68 桶界（edge(68)=1002.4Hz，logidx 67.96）→ 邻桶合计 ≈0dB。
//      判据：落桶邻域功率合计 ≈ 10log10(1.5A²)；主瓣整落单桶时取桶读数 ≈0dB±0.2
//      （A=1）；跨桶界按新几何单桶值重锚（分摊后各桶读数非 0）。
//   2. 采样率标定：windowSizeForRate 分档全边界（2048/4096/8192/16384 四档 + 0 守卫）；
//      同频正弦不同 fs 落相同几何桶（桶轴按实际 fs 生成，20Hz–20kHz 对数轴率无关）；
//      跨率 feed 重建后输出与全新实例逐桶一致（位等）。
//   3. 120 桶边界：binEdgeHz(0)=20/binEdgeHz(120)=20000 精确、边界单调；
//      binCenterHz 几何中心；binMeasurable/lastMeasurableBin 纯函数面；
//      fs<40k 截断行为：22050 → 桶 110..119 输出 kFloorDb（last=109）、
//      32000 → 桶 117..119 静音（last=116）、40000 → 仅桶 119 下界恰在奈氏内
//      （last=119）、44100 → 全 120 桶可测。
//   4. 下混：见头注释（device 真实路径）。
//   5. 重建 generation 弃帧：同纪元喂部分窗→换 generation→新纪元首窗不含旧纪元
//      样本（逐桶与全新实例一致，位等）；域变化弃未满窗（构造会混窗的序列验证
//      不混——丢弃实例全桶 floor，同域对照有能量）。
//   dB 标定数值锚：满刻度正弦主瓣整桶 ≈0dB（5kHz@22050 → 桶 95）；A=0.5 → −6.02dB；
//      A=0.1 → −20dB（±0.2dB）；静音输入全桶 −120。
//   不变量（新增，比例分摊语义锁）：
//      - energy-preservation：Σ可测桶 10^(binsDb/10) ≈ Σ参与 FFT bin 功率/1.5
//        （Parseval 独立复算，白噪/多音，44.1k 与 48k，±0.1dB）；
//      - no-dead-bin：白噪下可测桶无恒地板（窄对数桶由相邻 bin 比例共享）；
//      - 静音/截断尾恒 kFloorDb 语义不变。
//
// 正弦构造：相位 double 递推（仿既有 eq_dsp/eq_render 测试 fillSine）；喂帧按
// analyzer 期望（windowSizeForRate(fs) 帧整窗即产一份，无首窗延迟）；整窗倍数 +
// 余量喂入取最近分析，或直接喂整窗帧数。
#include "seriona/audio/device/audio_output_device.h"

#include <doctest.h>

#include "../../src/audio/spectrum_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace seriona::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

// —— 正弦 / 噪声 / 窗口生成（测试侧独立实现，相位 double 递推）——

void fillSineMono(std::vector<float>& out, double fs, double f0Hz, double amplitude,
                  std::size_t frames) {
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

std::vector<float> makeTone(double fs, double f0Hz, double amplitude, std::uint32_t frames) {
  std::vector<float> tone;
  fillSineMono(tone, fs, f0Hz, amplitude, frames);
  return tone;
}

std::vector<float> makeSilence(std::uint32_t frames) { return std::vector<float>(frames, 0.0F); }

// 白噪声（σ=0.5，确定性种子；测试只依赖统计语义——无死桶断言带 20dB 以上裕量、
// 能量守恒对任意输入成立，跨 stdlib 分布实现差异不敏感）。
std::vector<float> makeWhiteNoise(std::uint32_t frames, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 0.5);
  std::vector<float> noise(frames);
  for (float& sample : noise) {
    sample = static_cast<float>(gauss(rng));
  }
  return noise;
}

// 桶功率线性域（dB 已相对 1.5 参考；同参考求和后转回 dB）。
double binsPowerDb(const SpectrumAnalysis& a, std::size_t from, std::size_t toInclusive) {
  double power = 0.0;
  for (std::size_t i = from; i <= toInclusive; ++i) {
    power += std::pow(10.0, static_cast<double>(a.binsDb[i]) / 10.0);
  }
  return 10.0 * std::log10(power);
}

// 期望 dB 值：10log10(1.5A² / 1.5) = 20log10(A)。
double toneDb(double amplitude) { return 20.0 * std::log10(amplitude); }

// 喂一帧并返回最近分析（无分析则 doctest FAIL——调用方保证有）。
SpectrumAnalysis requireAnalysis(SpectrumAnalyzer& analyzer, const SpectrumFeedFrame& frame) {
  const auto result = analyzer.feed(frame);
  REQUIRE(result.has_value());
  return *result;
}

SpectrumAnalysis analyzeWindow(SpectrumAnalyzer& analyzer, const std::vector<float>& samples,
                               std::uint32_t generation, std::uint32_t sampleRate,
                               SpectrumDomainTag domain = SpectrumDomainTag::ChainInactiveOutput) {
  REQUIRE(!samples.empty());
  SpectrumFeedFrame frame{};
  frame.generation = generation;
  frame.sampleRate = sampleRate;
  frame.frameCount = static_cast<std::uint32_t>(samples.size());
  frame.domain = domain;
  frame.samples = samples.data();
  return requireAnalysis(analyzer, frame);
}

// 全新实例分析同一段样本（重建弃帧/位等对照基准）。
SpectrumAnalysis freshAnalysis(const std::vector<float>& samples, std::uint32_t generation,
                               std::uint32_t sampleRate,
                               SpectrumDomainTag domain = SpectrumDomainTag::ChainInactiveOutput) {
  SpectrumAnalyzer fresh;
  return analyzeWindow(fresh, samples, generation, sampleRate, domain);
}

void requireBinsAllEqual(const SpectrumAnalysis& actual, const SpectrumAnalysis& expected) {
  REQUIRE(actual.binsDb.size() == expected.binsDb.size());
  for (std::size_t i = 0; i < actual.binsDb.size(); ++i) {
    if (actual.binsDb[i] != expected.binsDb[i]) {
      FAIL("逐桶位等失败 @ bin " << i << "：" << actual.binsDb[i] << " vs " << expected.binsDb[i]);
    }
  }
}

void requireBinsFloor(const SpectrumAnalysis& a, std::size_t from, std::size_t toInclusive) {
  for (std::size_t i = from; i <= toInclusive; ++i) {
    if (a.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
      FAIL("期望静音标记 kFloorDb @ bin " << i << "，实读 " << a.binsDb[i]);
    }
  }
}

// 对数桶轴公式（测试侧独立参考）：edge(i) = 20×10^(3i/120)，center = 20×10^(3(i+0.5)/120)。
double edgeHzDouble(std::size_t i) { return 20.0 * std::pow(10.0, 3.0 * static_cast<double>(i) / 120.0); }
double centerHzDouble(std::size_t i) {
  return 20.0 * std::pow(10.0, 3.0 * (static_cast<double>(i) + 0.5) / 120.0);
}

// —— 能量守恒测试侧独立复算（Parseval 路径）——
// 参与 bin 集 = k=1..n/2−1 且 f_k = k·fs/n ∈ [20, 20000)。恒等式
//   Σ_{k=0..n/2} |X_k|² = n·Σ_j (w_j·x_j)² （非归一 RDFT Parseval）
// ⇒ 参与功率 = n·Σ(wx)² − |X_0|² − |X_{n/2}|² − Σ_{k≥k_hi} |X_k|²
// （k_hi = 首个 f_k ≥ 20000 的 bin；f_k 全 < 20000 时该项为空）。
// X'_k = 4·X_k/n（Σw = n/2 → 2/Σw = 4/n），故 Σ参与 P_k = (16/n²)·参与 |X_k|² 和。

// 直接 DFT 求单个 bin |X_k|²（x 为已加窗样本；相位旋转递推，避免逐点三角函数）。
double dftBinPowerSq(const std::vector<float>& windowed, std::size_t k) {
  const auto n = windowed.size();
  double real = 0.0;
  double imag = 0.0;
  double phase = 0.0;
  const double step = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
  for (std::size_t j = 0; j < n; ++j) {
    real += static_cast<double>(windowed[j]) * std::cos(phase);
    imag -= static_cast<double>(windowed[j]) * std::sin(phase);
    phase += step;
    if (phase >= 2.0 * kPi) {
      phase -= 2.0 * kPi;
    }
  }
  return real * real + imag * imag;
}

// 参与 FFT bin 总功率 P = Σ|X'_k|²，转 dB（相对 1.5 参考）。
double participatingPowerDb(const std::vector<float>& samples, std::uint32_t sampleRate) {
  REQUIRE(!samples.empty());
  const auto n = static_cast<std::size_t>(SpectrumAnalyzer::windowSizeForRate(sampleRate));
  REQUIRE(samples.size() == n);  // 本辅助按单窗输入设计
  const double fs = static_cast<double>(sampleRate);
  // Hann 周期窗（与 analyzer 同式，测试侧独立展开）
  std::vector<float> windowed(n);
  double sumWx2 = 0.0;
  for (std::size_t j = 0; j < n; ++j) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(j) / static_cast<double>(n));
    const double y = w * static_cast<double>(samples[j]);
    windowed[j] = static_cast<float>(y);
    sumWx2 += y * y;
  }
  const double totalRdfPower = static_cast<double>(n) * sumWx2;  // Σ_{k=0..n−1}|X_k|²（Parseval）
  // 实输入 RDFT 全谱 = |X_0|² + |X_{n/2}|² + 2·Σ_{k=1..n/2−1}|X_k|²（±k 共轭对称）→
  // 先取正频率半边，再扣参与集外 bin。
  double excluded = (dftBinPowerSq(windowed, 0U) + dftBinPowerSq(windowed, n / 2U)) * 0.5;
  double half = totalRdfPower * 0.5 - excluded;  // Σ_{k=1..n/2−1} |X_k|²
  // 低段排除：f_k = k·fs/n < 20 Hz 的 bin（中心不在对数轴内，不参与）
  const double kLo = 20.0 * static_cast<double>(n) / fs;
  const std::size_t lastLow = static_cast<std::size_t>(std::ceil(kLo - 1e-9)) - 1U;
  for (std::size_t k = 1U; k <= lastLow && k < n / 2U; ++k) {
    half -= dftBinPowerSq(windowed, k);
  }
  // 高段排除：f_k = k·fs/n ≥ 20000 的 k（仅 fs ≥ 40k 时非空）
  const double kHi = 20000.0 * static_cast<double>(n) / fs;
  const std::size_t firstExcluded = static_cast<std::size_t>(std::ceil(kHi - 1e-9));
  for (std::size_t k = firstExcluded; k < n / 2U; ++k) {
    half -= dftBinPowerSq(windowed, k);
  }
  const double participatingSum = half;
  REQUIRE(participatingSum > 0.0);
  // X'_k = 4·X_k/n → P_k = (16/n²)·|X_k|²
  const double pTotal = participatingSum * 16.0 / (static_cast<double>(n) * static_cast<double>(n));
  return 10.0 * std::log10(pTotal / 1.5);
}

// 输出侧：Σ可测桶 10^(binsDb/10) 转 dB（不可测/静音桶 = kFloorDb 不贡献能量）。
double measurablePowerDb(const SpectrumAnalysis& a, std::uint32_t sampleRate) {
  double power = 0.0;
  for (std::size_t i = 0; i < SpectrumAnalyzer::kBinCount; ++i) {
    if (!SpectrumAnalyzer::binMeasurable(i, sampleRate)) {
      continue;
    }
    power += std::pow(10.0, static_cast<double>(a.binsDb[i]) / 10.0);
  }
  return 10.0 * std::log10(power);
}

}  // namespace

TEST_CASE("eq spectrum: 窗长分档全边界——windowSizeForRate 档位与 0 率守卫") {
  // 头契约：fs ≤ 24k→2048；24k<fs≤48k→4096；48k<fs≤96k→8192；fs>96k→16384。
  CHECK(SpectrumAnalyzer::windowSizeForRate(0U) == 0U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(22050U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(24000U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(24001U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(32000U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(44100U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(48000U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(48001U) == 8192U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(96000U) == 8192U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(96001U) == 16384U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(192000U) == 16384U);
}

TEST_CASE("eq spectrum: 对数桶轴纯函数——端点/单调/几何中心/可测面（binMeasurable/lastMeasurableBin）") {
  // 端点精确：edge(0)=20、edge(120)=20000。
  CHECK(SpectrumAnalyzer::binEdgeHz(0U) == 20.0F);
  CHECK(SpectrumAnalyzer::binEdgeHz(SpectrumAnalyzer::kBinCount) == 20000.0F);
  CHECK(SpectrumAnalyzer::kMinLogHz == 20.0F);
  CHECK(SpectrumAnalyzer::kMaxLogHz == 20000.0F);

  // 全边界与独立 double 公式一致（f32 表示误差 ≤0.01 Hz 级，宽容差）且严格单调。
  for (std::size_t i = 0U; i <= SpectrumAnalyzer::kBinCount; ++i) {
    const double expect = edgeHzDouble(i);
    CHECK(std::fabs(static_cast<double>(SpectrumAnalyzer::binEdgeHz(i)) - expect) <= 0.01);
  }
  for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
    CHECK(SpectrumAnalyzer::binEdgeHz(i) < SpectrumAnalyzer::binEdgeHz(i + 1U));
  }
  // 超界入参钳制到 120（edge(150) == edge(120)）。
  CHECK(SpectrumAnalyzer::binEdgeHz(150U) == SpectrumAnalyzer::binEdgeHz(SpectrumAnalyzer::kBinCount));

  // 几何中心：位于 (edge(i), edge(i+1)) 且 ≈ 20×10^(3(i+0.5)/120)。
  for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
    const float center = SpectrumAnalyzer::binCenterHz(i);
    CHECK(center > SpectrumAnalyzer::binEdgeHz(i));
    CHECK(center < SpectrumAnalyzer::binEdgeHz(i + 1U));
    CHECK(std::fabs(static_cast<double>(center) - centerHzDouble(i)) <= 0.01);
  }

  // 可测面：binMeasurable = lower edge < fs/2−0.01；截断几何率无关桶轴（120 桶推导锚）。
  // lastMeasurableBin：22050 → 109（edge(110)=11246.9 > 11025）、32000 → 116
  // （edge(117)=16826 > 16000）、40000 → 119（edge(120)=20000 ≥ 19999.99，仅最后桶
  // 下界 18881 < 19999.99 可测）、≥44100 → 120（全 120 桶可测，哨兵 = kBinCount）。
  const std::array<std::pair<std::uint32_t, std::size_t>, 7> rateToLast = {{
      {22050U, 109U},
      {32000U, 116U},
      {40000U, 119U},
      {44100U, 120U},
      {48000U, 120U},
      {96000U, 120U},
      {192000U, 120U},
  }};
  for (const auto& [rate, expectedLast] : rateToLast) {
    CAPTURE(rate);
    CHECK(SpectrumAnalyzer::lastMeasurableBin(rate) == expectedLast);
    for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
      const bool expectMeasurable =
          (expectedLast == SpectrumAnalyzer::kBinCount) || (i <= expectedLast);
      CHECK(SpectrumAnalyzer::binMeasurable(i, rate) == expectMeasurable);
    }
    CHECK(SpectrumAnalyzer::binMeasurable(SpectrumAnalyzer::kBinCount, rate) == false);  // 越界恒 false
  }
  // 截断桶界精确复核：22050 桶 109 可测/110 不可测；32000 桶 116 可测/117 不可测。
  CHECK(SpectrumAnalyzer::binMeasurable(109U, 22050U));
  CHECK_FALSE(SpectrumAnalyzer::binMeasurable(110U, 22050U));
  CHECK(SpectrumAnalyzer::binMeasurable(116U, 32000U));
  CHECK_FALSE(SpectrumAnalyzer::binMeasurable(117U, 32000U));
  CHECK(SpectrumAnalyzer::binMeasurable(119U, 40000U));
  // 0 率守卫：binMeasurable false、lastMeasurableBin 返回 kBinCount（全不可测语义）。
  CHECK(SpectrumAnalyzer::binMeasurable(0U, 0U) == false);
  CHECK(SpectrumAnalyzer::lastMeasurableBin(0U) == SpectrumAnalyzer::kBinCount);
}

TEST_CASE("eq spectrum: 正弦落桶与比例分摊——22050 3kHz 跨 86/87 邻桶合计 ≈0dB、5kHz 整落 95 桶") {
  // 3kHz：对数轴 index = 120·log10(3000/20)/3 ≈ 87.04 → 主瓣尾越桶 87 上界
  // （edge(88)=3169.8Hz；k0 = 3000×2048/22050 ≈ 278.6，Hann 主瓣 ±2 bin ≈ ±21.5Hz）
  // → 功率按范围重叠比例分摊到 86/87：实测 b87≈−0.85、b86≈−7.49、合计 ≈0dB（A=1）。
  // 此锚锁比例分摊本身：若回退中心点整落，b87 将 ≈0dB 且 b86 = kFloorDb（双 FAIL）。
  {
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(22050.0, 3000.0, 1.0, 2048U);
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(22050U) == 2048U);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, 22050U);
    CHECK(std::fabs(binsPowerDb(analysis, 86U, 87U)) <= 0.3);  // 两片合计 = 总功率
    CHECK(analysis.binsDb[87U] > -1.5F);                       // 主体桶（实测 −0.85）
    CHECK(analysis.binsDb[87U] < -0.3F);
    CHECK(analysis.binsDb[86U] > -8.5F);  // 越界残片桶（实测 −7.49）
    CHECK(analysis.binsDb[86U] < -6.5F);
    CHECK(analysis.binsDb[85U] <= -40.0F);  // 主瓣外泄漏低（−80dB 级）
    CHECK(analysis.binsDb[88U] <= -40.0F);
  }

  // 5kHz：index ≈ 95.92 → 主瓣整落桶 95（edge(95)=4742.9..edge(96)=5023.8；
  // k0 ≈ 464.4，主瓣 ±21.5Hz ⊂ 桶内）→ 单桶读数 ≈ 0 dB（A=1，实测 −0.0012）。
  {
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(22050.0, 5000.0, 1.0, 2048U);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, 22050U);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[95U])) <= 0.2);
    CHECK(std::fabs(binsPowerDb(analysis, 94U, 96U)) <= 0.3);
    CHECK(analysis.binsDb[96U] <= -20.0F);  // 仅主瓣外泄漏（实测 −35dB 级）
  }
}

TEST_CASE("eq spectrum: 跨桶界纯音——44.1k 1kHz 跨 67/68 合计 ≈0dB、192k 10kHz 整落 107 ≈0dB") {
  // 1kHz 恰近桶界 edge(68)=1002.4Hz（index≈67.96）：主瓣按泄漏分跨 67/68，两边
  // 合计 = 总功率 ≈ 0 dB（A=1；实测 b67=−2.04/b68=−4.27、合计 −0.000）。
  {
    constexpr std::uint32_t fs = 44100U;
    constexpr std::uint32_t n = 4096U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    CHECK(SpectrumAnalyzer::binEdgeHz(68U) > 1000.0F);
    CHECK(SpectrumAnalyzer::binEdgeHz(68U) < 1010.0F);  // edge(68)=1002.4 锚
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 1000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(binsPowerDb(analysis, 67U, 68U)) <= 0.3);
    CHECK(analysis.binsDb[67U] < 0.0F);  // 分跨：单桶读数不足 0dB
    CHECK(analysis.binsDb[67U] > -5.0F);
    CHECK(analysis.binsDb[68U] < 0.0F);
    CHECK(analysis.binsDb[68U] > -6.0F);
  }

  // 10kHz：index≈107.96 → 主瓣整落桶 107（edge(108)=10023.7Hz；k0 ≈ 853.3，
  // 主瓣 ±23.4Hz ⊂ 桶内）→ 单桶 ≈ 0 dB（实测 −0.004）；桶 108 仅主瓣外泄漏。
  {
    constexpr std::uint32_t fs = 192000U;
    constexpr std::uint32_t n = 16384U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 10000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[107U])) <= 0.5);
    CHECK(std::fabs(binsPowerDb(analysis, 107U, 108U)) <= 0.3);
    CHECK(analysis.binsDb[108U] <= -15.0F);  // 泄漏宽容（实测 −29.9dB）
  }
}

TEST_CASE("eq spectrum: dB 标定数值锚——A=1→0dB / A=0.5→−6.02dB / A=0.1→−20dB（±0.2）与静音全桶 −120") {
  // 满刻度正弦（5kHz@22050，主瓣整落桶 95）：桶读数 = 10log10(功率/1.5)。
  for (const double amplitude : {1.0, 0.5, 0.1}) {
    CAPTURE(amplitude);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(22050.0, 5000.0, amplitude, 2048U);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, 22050U);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[95U]) - toneDb(amplitude)) <= 0.2);
    CHECK(std::fabs(binsPowerDb(analysis, 94U, 96U) - toneDb(amplitude)) <= 0.3);
  }

  // 静音输入：全 120 桶 == kFloorDb（−120.0F 逐桶精确）。
  SpectrumAnalyzer analyzer;
  const auto silence = makeSilence(2048U);
  const auto analysis = analyzeWindow(analyzer, silence, 0U, 22050U);
  for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
    if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
      FAIL("静音窗期望全桶 kFloorDb @ bin " << i << "，实读 " << analysis.binsDb[i]);
    }
  }
}

TEST_CASE("eq spectrum: 采样率标定——同频正弦跨 fs 落相同几何桶对（1kHz 恒跨 67/68 桶界）") {
  // 桶轴按实际 fs 生成：1kHz 在对数轴上恒为 index≈67.96（edge(68)=1002.4Hz 率无关），
  // 五档采样率各自按真实 Δf 解算 FFT bin → 能量恒落 {67,68} 几何桶对，合计 ≈0dB。
  // （若轴误按他率解析，例如把 22050 帧当 44100，1kHz 会落到 bin 90 级——本用例即
  // 率标定失效判别。）
  const std::array<std::uint32_t, 5> rates{22050U, 44100U, 48000U, 96000U, 192000U};
  for (const std::uint32_t fs : rates) {
    CAPTURE(fs);
    const auto n = SpectrumAnalyzer::windowSizeForRate(fs);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 1000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    // 跨桶界对 {67,68} 承载全部能量（其余桶 ≤ 旁瓣 −18dB）。
    CHECK(binsPowerDb(analysis, 67U, 68U) > -0.7);
    CHECK(binsPowerDb(analysis, 67U, 68U) < 0.7);
    CHECK(analysis.binsDb[67U] < 0.0F);
    CHECK(analysis.binsDb[67U] > -5.0F);
    CHECK(analysis.binsDb[68U] < 0.0F);
    CHECK(analysis.binsDb[68U] > -6.0F);
    CHECK(analysis.binsDb[66U] <= -18.0F);
    CHECK(analysis.binsDb[69U] <= -18.0F);
  }
}

TEST_CASE("eq spectrum: 跨率/跨代重建弃帧——新纪元首窗与全新实例逐桶位等（无旧纪元样本混入）") {
  // (a) generation 变化（同率）：gen1 喂半窗强 3kHz 音（未满窗无产出）→ 换 gen2 喂
  //     整窗静音 → 全桶 floor。若旧纪元半窗样本未被丢弃，将与静音混窗产生能量。
  {
    SpectrumAnalyzer analyzer;
    const auto loudHalf = makeTone(44100.0, 3000.0, 1.0, 2048U);  // n/2 = 2048（n=4096）
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 2048U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = loudHalf.data()}) == std::nullopt);
    const auto silence = makeSilence(4096U);
    const auto analysis = analyzeWindow(analyzer, silence, 2U, 44100U);
    REQUIRE(analysis.generation == 2U);
    for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
      if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
        FAIL("换代后静音窗期望全桶 floor @ bin " << i << "，实读 " << analysis.binsDb[i]);
      }
    }
  }

  // (b) 换采样率（44.1k→48k）：gen2 首窗 = 仅新纪元样本 → 与全新 48k 实例逐桶位等。
  {
    SpectrumAnalyzer analyzer;
    const auto oldHalf = makeTone(44100.0, 3000.0, 1.0, 2048U);
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 2048U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = oldHalf.data()}) == std::nullopt);
    const auto newWindow = makeTone(48000.0, 1000.0, 1.0, 4096U);
    const auto analysis = analyzeWindow(analyzer, newWindow, 2U, 48000U);
    REQUIRE(analysis.generation == 2U);
    REQUIRE(analysis.sampleRate == 48000U);
    const auto reference = freshAnalysis(newWindow, 2U, 48000U);
    requireBinsAllEqual(analysis, reference);
    // 语义锚：新纪元确以 48k 解析（1kHz 跨 67/68 而非按 44.1k 落到别桶）。
    CHECK(std::fabs(binsPowerDb(analysis, 67U, 68U)) <= 0.3);
  }

  // (c) 换代后新纪元首窗为强音：与全新实例逐桶位等（覆盖"非静音"路径的弃帧证明）。
  {
    SpectrumAnalyzer analyzer;
    const auto oldHalf = makeTone(44100.0, 5000.0, 1.0, 2048U);
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 2048U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = oldHalf.data()}) == std::nullopt);
    const auto newWindow = makeTone(44100.0, 3000.0, 0.5, 4096U);
    const auto analysis = analyzeWindow(analyzer, newWindow, 2U, 44100U);
    const auto reference = freshAnalysis(newWindow, 2U, 44100U);
    requireBinsAllEqual(analysis, reference);
    CHECK(std::fabs(binsPowerDb(analysis, 86U, 87U) - toneDb(0.5)) <= 0.3);
  }
}

TEST_CASE("eq spectrum: 域变化弃未满窗——构造会混窗的序列验证不混（丢弃实例全桶 floor + 同域对照）") {
  // 契约：domain 变化仅弃未满窗累积（率/FFT 保留），单窗内样本恒同域。
  constexpr std::uint32_t fs = 44100U;
  constexpr std::uint32_t n = 4096U;
  REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);

  // 同域对照：1/4 窗强音 + 同域 3/4 窗静音 → 混窗（窗内含强音能量，非全 floor）——
  // 证明"若域切换实例不弃窗，该序列必检出能量"。
  {
    SpectrumAnalyzer control;
    const auto loudQuarter = makeTone(static_cast<double>(fs), 3000.0, 1.0, n / 4U);
    SpectrumFeedFrame first{};
    first.generation = 0U;
    first.sampleRate = fs;
    first.frameCount = n / 4U;
    first.samples = loudQuarter.data();
    REQUIRE(control.feed(first) == std::nullopt);
    const auto silenceTail = makeSilence(n - n / 4U);
    const auto analysis = analyzeWindow(control, silenceTail, 0U, fs);
    double peak = -1e18;
    for (std::size_t i = 75U; i <= 100U; ++i) {  // 3kHz → logidx 87（桶 84..90 邻域）
      peak = std::max(peak, static_cast<double>(analysis.binsDb[i]));
    }
    CHECK(peak > -30.0);  // 强音混入窗 → 3kHz 邻域有能量
  }

  // 域切换实例：dom0 喂同款 1/4 窗强音 → 换 dom1 喂整窗静音 → 全桶 floor（未满窗已弃）。
  {
    SpectrumAnalyzer analyzer;
    const auto loudQuarter = makeTone(static_cast<double>(fs), 3000.0, 1.0, n / 4U);
    SpectrumFeedFrame first{};
    first.generation = 0U;
    first.sampleRate = fs;
    first.frameCount = n / 4U;
    first.domain = SpectrumDomainTag::ChainInactiveOutput;
    first.samples = loudQuarter.data();
    REQUIRE(analyzer.feed(first) == std::nullopt);
    const auto silence = makeSilence(n);
    const auto analysis = analyzeWindow(analyzer, silence, 0U, fs, SpectrumDomainTag::ChainActiveF32);
    REQUIRE(analysis.domain == SpectrumDomainTag::ChainActiveF32);
    for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
      if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
        FAIL("域变化弃窗后静音窗期望全桶 floor @ bin " << i << "，实读 " << analysis.binsDb[i]);
      }
    }
    // 率/FFT 状态保留：域切换后再喂整窗强音 → 正常分析（标定语义不变）。
    const auto tone = makeTone(static_cast<double>(fs), 3000.0, 0.5, n);
    const auto after = analyzeWindow(analyzer, tone, 0U, fs, SpectrumDomainTag::ChainActiveF32);
    CHECK(std::fabs(binsPowerDb(after, 86U, 87U) - toneDb(0.5)) <= 0.3);
  }
}

TEST_CASE("eq spectrum: feed 语义——空帧无害 nullopt、整窗首产、大块多窗只回最近一份、meta 透传、reset 如新") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t n = 4096U;
  REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);

  // (a) 空帧守卫：nullptr / 0 帧 / 0 率 → nullopt 无害；随后合法喂帧正常产出。
  SpectrumAnalyzer analyzer;
  CHECK(analyzer.feed(SpectrumFeedFrame{}) == std::nullopt);
  SpectrumFeedFrame zeroFrame{};
  zeroFrame.sampleRate = fs;
  zeroFrame.frameCount = 0U;
  zeroFrame.samples = nullptr;
  CHECK(analyzer.feed(zeroFrame) == std::nullopt);
  SpectrumFeedFrame zeroRate{};
  const auto zeroRateSamples = makeSilence(64U);
  zeroRate.frameCount = 64U;
  zeroRate.samples = zeroRateSamples.data();  // 0 率早退：样本指针不被解引用
  CHECK(analyzer.feed(zeroRate) == std::nullopt);
  const auto firstTone = makeTone(static_cast<double>(fs), 1000.0, 1.0, n);
  const auto analysis = analyzeWindow(analyzer, firstTone, 7U, fs, SpectrumDomainTag::ChainActiveF32);
  REQUIRE(analysis.generation == 7U);
  REQUIRE(analysis.sampleRate == fs);
  REQUIRE(analysis.domain == SpectrumDomainTag::ChainActiveF32);

  // (b) 整窗帧首产：喂 n−1 帧 nullopt → 补 1 帧产首窗；逐桶 == 一次喂 n 帧。
  {
    SpectrumAnalyzer incremental;
    SpectrumAnalyzer oneShot;
    const auto tone = makeTone(static_cast<double>(fs), 1000.0, 1.0, n);
    SpectrumFeedFrame part{};
    part.sampleRate = fs;
    part.frameCount = n - 1U;
    part.samples = tone.data();
    REQUIRE(incremental.feed(part) == std::nullopt);
    SpectrumFeedFrame last{};
    last.sampleRate = fs;
    last.frameCount = 1U;
    last.samples = tone.data() + (n - 1U);
    const auto incAnalysis = requireAnalysis(incremental, last);
    const auto oneAnalysis = analyzeWindow(oneShot, tone, 0U, fs);
    requireBinsAllEqual(incAnalysis, oneAnalysis);
  }

  // (c) 大块多窗只回最近一份：一次喂 1.5n 帧 → 产 3 窗、返回第 3 窗（覆盖
  //     [n/2, 3n/2) 样本）；逐桶 == 全新实例直接喂该切片（位等证明窗口对准）。
  {
    SpectrumAnalyzer bulk;
    const auto tone = makeTone(static_cast<double>(fs), 1000.0, 1.0, n + n / 2U);
    SpectrumFeedFrame big{};
    big.sampleRate = fs;
    big.frameCount = n + n / 2U;
    big.samples = tone.data();
    const auto latest = requireAnalysis(bulk, big);
    const std::vector<float> slice(tone.begin() + static_cast<std::ptrdiff_t>(n / 2U),
                                   tone.begin() + static_cast<std::ptrdiff_t>(n / 2U + n));
    const auto reference = freshAnalysis(slice, 0U, fs);
    requireBinsAllEqual(latest, reference);
    // 语义锚：第 3 窗 = 纯 1kHz（跨 67/68 合计 ≈0dB，与首窗同为整窗音）。
    CHECK(std::fabs(binsPowerDb(latest, 67U, 68U)) <= 0.3);
  }

  // (d) reset 全清：累积半窗强音后 reset → 再喂静音整窗全桶 floor（无旧状态残留）。
  {
    SpectrumAnalyzer analyzer;
    const auto loudHalf = makeTone(static_cast<double>(fs), 3000.0, 1.0, n / 2U);
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 0U,
                                            .sampleRate = fs,
                                            .frameCount = n / 2U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = loudHalf.data()}) == std::nullopt);
    analyzer.reset();
    const auto silence = makeSilence(n);
    const auto analysis = analyzeWindow(analyzer, silence, 0U, fs);
    for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
      if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
        FAIL("reset 后静音窗期望全桶 floor @ bin " << i << "，实读 " << analysis.binsDb[i]);
      }
    }
  }
}

TEST_CASE("eq spectrum: fs<40k 奈奎斯特截断——22050 桶 110+ 静音 / 32000 桶 117+ 静音（内容可测桶对照）") {
  // 22050：可测桶 0..109（edge(110)=11246.9 > 11025）。5kHz 强音整落桶 95 正常显示，
  // 110..119 输出 kFloorDb。
  {
    constexpr std::uint32_t fs = 22050U;
    constexpr std::uint32_t n = 2048U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 5000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[95U])) <= 0.5);  // 可测区正常
    requireBinsFloor(analysis, 110U, 119U);                              // 奈奎斯特外恒静音标记
  }
  // 32000：可测桶 0..116（edge(117)=16826 > 16000）。12kHz 强音整落桶 111 正常显示
  // （k0=1536 主瓣整落，实测 −0.000），桶 117..119 输出 kFloorDb。
  {
    constexpr std::uint32_t fs = 32000U;
    constexpr std::uint32_t n = 4096U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 12000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[111U])) <= 0.5);  // bin111 可测有能量
    requireBinsFloor(analysis, 117U, 119U);                               // 奈奎斯特外恒静音标记
  }
}

TEST_CASE("eq spectrum: energy-preservation 不变量——白噪/多音 Σ可测桶功率 ≈ Σ参与 FFT bin 功率（44.1k/48k）") {
  // 范围重叠比例分摊的守恒锁：Σ_i 10^(binsDb[i]/10)（可测桶）= Σ_{参与 k} |X'_k|² / 1.5。
  // 期望侧由 Parseval 独立复算（见 participatingPowerDb，测试侧自带 DFT，不经 analyzer
  // FFT）——能抓分摊实现吞/造能量类回归；±0.1dB（f32 binsDb 量化下实测差 <0.01dB）。
  const std::array<std::uint32_t, 2> rates{44100U, 48000U};
  for (const std::uint32_t fs : rates) {
    CAPTURE(fs);
    const auto n = SpectrumAnalyzer::windowSizeForRate(fs);
    // 白噪窗（σ=0.5）两粒种子 + 多音窗（55/440/1000/8000Hz × 0.3）——覆盖宽带与
    // 离散混合输入。
    std::vector<float> multi(n, 0.0F);
    for (const double f0 : {55.0, 440.0, 1000.0, 8000.0}) {
      const auto part = makeTone(static_cast<double>(fs), f0, 0.3, n);
      for (std::size_t j = 0U; j < n; ++j) {
        multi[j] += part[j];
      }
    }
    const std::vector<std::vector<float>> windows = {makeWhiteNoise(n, 0xA11CEU),
                                                     makeWhiteNoise(n, 0xB0B5U), multi};
    for (const auto& window : windows) {
      SpectrumAnalyzer analyzer;
      const auto analysis = analyzeWindow(analyzer, window, 0U, fs);
      const double measured = measurablePowerDb(analysis, fs);
      const double expected = participatingPowerDb(window, fs);
      CAPTURE(measured);
      CAPTURE(expected);
      CHECK(std::fabs(measured - expected) <= 0.1);
    }
  }
}

TEST_CASE("eq spectrum: no-dead-bin 不变量——白噪可测桶无恒地板（44.1k/48k）；真静音全桶地板不变") {
  // 窄对数桶（20Hz 处宽 ~1.19Hz < bin 宽 Δf~10.8Hz）在比例分摊下由相邻 bin 按
  // 重叠比例共享 → 白噪下无桶恒地板。回退中心点整落会让 20-30Hz 多 bin 全落桶
  // 1/2、桶 0 恒死 → 本断言必 FAIL。判据用 −70dB 门限（实测最低桶 ≈ −53dB，
  // 17dB 以上裕量），地板 = −120 精确判据只留给真静音/截断尾。
  const std::array<std::uint32_t, 2> rates{44100U, 48000U};
  for (const std::uint32_t fs : rates) {
    CAPTURE(fs);
    const auto n = SpectrumAnalyzer::windowSizeForRate(fs);
    for (const std::uint32_t seed : {1U, 2U}) {
      const auto noise = makeWhiteNoise(n, seed);
      SpectrumAnalyzer analyzer;
      const auto analysis = analyzeWindow(analyzer, noise, 0U, fs);
      for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
        if (!SpectrumAnalyzer::binMeasurable(i, fs)) {
          CHECK(analysis.binsDb[i] == SpectrumAnalyzer::kFloorDb);  // 截断尾恒地板
          continue;
        }
        if (analysis.binsDb[i] <= -70.0F) {
          FAIL("白噪下可测桶不应接近地板 @ bin " << i << "，实读 " << analysis.binsDb[i]);
        }
      }
    }
  }
  // 真静音语义不变：可测桶同样 = kFloorDb（静音标记与"不可测"同值但语义区分在
  // 注释契约；此处锁定全 120 桶地板，含可测区）。
  {
    SpectrumAnalyzer analyzer;
    const auto silence = makeSilence(4096U);
    const auto analysis = analyzeWindow(analyzer, silence, 0U, 48000U);
    for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
      if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
        FAIL("静音窗期望全桶 kFloorDb @ bin " << i << "，实读 " << analysis.binsDb[i]);
      }
    }
  }
}

// ================= 任务 29 摘录下混（device renderCallback 真实路径） =================

namespace {

constexpr std::uint32_t kCaptureBlockFrames = 512U;
constexpr std::uint32_t kCaptureRingFrames = 16384U;

std::uint32_t bytesPerSample(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return 2U;
  case AudioSampleFormat::Int24:
    return 3U;
  case AudioSampleFormat::Int32:
  case AudioSampleFormat::Float32:
    return 4U;
  case AudioSampleFormat::Unknown:
    return 0U;
  }
  return 0U;
}

std::uint32_t bytesPerFrame(AudioSampleFormat format, std::uint16_t channels) {
  return bytesPerSample(format) * channels;
}

// 把 DC 电平（[-1,1]）量化成 int 内容样本（s24 = 24 位有符号值；f32 不经此路径）。
std::int32_t encodeDc(double value, AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return static_cast<std::int32_t>(std::llround(value * 32768.0));
  case AudioSampleFormat::Int24:
    return static_cast<std::int32_t>(std::llround(value * 8388608.0));
  case AudioSampleFormat::Int32:
    return static_cast<std::int32_t>(std::llround(value * 2147483648.0));
  case AudioSampleFormat::Float32:
  case AudioSampleFormat::Unknown:
    return 0;
  }
  return 0;
}

void writeSampleBytes(std::int32_t value, AudioSampleFormat format, std::uint8_t* dst) {
  switch (format) {
  case AudioSampleFormat::Int16: {
    const auto v = static_cast<std::int16_t>(value);
    std::memcpy(dst, &v, sizeof(v));
    return;
  }
  case AudioSampleFormat::Int24: {
    const auto raw = static_cast<std::uint32_t>(value) & 0xFFFFFFU;
    dst[0] = static_cast<std::uint8_t>(raw & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
    return;
  }
  case AudioSampleFormat::Int32: {
    std::memcpy(dst, &value, sizeof(value));
    return;
  }
  case AudioSampleFormat::Float32: {
    const float v = static_cast<float>(value);
    std::memcpy(dst, &v, sizeof(v));
    return;
  }
  case AudioSampleFormat::Unknown:
    return;
  }
}

// 内容样本 → double（任务 24 冻结读侧尺度：s16 /2^15、s24 左对齐 /2^31、s32 /2^31、
// f32 直取——与摘录下混 int→f32 同尺度，测试侧独立参考实现）。
double decodeContentSample(std::int32_t value, AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return static_cast<double>(value) / 32768.0;
  case AudioSampleFormat::Int24:
    return static_cast<double>(static_cast<std::int64_t>(value) << 8) / 2147483648.0;
  case AudioSampleFormat::Int32:
    return static_cast<double>(value) / 2147483648.0;
  case AudioSampleFormat::Float32: {
    const float v = static_cast<float>(value);
    return static_cast<double>(v);
  }
  case AudioSampleFormat::Unknown:
    return 0.0;
  }
  return 0.0;
}

// 帧内按声道序 0..N-1 固定累加 ×1/N 的 double 参考均值（与 device 侧语义一致；
// f32 内容域不经 encodeDc——写侧直写 float，参考即该 float 值）。
double meanContent(const std::vector<double>& dcPerChannel, AudioSampleFormat format) {
  double sum = 0.0;
  for (const double value : dcPerChannel) {
    if (format == AudioSampleFormat::Float32) {
      sum += static_cast<double>(static_cast<float>(value));
    } else {
      sum += decodeContentSample(encodeDc(value, format), format);
    }
  }
  return sum / static_cast<double>(dcPerChannel.size());
}

class FakeCaptureBackend final : public AudioOutputDeviceBackend {
public:
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() override {
    return {format};
  }

  [[nodiscard]] bool initialize(const AudioOutputDeviceOpenRequest& request) override {
    format.deviceId = request.config.preferredDeviceId;
    format.sampleRate = request.sampleRate;
    format.sampleFormat = request.sampleFormat;
    format.channelCount = request.channelCount;
    format.bufferFrames = request.bufferFrames;
    return true;
  }

  [[nodiscard]] bool start() override { return true; }
  [[nodiscard]] bool stop() override { return true; }
  void uninitialize() noexcept override {}

  [[nodiscard]] AudioDeviceFormat currentFormat() const override { return format; }

  AudioDeviceFormat format{.deviceId = "fake-device",
                           .deviceName = "Fake Device",
                           .backendName = "fake",
                           .sampleRate = 48000,
                           .sampleFormat = AudioSampleFormat::Float32,
                           .channelCount = 2,
                           .bufferFrames = 4,
                           .actualMode = AudioOutputMode::Mixed,
                           .supportedSampleFormats = {AudioSampleFormat::Float32},
                           .supportedSampleRates = {48000}};
};

// 单队列单设备最小装置（EQ 出厂关 → 链关闭态输出域摘录路径；volume 显式 1.0）。
struct CaptureRig {
  std::unique_ptr<PcmBufferQueue> queue;
  std::unique_ptr<FakeCaptureBackend> backend;
  AudioOutputDevice device;
  AudioSampleFormat format{AudioSampleFormat::Unknown};
  std::uint32_t rate{0};
  std::uint16_t channels{0};

  CaptureRig(AudioSampleFormat fmt, std::uint32_t sampleRate, std::uint16_t ch)
      : queue(std::make_unique<PcmBufferQueue>(
            PcmBufferQueueConfig{kCaptureRingFrames, bytesPerFrame(fmt, ch)})),
        backend(std::make_unique<FakeCaptureBackend>()),
        device(std::move(backend)),
        format(fmt),
        rate(sampleRate),
        channels(ch) {
    AudioOutputConfig config{};
    config.preferredDeviceId = "fake-device";
    REQUIRE(device.initialize(AudioOutputDeviceOpenRequest{.config = config,
                                                           .sampleFormat = fmt,
                                                           .sampleRate = sampleRate,
                                                           .channelCount = ch,
                                                           .bufferFrames = kCaptureBlockFrames,
                                                           .pcmQueue = queue.get()}));
    device.setVolume(1.0F);
  }

  // 写一块各声道 DC 内容并驱动一次真实 renderCallback（链关闭态 → 输出域摘录）。
  void renderDcBlock(const std::vector<double>& dcPerChannel) {
    REQUIRE(dcPerChannel.size() == static_cast<std::size_t>(channels));
    std::vector<std::uint8_t> block(static_cast<std::size_t>(kCaptureBlockFrames) *
                                    bytesPerFrame(format, channels));
    auto* const bytes = block.data();
    for (std::uint32_t frame = 0U; frame < kCaptureBlockFrames; ++frame) {
      for (std::uint16_t ch = 0U; ch < channels; ++ch) {
        auto* const dst = bytes + (static_cast<std::size_t>(frame) * channels + ch) *
                                      bytesPerSample(format);
        if (format == AudioSampleFormat::Float32) {
          const float value = static_cast<float>(dcPerChannel[ch]);
          std::memcpy(dst, &value, sizeof(value));
        } else {
          writeSampleBytes(encodeDc(dcPerChannel[ch], format), format, dst);
        }
      }
    }
    REQUIRE(queue->write(bytes, kCaptureBlockFrames));
    std::vector<std::uint8_t> out(block.size(), 0x5A);
    AudioOutputDevice::renderCallback(&device, out.data(), kCaptureBlockFrames);
  }

  // 读最新摘录帧（断言存在）；返回单声道 f32 样本。
  std::vector<float> readCapture(AudioOutputDeviceCaptureMeta& meta) {
    const auto capacity = device.captureCapacityFrames();
    REQUIRE(capacity >= kCaptureBlockFrames);
    std::vector<float> samples(capacity, 0.0F);
    REQUIRE(device.latestCaptureFrame(samples.data(), capacity, meta));
    return samples;
  }
};

}  // namespace

TEST_CASE("eq spectrum: 摘录下混均值（device renderCallback 路径）——latestCaptureFrame 单声道均值") {
  // 覆盖点：均值下混实现位于任务 29 device 侧（audio_output_device.cpp 匿名空间
  // captureDownmixIntToMono/captureDownmixS24ToMono/captureDownmixFloatToMono），经
  // 真实 renderCallback → 链关闭态输出域摘录（capturePublishOutputDomain，int 逐格式
  // 冻结读侧尺度 /2^15、/2^31、s24 左对齐 /2^31）发布。四格式 × N=2 断言均值
  // （L≠R 才能检出是否真做了 ×1/N 均值）；N=1 直取（无均值运算）；N=3 覆盖
  // invChannels 舍入路径。
  const std::array<AudioSampleFormat, 4> formats{AudioSampleFormat::Int16,
                                                 AudioSampleFormat::Int24,
                                                 AudioSampleFormat::Int32,
                                                 AudioSampleFormat::Float32};
  const std::vector<double> pair{0.5, -0.25};  // 期望均值 0.125（各格式内内容量化后复算）
  for (const AudioSampleFormat fmt : formats) {
    CAPTURE(static_cast<int>(fmt));
    CaptureRig rig(fmt, 48000U, 2U);
    CHECK(rig.device.captureCapacityFrames() >= kCaptureBlockFrames);  // 槽容量 = 主队列容量
    rig.renderDcBlock(pair);
    AudioOutputDeviceCaptureMeta meta;
    const auto samples = rig.readCapture(meta);
    CHECK(meta.generation == 1U);  // initialize 内 captureReset 纪元 ++（0→1）
    CHECK(meta.sequence == 1U);
    CHECK(meta.sampleRate == 48000U);
    CHECK(meta.domain == AudioOutputDeviceCaptureDomain::ChainInactiveOutput);
    CHECK(meta.frameCount == kCaptureBlockFrames);
    const double expected = meanContent(pair, fmt);
    bool allMatch = true;
    for (std::uint32_t frame = 0U; frame < kCaptureBlockFrames; ++frame) {
      if (std::fabs(static_cast<double>(samples[frame]) - expected) > 1e-5) {
        allMatch = false;
      }
    }
    CHECK(allMatch);

    // 第二块换 DC 对（0.25/0.75 → 均值 0.5）：sequence 单调推进、槽内为最新块。
    const std::vector<double> pair2{0.25, 0.75};
    rig.renderDcBlock(pair2);
    const auto samples2 = rig.readCapture(meta);
    CHECK(meta.sequence == 2U);
    const double expected2 = meanContent(pair2, fmt);
    bool allMatch2 = true;
    for (std::uint32_t frame = 0U; frame < kCaptureBlockFrames; ++frame) {
      if (std::fabs(static_cast<double>(samples2[frame]) - expected2) > 1e-5) {
        allMatch2 = false;
      }
    }
    CHECK(allMatch2);
  }

  // N=1 直取：单声道 s16 无均值运算（×scale 恒等路径），捕获 == 内容样本本身。
  {
    CaptureRig rig(AudioSampleFormat::Int16, 48000U, 1U);
    rig.renderDcBlock({0.25});
    AudioOutputDeviceCaptureMeta meta;
    const auto samples = rig.readCapture(meta);
    const double expected = meanContent({0.25}, AudioSampleFormat::Int16);  // = 0.25
    bool allMatch = true;
    for (std::uint32_t frame = 0U; frame < kCaptureBlockFrames; ++frame) {
      if (std::fabs(static_cast<double>(samples[frame]) - expected) > 1e-6) {
        allMatch = false;
      }
    }
    CHECK(allMatch);
    CHECK(meta.domain == AudioOutputDeviceCaptureDomain::ChainInactiveOutput);
  }

  // N=3：声道序 0..2 固定累加 ×1/3（invChannels 舍入路径），三通道 DC 各不同。
  {
    CaptureRig rig(AudioSampleFormat::Int16, 48000U, 3U);
    const std::vector<double> triple{0.5, -0.25, 0.25};  // 期望均值 0.5/3 ≈ 0.16667
    rig.renderDcBlock(triple);
    AudioOutputDeviceCaptureMeta meta;
    const auto samples = rig.readCapture(meta);
    const double expected = meanContent(triple, AudioSampleFormat::Int16);
    bool allMatch = true;
    for (std::uint32_t frame = 0U; frame < kCaptureBlockFrames; ++frame) {
      if (std::fabs(static_cast<double>(samples[frame]) - expected) > 1e-5) {
        allMatch = false;
      }
    }
    CHECK(allMatch);
  }
}

}  // namespace seriona::audio
