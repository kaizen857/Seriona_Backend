#pragma once

// 均衡器 DSP 纯核心（任务 22，B2.1）——内部实现单元，非公共 API。
//
// 定位：与设备/回调对象/device 格式完全解耦的 f32 图形均衡处理核心。输入 =
// audio::EqualizerConfig + 采样率 + 声道数；输出 = 原地处理后的 interleaved f32
// 块。帧级可测：process 接受任意 frameCount（含 1），平滑按「块」推进，逐帧调用
// 即逐帧推进——任务 24/28 的帧级断言直接以本单元为被测面。
//
// 处理链（每块）：平滑推进（dB 域）→ 前置增益 → 逐 band peaking 滤波串接。
// 每 band 一个 ma_peak2（miniaudio v0.11.25；ma_biquad 内部持每声道状态，interleaved
// 原位处理）。非 0dB/未越界的 band 才调用 biquad；其余按直通纪律跳过——见下方
// 「直通纪律」。
//
// 直通纪律（决定任务 25 集成质量：无咔哒 + 出厂逐位不变主路径）：
//  1. 越界 band（f_center ≥ 0.95×fs/2，configure 期一次判定）：硬直通 + 告警面
//     （nyquistBypassBandCount / bandRangeBypassed）暴露给集成方，告警由集成方按
//     configure 粒度打点（本单元零日志、零每块输出，天然不刷屏）。
//  2. 0dB band 平滑收敛后硬直通（currentDb==targetDb==0 → 跳过 biquad 调用）。
//  3. enabled=false：目标全部归 0dB，先平滑退出（≤ ramp 时长）再整体旁路。
//  4. mode 切换后未用 band（Band10 的 11–31）同 0dB 直通纪律。
//  5. 全零稳态（出厂直通 / 关闭收敛完成）走 fastBypass 快路径：每块常量级判断即
//     返回，不触碰样本——接入后主路径与改动前逐位一致。
//  直通期间平滑状态照常推进（biquad 内部状态冻结于最近活动值），再激活无咔哒。
//
// 参数平滑（dB 域线性斜坡）：目标变化（configure）时记 segmentDelta = target − current；
// 之后每块推进固定步进 segmentDelta × min(1, blockMs/rampMs)，越过目标即精确到位
// （无残差），其中 blockMs = 1000×frameCount/fs，rampMs = max(20ms, 3×blockMs)，
// 分档交叉点 = 3×blockMs = 20ms，即 blockMs ≈ 6.67ms：
//  - 小块（blockMs ≤ ~6.67ms，3×blockMs ≤ 20ms）→ ramp = 20ms，总收敛时长精确
//    20ms（每块步进 = 块长/20ms×满步，即规格第一句语义）；
//  - 常规/大块（3×blockMs > 20ms，如 512 帧 @44.1k = 11.6ms → ramp ≈ 34.8ms）
//    → ramp = 3×块长，每块推进 1/3 段位移、约 3 块收敛，步进按块时长钳制
//    （规格第二句语义；大块场景即 fs ≲ 25.6k @512 帧块）。
// 收敛即精确置位（clamp，无残差）；平滑状态在 fs 变化/重建间保留；目标再次变化
// 时从当前点重新规划新段（不跳变）。
//
// 系数重算：仅在每块开头当前增益相对上次已应用值变化（或系数参数失效）时调用
// ma_peak2_reinit（miniaudio 语义 = 只换归一化系数、保留滤波状态），随后原地
// ma_peak2_process_pcm_frames。稳态下每块至多一次 band 判断、零 reinit。
//
// 线程契约：configure 允许分配（首次/声道或采样率变化时重建 ma_peak2 组），
// 只允许在与 process 不并发的时刻调用（任务 25 在音频 worker 侧、设备未活动期
// 调用，同 GainEnvelopeController 的 worker 侧账本先例）；process 实时安全：
// 不分配、不加锁、不日志、不抛异常（noexcept）。
//
// 依赖单点：band 中心频率与 Q 常量引用 inc/seriona/audio/equalizer_tables.h
// （任务 18 公共单点，禁拷贝）；配置形状引用 audio_contracts.h 契约类型
// EqualizerConfig/EqualizerBandMode（任务 15）。限幅器（limiterEnabled）不属本
// 单元职责（任务 23）；s16/s24/s32→f32 转换辅助与 device 集成属任务 24/25。
//
// 编译/链接注意：本文件引用 miniaudio 类型与 ma_peak2_* 符号。miniaudio 单头
// 实现（MINIAUDIO_IMPLEMENTATION）由 miniaudio_output_device_backend.cpp 单点
// 提供——直接编译本单元的测试/工具必须同时编入该 TU（仓库 dualsource 等设备
// 测试先例同构）或在某 TU 定义 MINIAUDIO_IMPLEMENTATION。

#include "seriona/audio/audio_contracts.h"
#include "seriona/audio/equalizer_tables.h"

#include <miniaudio.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace seriona::audio {

// f32 图形均衡纯处理核心（上述头注释的完整纪律为本类行为契约）。
class EqualizerDspProcessor {
public:
  EqualizerDspProcessor();
  ~EqualizerDspProcessor();

  EqualizerDspProcessor(const EqualizerDspProcessor&) = delete;
  EqualizerDspProcessor& operator=(const EqualizerDspProcessor&) = delete;

  // 配置报告：configure 一次调用的结果摘要，供集成方（任务 25）决定告警与状态
  // 发布（warn-once 语义 = 集成方按此报告在 configure 粒度打点，非每块）。
  struct ConfigReport {
    bool accepted = false;                 // fs/channels 合法且已生效
    std::uint32_t sampleRate = 0;          // 生效采样率
    std::uint32_t channelCount = 0;        // 生效声道数
    bool enabled = false;                  // 生效总开关
    EqualizerBandMode mode = EqualizerBandMode::Band10;
    std::size_t activeBandCount = 0;       // mode 生效前缀 band 数（10 或 31）
    std::size_t nyquistBypassBandCount = 0; // 越界硬直通 band 数（f_center ≥ 0.95×fs/2）
  };

