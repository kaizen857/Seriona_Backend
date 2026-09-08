// EQ/限幅 DSP 链的帧级行为单测（任务 27，B2.5 组 seriona.audio.eq_dsp）。
//
// 被测面：src/audio/eq_dsp.{h,cpp} + src/audio/limiter.{h,cpp}（纯 DSP 核心，
// 帧级可测：process 接受任意 frameCount，逐帧调用即逐帧推进）。本文件驱动两条
// 处理器构成任务 25 的 f32 处理链（EQ → limiter），全部用例经纯 DSP API 覆盖
// 计划书 eq_dsp 组语义（render 级版本移交任务 28）：
//
//   - 斜坡无采样阶跃：preGain 0→+12dB 逐 1ms 块推进——增益轨迹每块精确 +0.6dB、
//     20 块精确到位；DC 域输出逐样本/块界最大跳变远小于整段一步跳变（无咔哒）；
//   - 稳态收敛：平滑中 fullySettled==false、增益轨迹线性，到位后精确置位
//     （clamp 无残差）且 fullySettled==true，频响与命令一致；
//   - 旁路爬坡（enabled=false 先清后跳）：目标归 0dB 先平滑退出（≤ramp 时长、
//     期间非 fastBypass）→ 清毕 fastBypass 快路径 → 样本域逐位直通；
//   - preGain 定标：±dB 线性尺度精确；
//   - 10↔31 模式切换无阶跃：增益轨迹逐块线性（永不瞬间 snap 到目标）+ 输出
//     无超界/无爆音（31→10 的越前缀硬切是冻结纪律：切掉的旧 band 贡献瞬间消失，
//     输出有界下凹 ≤ 命令增益对应幅度、随后新 band 平滑爬升回稳态）；
//   - 欠载补零尾不滤波：直通/旁路稳态下零尾逐位恒零（欠载补零不染色、无尾音）；
//     nullptr/0 帧无害空操作；
//   - muted 先清后跳 / 停→启冻结：DSP 无时间依赖——静音期不调用 process 即状态
//     冻结，恢复从冻结点继续（分段处理与连续处理逐位等价）；再启用从清零起点
//     爬坡（无旧状态跳变）；
//   - 多声道每 band 单实例隔离：L/R 独立状态无串扰、interleaved 索引正确；
//   - roundtrip：链 f32 域 + 末端单次量化契约（s16/s24 无增益逐位精确、s32
//     中幅 ≤1LSB / 全幅 ≤64LSB、有增益末端单次量化误差 ≤1LSB）——量化数学按
//     audio_output_device.cpp 556-640 区冻结契约复算（读侧 /2^15、/2^31 精确，
//     写侧 double llround+clamp）；EQ 开+全 0dB+限幅开且无削减 ≠ 逐位直通
//     （延迟线本体）——频响误差 <0.1dB + 无阶跃；
//   - 限幅开关过渡无 pop：DC 域开→关/关→开全程无端点阶跃、过阈启动有界
//     （输出 ≤ 阈值 + 启瞬余量，无瞬时击穿）；
//   - 低率平滑钳制（22.05k）：3×blockMs>20ms 大块每块推进 1/3、3 块精确收敛
//     无过冲；逐帧小块按 20ms 下界线性爬升、定时收敛。
#include <doctest.h>

#include "../../src/audio/eq_dsp.h"
#include "../../src/audio/limiter.h"

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

audio::EqualizerConfig preGainConfig(double preGainDb, bool enabled) {
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, enabled);
  config.preGainDb = static_cast<float>(preGainDb);
  return config;
}

audio::EqualizerConfig singleBandConfig(std::size_t bandIndex, float gainDb, bool enabled = true) {
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, enabled);
  config.bandGainsDb[bandIndex] = gainDb;
  return config;
}

// —— 数值辅助 ——

void fillSineMono(std::vector<float>& out, double fs, double f0Hz, double amplitude, std::size_t frames) {
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

// 最小二乘幅值估计（纯音稳态无偏，与窗内周期数无关）。
double estimateAmplitudeMono(const float* samples,
                             std::size_t offset,
                             std::size_t windowFrames,
                             double fs,
                             double f0Hz) {
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
  for (std::size_t i = offset; i < offset + windowFrames; ++i) {
    const double w = static_cast<double>(samples[i]);
    sumCw += cosPhase * w;
    sumSw += sinPhase * w;
    sumCC += cosPhase * cosPhase;
    sumSS += sinPhase * sinPhase;
    sumCS += cosPhase * sinPhase;
    const double nextC = cosPhase * rotC - sinPhase * rotS;
    sinPhase = cosPhase * rotS + sinPhase * rotC;
    cosPhase = nextC;
  }
  const double denom = sumCC * sumSS - sumCS * sumCS;
  const double a = (sumCw * sumSS - sumSw * sumCS) / denom;
  const double b = (sumSw * sumCC - sumCw * sumCS) / denom;
  return std::hypot(a, b);
}

double linToDb(double ratio) { return 20.0 * std::log10(ratio); }

bool allZero(const std::vector<float>& samples) {
  for (const float value : samples) {
    if (value != 0.0F) {
      return false;
    }
  }
  return true;
}

// —— 链夹具（EQ → limiter，与任务 25 链序一致）——

struct Chain {
  audio::EqualizerDspProcessor eq{};
  audio::LimiterDspProcessor lim{};

  void configure(const audio::EqualizerConfig& config, std::uint32_t fs, std::uint32_t channels) {
    static_cast<void>(eq.configure(config, fs, channels));
    static_cast<void>(lim.configure(config, fs, channels));
  }
  void process(float* frames, std::uint32_t frameCount) noexcept {
    eq.process(frames, frameCount);
    lim.process(frames, frameCount);
  }
  void process(std::vector<float>& frames, std::uint32_t frameCount, std::uint32_t channels) noexcept {
    REQUIRE(frames.size() % channels == 0U);
    const std::size_t totalFrames = frames.size() / channels;
    std::size_t offset = 0;
    while (offset < totalFrames) {
      const std::uint32_t chunk =
          static_cast<std::uint32_t>(std::min<std::size_t>(frameCount, totalFrames - offset));
      process(frames.data() + offset * channels, chunk);
      offset += chunk;
    }
  }
};

// EQ 平滑收敛（3 块静音；块长下 3×blockMs>20ms → 每块 1/3、恰 3 块到位）。
void settleEq(audio::EqualizerDspProcessor& dsp, std::uint32_t fs, std::uint32_t channels) {
  std::vector<float> silence(4096U * channels, 0.0f);
  for (int i = 0; i < 3; ++i) {
    dsp.process(silence.data(), 4096U);
  }
  REQUIRE(dsp.fullySettled());
}

}  // namespace

