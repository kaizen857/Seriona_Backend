// 限幅器 DSP 核心的帧级单测（任务 27，B2.5 组 seriona.audio.eq_limiter）。
//
// 被测面：src/audio/limiter.{h,cpp}（纯 C++23，零 miniaudio 依赖——测试目标只编
// 入 limiter.cpp 即可）。limiter.h 头注释即行为契约，全部断言基于源码语义：
//
//   - 过阈稳态顶：DC 满载持续输入下输出收敛于阈值 10^(-0.5/20)≈0.94406（-0.5dBFS），
//     稳态峰值 ≤ 阈值 + 收敛余量（attack τ=1ms 使残差 e^-5≈0.7% 量级）；WarmUp
//     期输出 ≤ 输入（增益从 1 平滑下降，无瞬时击穿放大）；
//   - 不染色：阈值下内容透明——启用后 WarmUp 段输出 == 输入（逐位）、Active 段
//     输出 == 输入延迟 D 的逐位副本（增益恒 1.0f 路径）；关闭态逐位直通；
//   - attack/release 指数平滑 τ 精确可测：DC 阶跃 + 逐样本观测 currentGainDb，
//     k 样本后残差 = e^(-k/(τ·fs))（τ_att=1ms / τ_rel=100ms，双精度逐样本
//     update 与闭式解一致到 1e-9）；
//   - 单/多声道：检测取全声道窗最大 → 共享单一增益（L 过阈 R 同压）；
//     声道内容无串扰（interleave 索引/每声道延迟线正确）；
//   - 44.1k–192k 延迟线声明：D = lround(min(fs,192000)×0.005)，实际 ms≈5；
//     Activity 状态机 Bypass→WarmUp（D 样本）→Active→FadingOut（3ms）→Bypass；
//   - >192k 封顶分支：384k/768k → D 恒 960 帧（192k 封顶换算），384k 实际
//     lookahead=2.5ms（勿按名义 5ms 声明）；封顶率下满载输出仍收敛于阈值。
#include <doctest.h>

#include "../../src/audio/limiter.h"

#include "seriona/audio/audio_contracts.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio = seriona::audio;

namespace {

audio::EqualizerConfig limiterConfig(bool limiterEnabled) {
  audio::EqualizerConfig config{};
  config.limiterEnabled = limiterEnabled;
  return config;
}

// limiter 阈值（10^(-0.5/20)）；测试内复算自契约，勿引用私有成员。
double thresholdLin() { return 0.9440608762859235; }

void fillDc(std::vector<float>& out, float value, std::size_t samples) {
  out.assign(samples, value);
}

// 处理 frames 个样本（每次调用 frameCount 个，便于按调用点观测状态）。单声道专用
// （样本数 == 帧数）。
void processFrames(audio::LimiterDspProcessor& lim, std::vector<float>& buffer, std::uint32_t framesPerCall) {
  REQUIRE(buffer.size() % framesPerCall == 0U);
  const std::size_t calls = buffer.size() / framesPerCall;
  for (std::size_t i = 0; i < calls; ++i) {
    lim.process(buffer.data() + i * framesPerCall, framesPerCall);
  }
}

// 处理 frames×channels 交错缓冲：每次调用 framesPerCall 帧（跨 channels×帧的
// 交错样本）。多声道专用——复用 processFrames 会按单声道样本数推进指针，窗与
// stride 重叠致每元素被重复处理（stereo 增益平方、quad 四次方）。
void processInterleaved(audio::LimiterDspProcessor& lim, std::vector<float>& buffer,
                        std::uint32_t channels, std::uint32_t framesPerCall) {
  REQUIRE(buffer.size() % (static_cast<std::size_t>(channels) * framesPerCall) == 0U);
  const std::size_t calls = buffer.size() / (static_cast<std::size_t>(channels) * framesPerCall);
  for (std::size_t i = 0; i < calls; ++i) {
    lim.process(buffer.data() + i * static_cast<std::size_t>(channels) * framesPerCall, framesPerCall);
  }
}

}  // namespace

