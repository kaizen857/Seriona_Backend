// 频谱分析纯核心 + 摘录下混联测（任务 31，B3.3 组 seriona.audio.eq_spectrum）。
//
// 被测面：src/audio/spectrum_analyzer.{h,cpp}（任务 30 纯组件：feed(SpectrumFeedFrame)
// → optional<SpectrumAnalysis>，60 对数桶 binsDb；头注释即行为契约）+ 任务 29 设备摘录
// 下混（captureDownmix* 位于 audio_output_device.cpp 匿名空间，经 renderCallback 真实
// 路径 + latestCaptureFrame 断言单声道均值——本文件自带最小 fake backend 装置，独立
// 编写，不依赖 eq_render_integration_tests.cpp 内部符号；两态中链关闭态输出域逐格式
// int→f32 冻结尺度下混覆盖 captureDownmixIntToMono（s16/s32）/captureDownmixS24ToMono/
// captureDownmixFloatToMono（f32）三分支 × N=1 直取 / N>1 均值）。
//
// 覆盖（计划书 B3.3 五项 + dB 标定数值锚）：
//   1. 已知正弦 bin 命中（review 实测锁定锚点，勿用旧笔误锚）：22050 3kHz→bin43、
//      5kHz→bin47；44.1k 1kHz→跨 33/34 桶界（edge(34)=1002.4Hz）合计 ≈0dB；
//      192k 10kHz→53/54 合计 ≈0dB。判据：落桶邻域功率合计 ≈ 10log10(1.5A²)；
//      主瓣整落单桶时取桶读数 ≈0dB±0.2（A=1）。
//   2. 采样率标定：windowSizeForRate 分档全边界；同频正弦不同 fs 落相同几何桶
//      （桶轴按实际 fs 生成，20Hz–20kHz 对数轴率无关）；跨率 feed 重建后输出
//      与全新实例逐桶一致（位等）。
//   3. 60 桶边界：binEdgeHz(0)=20/binEdgeHz(60)=20000 精确、边界单调；
//      binCenterHz 几何中心；binMeasurable/lastMeasurableBin 纯函数面；
//      fs<40k 截断行为：22050 → 桶 55+ 输出 kFloorDb、32000 → 桶 59 静音、
//      44100 → 全 60 桶可测。
//   4. 下混：见头注释（device 真实路径）。
//   5. 重建 generation 弃帧：同纪元喂部分窗→换 generation→新纪元首窗不含旧纪元
//      样本（逐桶与全新实例一致，位等）；域变化弃未满窗（构造会混窗的序列验证
//      不混——丢弃实例全桶 floor，同域对照有能量）。
//   dB 标定数值锚：满刻度正弦（A=1.0）主瓣整桶 ≈0dB；A=0.5 → −6.02dB；A=0.1 →
//      −20dB（±0.2dB）；静音输入全桶 −120。
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
#include <vector>

namespace seriona::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

// —— 正弦 / 窗口生成（测试侧独立实现，相位 double 递推）——

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

// 对数桶轴公式（测试侧独立参考）：edge(i) = 20×10^(3i/60)，center = 20×10^(3(i+0.5)/60)。
double edgeHzDouble(std::size_t i) { return 20.0 * std::pow(10.0, 3.0 * static_cast<double>(i) / 60.0); }
double centerHzDouble(std::size_t i) {
  return 20.0 * std::pow(10.0, 3.0 * (static_cast<double>(i) + 0.5) / 60.0);
}

}  // namespace