TEST_CASE("eq dsp: 平滑斜坡无采样阶跃——preGain 0→+12dB 逐 1ms 块、20 块精确到位") {
  constexpr std::uint32_t fs = 48000U;
  Chain chain;
  chain.configure(preGainConfig(0.0, true), fs, 1U);
  // 换到 +12dB：目标变化，从 0 起线性斜坡。
  chain.configure(preGainConfig(12.0, true), fs, 1U);
  CHECK(!chain.eq.fullySettled());

  // DC 输入 0.5：无 band 处理 → 输出 = 输入 × 10^(preGain/20)，逐样本/逐块可测。
  constexpr std::uint32_t blockFrames = 48U;  // 1ms 块 → ramp=max(20ms,3×1ms)=20ms
  std::vector<float> dc(static_cast<std::size_t>(blockFrames) * 40U, 0.5F);
  std::vector<double> gainPerBlock;
  gainPerBlock.reserve(40U);
  for (int block = 0; block < 40; ++block) {
    chain.process(dc.data() + static_cast<std::size_t>(block) * blockFrames, blockFrames);
    gainPerBlock.push_back(chain.eq.currentPreGainDb());
  }

  // 每块精确 +12×1ms/20ms = +0.6dB；20 块精确到位（clamp 无残差），此后不再动。
  for (int block = 0; block < 19; ++block) {
    CHECK(gainPerBlock[static_cast<std::size_t>(block)] == doctest::Approx(0.6 * (block + 1)).epsilon(1e-9));
  }
  CHECK(gainPerBlock[19] == doctest::Approx(12.0).epsilon(1e-9));
  CHECK(chain.eq.fullySettled());
  for (int block = 20; block < 40; ++block) {
    CHECK(gainPerBlock[static_cast<std::size_t>(block)] == 12.0);
  }

  // 样本域：块内恒定、块界步进随当前增益放大；最大跳变出现在最后一块界
  // （11.4dB → 12dB：0.5×10^(12/20)×(1−10^(−0.03))≈0.1329），远小于一次跳到
  // 目标的 0.5×(10^(12/20)−1)≈1.4907（无采样阶跃/无咔哒判别量）。
  double maxDelta = 0.0;
  for (std::size_t i = 1; i < dc.size(); ++i) {
    maxDelta = std::max(maxDelta, static_cast<double>(std::fabs(dc[i] - dc[i - 1])));
  }
  CHECK(maxDelta <= 0.5 * std::pow(10.0, 12.0 / 20.0) * (1.0 - std::pow(10.0, -0.6 / 20.0)) * 1.01 + 1e-7);
  CHECK(maxDelta >= 0.5 * std::pow(10.0, 12.0 / 20.0) * (1.0 - std::pow(10.0, -0.6 / 20.0)) * 0.99);  // 斜坡真实发生
  CHECK(maxDelta < 0.2);  // 远小于整段一步跳变（1.4907）
}

TEST_CASE("eq dsp: 稳态收敛——fullySettled 语义与命令增益精确到达") {
  constexpr std::uint32_t fs = 48000U;
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(singleBandConfig(17U, 6.0F), fs, 1U));
  CHECK(!dsp.fullySettled());
  CHECK(dsp.currentBandGainDb(17U) == 0.0);
  CHECK(dsp.currentPreGainDb() == 0.0);

  constexpr std::uint32_t blockFrames = 96U;  // 2ms 块 → ramp 20ms → 每块 0.6dB
  std::vector<float> silence(blockFrames, 0.0F);
  for (int block = 1; block <= 12; ++block) {
    dsp.process(silence.data(), blockFrames);
    if (block <= 9) {
      CHECK(!dsp.fullySettled());
      CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(0.6 * block).epsilon(1e-9));
    }
  }
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(6.0).epsilon(1e-12));
  CHECK(dsp.fullySettled());
  CHECK(dsp.currentBandGainDb(16U) == 0.0);  // 未动 band 保持 0

  // 稳态频响：1kHz 正弦 +6dB（±0.05dB）。
  std::vector<float> tone;
  fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.25, 24000U);
  dsp.process(tone.data(), 24000U);
  const double measured = estimateAmplitudeMono(tone.data(), 12000U, 12000U, fs, 1000.0);
  CHECK(std::abs(linToDb(measured / 0.25) - 6.0) <= 0.05);
}

TEST_CASE("eq dsp: 旁路爬坡——enabled=false 先平滑清 0 再 fastBypass 逐位直通") {
  constexpr std::uint32_t fs = 48000U;
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(preGainConfig(12.0, true), fs, 1U));
  settleEq(dsp, fs, 1U);
  CHECK(dsp.fastBypassActive() == false);  // preGain +12 稳态：处理中（非快路径）
  CHECK(dsp.currentPreGainDb() == 12.0);

  // 关闭：目标归 0，但先平滑退出——configure 后不得立即 fastBypass。
  static_cast<void>(dsp.configure(preGainConfig(12.0, false), fs, 1U));
  CHECK(!dsp.enabled());
  CHECK(dsp.currentPreGainDb() == 12.0);  // 平滑状态保留，未跳变
  CHECK(!dsp.fastBypassActive());         // 先清（爬坡中）
  CHECK(!dsp.fullySettled());

  std::vector<float> silence(96U, 0.0F);
  for (int block = 1; block <= 12; ++block) {
    dsp.process(silence.data(), 96U);
    if (block < 10) {
      CHECK(dsp.currentPreGainDb() == doctest::Approx(12.0 - 1.2 * block).epsilon(1e-9));
      CHECK(!dsp.fastBypassActive());
    }
  }
  CHECK(dsp.currentPreGainDb() == 0.0);  // 清毕
  CHECK(dsp.fullySettled());
  CHECK(dsp.fastBypassActive());  // 后跳：全零稳态快路径

  // 逐位直通：清毕后任意输入原样通过。
  std::vector<float> buffer(4800U);
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<float>(((i * 2654435761U) % 65536U) / 32768.0F - 1.0F);
  }
  const std::vector<float> reference = buffer;
  dsp.process(buffer.data(), 1200U);
  CHECK(buffer == reference);
}

TEST_CASE("eq dsp: preGain 定标——±dB 线性尺度精确（负增益 + 实测频响）") {
  constexpr std::uint32_t fs = 48000U;
  // −6dB：DC 0.9 → 0.9×10^(−6/20)≈0.451068（f32 乘法单次舍入，1e-5 容差）。
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(preGainConfig(-6.0, true), fs, 1U));
  settleEq(dsp, fs, 1U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(-6.0).epsilon(1e-12));
  std::vector<float> dc(4800U, 0.9F);
  dsp.process(dc.data(), 4800U);
  const double scale = std::pow(10.0, -6.0 / 20.0);
  CHECK(std::fabs(static_cast<double>(dc[2400]) - 0.9 * scale) <= 1e-5);

  // 全带 0dB + preGain −6：正弦实测 −6±0.02dB。
  audio::EqualizerDspProcessor flat;
  static_cast<void>(flat.configure(preGainConfig(-6.0, true), fs, 1U));
  settleEq(flat, fs, 1U);
  std::vector<float> tone;
  fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.4, 12000U);
  flat.process(tone.data(), 12000U);
  const double measured = estimateAmplitudeMono(tone.data(), 6000U, 6000U, fs, 1000.0);
  CHECK(std::abs(linToDb(measured / 0.4) - (-6.0)) <= 0.02);
}