TEST_CASE("eq limiter: 过阈稳态顶——DC 满载收敛于 -0.5dBFS 阈值且 WarmUp 无击穿") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t delay = 240U;  // lround(48000×0.005)
  audio::LimiterDspProcessor lim;
  const auto report = lim.configure(limiterConfig(true), fs, 1U);
  REQUIRE(report.accepted);
  CHECK(report.enabled);
  CHECK(report.sampleRate == fs);
  CHECK(report.channelCount == 1U);
  CHECK(report.lookaheadSamples == delay);
  CHECK(lim.actualLookaheadSamples() == delay);
  CHECK(lim.enabled());
  CHECK(lim.bypassActive());  // 状态机静止：配置后首次 process 前仍为 Bypass（进程内迁移）
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Bypass);

  std::vector<float> dc;
  fillDc(dc, 1.0F, 9600U);
  processFrames(lim, dc, 1U);

  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Active);
  // WarmUp 直通段（前 D 样本）输出 = 输入×增益（增益从 1 平滑下降）→ 不放大。
  float warmupPeak = 0.0F;
  for (std::size_t i = 0; i < delay; ++i) {
    warmupPeak = std::max(warmupPeak, dc[i]);
  }
  CHECK(static_cast<double>(warmupPeak) <= 1.0);

  // Active 段（读延迟线内容 × 收敛增益）：峰值 ≤ 阈值 + 攻击收敛余量（≈0.7%）。
  float activePeak = 0.0F;
  for (std::size_t i = delay; i < dc.size(); ++i) {
    activePeak = std::max(activePeak, dc[i]);
  }
  CHECK(static_cast<double>(activePeak) <= thresholdLin() * 1.007 + 1e-6);

  // 收敛：攻击 snap（|target-current|<1e-6 dB，~630 样本 @48k）后输出精确贴阈值。
  const double targetDb = 20.0 * std::log10(thresholdLin() / 1.0);
  CHECK(lim.fullySettled());
  CHECK(std::fabs(lim.currentGainDb() - targetDb) <= 1e-6);
  CHECK(std::fabs(static_cast<double>(dc[5000]) - thresholdLin()) <= 1e-3);
  CHECK(std::fabs(static_cast<double>(dc[9599]) - thresholdLin()) <= 1e-3);
}

TEST_CASE("eq limiter: 不染色——阈值下透明（延迟副本逐位相等）+ 关闭逐位直通") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t delay = 240U;
  // (a) 关闭态：Bypass 快速路径，任意内容逐位直通。
  audio::LimiterDspProcessor off;
  static_cast<void>(off.configure(limiterConfig(false), fs, 1U));
  std::vector<float> arbitrary(4800U);
  for (std::size_t i = 0; i < arbitrary.size(); ++i) {
    arbitrary[i] = static_cast<float>(((i * 1103515245U + 12345U) % 100000U) / 50000.0F - 1.0F);
  }
  const std::vector<float> reference = arbitrary;
  processFrames(off, arbitrary, 480U);
  CHECK(arbitrary == reference);

  // (b) 启用 + 阈值下内容（正弦 0.4 与 DC 0.9 混合思路：两段分别断言）：
  //     WarmUp 段输出 == 输入（逐位），Active 段输出 == 输入延迟 D 的逐位副本
  //     （增益恒 1.0f → 乘法精确）。
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(true), fs, 1U));
  std::vector<float> tone(9600U);
  for (std::size_t i = 0; i < tone.size(); ++i) {
    const double phase = 2.0 * 3.14159265358979323846 * 997.0 * static_cast<double>(i) / fs;
    tone[i] = static_cast<float>(0.4 * std::sin(phase));
  }
  const std::vector<float> input = tone;
  processFrames(lim, tone, 480U);
  REQUIRE(lim.activity() == audio::LimiterDspProcessor::Activity::Active);
  CHECK(lim.currentGainDb() == 0.0);
  for (std::size_t i = 0; i < delay; ++i) {
    REQUIRE(tone[i] == input[i]);  // WarmUp 直通（逐位）
  }
  for (std::size_t i = delay + 1U; i < tone.size(); ++i) {
    if (tone[i] != input[i - delay]) {
      FAIL("Active 段非延迟逐位副本 @ " << i);
    }
  }
  CHECK(lim.fullySettled());
}

