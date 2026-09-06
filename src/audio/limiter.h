#pragma once

// 限幅器 DSP 纯核心（任务 23，B2.2）——内部实现单元，非公共 API。
//
// 定位：与设备/回调对象/device 格式完全解耦的 f32 输出限幅处理核心。输入 =
// audio::EqualizerConfig（仅消费 limiterEnabled 作开关源；config.enabled 即 EQ
// 主开关不消费——任务 25 集成链在 EQ 链激活时才运行本单元，主开关在链外门控）
// + 采样率 + 声道数；输出 = 原地处理后的 interleaved f32 块。帧级可测：process
// 接受任意 frameCount（含 1），全部平滑/检测按「样本」逐样本推进，逐帧调用即
// 逐样本推进，与分块方式无关（同输入同状态的逐样本轨迹与块大小无关——
// 任务 28 可整块/逐帧两法断言同一结果）。
//
// 信号链：输入 →（写入）环形延迟线 D 样本 → 读出样本 × 共享平滑增益 → 输出。
// 即限制生效期间整链恒加 D 样本延迟（lookahead 的本体）；关→开的预热直通与
// 开→关的交叉淡化见「开关过渡」节。
//
// 检测器（lookahead 窗内最大）：每声道维护「已写入未读出的延迟线内容」的滑窗
// 最大值（窗口 = 未来 D 个输出样本将读出的内容，即预看窗——读出样本的增益在
// 其写入时刻由含其自身及后续样本的窗口决定，因此增益对即将到来的峰值提前
// 反应）。实现为单调 deque（按幅值降序 + 序号过期）：逐样本 O(1) 摊还，configure
// 期预分配固定数组，process 零分配。窗最大 M_c 为线性幅值；跨声道取帧内各声道
// 窗最大之最大 M = max_c M_c 驱动**单一**共享增益平滑器（多声道同增益，
// 检测取全声道最大——L/R 任一超阈两声道同压，声道间无增益不一致）。
//
// 增益计算机（dB 域，逐样本指数平滑）：
//   阈值 thresholdLin = 10^(-0.5/20) ≈ 0.9440609（-0.5 dBFS，规格值）。
//   targetDb = M > thresholdLin ? min(0, 20·log10(thresholdLin / M)) : 0
//   （M ≤ 阈值 → 0dB 直通目标；超阈 → 压到恰 ≤ 阈值的限幅比增益，无下限——
//   硬限幅性质「输出 ≤ 阈值」优先，f32 域输入有界则增益有界）。
//   平滑：逐样本向 targetDb 逼近，系数按方向取——下坡（target < current，压限
//   启动）用 attack τ=1ms；上坡（target > current，恢复）用 release τ=100ms；
//   每样本一步：current += (target - current)·α_dir，α = 1 - exp(-1/(τ·fs))
//   在 configure 期按 fs 预计算（逐样本精确指数：k 样本后残差 = e^(-k/(τ·fs))，
//   无块级近似）。|target-current| < 1e-6 dB 精确置位（收敛可测）。
//   gainLin = 10^(currentDb/20)（dB 域平滑 → 线性标量）。
//   稳态压限实测特性（fs ≤ 192k）：超阈峰值在窗内停留 ≥ 5ms 而 attack 仅 1ms
//   （5τ），峰值读出的增益残差 ≈ e^(-5) ≈ 0.7%，即稳态输出峰值 ≤ -0.5dBFS +
//   ~0.06dB 量级（对 +15dB 极端输入残差比例同 0.7%，绝对超调 ~0.3dB 量级——
//   lookahead 原理保证先压后出，无瞬时击穿）。fs > 192k 时 lookahead 按 192k
//   封顶换算（实际 2.5ms = 2.5τ），启瞬残差相应放大（实测 5.0 尖峰超调
//   ~+1.2dB 量级，48k 同输入仅 ~+0.1dB）——规格封顶的固有结果，非实现缺陷。
//
// 延迟线：每声道 D = lround(min(fs, 192000) × 0.005) 样本（名义 rate×5ms，
// fs > 192k 封顶 192k 换算：384k → 960 样本 = 实际 2.5ms）。实际 lookahead 值
// 经 ConfigReport.lookaheadSamples / actualLookaheadMs 与
// actualLookaheadSamples() getter 声明（封顶时的 5ms→2.5ms 偏差可测面）。
//
// 开关过渡（本单元核心语义，开关源 = configure 的 limiterEnabled）：
//  关→开（limiterEnabled false→true）：预热直通——开后的前 D 个输出样本直通
//   输出（同时填充延迟线，检测/增益照常工作，增益部分生效），第 D+1 样本起接
//   真 D 延迟路径。即启用立即出声、无 D 样本延迟哑音；代价 = 启用瞬间一次
//   ≤D 样本的时间拼接（直通段与延迟段衔接处的单次 splice，文档化接受）。
//  开→关（true→false）：3ms（居中于规格 2-5ms）线性交叉淡化——输出 =
//   f·(延迟限幅路径) + (1-f)·直通路径，f: 1→0；淡毕 f=0 进入全旁路快速路径
//   （process 常量级判断即返回，不触碰样本——接入后限幅关闭态与改动前逐位
//   一致）。淡化保证两路径时间错位 D 的切换无端点阶跃（无咔哒）；淡化期间
//   延迟线/检测照常维护（内容保活，若中途再启用则回退到原处理路径——回切瞬间
//   含 (1−f)·(直通−湿) 残差阶跃，f 中途时幅度可观；规格只要求回退不要求零
//   阶跃，本单元保证无状态死锁、增益路径连续）。
//  旁路期间不维护延迟线（真零成本）；旁路中再启用一律走上述预热直通。
//  过渡为逐样本状态机（Bypass/WarmUp/Active/FadingOut），跨块边界无状态泄漏。
//
// 线程契约：configure 允许分配（首次/fs/声道数变化时重建全部数组；平滑系数
// 按新 fs 重算），只允许在与 process 不并发的时刻调用（任务 25 在音频 worker
// 侧、设备未活动期调用，同 eq_dsp 先例）；开关过渡**在 process 内**完成（配置
// 变化只置目标/触发状态迁移，淡化与预热逐样本执行）。process 实时安全：
// 不分配、不加锁、不日志、不抛异常（noexcept）。fs/声道数变化 = 全状态重建
// （延迟线/检测/增益清零——设备重建语境下任务 26 语义同 eq_dsp 的滤波历史
// 清零；开关目标态随 configure 重新生效）。
//
// 依赖：仅 audio_contracts.h 的 EqualizerConfig（类型引用）——**零 miniaudio
// 依赖**（延迟线/deque/平滑全自研，测试挂载无需链接 ma 实现符号）。
// 集成挂接点（任务 25）：EQ 处理链末级（…→preGain→音量→本单元→量化）；
// 限幅关且收敛走本单元快速路径，不影响链上其它级。