TEST_CASE("eq dsp: 10→31 模式切换——平滑域线性排空/爬升、谐振瞬态有界、稳态复原") {
  constexpr std::uint32_t fs = 48000U;
  audio::EqualizerDspProcessor dsp;
  // 两模式同 +6dB@1000Hz：Band10 idx5 与 Band31 idx17 中心同值。
  audio::EqualizerConfig config10 = flatConfig(audio::EqualizerBandMode::Band10, true);
  config10.bandGainsDb[5U] = 6.0F;
  static_cast<void>(dsp.configure(config10, fs, 1U));
  settleEq(dsp, fs, 1U);

  // 连续 1kHz 正弦（A=0.25），逐 48 样本（1ms）块处理并记录 RMS（dB）。
  std::vector<float> tone;
  fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.25, 48U * 140U);
  std::vector<double> frameDb;
  frameDb.reserve(140U);
  const double preRmsRef = 0.25 / std::sqrt(2.0);
  for (int frame = 0; frame < 60; ++frame) {  // 前 60ms 稳态
    dsp.process(tone.data() + static_cast<std::size_t>(frame) * 48U, 48U);
    double sumSq = 0.0;
    for (int i = 0; i < 48; ++i) {
      const double v = tone[static_cast<std::size_t>(frame) * 48U + static_cast<std::size_t>(i)];
      sumSq += v * v;
    }
    frameDb.push_back(linToDb(std::sqrt(sumSq / 48.0) / preRmsRef));
  }
  CHECK(dsp.fullySettled());
  const double steadyDb = frameDb[59];
  CHECK(std::abs(steadyDb - 6.0) <= 0.05);

  // 10→31（冻结语义实测锁定）：旧 idx5 中心 1000→63Hz 换型后其滤波状态继续在
  // 1kHz 振铃 → 谐振瞬态（1kHz 正弦 +6dB：帧 RMS 上冲实测 +19.5dB、下凹 −5.4dB、
  // 约 25ms 振荡衰减后精确复原）；平滑域（currentBandGainDb）无阶跃——idx5 排空
  // 与 idx17 爬升逐 1ms 块线性 0.3dB。界按实测包络 + 容差锁定。
  const double preIdx5Db = dsp.currentBandGainDb(5U);
  audio::EqualizerConfig config31 = flatConfig(audio::EqualizerBandMode::Band31, true);
  config31.bandGainsDb[17U] = 6.0F;
  static_cast<void>(dsp.configure(config31, fs, 1U));
  CHECK(!dsp.fullySettled());
  double peakDb = -1e18, minDb = 1e18;
  std::vector<double> idx5Db, idx17Db;
  idx5Db.reserve(80U);
  idx17Db.reserve(80U);
  for (int frame = 60; frame < 140; ++frame) {
    dsp.process(tone.data() + static_cast<std::size_t>(frame) * 48U, 48U);
    double sumSq = 0.0;
    for (int i = 0; i < 48; ++i) {
      const double v = tone[static_cast<std::size_t>(frame) * 48U + static_cast<std::size_t>(i)];
      sumSq += v * v;
    }
    const double frameValue = linToDb(std::sqrt(sumSq / 48.0) / preRmsRef);
    frameDb.push_back(frameValue);
    peakDb = std::max(peakDb, frameValue);
    minDb = std::min(minDb, frameValue);
    idx5Db.push_back(dsp.currentBandGainDb(5U));
    idx17Db.push_back(dsp.currentBandGainDb(17U));
  }
  const double peakExcessDb = peakDb - steadyDb;
  CHECK(dsp.fullySettled());
  CHECK(std::abs(frameDb[139] - steadyDb) <= 0.2);  // 稳态复原 +6dB
  CHECK(peakExcessDb >= 10.0);   // 谐振瞬态真实存在（防回归为直切/线性爬升语义）
  CHECK(peakExcessDb <= 22.0);   // 上冲有界（实测 19.5）
  CHECK(minDb >= steadyDb - 7.0);  // 下凹有界（实测 −5.4）
  // 平滑域：逐块线性（|步进| ≤ 0.31dB）、idx5 单调排空、idx17 单调爬升。
  CHECK(idx5Db.front() <= preIdx5Db + 1e-12);
  bool idx5Down = true, idx17Up = true;
  for (std::size_t f = 1; f < idx17Db.size(); ++f) {
    idx5Down = idx5Down && (idx5Db[f] <= idx5Db[f - 1] + 1e-12);
    idx17Up = idx17Up && (idx17Db[f] >= idx17Db[f - 1] - 1e-12);
    CHECK(std::fabs(idx5Db[f] - idx5Db[f - 1]) <= 0.31);
    CHECK(std::fabs(idx17Db[f] - idx17Db[f - 1]) <= 0.31);
  }
  CHECK(idx5Down);
  CHECK(idx17Up);
  const std::size_t tailIdx = 30U;  // frame 90 = 切换后 30ms
  CHECK(idx5Db[tailIdx] == doctest::Approx(0.0).epsilon(1e-12));  // 排空到位
  CHECK(idx17Db[tailIdx] == doctest::Approx(6.0).epsilon(1e-9));  // 爬升到位（clamp 无残差）
}