TEST_CASE("eq limiter: attack/release 指数平滑——k 样本后残差 = e^(-k/(τ·fs))") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t delay = 240U;
  constexpr double attackTauSamples = 48.0;    // 1ms@48k
  constexpr double releaseTauSamples = 4800.0;  // 100ms@48k
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(true), fs, 1U));

  // 阶段 1：阈值下 DC 0.5 → warmup + active + 增益 0 稳态。
  std::vector<float> below(4800U, 0.5F);
  processFrames(lim, below, 480U);
  CHECK(lim.currentGainDb() == 0.0);
  CHECK(lim.fullySettled());

  // 阶段 2：DC 阶跃 0.5 → 1.0（过阈）。窗最大即时变 1.0 → 目标 t。
  // 逐样本观测 attack：current(k) = t×(1 - e^(-k/48))。
  const double targetDb = 20.0 * std::log10(thresholdLin() / 1.0);
  std::vector<float> sample(1U, 1.0F);
  double previous = lim.currentGainDb();
  for (std::size_t k = 1; k <= 700U; ++k) {
    sample[0] = 1.0F;  // 处理为 in-place：上一调用的输出已覆写 sample[0]（≈0.944），必须重填
    lim.process(sample.data(), 1U);
    const double current = lim.currentGainDb();
    CHECK(current <= previous);  // attack 单调下降
    if (k == 48U) {
      CHECK(std::fabs(current - targetDb * (1.0 - std::exp(-static_cast<double>(k) / attackTauSamples))) <= 1e-9);
    }
    if (k == 240U) {
      CHECK(std::fabs(current - targetDb * (1.0 - std::exp(-static_cast<double>(k) / attackTauSamples))) <= 1e-9);
    }
    previous = current;
  }
  // 收敛精确置位：current == target（snap 1e-6 dB）。
  CHECK(std::fabs(lim.currentGainDb() - targetDb) <= 1e-6);

  // 阶段 3：DC 阶跃 1.0 → 0.2（回阈值下）。窗内 1.0 逐个过期：最后一个 1.0
  // （seq 5500）在第 240 个 0.2 写入时被弹出 → release 从该样本起算。逐样本观测：
  // current(i0+k) = t×e^(-k/4800)。入口时 current 仍钉在 t（<0）——release 是增益
  // 升离平台（目标变 0），不能用 current<0 检测（首样本即成立），须检测升离平台。
  // 过期与增益更新同一样本内发生：i0=239 处已走 1 步 → 闭式指数用 k2=i-i0+1。
  std::vector<float> dcBelow(static_cast<std::size_t>(delay) + 12000U, 0.2F);
  std::size_t releaseStart = 0;
  bool released = false;
  for (std::size_t i = 0; i < dcBelow.size(); ++i) {
    lim.process(dcBelow.data() + i, 1U);
    const double current = lim.currentGainDb();
    if (!released && current > targetDb + 1e-9) {
      released = true;
      releaseStart = i;  // release 首次生效样本（观测时已含首步）
      CHECK(i == delay - 1U);  // 与窗过期算术一致（D=240 的第 240 个 0.2 写入）
    }
    const std::size_t k2 = i >= releaseStart ? i - releaseStart + 1U : 0U;
    if (k2 == 2400U) {
      CHECK(std::fabs(current - targetDb * std::exp(-static_cast<double>(k2) / releaseTauSamples)) <= 1e-6);
    }
    if (k2 == 9600U) {
      CHECK(std::fabs(current - targetDb * std::exp(-static_cast<double>(k2) / releaseTauSamples)) <= 1e-6);
    }
    if (k2 == 10000U) {
      CHECK(std::fabs(current - targetDb * std::exp(-static_cast<double>(k2) / releaseTauSamples)) <= 1e-6);
    }
  }
  REQUIRE(released);
  const std::size_t finalK2 = dcBelow.size() - releaseStart;  // = (size-1)-releaseStart+1
  CHECK(std::fabs(lim.currentGainDb() -
                  targetDb * std::exp(-static_cast<double>(finalK2) / releaseTauSamples)) <= 1e-6);
}

TEST_CASE("eq limiter: 单/多声道——共享单一增益 + 每声道内容隔离") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t channels = 2U;
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(true), fs, channels));

  // L = DC 1.0（过阈），R = DC 0.5（阈值下）。检测取全声道最大 → L 过阈时
  // R 同压（共享增益）；但内容不串扰：R 永不出现 L 的值。
  constexpr std::uint32_t frames = 9600U;
  std::vector<float> stereo(frames * channels);
  for (std::uint32_t f = 0; f < frames; ++f) {
    stereo[f * channels] = 1.0F;
    stereo[f * channels + 1U] = 0.5F;
  }
  processInterleaved(lim, stereo, channels, 240U);

  const double convergedGain = thresholdLin();  // DC 1.0 满载 → gain≈阈值
  float maxR = 0.0F;
  for (std::uint32_t f = 5000U; f < frames; ++f) {
    CHECK(std::fabs(static_cast<double>(stereo[f * channels]) - convergedGain) <= 1e-3);
    const double r = static_cast<double>(stereo[f * channels + 1U]);
    CHECK(std::fabs(r - 0.5 * convergedGain) <= 5e-4);  // R 同压：0.5×g
    maxR = std::max(maxR, stereo[f * channels + 1U]);
  }
  CHECK(static_cast<double>(maxR) <= 0.473);  // R 无内容串扰（未被 1.0 污染）

  // 4 声道：四路不同 DC，共享增益比 = 各声道自身值（隔离 + 同压并存）。
  constexpr std::uint32_t quadChannels = 4U;
  audio::LimiterDspProcessor quad;
  static_cast<void>(quad.configure(limiterConfig(true), fs, quadChannels));
  const std::array<float, 4> values{1.0F, 0.25F, 0.5F, 0.75F};
  constexpr std::uint32_t quadFrames = 9600U;
  std::vector<float> quadBuf(quadFrames * quadChannels);
  for (std::uint32_t f = 0; f < quadFrames; ++f) {
    for (std::uint32_t c = 0; c < quadChannels; ++c) {
      quadBuf[f * quadChannels + c] = values[c];
    }
  }
  processInterleaved(quad, quadBuf, quadChannels, 240U);
  for (std::uint32_t f = 5000U; f < quadFrames; ++f) {
    for (std::uint32_t c = 0; c < quadChannels; ++c) {
      const double expected = static_cast<double>(values[c]) * convergedGain;
      CHECK(std::fabs(static_cast<double>(quadBuf[f * quadChannels + c]) - expected) <= 2e-3);
    }
  }
}