TEST_CASE("eq spectrum: 窗长分档全边界——windowSizeForRate 档位与 0 率守卫") {
  // 头契约：fs ≤ 24k→1024；24k<fs≤48k→2048；48k<fs≤96k→4096；fs>96k→8192。
  CHECK(SpectrumAnalyzer::windowSizeForRate(0U) == 0U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(22050U) == 1024U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(24000U) == 1024U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(24001U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(32000U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(44100U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(48000U) == 2048U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(48001U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(96000U) == 4096U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(96001U) == 8192U);
  CHECK(SpectrumAnalyzer::windowSizeForRate(192000U) == 8192U);
}

TEST_CASE("eq spectrum: 对数桶轴纯函数——端点/单调/几何中心/可测面（binMeasurable/lastMeasurableBin）") {
  // 端点精确：edge(0)=20、edge(60)=20000。
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
  // 超界入参钳制到 60（edge(61) == edge(60)）。
  CHECK(SpectrumAnalyzer::binEdgeHz(100U) == SpectrumAnalyzer::binEdgeHz(SpectrumAnalyzer::kBinCount));

  // 几何中心：位于 (edge(i), edge(i+1)) 且 ≈ 20×10^(3(i+0.5)/60)。
  for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
    const float center = SpectrumAnalyzer::binCenterHz(i);
    CHECK(center > SpectrumAnalyzer::binEdgeHz(i));
    CHECK(center < SpectrumAnalyzer::binEdgeHz(i + 1U));
    CHECK(std::fabs(static_cast<double>(center) - centerHzDouble(i)) <= 0.01);
  }

  // 可测面：binMeasurable = lower edge < fs/2−0.01；截断几何率无关桶轴（review 实测锚）。
  const std::array<std::pair<std::uint32_t, std::size_t>, 7> rateToLast = {{
      {22050U, 54U},  // edge(55)=11423 > 11025 → 桶 55+ 不可测
      {32000U, 58U},  // edge(59)=17825 > 16000 → 仅桶 59 不可测
      {40000U, 59U},  // edge(60)=20000 ≥ 20000−0.01 → 桶 59 可测、无 60 号桶
      {44100U, 60U},  // 全 60 桶可测（last= kBinCount 语义）
      {48000U, 60U},
      {96000U, 60U},
      {192000U, 60U},
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
  // 0 率守卫：binMeasurable false、lastMeasurableBin 返回 kBinCount（全不可测语义）。
  CHECK(SpectrumAnalyzer::binMeasurable(0U, 0U) == false);
  CHECK(SpectrumAnalyzer::lastMeasurableBin(0U) == SpectrumAnalyzer::kBinCount);
}

TEST_CASE("eq spectrum: 已知正弦落桶——22050 3kHz→bin43 主瓣整落 ≈0dB、5kHz→bin47+48 邻桶合计") {
  constexpr std::uint32_t fs = 22050U;
  constexpr std::uint32_t n = 1024U;
  REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);

  // 3kHz：对数轴 index = 60·log10(3000/20)/3 ≈ 43.5 → bin43（edge(43)=2825..edge(44)=3170）。
  // 窗内 k0 = 3000×1024/22050 ≈ 139.3，Hann 主瓣 ±2 bin 整落 bin43（131.2..147.3）→
  // 桶读数 ≈ 0 dB（A=1，review 实测 bin43 = 0.000 dB）。
  {
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 3000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    // 邻域 42..44 合计 ≈ 0 dB（判定建议口径），主瓣整落时桶读数本身 ≈0dB。
    CHECK(std::fabs(binsPowerDb(analysis, 42U, 44U)) <= 0.3);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[43U])) <= 0.2);
  }

  // 5kHz：index ≈ 47.96 → bin47（edge(47)=4477..edge(48)=5024）；k0 ≈ 232.2 主瓣尾
  // 越 bin48 上界（233.3）→ bin47 承载主体 + bin48 少量，47+48 合计 ≈ −6.02（A=0.5，
  // review 实测 bin47=−6.03、邻桶合计 −6.021）。
  {
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 5000.0, 0.5, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(binsPowerDb(analysis, 47U, 48U) - toneDb(0.5)) <= 0.3);
    CHECK(analysis.binsDb[47U] > -6.5F);
    CHECK(analysis.binsDb[47U] < -5.5F);
  }
}

TEST_CASE("eq spectrum: 跨桶界纯音——44.1k 1kHz 跨 33/34 合计 ≈0dB、192k 10kHz 跨 53/54 合计 ≈0dB") {
  // 1kHz 恰近桶界 edge(34)=1002.4Hz（index≈33.98）：主瓣按泄漏分跨 33/34，两边
  // 合计 = 总功率 ≈ 0 dB（A=1，review 实测 bin33=−2.63/bin34=−3.43、合计 −0.000）。
  {
    constexpr std::uint32_t fs = 44100U;
    constexpr std::uint32_t n = 2048U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    CHECK(SpectrumAnalyzer::binEdgeHz(34U) > 1000.0F);
    CHECK(SpectrumAnalyzer::binEdgeHz(34U) < 1010.0F);  // edge(34)=1002.4 锚
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 1000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(binsPowerDb(analysis, 33U, 34U)) <= 0.3);
    CHECK(analysis.binsDb[33U] < 0.0F);   // 分跨：单桶读数不足 0dB
    CHECK(analysis.binsDb[33U] > -5.0F);
    CHECK(analysis.binsDb[34U] < 0.0F);
    CHECK(analysis.binsDb[34U] > -6.0F);
  }

  // 10kHz：index≈53.98 → bin53 上缘（edge(54)=10023.7），主瓣尾少量落 bin54 →
  // 53+54 合计 ≈ 0 dB（A=1，review 实测 bin53=−0.21、邻桶合计 ≈0）。
  {
    constexpr std::uint32_t fs = 192000U;
    constexpr std::uint32_t n = 8192U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(fs, 10000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(binsPowerDb(analysis, 53U, 54U)) <= 0.5);
    CHECK(analysis.binsDb[53U] > -2.0F);  // 主体桶贴近 0dB（review −0.21）
    CHECK(analysis.binsDb[53U] <= 0.5F);
    CHECK(analysis.binsDb[54U] < -5.0F);  // 少量跨桶泄漏（≈−13dB 量级）
    CHECK(analysis.binsDb[54U] > -20.0F);
  }
}

