// 频谱分析纯核心实现（任务 30，B3.2）。行为契约见 spectrum_analyzer.h 头注释。

#include "spectrum_analyzer.h"

// 本机 FFmpeg 头（n9.0.1 系）无 extern "C" 包裹（b0-2 探测同源事实），C++ TU 引用
// libav* 符号须手动包 extern "C"——仓库既有模式（ffmpeg_audio_source.cpp 等）。
extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace seriona::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLogLower = 20.0;    // 对数轴下界（Hz）
constexpr double kLogUpper = 20000.0; // 对数轴上界（Hz）
constexpr double kLogDecades = 3.0;   // log10(1000)：桶边界 = 20×10^(3i/60)

// 满刻度正弦参考功率（dB 0 基准）：纯正弦实谱（正频率半边）总功率 =
// Σ_k∈1..n/2−1 |X'_k|² = 1.5×A² —— 当主瓣整落单个对数桶时该桶读数 ≈ 0 dB。
// 推导：Parseval Σ_k |X_k|² = n·Σ_j (w_j·x_j)²，周期 Hann Σw = n/2、Σw² = 3n/8，
// X'_k = 2·X_k/Σw，实谱正频率半边占总谱一半；A = 1 → Σ|X'|² = 1.5。
constexpr double kSineRefPower = 1.5;

}  // namespace

struct SpectrumAnalyzer::Impl {
  // —— 输入元数据状态（失效重建键）——
  bool valid = false;              // 已见过合法帧（元数据已建立）
  std::uint32_t generation = 0;    // 当前分析纪元（透传帧值）
  std::uint32_t sampleRate = 0;    // 当前采样率（重建键）
  SpectrumDomainTag domain{SpectrumDomainTag::ChainInactiveOutput};

  // —— 窗累积（按 headOffset 消费，避免每次整窗反复 erase）——
  std::vector<float> windowed{};   // 累积的原始样本（单声道 f32）
  std::size_t headOffset = 0;      // 已消费前缀样本数

  // —— FFT / 窗状态（按 sampleRate 档重建）——
  std::uint32_t n = 0;             // FFT 点数（2 的幂）
  std::uint32_t hop = 0;           // 重叠推进（= n/2，50% 重叠）
  std::vector<float> hann{};       // 周期 Hann 窗（长度 n，预计算）
  double windowSum = 0.0;          // Σw（周期 Hann = n/2）
  AVTXContext* fftContext = nullptr;
  av_tx_fn fftFn = nullptr;
  float* fftIn = nullptr;          // av_calloc：CPU 最大对齐（无需 AV_TX_UNALIGNED）
  float* fftOut = nullptr;         // n/2+1 个复数 = n+2 个 float

  ~Impl() { teardownFft(); }

  void teardownFft() {
    if (fftContext != nullptr) {
      av_tx_uninit(&fftContext);
    }
    av_free(fftIn);
    av_free(fftOut);
    fftContext = nullptr;
    fftFn = nullptr;
    fftIn = nullptr;
    fftOut = nullptr;
  }

  // 按采样率重建全部率相关状态（调用方保证 sampleRate > 0）。av_tx_init 负返回
  // 或分配失败时回到未配置态（n = 0）：后续 feed 静默跳过，不丢纪元——下帧同率
  // 重试重建。worker 线程调用（非实时回调路径），允许分配。
  void rebuild(std::uint32_t sampleRate) {
    teardownFft();
    windowed.clear();
    headOffset = 0;
    hann.clear();
    n = 0;
    hop = 0;
    windowSum = 0.0;

    const auto size = SpectrumAnalyzer::windowSizeForRate(sampleRate);
    if (size == 0U) {
      return;
    }
    n = size;
    hop = n / 2U;

    hann.resize(n);
    for (std::uint32_t j = 0; j < n; ++j) {
      // 周期 Hann（FFT 友好：w[j] = 0.5 − 0.5·cos(2πj/n)，窗首尾不重复采样点）
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * j / n);
      hann[j] = static_cast<float>(w);
      windowSum += w;
    }

    fftIn = static_cast<float*>(av_calloc(n, sizeof(float)));
    fftOut = static_cast<float*>(av_calloc(static_cast<std::size_t>(n) + 2U, sizeof(float)));
    const float scale = 1.0F;  // AV_TX_FLOAT_RDFT 的 scale 类型为 float（非 double）
    if (fftIn == nullptr || fftOut == nullptr ||
        av_tx_init(&fftContext, &fftFn, AV_TX_FLOAT_RDFT, /*inv=*/0, static_cast<int>(n), &scale,
                   /*flags=*/0) < 0) {
      teardownFft();
      n = 0;
      hop = 0;
      windowSum = 0.0;
    }
  }

  // 追加帧样本；headOffset 越过紧凑化阈值时物理移除已消费前缀（均摊 O(1)）。
  void append(const float* samples, std::uint32_t frameCount) {
    if (headOffset > 0 &&
        (headOffset >= windowed.size() || headOffset > static_cast<std::size_t>(n) * 4U)) {
      windowed.erase(windowed.begin(), windowed.begin() + static_cast<std::ptrdiff_t>(headOffset));
      headOffset = 0;
    }
    windowed.insert(windowed.end(), samples, samples + frameCount);
  }

  [[nodiscard]] bool windowReady() const { return windowed.size() - headOffset >= n; }
  [[nodiscard]] const float* windowStart() const { return windowed.data() + headOffset; }
  void consumeHop() { headOffset += hop; }
};