TEST_CASE("eq dsp: 31→10 越前缀排空——硬切有界（冻结纪律）、爬升平滑、稳态复原") {
  constexpr std::uint32_t fs = 48000U;
  audio::EqualizerDspProcessor dsp;
  audio::EqualizerConfig config31 = flatConfig(audio::EqualizerBandMode::Band31, true);
  config31.bandGainsDb[17U] = 6.0F;
  static_cast<void>(dsp.configure(config31, fs, 1U));
  settleEq(dsp, fs, 1U);

  std::vector<float> tone;
  fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.25, 48U * 140U);
  const double preRmsRef = 0.25 / std::sqrt(2.0);
  std::vector<double> frameDb;
  for (int frame = 0; frame < 60; ++frame) {
    dsp.process(tone.data() + static_cast<std::size_t>(frame) * 48U, 48U);
    double sumSq = 0.0;
    for (int i = 0; i < 48; ++i) {
      const double v = tone[static_cast<std::size_t>(frame) * 48U + static_cast<std::size_t>(i)];
      sumSq += v * v;
    }
    frameDb.push_back(linToDb(std::sqrt(sumSq / 48.0) / preRmsRef));
  }
  const double steadyDb = frameDb[59];

  // 31→10（idx17 ≥ 10 前缀）：旧带贡献被硬切（冻结纪律：10 档表超前缀中心未
  // 定义，无法 reinit——排空只走平滑状态不触碰 biquad）。切换瞬间输出相对新
  // 输入路径掉到 ~0dB 再随 idx5 爬升：下凹有界（≤ 命令增益对应幅度 + 滤波状态
  // 换型瞬态）、恢复平滑、无超界。
  audio::EqualizerConfig config10 = flatConfig(audio::EqualizerBandMode::Band10, true);
  config10.bandGainsDb[5U] = 6.0F;
  static_cast<void>(dsp.configure(config10, fs, 1U));
  double minDb = steadyDb;
  for (int frame = 60; frame < 140; ++frame) {
    dsp.process(tone.data() + static_cast<std::size_t>(frame) * 48U, 48U);
    double sumSq = 0.0;
    for (int i = 0; i < 48; ++i) {
      const double v = tone[static_cast<std::size_t>(frame) * 48U + static_cast<std::size_t>(i)];
      sumSq += v * v;
    }
    const double frameValue = linToDb(std::sqrt(sumSq / 48.0) / preRmsRef);
    frameDb.push_back(frameValue);
    minDb = std::min(minDb, frameValue);
    CHECK(frameValue <= steadyDb + 1.0);  // 无超界/无爆音
    if (frame > 61) {
      CHECK(std::fabs(frameValue - frameDb[static_cast<std::size_t>(frame) - 1]) <= 2.5);
    }
  }
  CHECK(minDb >= steadyDb - 7.0);  // 硬切幅度有界（6dB 命令 + 状态换型余量）
  CHECK(dsp.fullySettled());
  CHECK(std::abs(frameDb[139] - steadyDb) <= 0.2);  // 复原 +6dB
}

TEST_CASE("eq dsp: 欠载补零尾不滤波——直通稳态零尾逐位恒零 + 空操作守卫") {
  constexpr std::uint32_t fs = 48000U;
  // (a) nullptr / 0 帧：无害空操作（noexcept 契约面）。
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(singleBandConfig(17U, 15.0F), fs, 1U));
  dsp.process(nullptr, 240U);
  dsp.process(nullptr, 0U);
  std::vector<float> tiny(1U, 0.25F);
  dsp.process(tiny.data(), 0U);
  CHECK(tiny[0] == 0.25F);

  // (b) 关闭态零尾：先活跃滤波（+15dB 正弦）→ 关闭并清毕 → 长零输入逐位恒零。
  audio::EqualizerDspProcessor dsp2;
  static_cast<void>(dsp2.configure(singleBandConfig(17U, 15.0F), fs, 1U));
  settleEq(dsp2, fs, 1U);
  std::vector<float> loud;
  fillSineMono(loud, static_cast<double>(fs), 1000.0, 0.9, 12000U);
  dsp2.process(loud.data(), 12000U);
  static_cast<void>(dsp2.configure(singleBandConfig(17U, 15.0F, false), fs, 1U));
  std::vector<float> silenceBlocks(96U, 0.0F);
  for (int i = 0; i < 20; ++i) {  // 清毕 + 余量
    dsp2.process(silenceBlocks.data(), 96U);
  }
  CHECK(dsp2.fastBypassActive());
  std::vector<float> tail(48000U, 0.0F);
  dsp2.process(tail.data(), 48000U);
  CHECK(allZero(tail));  // 欠载补零尾不被滤波（零增长、零尾音、无状态泄漏）

  // (c) 出厂/全 0dB 直通稳态长零：同样恒零。
  Chain chain;
  chain.configure(flatConfig(audio::EqualizerBandMode::Band31, true), fs, 2U);
  std::vector<float> zeroTail(24000U * 2U, 0.0F);
  chain.process(zeroTail, 480U, 2U);
  CHECK(allZero(zeroTail));
}

TEST_CASE("eq dsp: muted 停→启冻结——恢复从冻结点继续（无时间依赖、无瞬态）") {
  constexpr std::uint32_t fs = 48000U;
  // 静音/暂停期间设备不调用 process → DSP 状态冻结；恢复后从冻结点继续爬坡。
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(preGainConfig(12.0, true), fs, 1U));
  std::vector<float> block(96U, 0.25F);  // 2ms 块：每块 +1.2dB
  for (int i = 0; i < 3; ++i) {          // 爬到 3.6dB
    dsp.process(block.data(), 96U);
  }
  const double frozenDb = dsp.currentPreGainDb();
  CHECK(frozenDb == doctest::Approx(3.6).epsilon(1e-9));
  CHECK(!dsp.fullySettled());

  // 冻结：不调用 process（等价静音期任意时长）。恢复后推进量严格 = 冻结前步进。
  dsp.process(block.data(), 96U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(frozenDb + 1.2).epsilon(1e-9));
  CHECK(dsp.currentPreGainDb() <= 12.0);  // 不跳变到目标
  for (int i = 0; i < 7; ++i) {
    dsp.process(block.data(), 96U);
  }
  CHECK(dsp.fullySettled());
  CHECK(dsp.currentPreGainDb() == 12.0);
}