TEST_CASE("eq spectrum: dB 标定数值锚——A=1→0dB / A=0.5→−6.02dB / A=0.1→−20dB（±0.2）与静音全桶 −120") {
  // 满刻度正弦（3kHz@22050，主瓣整落 bin43）：桶读数 = 10log10(功率/1.5)。
  for (const double amplitude : {1.0, 0.5, 0.1}) {
    CAPTURE(amplitude);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(22050.0, 3000.0, amplitude, 1024U);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, 22050U);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[43U]) - toneDb(amplitude)) <= 0.2);
    CHECK(std::fabs(binsPowerDb(analysis, 42U, 44U) - toneDb(amplitude)) <= 0.3);
  }

  // 静音输入：全 60 桶 == kFloorDb（−120.0F 逐桶精确）。
  SpectrumAnalyzer analyzer;
  const auto silence = makeSilence(1024U);
  const auto analysis = analyzeWindow(analyzer, silence, 0U, 22050U);
  for (std::size_t i = 0U; i < SpectrumAnalyzer::kBinCount; ++i) {
    if (analysis.binsDb[i] != SpectrumAnalyzer::kFloorDb) {
      FAIL("静音窗期望全桶 kFloorDb @ bin " << i << "，实读 " << analysis.binsDb[i]);
    }
  }
}

TEST_CASE("eq spectrum: 采样率标定——同频正弦跨 fs 落相同几何桶对（1kHz 恒跨 33/34 桶界）") {
  // 桶轴按实际 fs 生成：1kHz 在对数轴上恒为 index≈33.98（edge(34)=1002.4Hz 率无关），
  // 五档采样率各自按真实 Δf 解算 FFT bin → 能量恒落 {33,34} 几何桶对，合计 ≈0dB。
  // （若轴误按他率解析，例如把 22050 帧当 44100，1kHz 会落到 bin46 级——本用例即
  // 率标定失效判别。）
  const std::array<std::uint32_t, 5> rates{22050U, 44100U, 48000U, 96000U, 192000U};
  for (const std::uint32_t fs : rates) {
    CAPTURE(fs);
    const auto n = SpectrumAnalyzer::windowSizeForRate(fs);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 1000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    // 跨桶界对 {33,34} 承载全部能量（其余桶 ≤ 旁瓣 −18dB）。
    CHECK(binsPowerDb(analysis, 33U, 34U) > -0.7);
    CHECK(binsPowerDb(analysis, 33U, 34U) < 0.7);
    CHECK(analysis.binsDb[33U] < 0.0F);
    CHECK(analysis.binsDb[33U] > -5.0F);
    CHECK(analysis.binsDb[34U] < 0.0F);
    CHECK(analysis.binsDb[34U] > -6.0F);
    CHECK(analysis.binsDb[32U] <= -18.0F);
    CHECK(analysis.binsDb[35U] <= -18.0F);
  }
}