SpectrumAnalyzer::SpectrumAnalyzer() : impl_(new Impl()) {}

SpectrumAnalyzer::~SpectrumAnalyzer() { delete impl_; }

std::uint32_t SpectrumAnalyzer::windowSizeForRate(std::uint32_t sampleRate) noexcept {
  if (sampleRate == 0U) {
    return 0U;
  }
  if (sampleRate <= 24000U) {
    return 1024U;  // 22050 → Δf = 21.5 Hz
  }
  if (sampleRate <= 48000U) {
    return 2048U;  // 44100 → 21.5 Hz；48000 → 23.4 Hz
  }
  if (sampleRate <= 96000U) {
    return 4096U;  // 96000 → 23.4 Hz
  }
  return 8192U;    // 192000 → 23.4 Hz（长窗档：低频分辨率与 44.1k 族一致）
}

float SpectrumAnalyzer::binEdgeHz(std::size_t binIndex) noexcept {
  // 桶 i 边界 f_i = 20×1000^(i/60) = 20×10^(3i/60)，i = 0..60（f_60 = 20000）。
  if (binIndex > kBinCount) {
    binIndex = kBinCount;
  }
  const double exponent = kLogDecades * static_cast<double>(binIndex) / static_cast<double>(kBinCount);
  return static_cast<float>(kLogLower * std::pow(10.0, exponent));
}

float SpectrumAnalyzer::binCenterHz(std::size_t binIndex) noexcept {
  // 桶几何中心 = 20×1000^((i+0.5)/60)（对数等分桶的中点，测试正弦落桶判定面）。
  const double edgeExponent =
      kLogDecades * (static_cast<double>(binIndex) + 0.5) / static_cast<double>(kBinCount);
  return static_cast<float>(kLogLower * std::pow(10.0, edgeExponent));
}

bool SpectrumAnalyzer::binMeasurable(std::size_t binIndex, std::uint32_t sampleRate) noexcept {
  if (sampleRate == 0U || binIndex >= kBinCount) {
    return false;
  }
  // 桶轴按 fs/2 截断：整桶下界 ≥ 奈奎斯特的桶不可测（无能量、置静音标记）。
  // 下界判定带 0.01 Hz 容差吸收浮点误差（32-bit fs 的真实边界远离该尺度）。
  return binEdgeHz(binIndex) < static_cast<float>(static_cast<double>(sampleRate) * 0.5 - 0.01);
}

std::size_t SpectrumAnalyzer::lastMeasurableBin(std::uint32_t sampleRate) noexcept {
  if (sampleRate == 0U) {
    return kBinCount;
  }
  const double nyquist = static_cast<double>(sampleRate) * 0.5 - 0.01;
  if (nyquist <= kLogLower) {
    return kBinCount;  // 防御：fs ≤ 40 Hz 全桶不可测
  }
  // 理论值：最大的 i 使 20×10^(3i/60) < nyquist → 3i/60 < log10(nyquist/20)
  double index = 60.0 * std::log10(nyquist / kLogLower) / kLogDecades;
  std::size_t i = static_cast<std::size_t>(std::clamp(index, 0.0, static_cast<double>(kBinCount)));
  // 浮点边界复核：从理论值逐桶向下找第一个 edge < nyquist 的桶
  while (i > 0U && binEdgeHz(i) >= static_cast<float>(nyquist)) {
    --i;
  }
  if (i == 0U && binEdgeHz(0) >= static_cast<float>(nyquist)) {
    return kBinCount;
  }
  return i;
}

