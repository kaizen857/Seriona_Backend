#pragma once

// FFT 频谱分析纯核心（任务 30，B3.2）——内部实现单元，非公共 API。
//
// 定位：与 service / 设备类型完全解耦的「摘录帧 → 60 对数桶 dB 频谱」分析核心。
// 输入由调用方逐帧喂入（feed），帧元数据（generation / sampleRate / frameCount /
// domain）随帧提供，无任何 AudioOutputDevice / SingleTrackAudioPlaybackService
// 依赖——构造即用，任务 31 直接以纯组件面喂帧测试。
//
// 处理链（每满窗一次）：Hann 周期窗 → av_tx 前向 RDFT（AV_TX_FLOAT_RDFT，实测
// 非归一 DFT：δ0 → 全 1、bin 正弦峰值 = A×n/2）→ 相干增益归一 → 60 对数桶
// （20 Hz–20 kHz，边界 20×1000^(i/60)，i=0..60——与任务 15 的 181 点曲线轴
// 20×1000^(i/180) 同族对数约定）能量求和 → dB（满刻度正弦 ≈ 0 dBFS 的相对电平）。
//
// 窗长/重叠（率相关，低频分辨率 ~20-25 Hz/bin 恒定）：
//   fs ≤ 24k      → n = 1024（如 22050：Δf = 21.5 Hz）
//   24k < fs ≤ 48k → n = 2048（44.1k：21.5 Hz；48k：23.4 Hz）
//   48k < fs ≤ 96k → n = 4096（96k：23.4 Hz）
//   fs > 96k       → n = 8192（192k：23.4 Hz——长窗档，"192k 正确"含频率轴标定）
//   50% 重叠（hop = n/2）：每消耗 hop 帧产出一份分析；帧率分析耗材对应
//   hop/fs（44.1k → 23.2 ms 一份的观察耗材）——worker 侧实际发布节奏受
//   50 ms 节流 × 摘录块周期双重限制（见任务 30 notes 记录）。
//
// 失效/重建纪律：
//   - generation 或 sampleRate 变化（设备重建/换率）→ 弃全部窗累积与跨代状态，
//     按新率重建（Hann 表 / FFT 上下文 / 桶频率表）——防混率帧被消费；
//   - domain 变化 → 弃未满窗累积（不弃率/FFT 状态）：单窗内样本恒同域，避免
//     跨域电平混窗；dB 标定保持相对满刻度（两域帧均为满刻度归一语义；链激活域
//     含 preGain，可能 >0 dBFS——属该域真实电平，显示端按域注明）。
//
// dB 标定细节（任务 31 数值锚复算依据）：
//   X_k = 前向 RDFT 输出（复排 out[2k]/out[2k+1]），归一 X'_k = 2×X_k/Σw
//   （Σw = Hann 周期窗和 = n/2；相干增益归一：窗内满刻度正弦峰值 ≈ 幅度 A）。
//   桶 i 功率 = Σ_k |X'_k|²，k 覆盖 k×fs/n ∈ [f_i, f_{i+1}) 的全部 FFT bin
//   （k = 1..n/2−1，DC 与奈奎斯特点不计入；n/2−1 点 k×fs/n < fs/2 恒成立）。
//   binsDb[i] = 10×log10(功率)；静音/不可测桶置 kFloorDb（-120 dB，"静音标记"）。
//   fs < 40k（22050/32000）：桶轴按 fs/2 截断——binMeasurable 判 lower edge
//   < fs/2；整桶在奈奎斯特之上的桶输出 kFloorDb。解析：满刻度 bin 内正弦
//   |X'_k|² = A² → 0 dB；Hann 泄漏使邻近 ±1 bin 有能量，桶内含峰值 bin 时
//   读数 ≈ 10log10(A²×(1+少量泄漏))，故纯单音 ≈ 0 dB、宽带噪声按带宽积分。
//
// 编译/链接注意：本文件引用 av_tx（<libavutil/tx.h>）与 av_calloc/av_free
// （<libavutil/mem.h>）。seriona_audio 已 PUBLIC 链接 PkgConfig::SERIONA_FFMPEG
// （含 libavutil，根 CMakeLists）——库内编译零额外依赖。直接编译本单元的测试/
// 工具必须另行链接 PkgConfig::SERIONA_FFMPEG（或至少 -lavutil）。禁止手动
// include /usr/include/ffmpeg4.4（陈旧 4.4 头无 av_tx，ABI 错配，见 b0-2 notes）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

struct AVTXContext;