TEST_CASE("eq limiter: 44.1k–192k 延迟线声明 + Activity 状态机迁移") {
  // 延迟线声明：D = lround(fs×0.005)，名义 ~5ms。
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> rateDelay{
      std::make_pair(44100U, 221U),  // lround(220.5)=221
      std::make_pair(48000U, 240U),
      std::make_pair(96000U, 480U),
      std::make_pair(192000U, 960U)};
  for (const auto& probe : rateDelay) {
    audio::LimiterDspProcessor lim;
    const auto report = lim.configure(limiterConfig(true), probe.first, 1U);
    REQUIRE(report.accepted);
    CHECK(report.lookaheadSamples == probe.second);
    CHECK(lim.actualLookaheadSamples() == probe.second);
    const double actualMs = 1000.0 * static_cast<double>(probe.second) / static_cast<double>(probe.first);
    CHECK(std::fabs(actualMs - 5.0) <= 0.1);
    CHECK(report.actualLookaheadMs == doctest::Approx(actualMs).epsilon(1e-12));
  }

  // 状态机迁移（48k，D=240）：Bypass → WarmUp（前 D 样本）→ Active → FadingOut
  // （3ms=144 样本）→ Bypass。
  constexpr std::uint32_t fs = 48000U;
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(false), fs, 1U));
  std::vector<float> sample(1U, 0.9F);
  lim.process(sample.data(), 1U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Bypass);
  CHECK(lim.bypassActive());
  CHECK(lim.fullySettled());

  static_cast<void>(lim.configure(limiterConfig(true), fs, 1U));
  lim.process(sample.data(), 1U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::WarmUp);
  CHECK(!lim.bypassActive());
  for (std::uint32_t i = 1; i < 240U; ++i) {  // 共 240 样本：仍在 WarmUp
    lim.process(sample.data(), 1U);
    CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::WarmUp);
  }
  lim.process(sample.data(), 1U);  // 第 241 样本：writeCount=240 → Active
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Active);

  static_cast<void>(lim.configure(limiterConfig(false), fs, 1U));
  lim.process(sample.data(), 1U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::FadingOut);
  for (std::uint32_t i = 1; i < 143U; ++i) {
    lim.process(sample.data(), 1U);
    CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::FadingOut);
  }
  lim.process(sample.data(), 1U);  // 第 144 样本淡毕
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Bypass);
  CHECK(lim.bypassActive());
  CHECK(lim.currentGainDb() == 0.0);
  CHECK(lim.fullySettled());
}

