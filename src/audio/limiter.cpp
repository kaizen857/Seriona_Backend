// 限幅器 DSP 纯核心实现（任务 23，B2.2）。行为契约见 limiter.h 头注释。
// 实现要点：
//  - 检测器 = 每声道滑窗最大值（单调 deque：幅值降序 + 写入序号）。逐样本
//    维护：push 前从头部弹掉已过期候选（seq + D ≤ 新序号 ⇔ 已读出），从尾部
//    弹掉被新幅值支配的候选，再插入（|x|, seq）。max = 头部。空 = head==tail
//    （容量 D+1，live ≤ D 无头尾重合歧义）。
//  - 延迟线 = 每声道环形缓冲（容量 D）。读出样本 seq = 本次写入序号 − D，槽位
//    = (seq−1) mod D = 本次写槽（模 D 同余）→ 先读后写。
//  - 增益计算机 = dB 域逐样本指数（attack/release 方向系数），gainLin 每样本
//    转换一次；跨声道取各声道窗最大之最大驱动单一平滑器。
//  - 开关过渡为逐样本状态机：WarmUp（前 D 样本直通 + 填延迟线）→ Active（真
//    D 延迟）；disable → FadingOut（3ms 线性交叉淡化 f·限幅路径 + (1−f)·直通）
//    → Bypass（快速路径，不触碰样本）；Bypass 中再启用 → 重新 WarmUp。
//  - 零 miniaudio 依赖（与 eq_dsp 不同，本单元无 ma 符号，测试可直接链接本 TU）。

#include "limiter.h"

#include <algorithm>
#include <cmath>