TEST_CASE("eq spectrum: 跨率/跨代重建弃帧——新纪元首窗与全新实例逐桶位等（无旧纪元样本混入）") {
  // (a) generation 变化（同率）：gen1 喂半窗强 3kHz 音（未满窗无产出）→ 换 gen2 喂
  //     整窗静音 → 全桶 floor。若旧纪元半窗样本未被丢弃，将与静音混窗产生能量。
  {
    SpectrumAnalyzer analyzer;
    const auto loudHalf = makeTone(44100.0, 3000.0, 1.0, 1024U);  // n/2 = 1024（n=2048）
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 1024U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = loudHalf.data()}) == std::nullopt);
    const auto silence = makeSilence(2048U);
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
    const auto oldHalf = makeTone(44100.0, 3000.0, 1.0, 1024U);
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 1024U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = oldHalf.data()}) == std::nullopt);
    const auto newWindow = makeTone(48000.0, 1000.0, 1.0, 2048U);
    const auto analysis = analyzeWindow(analyzer, newWindow, 2U, 48000U);
    REQUIRE(analysis.generation == 2U);
    REQUIRE(analysis.sampleRate == 48000U);
    const auto reference = freshAnalysis(newWindow, 2U, 48000U);
    requireBinsAllEqual(analysis, reference);
    // 语义锚：新纪元确以 48k 解析（1kHz 跨 33/34 而非按 44.1k 落到别桶）。
    CHECK(std::fabs(binsPowerDb(analysis, 33U, 34U)) <= 0.3);
  }

  // (c) 换代后新纪元首窗为强音：与全新实例逐桶位等（覆盖"非静音"路径的弃帧证明）。
  {
    SpectrumAnalyzer analyzer;
    const auto oldHalf = makeTone(44100.0, 5000.0, 1.0, 1024U);
    REQUIRE(analyzer.feed(SpectrumFeedFrame{.generation = 1U,
                                            .sampleRate = 44100U,
                                            .frameCount = 1024U,
                                            .domain = SpectrumDomainTag::ChainInactiveOutput,
                                            .samples = oldHalf.data()}) == std::nullopt);
    const auto newWindow = makeTone(44100.0, 3000.0, 0.5, 2048U);
    const auto analysis = analyzeWindow(analyzer, newWindow, 2U, 44100U);
    const auto reference = freshAnalysis(newWindow, 2U, 44100U);
    requireBinsAllEqual(analysis, reference);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[43U]) - toneDb(0.5)) <= 0.2);
  }
}

TEST_CASE("eq spectrum: 域变化弃未满窗——构造会混窗的序列验证不混（丢弃实例全桶 floor + 同域对照）") {
  // 契约：domain 变化仅弃未满窗累积（率/FFT 保留），单窗内样本恒同域。
  constexpr std::uint32_t fs = 44100U;
  constexpr std::uint32_t n = 2048U;

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
    for (std::size_t i = 30U; i <= 45U; ++i) {
      peak = std::max(peak, static_cast<double>(analysis.binsDb[i]));
    }
    CHECK(peak > -30.0);  // 强音混入窗 → 300Hz 邻域有能量
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
    CHECK(std::fabs(static_cast<double>(after.binsDb[43U]) - toneDb(0.5)) <= 0.2);
  }
}

TEST_CASE("eq spectrum: feed 语义——空帧无害 nullopt、整窗首产、大块多窗只回最近一份、meta 透传、reset 如新") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t n = 2048U;

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

  // (c) 大块多窗只回最近一份：一次喂 1.5n 帧 → 产 2 窗、返回第 2 窗（覆盖
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
    // 语义锚：第 2 窗 = 纯 1kHz（跨 33/34 合计 ≈0dB，与首窗同为整窗音）。
    CHECK(std::fabs(binsPowerDb(latest, 33U, 34U)) <= 0.3);
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

TEST_CASE("eq spectrum: fs<40k 奈奎斯特截断——22050 桶 55+ 静音 / 32000 桶 59 静音（内容可测桶对照）") {
  // 22050：可测桶 0..54（edge(55)=11423 > 11025）。5kHz 强音落 bin47 正常显示，
  // 55..59 输出 kFloorDb。
  {
    constexpr std::uint32_t fs = 22050U;
    constexpr std::uint32_t n = 1024U;
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 5000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(binsPowerDb(analysis, 47U, 48U)) <= 0.3);  // 可测区正常
    requireBinsFloor(analysis, 55U, 59U);                        // 奈奎斯特外恒静音标记
  }
  // 32000：可测桶 0..58（edge(59)=17825 > 16000）。12kHz 强音落 bin55 正常显示
  // （k0=768 主瓣整落），桶 59 输出 kFloorDb。
  {
    constexpr std::uint32_t fs = 32000U;
    constexpr std::uint32_t n = 2048U;
    REQUIRE(SpectrumAnalyzer::windowSizeForRate(fs) == n);
    SpectrumAnalyzer analyzer;
    const auto samples = makeTone(static_cast<double>(fs), 12000.0, 1.0, n);
    const auto analysis = analyzeWindow(analyzer, samples, 0U, fs);
    CHECK(std::fabs(static_cast<double>(analysis.binsDb[55U])) <= 0.5);  // bin55 可测有能量
    requireBinsFloor(analysis, 59U, 59U);                                 // 奈奎斯特外恒静音标记
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
