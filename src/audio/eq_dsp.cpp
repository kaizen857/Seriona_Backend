// 均衡器 DSP 纯核心实现（任务 22，B2.1）。行为契约见 eq_dsp.h 头注释。
// 实现要点：
//  - dB 域线性斜坡平滑：每块 stepRatio = min(1, blockMs / max(20ms, 3×blockMs))，
//    收敛精确置位（clamp 无残差），全收敛后 smoothingActive_ 关闭。
//  - 系数重算：每块开头对「将处理且 currentDb != appliedDb（或 coeffStale_）」
//    的 band 调 ma_peak2_reinit（miniaudio 语义：只换归一化系数、保留状态），
//    随后 ma_peak2_process_pcm_frames 原地处理 interleaved 块。
//  - 直通：未用 band（超出 mode 前缀）/越界 band/0dB 收敛 band 一律不调用 biquad；
//    全零稳态走 fastBypass_ 常量级直通（接入后出厂主路径逐位不变）。
//  - miniaudio 实现符号（MINIAUDIO_IMPLEMENTATION）由 miniaudio_output_device_
//    backend.cpp 单点提供，本 TU 只引用其声明。

#include "eq_dsp.h"

#include <algorithm>
#include <cmath>

namespace seriona::audio {

double EqualizerDspProcessor::sanitizeDb(float db) noexcept {
  // ±15dB 防御钳制；NaN/非有限按 0dB 处理（上游 reducer 已拒 NaN/越界，此处不
  // 信任上游——NaN 增益会经系数传播污染输出）。
  if (!std::isfinite(db)) {
    return 0.0;
  }
  return std::clamp(static_cast<double>(db), -kGainSanitizeDb, kGainSanitizeDb);
}

EqualizerDspProcessor::EqualizerDspProcessor() = default;

EqualizerDspProcessor::~EqualizerDspProcessor() {
  for (auto& band : bands_) {
    if (band.initialized) {
      ma_peak2_uninit(&band.filter, nullptr);
      band.initialized = false;
    }
  }
}

double EqualizerDspProcessor::bandCenterHz(std::size_t bandIndex) const noexcept {
  // 中心频率只对 mode 生效前缀有定义；未用 band 返回 0（不参与越界判定/系数）。
  if (mode_ == EqualizerBandMode::Band10) {
    return bandIndex < kEqualizer10BandCenterHz.size() ? kEqualizer10BandCenterHz[bandIndex] : 0.0;
  }
  return bandIndex < kEqualizer31BandCenterHz.size() ? kEqualizer31BandCenterHz[bandIndex] : 0.0;
}

double EqualizerDspProcessor::bandQ(std::size_t bandIndex) const noexcept {
  // Q 常量单点引用（equalizer_tables.h，任务 18；禁拷贝）。10/31 段各一常量，
  // 与 bandIndex 无关（表语义：整段共用 Q）。未用 band 返回值无意义（永不被消费）。
  static_cast<void>(bandIndex);
  return mode_ == EqualizerBandMode::Band10 ? kEqualizer10BandQ : kEqualizer31BandQ;
}

EqualizerDspProcessor::ConfigReport EqualizerDspProcessor::configure(const EqualizerConfig& config,
                                                                     std::uint32_t sampleRate,
                                                                     std::uint32_t channelCount) {
  ConfigReport report{};
  report.sampleRate = sampleRate;
  report.channelCount = channelCount;
  report.enabled = config.enabled;
  report.mode = config.mode;

  if (sampleRate == 0U || channelCount == 0U) {
    // 非法 DSP 域参数：保持既有已接受态不动，本次配置不接受（调用方应保留旧配置）。
    report.accepted = false;
    return report;
  }

  const bool rebuild = !accepted_ || sampleRate != sampleRate_ || channelCount != channelCount_;
  const bool modeChanged = accepted_ && config.mode != mode_;
  config_ = config;
  enabled_ = config.enabled;
  mode_ = config.mode;
  sampleRate_ = sampleRate;
  channelCount_ = channelCount;
  activeBandCount_ = config.mode == EqualizerBandMode::Band10 ? kEqualizer10BandCenterHz.size()
                                                              : kEqualizer31BandCenterHz.size();

  const double nyquistHz = 0.5 * static_cast<double>(sampleRate);
  std::size_t bypassCount = 0;
  for (std::size_t bandIndex = 0; bandIndex < kBandCount; ++bandIndex) {
    auto& band = bands_[bandIndex];
    const bool inUse = enabled_ && bandIndex < activeBandCount_;
    const double centerHz = bandCenterHz(bandIndex);
    // 越界判定：f_center ≥ 0.95×(fs/2)（configure 期一次判定，不每块刷）。
    band.rangeBypass = inUse && centerHz >= kNyquistBypassRatio * nyquistHz;
    if (band.rangeBypass) {
      ++bypassCount;
    }
    // 目标：启用且未越界 → 配置增益；否则 0dB（未用/越界/关闭统一直通目标）。
    const double newTargetDb =
        inUse && !band.rangeBypass ? sanitizeDb(config.bandGainsDb[bandIndex]) : 0.0;
    if (newTargetDb != band.targetDb) {
      // 目标变化：从当前点重新规划线性斜坡段（不跳变、不重走旧段）。
      band.segmentDeltaDb = newTargetDb - band.currentDb;
      band.targetDb = newTargetDb;
    }
    if (modeChanged) {
      // mode 切换改变中心频率/Q 表语义：系数参数失效，处理前强制 reinit。
      band.coeffStale = true;
    }
  }
  const double newPreGainTargetDb = enabled_ ? sanitizeDb(config.preGainDb) : 0.0;
  if (newPreGainTargetDb != preGainTargetDb_) {
    preGainSegmentDeltaDb_ = newPreGainTargetDb - preGainCurrentDb_;
    preGainTargetDb_ = newPreGainTargetDb;
  }

  if (rebuild) {
    // fs/声道数变化：ma_biquad 不允许 reinit 改声道数，滤波器组整体重建（平滑
    // 状态与目标保留；滤波历史清零）。init 失败（OOM）的 band 保持未初始化 →
    // process 按直通跳过（安全侧）。
    rebuildFilters();
  }

  accepted_ = true;
  smoothingActive_ = false;
  for (const auto& band : bands_) {
    if (band.currentDb != band.targetDb) {
      smoothingActive_ = true;
      break;
    }
  }
  if (preGainCurrentDb_ != preGainTargetDb_) {
    smoothingActive_ = true;
  }
  updateFastBypass();

  report.accepted = true;
  report.activeBandCount = activeBandCount_;
  report.nyquistBypassBandCount = bypassCount;
  return report;
}

void EqualizerDspProcessor::rebuildFilters() {
  for (std::size_t bandIndex = 0; bandIndex < kBandCount; ++bandIndex) {
    auto& band = bands_[bandIndex];
    if (band.initialized) {
      ma_peak2_uninit(&band.filter, nullptr);
      band.initialized = false;
    }
    const double centerHz = bandCenterHz(bandIndex);
    const ma_peak2_config filterConfig =
        ma_peak2_config_init(ma_format_f32, channelCount_, sampleRate_, 0.0 /* gainDB */,
                             bandQ(bandIndex), centerHz);
    if (ma_peak2_init(&filterConfig, nullptr, &band.filter) == MA_SUCCESS) {
      band.initialized = true;
    }
    band.appliedDb = 0.0;  // init 系数对应 0dB 增益；coeffStale 强制首次处理前 reinit
    band.coeffStale = true;
    // currentDb/targetDb 有意保留：fs 变化不断平滑（平滑状态跨重建存活）。
  }
}

double EqualizerDspProcessor::stepRatioForBlock(double blockMs) const noexcept {
  // 平滑时长 = max(20ms, 3×块长)；每块推进本段总位移的 blockMs/rampMs（恒 < 1，
  // 末块由 advanceOne 的 clamp 精确到位）。
  const double rampMs = std::max(kSmoothingMinMs, kSmoothingBlockMultiplier * blockMs);
  return std::min(1.0, blockMs / rampMs);
}

bool EqualizerDspProcessor::advanceOne(double& current,
                                       double target,
                                       double segmentDelta,
                                       double stepRatio) noexcept {
  if (current == target) {
    return false;  // 已收敛（收敛由下方 clamp 精确置位保证）
  }
  if (segmentDelta == 0.0) {
    // 段退化防御：目标恰好等于平滑中当前值（configure 边缘的浮点巧合）→ 一步到位。
    current = target;
    return false;
  }
  // 线性斜坡：每块推进本段总位移的固定比例，与剩余距离无关（恒定块长下为
  // 恒定步进）；末块越过目标即精确到位，无残差。
  const double next = current + segmentDelta * stepRatio;
  // 末块判定带浮点容差：数百块累积误差（~1e-12 dB 量级）不会挡住精确到位。
  constexpr double kConvergenceToleranceDb = 1e-9;
  if ((segmentDelta > 0.0 && next >= target - kConvergenceToleranceDb) ||
      (segmentDelta < 0.0 && next <= target + kConvergenceToleranceDb)) {
    current = target;
  } else {
    current = next;
  }
  return current != target;
}

void EqualizerDspProcessor::advanceSmoothing(double stepRatio) noexcept {
  smoothingActive_ = false;
  if (advanceOne(preGainCurrentDb_, preGainTargetDb_, preGainSegmentDeltaDb_, stepRatio)) {
    smoothingActive_ = true;
  }
  for (auto& band : bands_) {
    if (advanceOne(band.currentDb, band.targetDb, band.segmentDeltaDb, stepRatio)) {
      smoothingActive_ = true;
    }
  }
}

void EqualizerDspProcessor::updateFastBypass() noexcept {
  // 全零稳态判定：平滑全收敛且无任何非零处理项 → process 走常量级快路径。
  fastBypass_ = !smoothingActive_ && preGainCurrentDb_ == 0.0;
  if (fastBypass_) {
    for (const auto& band : bands_) {
      if (band.currentDb != 0.0 || (band.targetDb != 0.0 && !band.rangeBypass)) {
        fastBypass_ = false;
        break;
      }
    }
  }
}

void EqualizerDspProcessor::process(float* interleavedFrames, std::uint32_t frameCount) noexcept {
  if (interleavedFrames == nullptr || frameCount == 0U || !accepted_ || fastBypass_) {
    return;
  }

  const double blockMs = 1000.0 * static_cast<double>(frameCount) / static_cast<double>(sampleRate_);

  // 1) 平滑推进（每块一次；全收敛后跳过扫描，稳态块零额外成本）。
  if (smoothingActive_) {
    advanceSmoothing(stepRatioForBlock(blockMs));
    updateFastBypass();
    if (fastBypass_) {
      return;  // 平滑退出完成：整链旁路（enabled=false 的「先收敛再旁路」落点）
    }
  }

  // 2) 前置增益（dB 域平滑 → 线性标量）。0dB 直通（不触碰样本）。
  const double preGainDb = preGainCurrentDb_;
  if (preGainDb != 0.0) {
    const float scale = static_cast<float>(std::pow(10.0, preGainDb / 20.0));
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * channelCount_;
    auto* samples = interleavedFrames;
    for (std::size_t index = 0; index < sampleCount; ++index) {
      samples[index] *= scale;
    }
  }

  // 3) 逐 band peaking 串接（低频→高频；LTI 级联次序不影响结果）。
  for (std::size_t bandIndex = 0; bandIndex < kBandCount; ++bandIndex) {
    auto& band = bands_[bandIndex];
    const double currentDb = band.currentDb;
    // 直通条件含 mode 前缀守卫：mode 切换（31→10）后未用 band（idx ≥ 前缀）排空期
    // currentDb≠0 也绝不处理——centerHz=0 的系数会退化为 z=1 二重极点（状态线性
    // 增长爆炸，review 实测 44.98×）。排空只走平滑状态，不触碰 biquad。
    if (currentDb == 0.0 || band.rangeBypass || !band.initialized ||
        bandIndex >= activeBandCount_) {
      continue;  // 0dB 收敛 / 越界 / 未用(超出 mode 前缀) / 重建失败 → 硬直通
    }
    if (band.coeffStale || currentDb != band.appliedDb) {
      // 系数重算触发：每块开头 current 变化（平滑中每块变化 → 每块一次 reinit，
      // 保留滤波状态换系数）；fs/中心频率/Q 参数失效（coeffStale）同样强制。
      const ma_peak2_config filterConfig =
          ma_peak2_config_init(ma_format_f32, channelCount_, sampleRate_, currentDb,
                               bandQ(bandIndex), bandCenterHz(bandIndex));
      static_cast<void>(ma_peak2_reinit(&filterConfig, &band.filter));
      band.appliedDb = currentDb;
      band.coeffStale = false;
    }
    static_cast<void>(
        ma_peak2_process_pcm_frames(&band.filter, interleavedFrames, interleavedFrames, frameCount));
  }
}

bool EqualizerDspProcessor::fullySettled() const noexcept {
  if (smoothingActive_) {
    return false;
  }
  if (preGainCurrentDb_ != preGainTargetDb_) {
    return false;
  }
  for (const auto& band : bands_) {
    if (band.currentDb != band.targetDb) {
      return false;
    }
  }
  return true;
}

double EqualizerDspProcessor::currentBandGainDb(std::size_t bandIndex) const noexcept {
  return bandIndex < kBandCount ? bands_[bandIndex].currentDb : 0.0;
}

bool EqualizerDspProcessor::bandBypassed(std::size_t bandIndex) const noexcept {
  if (bandIndex >= kBandCount) {
    return true;
  }
  const auto& band = bands_[bandIndex];
  if (band.rangeBypass) {
    return true;  // 越界硬直通
  }
  if (bandIndex >= activeBandCount_) {
    return true;  // 超出 mode 生效前缀（未用 band）
  }
  // 0dB 收敛（当前与目标均为 0）→ 硬直通。目标非 0 的激活前瞬间（current==0、
  // 首个 process 块将推进）不属直通——平滑期按残余增益照常处理。
  return band.currentDb == 0.0 && band.targetDb == 0.0;
}

bool EqualizerDspProcessor::bandRangeBypassed(std::size_t bandIndex) const noexcept {
  return bandIndex < kBandCount && bands_[bandIndex].rangeBypass;
}

}  // namespace seriona::audio