namespace seriona::audio {

// 帧电平语义域标注（自有枚举，值 = AudioOutputDeviceCaptureDomain 映射；分析器
// 不感知设备类型，仅保证「域变化弃未满窗」与「输出透传域值」两语义）。
enum class SpectrumDomainTag : std::uint8_t {
  ChainInactiveOutput = 0,  // 链关闭态：设备输出域（volume/muted/混音已应用）
  ChainActiveF32 = 1,       // 链激活态：EQ(含 preGain) 后、volume 前 f32 中间域
};

// 单次喂入帧（调用方组装；frameCount == 0 或 samples == nullptr 为无害空操作）。
struct SpectrumFeedFrame {
  std::uint32_t generation = 0;    // 设备重建纪元；变化 → 全状态重建
  std::uint32_t sampleRate = 0;    // 帧采样率（Hz）；0 或变化 → 全状态重建
  std::uint32_t frameCount = 0;    // samples 有效样本数
  SpectrumDomainTag domain{SpectrumDomainTag::ChainInactiveOutput};
  const float* samples = nullptr;  // 单声道 f32，frameCount 个有效样本
};

// 一次完整分析产出（feed 命中整窗时返回，每 feed 至多返回最近一份）。
struct SpectrumAnalysis {
  std::uint32_t generation = 0;    // 本分析所属帧纪元（透传）
  std::uint32_t sampleRate = 0;    // 本分析采样率
  SpectrumDomainTag domain{SpectrumDomainTag::ChainInactiveOutput};
  std::array<float, 60> binsDb{};  // 60 对数桶 dB（-120 = 静音/不可测标记）
};

// f32 单声道摘录帧 → 60 对数桶 dB 频谱分析核心（上述头注释为完整行为契约）。
class SpectrumAnalyzer {
public:
  SpectrumAnalyzer();
  ~SpectrumAnalyzer();

  SpectrumAnalyzer(const SpectrumAnalyzer&) = delete;
  SpectrumAnalyzer& operator=(const SpectrumAnalyzer&) = delete;
  SpectrumAnalyzer(SpectrumAnalyzer&&) = delete;
  SpectrumAnalyzer& operator=(SpectrumAnalyzer&&) = delete;

  // 喂入一帧（noexcept，允许任意 frameCount ≥ 0）。内部按需累积窗样本；每消耗
  // hop（= 窗长一半，50% 重叠）帧产出一份分析并返回最近一份。generation/
  // sampleRate/domain 变化时自动完成对应失效重建（见头注释）。未产生完整分析
  // 返回 std::nullopt。
  [[nodiscard]] std::optional<SpectrumAnalysis> feed(const SpectrumFeedFrame& frame) noexcept;

  // 全状态清零（窗累积/域/率/Hann/FFT/桶表全部重建于下次喂入；不抛异常）。
  void reset() noexcept;

  // ---- 常量与纯函数（测试/文档/集成可读面；不参与状态）----
  static constexpr std::size_t kBinCount = 60;  // 对数桶数
  static constexpr float kFloorDb = -120.0F;    // 静音/不可测桶 dB 标记
  static constexpr float kMinLogHz = 20.0F;     // 对数轴下界（Hz）
  static constexpr float kMaxLogHz = 20000.0F;  // 对数轴上界（Hz）

  // 按采样率选的 FFT 窗长（2 的幂；见头注释分档；sampleRate == 0 返回 0 = 不合法）。
  [[nodiscard]] static std::uint32_t windowSizeForRate(std::uint32_t sampleRate) noexcept;

  // 对数桶边界频率：binEdgeHz(i) = 20×1000^(i/60)，i = 0..60（桶 i 覆盖
  // [edge(i), edge(i+1))；edge(60) = 20000）。
  [[nodiscard]] static float binEdgeHz(std::size_t binIndex) noexcept;
  // 桶几何中心：20×1000^((i+0.5)/60)（测试正弦落桶判定的文档面）。
  [[nodiscard]] static float binCenterHz(std::size_t binIndex) noexcept;

  // 桶是否可测：lower edge < fs/2（fs < 40k 时桶轴按 fs/2 截断，整桶在奈奎斯特
  // 之上的桶不可测 → 输出 kFloorDb）。fs 0 返回 false。
  [[nodiscard]] static bool binMeasurable(std::size_t binIndex, std::uint32_t sampleRate) noexcept;

  // 可测最高桶下标上限（含）：= 最大的 i 使 binEdgeHz(i) < fs/2；无则返回
  // kBinCount（此时全桶不可测）。纯文档/测试面。
  [[nodiscard]] static std::size_t lastMeasurableBin(std::uint32_t sampleRate) noexcept;

private:
  struct Impl;  // 状态与 FFmpeg 句柄（不透明持有，见 cpp）
  Impl* impl_;
};

}  // namespace seriona::audio