TEST_CASE("eq limiter: >192k 封顶分支——384k lookahead=960 帧 = 实际 2.5ms（勿按 5ms）") {
  // 名义 rate×5ms，fs>192k 封顶 192k 换算：384k → 960 帧 = 实际 2.5ms。
  for (const std::uint32_t fs : {384000U, 768000U}) {
    audio::LimiterDspProcessor lim;
    const auto report = lim.configure(limiterConfig(true), fs, 1U);
    REQUIRE(report.accepted);
    CHECK(report.lookaheadSamples == 960U);
    CHECK(lim.actualLookaheadSamples() == 960U);
    const double actualMs = 1000.0 * 960.0 / static_cast<double>(fs);
    CHECK(std::fabs(report.actualLookaheadMs - actualMs) <= 1e-9);
    CHECK(actualMs < 5.0);  // 封顶换算后的实际 lookahead 恒 < 名义 5ms
    if (fs == 384000U) {
      CHECK(std::fabs(actualMs - 2.5) <= 1e-6);  // 384k：960 帧 = 2.5ms
    }
    CHECK(lim.sampleRate() == fs);
    CHECK(lim.enabled());
  }

  // 384k 满载 DC：WarmUp=960 样本后接真延迟路径；攻击 snap（τ=1ms=384 样本）
  // 后输出收敛于阈值（持续内容无尖峰击穿——封顶换算不减限幅有效性）。
  constexpr std::uint32_t fs = 384000U;
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(true), fs, 1U));
  std::vector<float> sample(1U, 1.0F);
  lim.process(sample.data(), 1U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::WarmUp);
  std::vector<float> dc(9600U, 1.0F);
  processFrames(lim, dc, 1U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Active);
  float warmupPeak = 0.0F;
  for (std::size_t i = 0; i < 960U; ++i) {
    warmupPeak = std::max(warmupPeak, dc[i]);
  }
  CHECK(static_cast<double>(warmupPeak) <= 1.0);
  float activePeak = 0.0F;
  for (std::size_t i = 960U; i < dc.size(); ++i) {
    activePeak = std::max(activePeak, dc[i]);
  }
  CHECK(static_cast<double>(activePeak) <= thresholdLin() * 1.007 + 1e-6);
  const double targetDb = 20.0 * std::log10(thresholdLin() / 1.0);
  CHECK(std::fabs(lim.currentGainDb() - targetDb) <= 1e-6);
  CHECK(std::fabs(static_cast<double>(dc[9000]) - thresholdLin()) <= 1e-3);
}

// ==================== applyTargets（播放中实时开关目标更新） ====================

TEST_CASE("eq limiter: applyTargets 前置——未 configure 返回 false、configure 后目标态即时生效") {
  audio::LimiterDspProcessor fresh;
  CHECK_FALSE(fresh.applyTargets(limiterConfig(true)));  // 未 accepted：无操作
  CHECK_FALSE(fresh.configured());
  CHECK(fresh.bypassActive());
  CHECK_FALSE(fresh.enabled());

  static_cast<void>(fresh.configure(limiterConfig(false), 48000U, 1U));
  CHECK(fresh.applyTargets(limiterConfig(true)));  // accepted：目标态更新
  CHECK(fresh.enabled());
  CHECK(fresh.bypassActive());  // 迁移未开始（process 未调用——状态机在 process 内执行）
}

TEST_CASE("eq limiter: applyTargets 关→开→关——WarmUp/Active/FadingOut/Bypass 状态机迁移") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t delay = 240U;  // lround(48000×0.005)
  audio::LimiterDspProcessor lim;
  static_cast<void>(lim.configure(limiterConfig(false), fs, 1U));
  REQUIRE(lim.bypassActive());

  // 播放中开（applyTargets limiterEnabled true）→ process 入口 WarmUp（首 D 帧直通
  // 填延迟线）→ 满 D 帧后 Active。与 configure 开机的既有状态机语义一致（无差异路径）。
  CHECK(lim.applyTargets(limiterConfig(true)));
  CHECK(lim.enabled());
  std::vector<float> dc(static_cast<std::size_t>(delay) + 8U, 0.9F);
  processFrames(lim, dc, static_cast<std::uint32_t>(delay) + 8U);  // 单次调用跨过 D 帧 → Active
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Active);
  // WarmUp 期输出 == 输入（首 D 帧直通逐位）。
  std::vector<float> warm(static_cast<std::size_t>(delay), 0.9F);
  auto warmCopy = warm;
  processFrames(lim, warm, delay);
  CHECK(warm == warmCopy);

  // 播放中关（applyTargets false）→ process 入口 FadingOut（3ms = 144 帧 @48k）→ Bypass。
  CHECK(lim.applyTargets(limiterConfig(false)));
  CHECK_FALSE(lim.enabled());
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Active);  // 迁移未执行
  std::vector<float> fade(static_cast<std::size_t>(144U) + 16U, 0.9F);
  processFrames(lim, fade, static_cast<std::uint32_t>(144U) + 16U);  // 淡化 + 余量单次调用
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::Bypass);
  CHECK(lim.bypassActive());
  CHECK(lim.fullySettled());

  // 旁路态 process 零触碰（逐位直通）；再开一律重新 WarmUp（Bypass 中再启用）。
  std::vector<float> pattern(512U, 0.5F);
  const auto original = pattern;
  processFrames(lim, pattern, 512U);
  CHECK(pattern == original);
  CHECK(lim.applyTargets(limiterConfig(true)));
  std::vector<float> probe(4U, 0.5F);
  processFrames(lim, probe, 4U);
  CHECK(lim.activity() == audio::LimiterDspProcessor::Activity::WarmUp);
}