  // 生效配置 + DSP 域采样率/声道数。fs 或声道数变化时重建滤波器组（平滑状态与
  // 目标保留，滤波历史清零）；仅配置变化时不重建（换系数走 process 内 reinit，
  // 滤波状态保留）。防御：band/preGain 增益 sanitize 到 ±15dB（NaN→0）。
  [[nodiscard]] ConfigReport configure(const EqualizerConfig& config,
                                       std::uint32_t sampleRate,
                                       std::uint32_t channelCount);

  // 处理一个 interleaved f32 块（原地，in-place——miniaudio biquad 显式支持）。
  // frameCount==0 或未配置/不可用态为无害空操作。实时安全，noexcept。
  void process(float* interleavedFrames, std::uint32_t frameCount) noexcept;

  // ---- 可观测状态（测试 / 集成读回；不参与实时数据路径）----

  [[nodiscard]] bool configured() const noexcept { return accepted_; }
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  [[nodiscard]] EqualizerBandMode mode() const noexcept { return mode_; }
  [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }
  [[nodiscard]] std::uint32_t channelCount() const noexcept { return channelCount_; }

  // 平滑全收敛（每 band 与前置增益均 current==target）。
  [[nodiscard]] bool fullySettled() const noexcept;

  // 当前平滑增益（dB）。测试逐帧断言平滑曲线的可观测面。
  [[nodiscard]] double currentPreGainDb() const noexcept { return preGainCurrentDb_; }
  [[nodiscard]] double currentBandGainDb(std::size_t bandIndex) const noexcept;

  // band 当前是否硬直通（跳过 biquad）：未用（超出 mode 前缀）/越界/0dB 收敛
  // （current 与 target 均 0）。目标非 0 的激活前瞬间不属直通——平滑期按残余
  // 增益照常处理（首个 process 块推进后即参与滤波）。
  [[nodiscard]] bool bandBypassed(std::size_t bandIndex) const noexcept;

  // band 是否因奈奎斯特越界硬直通（告警面；其余直通原因不含在此）。
  [[nodiscard]] bool bandRangeBypassed(std::size_t bandIndex) const noexcept;

  // 出厂直通稳态（全零、无任何处理状态可动）：当前 process 走零开销快路径。
  [[nodiscard]] bool fastBypassActive() const noexcept { return fastBypass_; }

private:
  struct BandState {
    ma_peak2 filter{};                 // 该 band 全声道 peaking 滤波器（内部每声道状态）
    bool initialized = false;          // ma_peak2 已 init（含状态堆）
    double currentDb = 0.0;            // 平滑中当前应用的增益（dB）
    double targetDb = 0.0;             // 平滑目标（dB，0 = 直通目标）
    double segmentDeltaDb = 0.0;       // 本段线性斜坡总位移（自上次目标变化起，见 advanceOne）
    double appliedDb = 0.0;            // 滤波器系数当前对应的增益（dB）
    bool coeffStale = true;            // 系数参数（fs/mode 中心/Q）已变，处理前强制 reinit
    bool rangeBypass = false;          // 越界硬直通（configure 期按 fs 一次判定）
  };

  static constexpr double kSmoothingMinMs = 20.0;  // 平滑时长下界（规格：~20ms）
  static constexpr double kSmoothingBlockMultiplier = 3.0;  // 低 fs 大块：ramp = 3×块长
  static constexpr double kNyquistBypassRatio = 0.95;  // f_center ≥ 0.95×(fs/2) 越界
  static constexpr double kGainSanitizeDb = 15.0;     // 防御钳制（reducer 已拒 ±15 越界）

  static constexpr std::size_t kBandCount = kEqualizer31BandCenterHz.size();  // 31

  [[nodiscard]] double bandCenterHz(std::size_t bandIndex) const noexcept;
  [[nodiscard]] double bandQ(std::size_t bandIndex) const noexcept;
  [[nodiscard]] static double sanitizeDb(float db) noexcept;

  void rebuildFilters();
  void advanceSmoothing(double stepRatio) noexcept;
  [[nodiscard]] static bool advanceOne(double& current,
                                       double target,
                                       double segmentDelta,
                                       double stepRatio) noexcept;
  [[nodiscard]] double stepRatioForBlock(double blockMs) const noexcept;
  void updateFastBypass() noexcept;

  EqualizerConfig config_{};             // 最近一次生效配置（快照回读）
  std::array<BandState, kBandCount> bands_{};
  double preGainCurrentDb_ = 0.0;        // 前置增益平滑当前值（dB）
  double preGainTargetDb_ = 0.0;         // 前置增益平滑目标（dB）
  double preGainSegmentDeltaDb_ = 0.0;   // 前置增益本段线性斜坡总位移（dB）
  std::uint32_t sampleRate_ = 0;         // DSP 域采样率（configure 生效值）
  std::uint32_t channelCount_ = 0;
  EqualizerBandMode mode_{EqualizerBandMode::Band10};
  std::size_t activeBandCount_ = 0;      // 按 mode 的前缀 band 数
  bool enabled_ = false;                 // 生效总开关（镜像 config_）
  bool accepted_ = false;                // configure 接受过合法 fs/channels
  bool smoothingActive_ = false;         // 任一平滑状态未收敛
  bool fastBypass_ = true;               // 全零稳态：process 常量级直通
};

}  // namespace seriona::audio