namespace seriona::audio {

namespace {
// 增益/检测处理所需的最小非零延迟样本数（fs 极低时的防御下限）。
constexpr std::uint32_t kMinLookaheadSamples = 1;
}  // namespace

LimiterDspProcessor::LimiterDspProcessor() = default;

LimiterDspProcessor::~LimiterDspProcessor() = default;

LimiterDspProcessor::ConfigReport LimiterDspProcessor::configure(
    const EqualizerConfig& config, std::uint32_t sampleRate, std::uint32_t channelCount) {
  ConfigReport report{};
  report.sampleRate = sampleRate;
  report.channelCount = channelCount;
  report.enabled = config.limiterEnabled;

  if (sampleRate == 0U || channelCount == 0U) {
    // 非法 DSP 域参数：保持既有已接受态不动，本次配置不接受（调用方应保留旧配置）。
    report.accepted = false;
    return report;
  }

  // 实际延迟样本数：名义 rate×5ms，fs > 192k 封顶 192k 换算（384k → 960 样本
  // = 2.5ms 实际，规格要求按封顶换算声明实际 lookahead）。
  const double cappedRate =
      std::min(kLookaheadCapRateHz, static_cast<double>(sampleRate));
  const std::uint32_t lookahead =
      std::max(kMinLookaheadSamples,
               static_cast<std::uint32_t>(
                   std::lround(cappedRate * kLookaheadMs / 1000.0)));

  const bool rebuild = !accepted_ || sampleRate != sampleRate_ ||
                       channelCount != channelCount_ || lookahead != lookaheadSamples_;
  config_ = config;
  enabled_ = config.limiterEnabled;
  sampleRate_ = sampleRate;
  channelCount_ = channelCount;
  lookaheadSamples_ = lookahead;

  // 平滑系数：逐样本指数 1 - exp(-1/(τ·fs))，按新 fs 重算（与块大小无关）。
  attackCoeff_ = 1.0 - std::exp(-1.0 / (kAttackMs / 1000.0 * static_cast<double>(sampleRate)));
  releaseCoeff_ = 1.0 - std::exp(-1.0 / (kReleaseMs / 1000.0 * static_cast<double>(sampleRate)));

  if (rebuild) {
    // fs/声道数/D 变化：全状态重建（延迟线/检测/增益清零——设备重建语境；
    // 开关目标态 enabled_ 已随本次 configure 生效，下次 process 按需 WarmUp）。
    channels_.clear();
    channels_.resize(channelCount);
    for (auto& ch : channels_) {
      ch.ring.assign(lookaheadSamples_, 0.0f);
      // deque 数组容量 D+1：环形 head==tail 只表示空，live ≤ D < 容量无冲突。
      ch.window.mags.assign(lookaheadSamples_ + 1U, 0.0f);
      ch.window.seqs.assign(lookaheadSamples_ + 1U, 0U);
      ch.window.head = 0U;
      ch.window.tail = 0U;
    }
    activity_ = Activity::Bypass;
    writeCount_ = 0U;
    fadeRemaining_ = 0U;
    fadeTotal_ = 0U;
    currentGainDb_ = 0.0;
    targetGainDb_ = 0.0;
  }

  accepted_ = true;
  report.accepted = true;
  report.lookaheadSamples = lookaheadSamples_;
  report.actualLookaheadMs =
      1000.0 * static_cast<double>(lookaheadSamples_) / static_cast<double>(sampleRate);
  return report;
}

void LimiterDspProcessor::resetEngaged() {
  for (auto& ch : channels_) {
    std::fill(ch.ring.begin(), ch.ring.end(), 0.0f);
    ch.window.head = 0U;
    ch.window.tail = 0U;
  }
  writeCount_ = 0U;
  fadeRemaining_ = 0U;
  fadeTotal_ = 0U;
  currentGainDb_ = 0.0;
  targetGainDb_ = 0.0;
}

void LimiterDspProcessor::process(float* interleavedFrames,
                                  std::uint32_t frameCount) noexcept {
  if (interleavedFrames == nullptr || frameCount == 0U || !accepted_) {
    return;
  }

  // 入口状态迁移（configure 只置开关目标态，过渡在此逐样本执行）：
  if (activity_ == Activity::Bypass) {
    if (!enabled_) {
      return;  // 旁路快速路径：不触碰样本（限幅关闭态与改动前逐位一致）
    }
    resetEngaged();  // 关→开：预热直通起点（延迟线/检测从空开始）
    activity_ = Activity::WarmUp;
  } else if (activity_ == Activity::FadingOut) {
    if (enabled_) {
      // 淡化中途再启用：淡化期间延迟线/检测保活 → 无缝回退到原处理路径。
      activity_ = writeCount_ >= lookaheadSamples_ ? Activity::Active : Activity::WarmUp;
    }
  } else if (!enabled_) {
    // 开→关：进入 3ms 线性交叉淡化（fadeTotal_ 由 fs 换算，居中规格 2-5ms）。
    fadeTotal_ = fadeRemaining_ = static_cast<std::uint32_t>(std::max(
        1.0, static_cast<double>(std::lround(
                 static_cast<double>(sampleRate_) * kFadeOutMs / 1000.0))));
    activity_ = Activity::FadingOut;
  }

  const std::uint32_t channelCount = channelCount_;
  const std::uint32_t delay = lookaheadSamples_;
  const std::uint32_t dequeCap = delay + 1U;
  const double thresholdLinValue = thresholdLin();

  std::uint32_t frame = 0U;
  while (frame < frameCount) {
    // 预热完成判定（帧粒度，writeCount_ 为帧域）：已写满 D 帧 → 接真延迟路径。
    if (activity_ == Activity::WarmUp && writeCount_ >= delay) {
      activity_ = Activity::Active;
    }

    // ---- pass1（逐声道）：读出延迟样本 + 滑窗更新 + 延迟线写入 ----
    // 读出样本 seq = 本次写入序号 − D，槽位与本次写槽模 D 同余 → 先读后写。
    const bool canRead = writeCount_ >= delay;  // Active/FadingOut 恒成立
    const std::uint64_t readSlot = writeCount_ % delay;
    const std::uint64_t newSeq = writeCount_ + 1U;  // 本次写入序号（帧域，64 位防回绕）
    float windowMax = 0.0f;
    for (std::uint32_t c = 0U; c < channelCount; ++c) {
      auto& ch = channels_[c];
      const float sample = interleavedFrames[frame * channelCount + c];

      // 1) 延迟读出（先于写——槽位冲突）；WarmUp 期无读出（直通基准 = 原样本）。
      //    基准样本暂存输出槽，pass2 应用共享增益（免分配跨声道暂存）。
      interleavedFrames[frame * channelCount + c] =
          canRead ? ch.ring[readSlot] : sample;

      // 2) 滑窗最大值更新：过期（seq + D ≤ newSeq）+ 支配弹出（≤ 新幅值）+ 插入。
      auto& wm = ch.window;
      while (wm.head != wm.tail) {
        const std::uint32_t front = wm.head;
        if (wm.seqs[front] + delay > newSeq) {
          break;  // 头部未过期（头部是窗内最旧者，其后皆更新）
        }
        wm.head = (front + 1U) % dequeCap;
      }
      const float mag = std::fabs(sample);
      while (wm.head != wm.tail) {
        const std::uint32_t back = (wm.tail + dequeCap - 1U) % dequeCap;
        if (wm.mags[back] > mag) {
          break;  // 尾部幅值仍大于新样本：保留（单调递减不变量成立）
        }
        wm.tail = back;
      }
      wm.mags[wm.tail] = mag;
      wm.seqs[wm.tail] = newSeq;
      wm.tail = (wm.tail + 1U) % dequeCap;

      // 3) 延迟线写入（槽位 = (newSeq−1) mod D；已先读出，安全覆写）。
      ch.ring[writeCount_ % delay] = sample;

      // 跨声道取最大（检测取全声道最大——单平滑器共享驱动的输入面）。
      if (wm.head != wm.tail && wm.mags[wm.head] > windowMax) {
        windowMax = wm.mags[wm.head];
      }
    }
    ++writeCount_;

    // ---- 增益计算机（逐样本，dB 域）----
    double target = 0.0;
    if (static_cast<double>(windowMax) > thresholdLinValue) {
      target = 20.0 * std::log10(thresholdLinValue / static_cast<double>(windowMax));
      if (target > 0.0) {
        target = 0.0;  // 防御（windowMax ≤ 1 时恒负；此处保 min(0,·) 语义）
      }
    }
    const double coeff = target < currentGainDb_ ? attackCoeff_
                         : target > currentGainDb_ ? releaseCoeff_
                                                   : 0.0;
    if (coeff > 0.0) {
      currentGainDb_ += (target - currentGainDb_) * coeff;
      if (std::fabs(target - currentGainDb_) < kConvergenceToleranceDb) {
        currentGainDb_ = target;  // 收敛精确置位（无残差，settled 可测）
      }
    }
    targetGainDb_ = target;
    const float gain = currentGainDb_ >= -kUnityDb
                           ? 1.0f
                           : static_cast<float>(std::pow(10.0, currentGainDb_ / 20.0));

    // ---- pass2（逐声道）：共享增益应用 / 淡化混合 ----
    if (activity_ == Activity::FadingOut) {
      // 开→关交叉淡化：f: 1→0，f·(限幅路径) + (1−f)·直通；淡毕全旁路。
      const float f = static_cast<float>(static_cast<double>(fadeRemaining_) /
                                         static_cast<double>(fadeTotal_));
      const float direct = 1.0f - f;
      for (std::uint32_t c = 0U; c < channelCount; ++c) {
        const float limited = interleavedFrames[frame * channelCount + c] * gain;
        // 直通路径样本 = 刚写入延迟线的原样本（槽位 = (writeCount_−1) mod D）。
        const float bypassSample = channels_[c].ring[(writeCount_ - 1U) % delay];
        interleavedFrames[frame * channelCount + c] = f * limited + direct * bypassSample;
      }
      if (--fadeRemaining_ == 0U) {
        // 淡化完成：冻结处理状态（增益归零使 fullySettled 在旁路态恒真）。
        activity_ = Activity::Bypass;
        currentGainDb_ = 0.0;
        targetGainDb_ = 0.0;
        ++frame;  // 本帧已写完；余下帧保持原样（快速路径）
        break;
      }
    } else {
      for (std::uint32_t c = 0U; c < channelCount; ++c) {
        interleavedFrames[frame * channelCount + c] *= gain;
      }
    }
    ++frame;
  }
}

bool LimiterDspProcessor::fullySettled() const noexcept {
  if (activity_ == Activity::WarmUp || activity_ == Activity::FadingOut) {
    return false;  // 过渡中不视为收敛
  }
  // 增益相对当前窗目标收敛（旁路态两者恒为 0）。稳态压限时窗最大随内容微动，
  // target 有小幅扰动 → 用 kSettledBandDb 带宽判定（±0.05dB 内即收敛）。
  return std::fabs(currentGainDb_ - targetGainDb_) <= kSettledBandDb;
}

}  // namespace seriona::audio