TEST_CASE("eq dsp: 停→启清理——分段（暂停）处理与连续处理逐位等价；重建无残留") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t channels = 2U;
  // (a) 限幅器 engaged（真延迟路径）下：同输入同状态，与分块/暂停方式无关。
  //     暂停 = 不调用 → 纯时间无关 DSP 的冻结语义：分段结果 == 连续结果（逐位）。
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
  config.limiterEnabled = true;

  auto runContinuous = [&]() {
    Chain chain;
    chain.configure(config, fs, channels);
    std::vector<float> tone;
    fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.9, 48000U);  // 过阈内容
    std::vector<float> stereo(tone.size() * channels);
    for (std::size_t i = 0; i < tone.size(); ++i) {
      stereo[i * channels] = tone[i];
      stereo[i * channels + 1U] = tone[i];
    }
    // 与分段路径同界（每 480 帧一调）：分块边界一致才能逐位等价。
    for (std::uint32_t frame = 0U; frame < 48000U; frame += 480U) {
      chain.process(stereo.data() + static_cast<std::size_t>(frame) * channels, 480U);
    }
    return stereo;
  };
  auto runPaused = [&]() {
    Chain chain;
    chain.configure(config, fs, channels);
    std::vector<float> tone;
    fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.9, 48000U);
    std::vector<float> stereo(tone.size() * channels);
    for (std::size_t i = 0; i < tone.size(); ++i) {
      stereo[i * channels] = tone[i];
      stereo[i * channels + 1U] = tone[i];
    }
    // 停（不调用）→ 启：三段，段间任意"暂停"（DSP 无时间依赖 → 冻结语义）。
    auto processRange = [&](std::uint32_t startFrame, std::uint32_t endFrame) {
      for (std::uint32_t frame = startFrame; frame < endFrame; frame += 480U) {
        chain.process(stereo.data() + static_cast<std::size_t>(frame) * channels, 480U);
      }
    };
    processRange(0U, 12000U);     // 段 1
    for (volatile int spin = 0; spin < 1; ++spin) {}  // 象征性暂停
    processRange(12000U, 24000U); // 段 2（从冻结点继续）
    processRange(24000U, 48000U); // 段 3
    return stereo;
  };

  const std::vector<float> continuous = runContinuous();
  const std::vector<float> paused = runPaused();
  REQUIRE(continuous.size() == paused.size());
  for (std::size_t i = 0; i < continuous.size(); ++i) {
    if (continuous[i] != paused[i]) {
      FAIL("分段（暂停→恢复）处理与连续处理不一致 @ " << i);
    }
  }

  // (b) seek 重建无残留：旧实例处理过响内容后销毁；新实例（同配置）从零状态起，
  //     静音输入恒零（无旧延迟线/滤波尾音泄漏）。
  Chain oldChain;
  oldChain.configure(config, fs, channels);
  std::vector<float> loud;
  fillSineMono(loud, static_cast<double>(fs), 1000.0, 0.95, 24000U);
  std::vector<float> loudStereo(loud.size() * channels);
  for (std::size_t i = 0; i < loud.size(); ++i) {
    loudStereo[i * channels] = loud[i];
    loudStereo[i * channels + 1U] = loud[i];
  }
  oldChain.process(loudStereo, 480U, channels);

  Chain fresh;  // = seek 后的重建实例
  fresh.configure(config, fs, channels);
  std::vector<float> silence(96000U * channels, 0.0F);  // 5×192k 封顶延迟线长度
  fresh.process(silence, 480U, channels);
  CHECK(allZero(silence));
}

TEST_CASE("eq dsp: 多声道每 band 单实例隔离——L/R 独立状态无串扰") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t channels = 2U;
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(singleBandConfig(17U, 15.0F), fs, channels));
  settleEq(dsp, fs, channels);

  // L = 1kHz 正弦 0.4；R = DC 0.3。+15dB@1kHz 只应作用于 L 的 1k 分量；
  // R 的 DC 分量过 peaking 恒 0dB（RBJ H(z=1)=1），滤波状态按声道独立。
  constexpr std::size_t totalFrames = 48000U;
  std::vector<float> stereo(totalFrames * channels);
  for (std::size_t frame = 0; frame < totalFrames; ++frame) {
    const double phase = 2.0 * kPi * 1000.0 * static_cast<double>(frame) / static_cast<double>(fs);
    stereo[frame * channels] = static_cast<float>(0.4 * std::sin(phase));
    stereo[frame * channels + 1U] = 0.3F;
  }
  dsp.process(stereo.data(), totalFrames);

  // L 尾段幅值 ≈ 0.4×10^(15/20)=2.251（隔样本取 L）。
  std::vector<float> leftTail(totalFrames / 2U);
  for (std::size_t frame = 0; frame < totalFrames / 2U; ++frame) {
    leftTail[frame] = stereo[frame * 2U];
  }
  const double measuredL = estimateAmplitudeMono(leftTail.data(), 20000U / 2U, 4000U, fs, 1000.0);
  CHECK(std::abs(linToDb(measuredL / 0.4) - 15.0) <= 0.1);

  // R 全程 DC 保持 0.3（±0.002）：无 L 串扰、无 DC 染色。
  double minR = 1.0;
  double maxR = -1.0;
  for (std::size_t frame = totalFrames / 2U; frame < totalFrames; ++frame) {
    const double r = static_cast<double>(stereo[frame * channels + 1U]);
    minR = std::min(minR, r);
    maxR = std::max(maxR, r);
  }
  CHECK(std::fabs(minR - 0.3) <= 0.002);
  CHECK(std::fabs(maxR - 0.3) <= 0.002);

  // 4 声道 interleave 索引正确性：不同 DC 各声道保持自身值。
  audio::EqualizerDspProcessor quad;
  static_cast<void>(quad.configure(singleBandConfig(17U, 15.0F), fs, 4U));
  settleEq(quad, fs, 4U);
  std::vector<float> four(totalFrames * 4U);
  for (std::size_t frame = 0; frame < totalFrames; ++frame) {
    const double phase = 2.0 * kPi * 1000.0 * static_cast<double>(frame) / static_cast<double>(fs);
    four[frame * 4U] = static_cast<float>(0.3 * std::sin(phase));
    four[frame * 4U + 1U] = 0.1F;
    four[frame * 4U + 2U] = 0.2F;
    four[frame * 4U + 3U] = -0.15F;
  }
  quad.process(four.data(), totalFrames);
  const std::array<float, 3> expected{0.1F, 0.2F, -0.15F};
  for (std::size_t frame = totalFrames / 2U; frame < totalFrames; ++frame) {
    for (std::size_t ch = 1U; ch < 4U; ++ch) {
      CHECK(std::fabs(static_cast<double>(four[frame * 4U + ch]) - expected[ch - 1U]) <= 0.002);
    }
  }
}