std::optional<SpectrumAnalysis> SpectrumAnalyzer::feed(const SpectrumFeedFrame& frame) noexcept {
  if (frame.samples == nullptr || frame.frameCount == 0U || frame.sampleRate == 0U) {
    return std::nullopt;  // 空帧/未配置率：无害空操作
  }

  // —— 失效重建门控 ——
  if (!impl_->valid) {
    impl_->rebuild(frame.sampleRate);
    impl_->valid = true;
    impl_->generation = frame.generation;
    impl_->sampleRate = frame.sampleRate;
    impl_->domain = frame.domain;
  } else if (frame.generation != impl_->generation || frame.sampleRate != impl_->sampleRate) {
    // 设备重建/换率：丢弃全部跨代 FFT 窗/累积状态，从新纪元重建
    impl_->rebuild(frame.sampleRate);
    impl_->generation = frame.generation;
    impl_->sampleRate = frame.sampleRate;
    impl_->domain = frame.domain;
  } else if (frame.domain != impl_->domain) {
    // 域变化：弃未满窗累积（防跨域电平混窗）；率/FFT 状态保留，标定语义不变
    impl_->windowed.clear();
    impl_->headOffset = 0;
    impl_->domain = frame.domain;
  }
  if (impl_->n == 0U) {
    return std::nullopt;  // FFT 上下文不可用（分配/init 失败，n=0）：跳过本帧；重建仅随纪元键（generation/sampleRate）变化触发，稳态下保持零产出直到纪元推进或 reset()
  }

  impl_->append(frame.samples, frame.frameCount);

  // 每消耗 hop（= n/2，50% 重叠）帧产出一份分析；feed 内可能命中多窗
  // （大块喂入），只返回最近一份（内部状态照常推进）。
  std::optional<SpectrumAnalysis> latest;
  while (impl_->windowReady()) {
    SpectrumAnalysis analysis;
    analysis.generation = impl_->generation;
    analysis.sampleRate = impl_->sampleRate;
    analysis.domain = impl_->domain;

    const float* const start = impl_->windowStart();
    const auto n = impl_->n;
    const double windowSum = impl_->windowSum;

    // Hann 加窗 → 前向 RDFT（非归一 DFT；输出 n/2+1 个复数，复排 out[2k]/out[2k+1]）
    for (std::uint32_t j = 0; j < n; ++j) {
      impl_->fftIn[j] = start[j] * impl_->hann[j];
    }
    impl_->fftFn(impl_->fftContext, impl_->fftOut, impl_->fftIn, sizeof(float));

    // 相干增益归一：X'_k = 2·X_k/Σw（Σw = n/2）→ 窗内满刻度正弦峰值幅度 ≈ A。
    const double norm = 2.0 / windowSum;

    // 桶能量求和：k×fs/n ∈ [f_i, f_{i+1}) 的全部正频率 FFT bin（k = 1..n/2−1；
    // DC 与奈奎斯特点不参与——f = 0 与 f = fs/2 不在轴内；fs < 40k 时天然只对
    // fs/2 截断轴内的部分求和）。
    const double fs = static_cast<double>(impl_->sampleRate);
    std::array<double, kBinCount> powers{};
    for (std::uint32_t k = 1; k < n / 2U; ++k) {
      const double freq = fs * static_cast<double>(k) / static_cast<double>(n);
      if (freq < kLogLower || freq >= kLogUpper) {
        continue;  // 对数轴外（20 Hz 以下 / 20 kHz 及以上）
      }
      // i(f) = floor(60·log10(f/20)/3)；双精度索引计算 + 边界复核
      const double index = 60.0 * std::log10(freq / kLogLower) / kLogDecades;
      if (index < 0.0) {
        continue;
      }
      std::size_t i = static_cast<std::size_t>(index);
      if (i >= kBinCount) {
        continue;  // ≥ 20 kHz（f_60 = 20000，双精度边界已含）
      }
      // 浮点边界：index 恰为整数的极窄情形归入下桶（freq ≥ edge(i+1) 时落 i+1，
      // 由下一轮自然处理——floor 语义即 [edge_i, edge_{i+1}) 左闭右开）
      if (freq >= static_cast<double>(binEdgeHz(i + 1U))) {
        ++i;
        if (i >= kBinCount) {
          continue;
        }
      }
      const double re = static_cast<double>(impl_->fftOut[2U * k]) * norm;
      const double im = static_cast<double>(impl_->fftOut[2U * k + 1U]) * norm;
      powers[i] += re * re + im * im;
    }

    for (std::size_t i = 0; i < kBinCount; ++i) {
      if (!binMeasurable(i, impl_->sampleRate)) {
        analysis.binsDb[i] = kFloorDb;  // 奈奎斯特截断外的桶：静音标记
        continue;
      }
      if (powers[i] <= 0.0) {
        analysis.binsDb[i] = kFloorDb;  // 纯静音：静音标记
        continue;
      }
      // dB = 10·log10(功率 / 满刻度正弦参考功率 1.5)：满刻度纯音主瓣整落桶内 ≈ 0 dB
      const double db = 10.0 * std::log10(powers[i] / kSineRefPower);
      analysis.binsDb[i] = static_cast<float>(std::clamp(db, static_cast<double>(kFloorDb), 60.0));
    }

    latest = analysis;
    impl_->consumeHop();
  }

  // 尾部紧凑化兜底：全部样本已消费完时清空累积（防长会话内存滞留）
  if (impl_->headOffset >= impl_->windowed.size()) {
    impl_->windowed.clear();
    impl_->headOffset = 0;
  }
  return latest;
}

void SpectrumAnalyzer::reset() noexcept {
  impl_->valid = false;
  impl_->generation = 0;
  impl_->sampleRate = 0;
  impl_->domain = SpectrumDomainTag::ChainInactiveOutput;
  impl_->rebuild(0U);  // teardown + 清累积；下次 feed 按帧率重建
}

}  // namespace seriona::audio