#include "seriona/audio/audio_contracts.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace seriona::audio {

// f32 输出限幅纯处理核心（上述头注释的完整纪律为本类行为契约）。
class LimiterDspProcessor {
public:
  LimiterDspProcessor();
  ~LimiterDspProcessor();

  LimiterDspProcessor(const LimiterDspProcessor&) = delete;
  LimiterDspProcessor& operator=(const LimiterDspProcessor&) = delete;

  // 配置报告：configure 一次调用的结果摘要，供集成方（任务 25）发布生效配置。
  // lookaheadSamples / actualLookaheadMs 声明**实际**延迟线样本数与毫秒
  // （名义 rate×5ms，fs > 192k 封顶 192k 换算后实际 < 5ms——如 384k → 960
  // 样本 = 2.5ms）。
  struct ConfigReport {
    bool accepted = false;            // fs/channels 合法且已生效
    std::uint32_t sampleRate = 0;     // 生效采样率
    std::uint32_t channelCount = 0;   // 生效声道数
    bool enabled = false;             // 生效总开关（limiterEnabled）
    std::uint32_t lookaheadSamples = 0;  // 实际延迟线样本数（封顶换算后）
    double actualLookaheadMs = 0.0;      // 实际 lookahead 毫秒（封顶后 < 名义 5ms）
  };

  // 生效配置 + DSP 域采样率/声道数。开关源 = config.limiterEnabled。fs 或声道数
  // 变化时全状态重建（延迟线/检测/增益清零；开关目标态重新生效）；仅开关变化
  // 时置目标态、迁移由 process 内的逐样本状态机执行（预热/淡化不在此完成）。
  // 非法参数（fs/channels 为 0）不接受：保持既有已接受态不动。
  [[nodiscard]] ConfigReport configure(const EqualizerConfig& config,
                                       std::uint32_t sampleRate,
                                       std::uint32_t channelCount);

  // 处理一个 interleaved f32 块（原地，in-place）。frameCount==0、未配置/不可用
  // 态、或旁路收敛态（关闭且过渡完成）为无害空操作（快速路径不触碰样本）。
  // 实时安全，noexcept。
  void process(float* interleavedFrames, std::uint32_t frameCount) noexcept;

  // ---- 可观测状态（测试 / 集成读回；不参与实时数据路径）----