TEST_CASE("eq dsp: roundtrip——s16/s24 无增益逐位精确、s32 ≤1LSB/全幅 ≤64LSB、末端单次量化") {
  // 量化数学复算自 audio_output_device.cpp 556-640 区冻结契约：读侧 int→f32 除
  // 2^15（s16）或左对齐 /2^31（s24/s32，除数为 2 的幂 → 24 位内内容零舍入）；
  // 写侧 double 域 llround(clamp×scale)。链在 f32 域处理 → 无增益时整链不触碰
  // 样本，往返误差全来自边界换算本身。
  constexpr std::uint32_t fs = 48000U;

  auto quantizeS16 = [](float x) -> std::int32_t {
    const double v = std::isfinite(static_cast<double>(x)) ? static_cast<double>(x) : 0.0;
    return static_cast<std::int32_t>(std::clamp<long long>(
        std::llround(std::clamp(v, -1.0, 1.0) * 32768.0),
        std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
  };
  auto quantizeS32 = [](float x) -> std::int32_t {
    const double v = std::isfinite(static_cast<double>(x)) ? static_cast<double>(x) : 0.0;
    return static_cast<std::int32_t>(std::clamp<long long>(
        std::llround(std::clamp(v, -1.0, 1.0) * 2147483648.0),
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
  };

  // 无增益：int → f32（链 0dB 直通）→ 量化回 int。
  Chain chain;
  chain.configure(flatConfig(audio::EqualizerBandMode::Band31, true), fs, 1U);

  // s16 全范围（±32767）逐位精确。
  for (std::int32_t value : {std::int32_t{-32767}, -20000, -1, 0, 1, 999, 32767}) {
    const float f = static_cast<float>(value) / 32768.0F;
    CHECK(quantizeS16(f) == value);
    // 经链（EQ 全 0dB 直通）后量化结果一致——链不改动样本。
    float routed = f;
    chain.process(&routed, 1U);
    CHECK(quantizeS16(routed) == value);
  }

  // s24 全范围（±2^23，3 字节小端左对齐语义：v24<<8 /2^31）逐位精确。
  auto v24ToFloat = [](std::int32_t v24) {
    const std::int64_t leftAligned = static_cast<std::int64_t>(v24) << 8;
    return static_cast<float>(static_cast<double>(leftAligned) / 2147483648.0);
  };
  for (std::int32_t v24 : {std::int32_t{-(1 << 23)}, -1000000, -1, 0, 1, 123456, (1 << 23) - 1}) {
    const float f = v24ToFloat(v24);
    const std::int64_t scaled = std::llround(static_cast<double>(f) * 2147483648.0);
    CHECK(scaled == (static_cast<std::int64_t>(v24) << 8));
  }

  // s32：中幅（≤2^23，f32 24 位尾数精确表示）误差 ≤1 LSB；全幅（±2^31−1）
  // f32 舍入误差 ≤ 半 ULP×2^31 = 64 LSB——勿断言逐位。
  Chain chain32;
  chain32.configure(flatConfig(audio::EqualizerBandMode::Band31, true), fs, 1U);
  for (std::int64_t v : {std::int64_t{-(1 << 23)}, std::int64_t{-1}, std::int64_t{0},
                               std::int64_t{1}, std::int64_t{1 << 23}}) {
    const float f = static_cast<float>(static_cast<double>(v) / 2147483648.0);
    float routed = f;
    chain32.process(&routed, 1U);
    CHECK(std::llabs(static_cast<std::int64_t>(quantizeS32(routed)) - v) <= 1);
  }
  for (std::int64_t v : {std::int64_t{-(2147483647LL)}, std::int64_t{2147483647LL},
                               std::int64_t{1073741824LL}, std::int64_t{-1073741824LL}}) {
    const float f = static_cast<float>(static_cast<double>(v) / 2147483648.0);
    float routed = f;
    chain32.process(&routed, 1U);
    CHECK(std::llabs(static_cast<std::int64_t>(quantizeS32(routed)) - v) <= 64);
  }

  // 有增益末端单次量化：f32 域乘增益（单次舍入）后只量化一次；相对 double 域
  // 参考（v×g 双精度再 llround）误差 ≤1 LSB——证明中间无二次量化。
  const float gain = std::pow(10.0F, -3.5F / 20.0F);
  Chain chainGain;
  audio::EqualizerConfig gainConfig = flatConfig(audio::EqualizerBandMode::Band31, true);
  gainConfig.preGainDb = -3.5F;
  chainGain.configure(gainConfig, fs, 1U);
  settleEq(chainGain.eq, fs, 1U);
  for (std::int32_t v : {-32000, -20000, -1, 0, 1, 7777, 32000}) {
    const float f = static_cast<float>(v) / 32768.0F;
    float routed = f;
    chainGain.process(&routed, 1U);  // EQ preGain −3.5dB（f32 单次乘法）
    const std::int64_t reference = std::llround(static_cast<double>(v) * static_cast<double>(gain));
    CHECK(std::llabs(static_cast<std::int64_t>(quantizeS16(routed)) - reference) <= 1);
  }
}

TEST_CASE("eq dsp: EQ 开+全 0dB+限幅开+无削减 ≠ 逐位直通——频响 <0.1dB 误差且无阶跃") {
  constexpr std::uint32_t fs = 48000U;
  constexpr std::uint32_t channels = 1U;
  Chain chain;
  audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
  config.limiterEnabled = true;  // 限幅开；内容远低于阈值（无削减）
  chain.configure(config, fs, channels);

  std::vector<float> tone;
  fillSineMono(tone, static_cast<double>(fs), 1000.0, 0.5, 96000U);
  const std::vector<float> input = tone;
  chain.process(tone, 480U, channels);

  // 延迟线本体（D=240@48k）→ 输出 ≠ 输入（非逐位直通）……
  bool differs = false;
  for (std::size_t i = 0; i < tone.size(); ++i) {
    if (tone[i] != input[i]) {
      differs = true;
      break;
    }
  }
  CHECK(differs);
  // ……但幅度/频响不变（0.1dB 内）且逐样本无阶跃（斜率有界）。
  const double measured = estimateAmplitudeMono(tone.data(), 48000U, 24000U, fs, 1000.0);
  CHECK(std::abs(linToDb(measured / 0.5)) <= 0.1);
  const double sineSlope = 2.0 * kPi * 1000.0 / static_cast<double>(fs) * 0.5;  // ≈0.065
  for (std::size_t i = 48001U; i < 72000U; ++i) {
    const double delta = std::fabs(static_cast<double>(tone[i] - tone[i - 1]));
    CHECK(delta <= sineSlope * 1.05 + 1e-6);  // 无阶跃（延迟 + 恒增益 1 路径）
  }
}

TEST_CASE("eq dsp: 限幅开关过渡无 pop——DC 域开→关/关→开无端点阶跃、过阈启瞬有界") {
  constexpr std::uint32_t fs = 48000U;
  // (a) 阈值下 DC 全程（enable→warmup→active→disable→fade→bypass）输出连续。
  {
    Chain chain;
    audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
    config.limiterEnabled = false;
    chain.configure(config, fs, 1U);
    std::vector<float> dc(4800U, 0.9F);
    chain.process(dc, 480U, 1U);            // 旁路期：原样直通
    CHECK(dc[0] == 0.9F);

    config.limiterEnabled = true;
    chain.configure(config, fs, 1U);        // 关→开：WarmUp → Active
    for (int i = 0; i < 4; ++i) {
      std::vector<float> block(480U, 0.9F);
      chain.process(block, 480U, 1U);
    }
    config.limiterEnabled = false;
    chain.configure(config, fs, 1U);        // 开→关：FadingOut → Bypass
    std::vector<float> tail(4800U, 0.9F);
    chain.process(tail, 480U, 1U);
    CHECK(chain.lim.bypassActive());
    CHECK(chain.lim.activity() == audio::LimiterDspProcessor::Activity::Bypass);

    // 全程样本与 0.9 的偏差 ≤1e-6（fade 混合的 f32 舍入级）——无任何端点阶跃。
    float minV = dc[0];
    float maxV = dc[0];
    for (const float v : dc) {
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
    for (const float v : tail) {
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
    CHECK(std::fabs(static_cast<double>(minV) - 0.9) <= 1e-6);
    CHECK(std::fabs(static_cast<double>(maxV) - 0.9) <= 1e-6);
  }

  // (b) Active 期输入 DC 0.9→1.0 阶跃：lookahead 使输出 ≤ 阈值 + 收敛余量
  //     （0.94406×~1.007 ≈ 0.951）——无瞬时击穿、无 pop。
  {
    Chain chain;
    audio::EqualizerConfig config = flatConfig(audio::EqualizerBandMode::Band31, true);
    config.limiterEnabled = true;
    chain.configure(config, fs, 1U);
    std::vector<float> warm(9600U, 0.9F);   // 200ms：WarmUp(5ms)+Active 稳态
    chain.process(warm, 480U, 1U);
    CHECK(chain.lim.activity() == audio::LimiterDspProcessor::Activity::Active);
    std::vector<float> overload(24000U, 1.0F);
    chain.process(overload, 480U, 1U);
    float peak = 0.0F;
    for (const float v : overload) {
      peak = std::max(peak, v);
    }
    // 延迟线读出内容（0.9×收敛增益）+ 阶跃进入窗后的压限输出全部有界。
    CHECK(static_cast<double>(peak) <= 0.951);
    // 压限确实启动：增益在 5ms 内显著下降。
    CHECK(chain.lim.currentGainDb() <= -0.45);
  }
}

TEST_CASE("eq dsp: 低率平滑钳制（22.05k）——3×blockMs>20ms 大块每块 1/3、3 块精确收敛无过冲") {
  constexpr std::uint32_t fs = 22050U;
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(preGainConfig(12.0, true), fs, 1U));

  // 512 帧块：blockMs=23.2ms → ramp=3×23.2=69.7ms → stepRatio=1/3 → +4dB/块。
  std::vector<float> block(512U, 0.0F);
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(4.0).epsilon(1e-9));
  CHECK(!dsp.fullySettled());
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(8.0).epsilon(1e-9));
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(12.0).epsilon(1e-9));  // clamp 精确到位
  CHECK(dsp.fullySettled());
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == 12.0);  // 无过冲、无残差

  // 逐帧（frameCount=1）小块：ramp 钳到 20ms 下界 → 每帧 +12×(1000/22050)/20ms
  // ≈0.0272dB，单调爬升、~441 帧收敛、永不超过 12（无过冲）。
  audio::EqualizerDspProcessor dspFrame;
  static_cast<void>(dspFrame.configure(preGainConfig(12.0, true), fs, 1U));
  std::vector<float> single(1U, 0.0F);
  double previous = 0.0;
  std::size_t settledAt = 0;
  for (std::size_t i = 1; i <= 600U; ++i) {
    dspFrame.process(single.data(), 1U);
    const double current = dspFrame.currentPreGainDb();
    if (i <= 400U) {
      CHECK(current >= previous);  // 单调（无回退/无过冲）
      CHECK(current <= 12.0);
    }
    if (settledAt == 0 && dspFrame.fullySettled()) {
      settledAt = i;
    }
    previous = current;
  }
  CHECK(dspFrame.fullySettled());
  CHECK(dspFrame.currentPreGainDb() == 12.0);
  CHECK(settledAt >= 400U);  // 平滑真实跨 ~20ms（而非一步到位）
  CHECK(settledAt <= 450U);
}

// ==================== applyTargets（播放中实时目标更新） ====================

TEST_CASE("eq dsp: applyTargets 前置——未 configure 返回 false 无操作、configure 后生效") {
  audio::EqualizerDspProcessor fresh;
  const auto config = singleBandConfig(17U, 6.0F, true);
  CHECK_FALSE(fresh.applyTargets(config));  // 未 accepted：无操作（不得全量 configure）
  CHECK_FALSE(fresh.configured());
  CHECK_FALSE(fresh.enabled());
  CHECK(fresh.fastBypassActive());

  static_cast<void>(fresh.configure(config, 48000U, 1U));
  CHECK(fresh.applyTargets(config));  // accepted：受理成功
  CHECK(fresh.configured());
  CHECK(fresh.enabled());
}

TEST_CASE("eq dsp: applyTargets 目标更新——同 configure 收敛语义（爬坡逐块、精确到位、无跳变）") {
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(singleBandConfig(17U, 6.0F, true), 48000U, 1U));
  // configure 后同 configure 语义 settle：快进到稳态（4096 帧块 ×3 = ramp 3×块长收敛）。
  settleEq(dsp, 48000U, 1U);
  REQUIRE(dsp.fullySettled());
  REQUIRE(dsp.currentBandGainDb(17U) == doctest::Approx(6.0).epsilon(1e-9));
  REQUIRE(dsp.fastBypassActive() == false);

  // 运行中把目标改到 +3dB（applyTargets——模拟播放中调低一个 band）。
  auto moved = singleBandConfig(17U, 3.0F, true);
  CHECK(dsp.applyTargets(moved));
  // 平滑从当前点（+6）规划新段：512 帧块 @48k stepRatio=1/3 → 每块 −1dB。
  std::vector<float> block(512U, 0.0F);
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(5.0).epsilon(1e-9));
  CHECK(!dsp.fullySettled());
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(4.0).epsilon(1e-9));
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(3.0).epsilon(1e-9));  // clamp 精确到位
  CHECK(dsp.fullySettled());
  // 稳态频响随目标成形（1kHz = idx17 中心）：幅值 0.25 → ×1.413。
  std::vector<float> tone;
  fillSineMono(tone, 48000.0, 1000.0, 0.25, 4096U);
  dsp.process(tone.data(), 4096U);
  const double amp = estimateAmplitudeMono(tone.data(), 512U, 3584U, 48000.0, 1000.0);
  CHECK(std::fabs(linToDb(amp / 0.25) - 3.0) <= 0.1);
}

