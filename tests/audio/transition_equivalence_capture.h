#pragma once

#include "seriona/audio/audio_contracts.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace seriona::audio {

// 任务 13 默认值等价总闸（.omo/plans/fade-crossfade-output-engine.md 任务 13）：
// 在 9 项过渡默认（= TransitionConfig 默认构造 = "旧行为等价"）下，把改动前基线
// （Seriona_Backend f5b1f5c，不含任何过渡语义）与当前树各自重放同一输入序列，输出
// 缓冲必须逐样本/逐字节相等。本驱动不调用任何过渡配置/事件（configureTransition、
// abortTransition、2 参 prepareNext、EndApproaching/AdvanceCompleted/PrepareNext* 在
// 基线均不存在——两树可编译是硬约束），9 项默认即录制前提。
//
// 录制路径（既有行为集）：
//   playthrough    固定段播到自然播完（EOF 自然结束）
//   pause_resume   播放中暂停/恢复（50ms + 50ms）
//   seek           播放中 seek（50ms + 700ms 起 50ms）
//   switch_mixed   Mixed 同参切歌（默认档关 = 瞬时硬切；T7 免重开语义）
//   switch_direct  Direct 切歌（恒重开 + 硬切）——输出格式恒为轨道原生参数
// 每条路径按请求的输出采样格式录制（Mixed 四档；Direct 仅一档 = 流原生 Int16）。
// 渲染完全由测试线程 consumeFrames 驱动（fake 设备回调），以 ≈ 实时节奏推进——
// 两树任何 worker 命令处理窗口都不会制造人工欠载（见 learnings T11 实时节奏教训）。

struct EquivCaptureOptions {
  std::vector<AudioSampleFormat> formats{AudioSampleFormat::Float32};
  bool includeDirectSwitch{true};
  std::filesystem::path fixtureDir{};  // 空 = current_path()/"generated_task13_fixtures"
};

struct EquivCapture {
  std::vector<std::uint8_t> bytes;  // 渲染回调输出的原始交错字节（含尾块零填充）
  AudioSampleFormat format{AudioSampleFormat::Float32};
  std::uint32_t sampleRate{0};
  std::uint16_t channelCount{0};
  std::uint64_t renderedFrames{0};
  bool underrunObserved{false};  // 捕获期内出现 BufferUnderrun = 有效性质疑（须为 false）
};

using EquivCaptureMap = std::map<std::string, EquivCapture>;

EquivCaptureMap runEquivalenceCaptures(const EquivCaptureOptions& options);

// 捕获键 → 描述（供错误报告）。键 = "<scenario>_<FormatName>"（Direct 无格式后缀）。
std::string equivalenceCaptureKey(AudioSampleFormat format, const char* scenario);
std::string formatName(AudioSampleFormat format);

}  // namespace seriona::audio