  [[nodiscard]] bool configured() const noexcept { return accepted_; }
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }  // limiterEnabled 目标态
  [[nodiscard]] std::uint32_t sampleRate() const noexcept { return sampleRate_; }
  [[nodiscard]] std::uint32_t channelCount() const noexcept { return channelCount_; }

  // 实际延迟线样本数（lround(min(fs,192000)×0.005)）——封顶换算后的声明面。
  [[nodiscard]] std::uint32_t actualLookaheadSamples() const noexcept {
    return lookaheadSamples_;
  }

  // 当前活动状态（任务 28 逐样本断言过渡行为的观测面）。
  enum class Activity {
    Bypass,     // 关闭且过渡完成：process 不触碰样本（快速路径）
    WarmUp,     // 关→开预热直通中（前 D 输出样本直通 + 填充延迟线）
    Active,     // 正常限幅（真 D 延迟路径）
    FadingOut,  // 开→关 3ms 交叉淡化中
  };
  [[nodiscard]] Activity activity() const noexcept { return activity_; }

  // 全收敛：增益平滑收敛（currentDb 精确 == targetDb）且不在预热/淡化过渡中。
  // 稳态压限时窗最大随内容微动，targetDb 有微小扰动 → 收敛判定面向增益已稳定
  // （|current-target| 已 snap）；直通/旁路稳态下恒为 true。
  [[nodiscard]] bool fullySettled() const noexcept;

  // 当前平滑增益（dB，≤0）。测试逐样本/逐帧断言平滑曲线的可观测面。
  [[nodiscard]] double currentGainDb() const noexcept { return currentGainDb_; }

  // 当前是否全旁路快速路径生效（process 本次调用零工作）。
  [[nodiscard]] bool bypassActive() const noexcept {
    return activity_ == Activity::Bypass;
  }

private:
  // 滑窗最大值（单调 deque，configure 期分配，process 零分配）。
  // 窗口中只保留「可能成为未来窗最大」的候选——按幅值单调递减：push 时尾部
  // 弹掉被新样本支配（≤ 新幅值）的旧候选；头部按写入序号过期（seq + D ≤ 已写
  // 序号 ⇔ 该样本已读出/离开窗口——最旧者只可能在头部，见实现注释）。max =
  // 头部；空（head==tail）max = 0。mag 与 seq 同槽成对。数组容量 D+1（环形
  // 头尾重合只表示空，live ≤ D < 容量无冲突）。
  struct WindowMax {
    std::vector<float> mags;            // 候选幅值（环形，容量 D+1）
    std::vector<std::uint64_t> seqs;  // 候选写入序号（与 mags 同槽；64 位防 2^32 帧回绕）
    std::uint32_t head = 0;             // 头下标（最大候选）
    std::uint32_t tail = 0;             // 尾下标（最新候选的下一槽）
  };

  // 每声道状态：延迟线环形缓冲 + 滑窗检测器。process 内每样本至多 O(1) 触碰。
  struct ChannelState {
    std::vector<float> ring;  // 延迟线（容量 D，f32）
    WindowMax window;
  };

  static constexpr double kThresholdDb = -0.5;            // 阈值（规格：-0.5 dBFS）
  static constexpr double kLookaheadMs = 5.0;             // 名义 lookahead（规格：rate×5ms）
  static constexpr double kLookaheadCapRateHz = 192000.0; // 封顶采样率（规格）
  static constexpr double kAttackMs = 1.0;                // attack（规格：~1ms）
  static constexpr double kReleaseMs = 100.0;             // release（规格：~100ms）
  static constexpr double kFadeOutMs = 3.0;               // 开→关淡化时长（规格 2-5ms 内）
  static constexpr double kConvergenceToleranceDb = 1e-6; // 收敛 snap 容差
  static constexpr double kUnityDb = 1e-9;                // current≈0dB 判定（跳过转换）
  static constexpr double kSettledBandDb = 0.05;          // fullySettled 增益带宽（窗最大随内容微动）

  void resetEngaged();  // 清延迟线/检测/增益（WarmUp 起点）
  // 阈值线性值 10^(-0.5/20)，编译期常量（避免运行时 pow）。
  static constexpr double thresholdLin() noexcept { return 0.9440608762859235; }

  EqualizerConfig config_;              // 最近一次生效配置快照（回读 enabled 用）
  std::vector<ChannelState> channels_;  // 每声道状态（configure 期 resize）
  double currentGainDb_ = 0.0;          // 平滑增益当前值（dB）
  double targetGainDb_ = 0.0;           // 增益目标（dB，≤0）
  double attackCoeff_ = 0.0;            // 1 - exp(-1/(τ_att·fs))，configure 预计算
  double releaseCoeff_ = 0.0;           // 1 - exp(-1/(τ_rel·fs))
  std::uint32_t sampleRate_ = 0;
  std::uint32_t channelCount_ = 0;
  std::uint32_t lookaheadSamples_ = 0;  // 实际延迟样本数（lround(min(fs,192k)×0.005)）
  std::uint64_t writeCount_ = 0;        // 本次启用（WarmUp 起）已写样本数（帧域，全声道共享；
                                        // 64 位防 2^32 帧回绕——回绕会使 ring 读槽位错位）
  std::uint32_t fadeRemaining_ = 0; // FadingOut 剩余样本数
  std::uint32_t fadeTotal_ = 0;     // FadingOut 总样本数（本次淡化的 f 分母）
  Activity activity_{Activity::Bypass};
  bool enabled_ = false;                // limiterEnabled 目标态（镜像 config_）
  bool accepted_ = false;               // configure 接受过合法 fs/channels
};

}  // namespace seriona::audio