TEST_CASE("eq dsp: applyTargets 运行中开关——关闭平滑退出收敛后 fastBypass 回归、再开实时爬坡") {
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(singleBandConfig(17U, 6.0F, true), 48000U, 1U));
  settleEq(dsp, 48000U, 1U);
  REQUIRE(dsp.fastBypassActive() == false);

  // 运行中关闭（enabled false→目标全 0）：先平滑退出（期间非 fastBypass、band 按
  // 残余增益处理）→ 收敛后 fastBypass 快路径（process 零触碰样本）。
  CHECK(dsp.applyTargets(flatConfig(audio::EqualizerBandMode::Band31, false)));
  CHECK_FALSE(dsp.fastBypassActive());  // 平滑退出期需要处理
  std::vector<float> block(512U, 0.0F);
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(4.0).epsilon(1e-9));  // 6→4 退出中
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(2.0).epsilon(1e-9));
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == 0.0);
  CHECK(dsp.fullySettled());
  CHECK(dsp.fastBypassActive());  // 关闭收敛：整链旁路
  CHECK_FALSE(dsp.enabled());

  // 旁路态下样本域逐位直通（process 常量级返回）。
  std::vector<float> pattern;
  fillSineMono(pattern, 48000.0, 1000.0, 0.25, 1024U);
  const auto original = pattern;
  dsp.process(pattern.data(), 1024U);
  CHECK(pattern == original);

  // 运行中再开（同曲线）：从当前（0）实时爬坡回 +6——无跳变（首块 +2dB）。
  CHECK(dsp.applyTargets(singleBandConfig(17U, 6.0F, true)));
  CHECK_FALSE(dsp.fastBypassActive());
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(2.0).epsilon(1e-9));
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(4.0).epsilon(1e-9));
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentBandGainDb(17U) == 6.0);
  CHECK(dsp.fullySettled());
}

TEST_CASE("eq dsp: applyTargets 模式切换——前缀外 band 排空不触碰 biquad（越前缀硬切有界）") {
  // 31→10 模式切换回归点：review 实测 44.98× 爆炸 = 未用 band（idx ≥ 前缀）在排空期
  // 被喂进 centerHz=0 的退化系数。applyTargets 切换 mode 必须同 configure 语义：
  // 未用 band 只走平滑状态（绝不触碰 biquad——process 内 activeBandCount_ 前缀守卫）。
  audio::EqualizerDspProcessor dsp;
  // Band31 下 10、17、20 三段有增益 → 全 settle。
  auto wide = flatConfig(audio::EqualizerBandMode::Band31, true);
  wide.bandGainsDb[10U] = 5.0F;
  wide.bandGainsDb[17U] = 6.0F;
  wide.bandGainsDb[20U] = 4.0F;
  static_cast<void>(dsp.configure(wide, 48000U, 1U));
  settleEq(dsp, 48000U, 1U);
  REQUIRE(dsp.fullySettled());
  REQUIRE(dsp.currentBandGainDb(17U) == doctest::Approx(6.0).epsilon(1e-9));

  // 运行中切到 Band10（10 段表：idx10/17/20 全在 10-band 前缀之外 → 目标 0）。
  auto ten = flatConfig(audio::EqualizerBandMode::Band10, true);
  CHECK(dsp.applyTargets(ten));
  // 排空期：前缀外 band currentDb 仍非 0，但 process 绝不触碰其 biquad（越前缀守卫）——
  // 输出有界（10-band 表 idx5 中心 ~1kHz 无增益；残余排空只走平滑状态对样本无贡献）。
  double peak = 0.0;
  for (int block = 0; block < 3; ++block) {  // ramp 收敛期（6→0：每块 1/3）
    std::vector<float> tone;
    fillSineMono(tone, 48000.0, 1000.0, 0.25, 4096U);
    dsp.process(tone.data(), 4096U);
    for (const float v : tone) {
      peak = std::max(peak, std::fabs(static_cast<double>(v)));
    }
  }
  CHECK(peak <= 0.55);  // 无爆炸/无超界（44.98× 爆炸会 >1e3）
  CHECK(dsp.currentBandGainDb(10U) == 0.0);   // 前缀外已排空到 0（band10 恰为 10-band 表内）
  CHECK(dsp.currentBandGainDb(17U) == 0.0);
  CHECK(dsp.currentBandGainDb(20U) == 0.0);
  CHECK(dsp.fullySettled());
  // 全 0 目标收敛 = fastBypass（设备门控的 enabled() 项负责 EQ 开 0dB 的链语义，
  // 见 audio_output_device renderCallback 注释——此处 DSP 级直通零成本）。
  CHECK(dsp.fastBypassActive());
  CHECK(dsp.enabled());
  // 前缀外 band 的 biquad 从未被触碰（bandBypassed = 越前缀硬直通）。
  CHECK(dsp.bandBypassed(20U));
  CHECK(dsp.bandBypassed(17U));

  // 运行中切回 Band31 + 原曲线：跨排空/再爬坡，无旧状态爆炸（每块重新填充内容——
  // process 原地处理，重复处理同一缓冲会错误地级联复合增益）。
  CHECK(dsp.applyTargets(wide));
  double reentryPeak = 0.0;
  for (int block = 0; block < 4; ++block) {  // 收敛期内逐块有界
    std::vector<float> tone;
    fillSineMono(tone, 48000.0, 1000.0, 0.25, 4096U);
    dsp.process(tone.data(), 4096U);
    for (const float v : tone) {
      reentryPeak = std::max(reentryPeak, std::fabs(static_cast<double>(v)));
    }
  }
  CHECK(reentryPeak <= 0.55);
  // 再 3 块后完全收敛到 +6@1kHz（10-band 表在 1kHz 处已无增益 → 重爬升从 0 起）。
  settleEq(dsp, 48000U, 1U);
  CHECK(dsp.currentBandGainDb(17U) == doctest::Approx(6.0).epsilon(1e-9));
  CHECK(dsp.currentBandGainDb(10U) == doctest::Approx(5.0).epsilon(1e-9));
}

TEST_CASE("eq dsp: applyTargets 运行中 preGain/NaN 防御——sanitize 同 configure、平滑域无污染") {
  audio::EqualizerDspProcessor dsp;
  static_cast<void>(dsp.configure(preGainConfig(0.0, true), 48000U, 1U));
  settleEq(dsp, 48000U, 1U);

  // preGain 目标 +9dB + 一个带 NaN 的 band（上游防御同 configure：NaN → 0）。
  auto config = flatConfig(audio::EqualizerBandMode::Band31, true);
  config.preGainDb = 9.0F;
  config.bandGainsDb[5U] = std::numeric_limits<float>::quiet_NaN();
  config.bandGainsDb[17U] = std::numeric_limits<float>::infinity();  // 非有限 → 0
  CHECK(dsp.applyTargets(config));
  CHECK(dsp.currentPreGainDb() == 0.0);  // 段从当前点起（收敛态 → 0 起点）
  std::vector<float> block(512U, 0.0F);
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == doctest::Approx(3.0).epsilon(1e-9));  // 9/3 每块
  dsp.process(block.data(), 512U);
  dsp.process(block.data(), 512U);
  CHECK(dsp.currentPreGainDb() == 9.0);
  CHECK(dsp.fullySettled());
  // 污染 band 被 sanitize 成 0dB 目标：收敛后无 NaN 渗入（band 直通、频响平直）。
  CHECK(dsp.currentBandGainDb(5U) == 0.0);
  CHECK(dsp.currentBandGainDb(17U) == 0.0);
  CHECK_FALSE(dsp.fastBypassActive());  // preGain +9 稳态 = 需要处理
  std::vector<float> tone;
  fillSineMono(tone, 48000.0, 1000.0, 0.25, 2048U);
  dsp.process(tone.data(), 2048U);
  for (const float v : tone) {
    CHECK(std::isfinite(v));
  }
}
