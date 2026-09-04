#include "seriona/audio/audio_playback_service.h"

#include "path_text.h"
#include "thread_priority.h"

#include "spdlog/spdlog.h"

#include "seriona/audio/buffer/pcm_buffer_queue.h"
#include "seriona/audio/clock/playback_clock.h"
#include "seriona/audio/device/audio_device_format_enumerator.h"
#include "seriona/audio/events/audio_event_dispatcher.h"
#include "seriona/audio/ffmpeg_audio_source.h"
#include "seriona/audio/ffmpeg_filter_pipeline.h"
#include "seriona/audio/playback_state_machine.h"
#include "transition/gain_envelope.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace {

constexpr auto kProgressPublishInterval = std::chrono::milliseconds{100};

// T6 归零判定阈值：包络读回为回调块末写回（粒度 ≈ 1/淡出帧数，块恰好止于轨迹终点
// 时读回 1/duration≈0，其后一块读回精确 0.0）；阈值 0.02 吸收该粒度且远小于淡出中段
// 增益——finishing ticker 仅在单程下行期间使用它，增益单调下降无假阳性。
constexpr float kFadeCompleteGainEpsilon = 0.02F;

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

std::uint32_t bufferFrameCount(std::uint32_t sampleRate, std::chrono::milliseconds duration) {
  const auto durationMs = std::max<std::chrono::milliseconds::rep>(duration.count(), 1);
  const auto frameCount = (static_cast<std::uint64_t>(sampleRate) * static_cast<std::uint64_t>(durationMs)) / 1000U;
  return static_cast<std::uint32_t>(std::max<std::uint64_t>(frameCount, 1U));
}

std::string outputModeName(AudioOutputMode mode) {
  switch (mode) {
  case AudioOutputMode::Direct:
    return "direct";
  case AudioOutputMode::Mixed:
    return "mixed";
  }

  return "unknown";
}

std::string sampleFormatName(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::Int16:
    return "int16";
  case AudioSampleFormat::Int24:
    return "int24";
  case AudioSampleFormat::Int32:
    return "int32";
  case AudioSampleFormat::Float32:
    return "float32";
  case AudioSampleFormat::Unknown:
    return "unknown";
  }

  return "unknown";
}

std::string describeTarget(AudioOutputMode mode, const FfmpegFilterTargetFormat& target) {
  std::ostringstream description;
  description << outputModeName(mode) << ' ' << target.sampleRate << " Hz " << sampleFormatName(target.sampleFormat)
              << ' ' << target.channelCount << " ch";
  return description.str();
}

bool sameTarget(const FfmpegFilterTargetFormat& left, const FfmpegFilterTargetFormat& right) {
  return left.sampleRate == right.sampleRate && left.sampleFormat == right.sampleFormat &&
         left.channelCount == right.channelCount;
}

// T7 免重开判定（D4）：驱动设备开启的输出配置是否未变。configureOutput/
// selectOutputDevice 的变更命令都先于其触发的 LoadTrack 到达 worker，配置级比较
// 能覆盖"仅缓冲时长/目标设备等变化"的场景——这类变化不改变请求格式三要素，
// 若短路判据只比格式会漏判而错误复用旧设备。
bool sameOutputConfig(const AudioOutputConfig& left, const AudioOutputConfig& right) {
  return left.outputMode == right.outputMode && left.targetSampleRate == right.targetSampleRate &&
         left.targetSampleFormat == right.targetSampleFormat &&
         left.targetChannelCount == right.targetChannelCount &&
         left.bufferDuration == right.bufferDuration && left.keepDeviceOpen == right.keepDeviceOpen &&
         left.allowFallback == right.allowFallback && left.preferredDeviceId == right.preferredDeviceId;
}

std::string formatFallbackReason(AudioSampleFormat from, AudioSampleFormat to) {
  const auto bitDepthName = [](AudioSampleFormat format) -> const char* {
    switch (format) {
    case AudioSampleFormat::Int16:
      return "16 位整数";
    case AudioSampleFormat::Int24:
      return "24 位整数";
    case AudioSampleFormat::Int32:
      return "32 位整数";
    case AudioSampleFormat::Float32:
      return "32 位浮点";
    case AudioSampleFormat::Unknown:
      return "未知格式";
    }
    return "未知格式";
  };

  if (from == AudioSampleFormat::Unknown) {
    return "请求的输出格式不受支持，已回退到 " + std::string(bitDepthName(to));
  }
  return "设备不支持 " + std::string(bitDepthName(from)) + " 输出，已回退到 " + bitDepthName(to);
}

std::optional<std::chrono::milliseconds> endPositionFor(const TrackPlaybackRequest& request) {
  if (!request.boundedSegment || !request.offset.has_value() || !request.duration.has_value()) {
    return std::nullopt;
  }

  return *request.offset + *request.duration;
}

struct BoundaryTrimResult {
  std::optional<FfmpegAudioFrame> frame;
  bool reachedBoundary{false};
};

BoundaryTrimResult trimFrameToBoundary(FfmpegAudioFrame frame,
                                       std::optional<std::chrono::milliseconds> endPosition) {
  if (!endPosition.has_value() || frame.sampleRate == 0U || frame.frameCount == 0U) {
    return BoundaryTrimResult{std::move(frame), false};
  }

  const auto frameStart = std::chrono::duration_cast<std::chrono::microseconds>(frame.position);
  const auto boundary = std::chrono::duration_cast<std::chrono::microseconds>(*endPosition);
  if (frameStart >= boundary) {
    return BoundaryTrimResult{std::nullopt, true};
  }

  const auto bytesPerFrame = static_cast<std::size_t>(frame.channelCount) * bytesPerSample(frame.sampleFormat);
  if (bytesPerFrame == 0U || frame.sampleBytes.size() != static_cast<std::size_t>(frame.frameCount) * bytesPerFrame) {
    return BoundaryTrimResult{std::move(frame), false};
  }

  const auto frameDurationUs = (static_cast<std::uint64_t>(frame.frameCount) * 1'000'000ULL) / frame.sampleRate;
  const auto frameEnd = frameStart + std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(frameDurationUs)};
  if (frameEnd <= boundary) {
    return BoundaryTrimResult{std::move(frame), false};
  }

  const auto availableUs = std::max<std::chrono::microseconds::rep>(0, (boundary - frameStart).count());
  const auto allowedFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      frame.frameCount,
      (static_cast<std::uint64_t>(availableUs) * frame.sampleRate) / 1'000'000ULL));
  if (allowedFrames == 0U) {
    return BoundaryTrimResult{std::nullopt, true};
  }

  frame.frameCount = allowedFrames;
  frame.sampleBytes.resize(static_cast<std::size_t>(allowedFrames) * bytesPerFrame);
  return BoundaryTrimResult{std::move(frame), true};
}

AudioOutputConfig explicitConfig(AudioOutputConfig config, AudioOutputMode mode, const FfmpegFilterTargetFormat& target) {
  config.outputMode = mode;
  config.targetSampleRate = target.sampleRate;
  config.targetSampleFormat = target.sampleFormat;
  config.targetChannelCount = target.channelCount;
  return config;
}

}

class SingleTrackAudioPlaybackService final : public AudioPlaybackService {
public:
  explicit SingleTrackAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend,
                                           std::unique_ptr<DeviceFormatEnumerator> formatEnumerator)
      : device_(std::move(backend)),
        formatEnumerator_(std::move(formatEnumerator)),
        dispatcher_(BackendSourceModule::AudioPlaybackService) {
    stateMachine_.setEventSink([this](BackendEvent event) { dispatcher_.dispatch(std::move(event)); });
    audioWorker_ = std::thread{[this] { runAudioWorker(); }};
  }

  ~SingleTrackAudioPlaybackService() override {
    stopAudioWorker();
    dispatcher_.clearEventSink();
    stateMachine_.clearEventSink();
  }

  void setEventSink(BackendEventSink sink) override { dispatcher_.setEventSink(std::move(sink)); }

  void configureOutput(const AudioOutputConfig& config) override {
    enqueueCommand([this, config] {
      config_ = config;
      // T11：输出配置变更后的首个 LoadTrack = 配置整轨重载（非"改变音轨"），手动切歌
      // 档位对重载不生效（现状瞬时体保持）；标志在 loadTrackOnWorker 消费。
      outputConfigChangePending_ = true;
    });
  }

  // 过渡参数：worker 命令仅存配置，零重载/零设备操作/零事件（区别于 configureOutput
  // 的整轨重载语义）；消费在过渡引擎任务（T5/T11）接线。
  // T8：配置变化 = 预解码预告重新武装（endApproachEmitted_ 复位），阈值按裁定
  // max(crossfadeMs, gaplessPreloadMs) 即时读取、无需存储。
  // T10：配置变化同时撤除在途重叠面（第二源/源包络轨迹/窗口裁决锁存）并弃预解码槽——
  // 新配置 = 新臂，旧槽作废（同 abortTransition 语义；服务侧直接重配不再残留一臂旧槽
  // 做最后一次直切）。endApproachEmitted_ 复位重新武装预告。
  void configureTransition(const TransitionConfig& config) override {
    enqueueCommand([this, config] {
      cancelOverlapFaceOnWorker();
      preloadSlot_.reset();
      transitionConfig_ = config;
      endApproachEmitted_ = false;
    });
  }

  // T10：中止在途过渡（裁定基线⑦ 失效域的服务侧落地）。控制器在重叠窗口内收到失效
  // 操作命令/版本校验失败时经 AbortTransition 意图调用：撤第二源 + 打断源包络轨迹 +
  // 弃预解码槽 + 重新武装预告（服务将再次发 EndApproaching → 控制器按新状态重调度）。
  void abortTransition() override {
    enqueueCommand([this] {
      cancelOverlapFaceOnWorker();
      preloadSlot_.reset();
      endApproachEmitted_ = false;
    });
  }

  void loadTrack(const TrackPlaybackRequest& request) override {
    enqueueCommand([this, request] { loadTrackOnWorker(request); });
  }

  void prepareNext(const TrackPlaybackRequest& request) override {
    prepareNext(request, PrepareNextMeta{});
  }

  void prepareNext(const TrackPlaybackRequest& request, const PrepareNextMeta& meta) override {
    enqueueCommand([this, request, meta] { prepareNextOnWorker(request, meta); });
  }

  void play() override { enqueueCommand([this] { playOnWorker(); }); }

  void pause() override { enqueueCommand([this] { pauseOnWorker(); }); }

  void resume() override { enqueueCommand([this] { resumeOnWorker(); }); }

  void stop() override { enqueueCommand([this] { stopOnWorker(); }); }

  void seek(std::chrono::milliseconds position) override {
    enqueueCommand([this, position] { seekOnWorker(position); });
  }

  void setVolume(float linearGain) override { enqueueCommand([this, linearGain] { device_.setVolume(linearGain); }); }

  void setMuted(bool muted) override { enqueueCommand([this, muted] { device_.setMuted(muted); }); }

  void selectOutputDevice(const std::string& deviceId) override {
    enqueueCommand([this, deviceId] {
      config_.preferredDeviceId = deviceId;
      outputConfigChangePending_ = true;
    });
  }

  PlaybackClockSnapshot queryPlaybackClock() const override {
    auto promise = std::make_shared<std::promise<PlaybackClockSnapshot>>();
    auto future = promise->get_future();
    auto* self = const_cast<SingleTrackAudioPlaybackService*>(this);
    self->enqueueCommand([self, promise] {
      self->servicePlaybackProgress();
      promise->set_value(self->clock_.snapshot());
    });
    if (future.wait_for(std::chrono::seconds{2}) == std::future_status::ready) {
      return future.get();
    }

    std::lock_guard lock{snapshotMutex_};
    return lastClockSnapshot_;
  }

  // 线程模型：本方法只允许命令线程（UI/控制线程）调用，绝不在 audio worker
  // 线程内调用——enqueueCommand 把任务交给 worker 自身执行，在 worker 内等待
  // future 会直接死锁。worker 线程内执行 device_.enumeratePlaybackDevices()，
  // 枚举路径不触碰渲染回调/事件；超时（2s）兜底返回空列表。
  [[nodiscard]] std::vector<AudioDeviceFormat> enumeratePlaybackDevices() const override {
    auto promise = std::make_shared<std::promise<std::vector<AudioDeviceFormat>>>();
    auto future = promise->get_future();
    auto* self = const_cast<SingleTrackAudioPlaybackService*>(this);
    self->enqueueCommand([self, promise] {
      promise->set_value(self->enumeratePlaybackDevicesOnWorker());
    });
    if (future.wait_for(std::chrono::seconds{2}) == std::future_status::ready) {
      return future.get();
    }

    return {};
  }

private:
  // T6/T11 物理过渡监督动作（仅 audio worker 线程访问；finishingAction_ != None = 过渡中）。
  // 命名沿用 T6"finishing"：PauseFreeze/StopCleanup = T6 收尾；T11 追加的 SeekDipDown/
  // ManualDipDown 复用同一监督通道（ticker 分支 + 归零点分派），语义同为"逻辑态保持/
  // 已翻转、物理包络单程在途"。嵌套类型须先于类内成员函数体声明（类内作用域自声明点起可见）。
  enum class FinishingAction { None, PauseFreeze, StopCleanup, SeekDipDown, ManualDipDown };

  // worker 线程内的枚举：先取播放后端（miniaudio）设备列表，再用平台
  // 原生枚举器（PipeWire/WASAPI）的能力数据覆盖格式/采样率列表。
  std::vector<AudioDeviceFormat> enumeratePlaybackDevicesOnWorker() {
    auto devices = device_.enumeratePlaybackDevices();
    mergeDeviceFormatCapabilities(devices);
    return devices;
  }

  // 用 DeviceFormatCapabilities 覆盖 AudioDeviceFormat 的能力字段：
  // 按 deviceId 精确匹配 → deviceName 精确匹配 的顺序定位设备；仅当 caps
  // 至少一个列表非空时覆盖（空列表=未枚举或全支持，覆盖会丢失该语义）；
  // backendName 等播放字段不覆盖（保持 miniaudio 播放后端标识）。
  void mergeDeviceFormatCapabilities(std::vector<AudioDeviceFormat>& devices) {
    if (!formatEnumerator_) {
      return;
    }
    const auto capabilities = formatEnumerator_->enumerate();
    if (capabilities.empty()) {
      return;
    }
    for (auto& device : devices) {
      for (const auto& caps : capabilities) {
        const bool idMatched = !caps.deviceId.empty() && caps.deviceId == device.deviceId;
        const bool nameMatched = !idMatched && !caps.deviceName.empty() && caps.deviceName == device.deviceName;
        if (!idMatched && !nameMatched) {
          continue;
        }
        if (caps.supportedSampleFormats.empty() && caps.supportedSampleRates.empty()) {
          continue;
        }
        if (!caps.supportedSampleFormats.empty()) {
          device.supportedSampleFormats = caps.supportedSampleFormats;
        }
        if (!caps.supportedSampleRates.empty()) {
          device.supportedSampleRates = caps.supportedSampleRates;
        }
        spdlog::debug("device format capabilities merged for '{}' (match: {}): {} formats, {} rates",
                      device.deviceName,
                      idMatched ? "deviceId" : "deviceName",
                      device.supportedSampleFormats.size(),
                      device.supportedSampleRates.size());
        break;
      }
    }
  }

  void loadTrackOnWorker(const TrackPlaybackRequest& request) {
    spdlog::info("loading track '{}'", pathToUtf8(request.filePath));
    // T10/T11：新曲命令 = 中止在途过渡面（第二源撤销 + 源包络打断 + 窗口锁存复位；幂等，
    // 无重叠面时零操作）。控制器流已在重叠窗口内先发 AbortTransition，此处是服务侧兜底
    // （手动切歌/重载直连 LoadTrack 同样先撤面再执行，裁定⑦ 打断即瞬时）。
    cancelOverlapFaceOnWorker();

    // T11：手动切歌档位接线。判别 = 本 LoadTrack 非输出配置重载 + 档位适用（Mixed、Playing
    // 且出声、长度 > 0、非冷启动淡入握手期）。满足 → 按档位进入切换监督（档 2 dip / 档 3
    // 真重叠或降级 dip），瞬时体（performPlainLoadTrack）延后到监督归零点执行。
    const bool reloadAfterOutputChange = outputConfigChangePending_;
    outputConfigChangePending_ = false;
    if (!reloadAfterOutputChange && manualSwitchTransitionEligible() && manualSwitchLoadTrack(request)) {
      return;
    }
    performPlainLoadTrack(request);
  }

  // T11：手动切歌监督适用性（调用前提：LoadTrack 非配置重载）。
  bool manualSwitchTransitionEligible() const {
    if (config_.outputMode != AudioOutputMode::Mixed || stateMachine_.state() != PlaybackState::Playing ||
        !device_.started() || !queue_ || !source_ || !pipeline_ || fadeInPending_) {
      return false;
    }
    const auto mode = transitionConfig_.manualAdvanceFadeMode;
    if (mode == ManualAdvanceFadeMode::Off) {
      return false;
    }
    const auto lengthMs = mode == ManualAdvanceFadeMode::FullCrossfade ? transitionConfig_.crossfadeMs
                                                                       : transitionConfig_.manualShortCrossfadeMs;
    return lengthMs.count() > 0;
  }

  // T11：手动切歌档位分发（档 3 真重叠优先；未就绪/档 2 走 dip；长度不足降级硬切）。
  // 返回 true = 已进入切换监督（LoadTrack 瞬时体暂停执行）。
  bool manualSwitchLoadTrack(const TrackPlaybackRequest& request) {
    const auto mode = transitionConfig_.manualAdvanceFadeMode;
    if (mode == ManualAdvanceFadeMode::FullCrossfade && preloadSlot_ && preloadSlot_->queue &&
        preloadSlot_->ready && preloadSlot_->request.trackId == request.trackId &&
        preloadSlot_->request.filePath == request.filePath &&
        sameTarget(preloadSlot_->target, currentTarget_)) {
      // —— 档 3 且目标恰为已预解码槽（裁定⑤ 复用）：真重叠交叉（任务 9 双源面）。
      // 交叉长 = min(crossfadeMs, 当前曲剩余)（裁定⑧ 临近曲末钳制）；不足一腿 → 硬切降级。
      auto legMs = transitionConfig_.crossfadeMs;
      if (const auto remaining = estimateRemainingMs(); remaining.has_value() && *remaining < legMs) {
        legMs = *remaining;
      }
      if (legMs.count() <= 0) {
        preloadSlot_.reset();
        spdlog::debug("manual overlap window exhausted; falling back to instant switch");
        return false;
      }
      if (startManualOverlap(legMs)) {
        // F1：档 3 就绪槽重叠接管在途 dip 监督（seek dip / 手动 dip 下行在途时）。
        // 手动切歌取代了 dip 的 pending 目标（裁定 打断=立即中断并接管）——陈旧 seek/
        // staging 绝不能在重叠中途被应用：清 pendings + 解除 dip 屏蔽（finishingAction_
        // 清空 → Playing 段恢复：预解码续喂/重叠腿推进不停顿）。在途 master 下行腿
        // 按旧参数走完（执行器不接受中途取消），其后必须把 master 升回 1.0（重叠交叉
        // 在源层 EQ 上，master 应持平 1.0）——预发布恢复腿（时长 = 被取代 dip 的上行
        // 腿对称长），执行器自动延迟受理（下行终点后从 ≈0 平滑回升）；只清 pending
        // 留 action 会落入 completeSeekDip/completeManualDip 空 pending 早退 → master
        // 冻结于 0（永久静音，不可取）。
        if (finishingAction_ == FinishingAction::SeekDipDown ||
            finishingAction_ == FinishingAction::ManualDipDown) {
          auto restoreMs = std::chrono::milliseconds{0};
          if (seekDip_) {
            restoreMs = seekDip_->upMs;
          } else if (manualDip_) {
            restoreMs = manualDip_->upMs;
          }
          if (restoreMs.count() <= 0) {
            restoreMs = legMs / 2;
          }
          seekDip_.reset();
          manualDip_.reset();
          finishingAction_ = FinishingAction::None;
          fadeInPending_ = false;
          publishMasterEnvelope(1.0F, restoreMs);
          spdlog::info("manual overlap superseded in-flight dip supervision: master restore ({}ms)",
                       restoreMs.count());
        }
        spdlog::info("manual switch: full crossfade via preloaded slot ({}ms)", legMs.count());
        return true;
      }
      // 设备拒绝激活（N8 守卫，槽已弃）→ 落入下方 dip 降级（重新现解 staging，永不等待）。
      spdlog::debug("manual switch: overlap refused by device, degrading to dip");
    }

    // —— 档 2 dip，或档 3 槽未就绪/不匹配 → 降级 dip（裁定⑧：可重叠就重叠、就绪不足降级、
    //    永不等待）。dip 全长 = 档位长度，档 3 另受剩余时长钳制（min(L, 剩余)）。
    auto dipMs = mode == ManualAdvanceFadeMode::FullCrossfade ? transitionConfig_.crossfadeMs
                                                              : transitionConfig_.manualShortCrossfadeMs;
    if (mode == ManualAdvanceFadeMode::FullCrossfade) {
      if (const auto remaining = estimateRemainingMs(); remaining.has_value() && *remaining < dipMs) {
        dipMs = *remaining;
      }
    }
    if (dipMs.count() <= 0) {
      preloadSlot_.reset();
      return false;
    }
    // 目标预解码（staging）：复用匹配的就绪槽；否则弃旧槽、经 prepareNextOnWorker 现解。
    // 现解失败（已发 preload 风格错误）→ 回退瞬时体（重新尝试加载并走正常错误路径）。
    if (preloadSlot_ && preloadSlot_->queue && preloadSlot_->ready &&
        preloadSlot_->request.trackId == request.trackId &&
        preloadSlot_->request.filePath == request.filePath) {
      manualDip_ = PendingManualDip{request,
                                    std::move(*preloadSlot_),
                                    dipMs - dipMs / 2};
      preloadSlot_.reset();
    } else {
      preloadSlot_.reset();
      prepareNextOnWorker(request, PrepareNextMeta{});
      if (!preloadSlot_ || !preloadSlot_->queue || !preloadSlot_->ready) {
        spdlog::error("manual switch target decode failed; falling back to plain load");
        return false;
      }
      manualDip_ = PendingManualDip{request, std::move(*preloadSlot_), dipMs - dipMs / 2};
      preloadSlot_.reset();
    }
    spdlog::info("manual switch: dip transition (down {}ms + up {}ms)",
                 (dipMs / 2).count(),
                 (dipMs - dipMs / 2).count());
    // 下行腿：若已有在途下行（seek dip / 先前手动 dip 被打断）则不重复发布——在途轨迹
    // 继续走完（打断即瞬时），归零点按新 pending 目标交接。seek dip 被手动切换接管时
    // 其 pending 一并作废（seekDip_ 不得残留——否则归零点分派后 seekDip_ 成陈旧悬挂）。
    seekDip_.reset();
    if (finishingAction_ != FinishingAction::SeekDipDown &&
        finishingAction_ != FinishingAction::ManualDipDown) {
      finishingAction_ = FinishingAction::ManualDipDown;
      fadeInPending_ = false;
      publishMasterEnvelope(0.0F, dipMs / 2);
      startProgressWorker();
    } else {
      finishingAction_ = FinishingAction::ManualDipDown;
    }
    return true;
  }

  // 现状瞬时加载体（原 loadTrackOnWorker 主体；Direct/档关/非出声态/降级硬切恒走此处）。
  void performPlainLoadTrack(const TrackPlaybackRequest& request) {
    spdlog::info("loading track '{}'", pathToUtf8(request.filePath));
    stopProgressWorker();
    // T6：新曲命令到达 = 任何在途收尾/淡入握手作废（stopDevice 会复位包络轨迹）。
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    // T11：切换监督 pending 一并作废（seek dip / 手动 dip / finishing 期定格 seek）。
    seekDip_.reset();
    manualDip_.reset();
    pendingFrozenSeekPosition_.reset();
    // T7（D4）：uninitialize/协商/initialize 不再无条件执行——Mixed 同参数切歌在
    // canReuseOutputDevice 短路后跳过整段，仅重建 source/pipeline/queue。此处仍先
    // stopDevice：切歌到达时旧曲可能仍在播放（手动切歌路径），保持既有"切歌即
    // 静音"时序，也让随后原地换队列时无活跃回调（未启动时 stop 为 no-op）。
    stopDevice();

    // 协商前设备格式快照：AudioOutputDevice::uninitialize 会清空 currentFormat，
    // OutputFormatChanged 的"实变收紧"（L1 已决）需与拆卸前实际格式比较。
    std::optional<AudioDeviceFormat> previousDeviceFormat;
    if (device_.initialized()) {
      previousDeviceFormat = device_.currentFormat();
    }

    source_ = std::make_unique<FfmpegAudioSource>();
    pipeline_ = std::make_unique<FfmpegFilterPipeline>();
    currentRequest_ = request;
    loadedToEnd_ = false;
    hasCurrentTarget_ = false;
    currentTarget_ = {};
    pendingFrameWrite_.reset();
    preloadSlot_.reset();
    // T8：新曲加载 = 预解码预告重新武装（手动切歌/自然播完后的下一曲均走本路径）。
    endApproachEmitted_ = false;
    // T10：新曲 = 新臂（交叉窗口裁决锁存/第二腿待发复位；在途交叉面已随上方
    // stopDevice 清空——设备停边界撤销第二源 + 复位包络）。
    overlapWindowSettled_ = false;
    overlapRampPending_ = false;

    trackEndPosition_ = endPositionFor(request);
    if (trackEndPosition_.has_value()) {
      spdlog::debug("track boundary: offset={}ms, duration={}ms, end={}ms",
                    request.offset.value_or(std::chrono::milliseconds{0}).count(),
                    request.duration.value_or(std::chrono::milliseconds{0}).count(),
                    trackEndPosition_->count());
    }

    stateMachine_.loadTrack(request);

    if (const auto error = source_->open(request.filePath)) {
      spdlog::error("track load failed (open): {} - {}", error->message, error->detail);
      fail(error->code, error->message, error->detail);
      return;
    }

    if (request.offset.has_value() && request.offset->count() > 0) {
      if (const auto error = source_->seek(*request.offset)) {
        spdlog::error("track load failed (initial seek to offset): {} - {}", error->message, error->detail);
        fail(error->code, error->message, error->detail);
        return;
      }
      spdlog::debug("seeked to CUE track offset: {}ms", request.offset->count());
    }

    // 免重开判定需要新曲 streamInfo（决定协商目标），故把设备拆卸移到打开成功之后。
    const auto& streamInfo = source_->streamInfo();
    spdlog::debug("source stream: {}Hz {}ch {}", streamInfo.sampleRate, streamInfo.channelCount,
                  sampleFormatName(streamInfo.sampleFormat));
    std::string negotiationFailure;
    std::optional<AudioOutputDeviceError> negotiationDeviceError;
    std::optional<OutputNegotiationResult> negotiation;

    if (canReuseOutputDevice(streamInfo)) {
      // —— T7 免重开短路：跳过 stopDevice(已停)/uninitialize/negotiateOutput/initialize
      // 整段，仅重建 source/pipeline/queue（任务 9 双队列面未启用时为单 ring 重建）。
      // 目标与设备当前格式一致由判定保证，候选即 requestedTarget；队列指针经
      // rebindQueue 原子发布到回调面后旧队列方可析构（设备已停、无活跃回调）。
      const auto target = requestedTarget(streamInfo);
      if (const auto error = pipeline_->configure(target)) {
        spdlog::error("track load failed (reuse pipeline configure): {} - {}", error->message, error->detail);
        fail(PlaybackErrorCode::FormatNegotiationFailed,
             "failed to negotiate an output format",
             error->detail);
        return;
      }
      const auto capacityFrames = bufferFrameCount(target.sampleRate, config_.bufferDuration);
      auto nextQueue = std::make_unique<PcmBufferQueue>(
          PcmBufferQueueConfig{capacityFrames, target.channelCount * bytesPerSample(target.sampleFormat)});
      device_.rebindQueue(*nextQueue);
      queue_ = std::move(nextQueue);
      negotiation = OutputNegotiationResult{explicitConfig(config_, AudioOutputMode::Mixed, target),
                                            target,
                                            device_.currentFormat(),
                                            {}};
      spdlog::info("track load kept device open (mixed output, identical format)");
    } else {
      // —— 原路径：拆卸设备后整段协商（Direct 恒走此处；Mixed 参数实变也走此处）——
      device_.uninitialize();
      queue_.reset();
      negotiation = negotiateOutput(streamInfo, negotiationFailure, negotiationDeviceError);
    }
    if (!negotiation) {
      spdlog::error("track load failed (negotiation): {}", negotiationFailure);
      if (negotiationDeviceError) {
        fail(negotiationDeviceError->code, negotiationDeviceError->message, negotiationDeviceError->detail);
        return;
      }

      fail(PlaybackErrorCode::FormatNegotiationFailed,
           "failed to negotiate an output format",
           negotiationFailure.empty() ? "no output format candidates were accepted" : negotiationFailure);
      return;
    }
    currentTarget_ = negotiation->target;
    hasCurrentTarget_ = true;
    activeDeviceConfig_ = config_;
    spdlog::debug("output negotiated: {}Hz {}ch {} mode={}",
                  currentTarget_.sampleRate, currentTarget_.channelCount,
                  sampleFormatName(currentTarget_.sampleFormat),
                  outputModeName(negotiation->effectiveConfig.outputMode));

    // pipeline 已由 negotiateOutput 候选循环或免重开分支完成配置。
    clock_.reset(request.trackId, currentTarget_.sampleRate, request.offset.value_or(std::chrono::milliseconds{0}));
    observedQueueCounters_ = {};

    if (!negotiation->fallbackReason.empty()) {
      spdlog::warn("output mode fallback: {}", negotiation->fallbackReason);
      dispatcher_.dispatch(BackendEventType::OutputModeFallback,
                           OutputModeFallback{config_,
                                              negotiation->effectiveConfig,
                                              negotiation->deviceFormat,
                                              negotiation->fallbackReason});
    }

    // L1 已决：OutputFormatChanged 收紧为设备格式实变才发（短路路径无实变天然不发；
    // 协商结果与开段前格式一致时同样不发）。唯一消费者 reducer:markPlayerChanged 只
    // 标记快照重发，载荷不进快照；切歌时的快照刷新由 TrackChanged/状态/位置事件照常
    // 驱动，无测试依赖"同参数必发"（output_format_negotiation_tests :292/:362 语义复核）。
    const auto& deviceFormat = negotiation->deviceFormat;
    const bool formatChanged = !previousDeviceFormat.has_value() ||
                               previousDeviceFormat->sampleRate != deviceFormat.sampleRate ||
                               previousDeviceFormat->sampleFormat != deviceFormat.sampleFormat ||
                               previousDeviceFormat->channelCount != deviceFormat.channelCount ||
                               previousDeviceFormat->bufferFrames != deviceFormat.bufferFrames ||
                               previousDeviceFormat->actualMode != deviceFormat.actualMode ||
                               previousDeviceFormat->fallbackApplied != deviceFormat.fallbackApplied;
    if (formatChanged) {
      dispatcher_.dispatch(BackendEventType::OutputFormatChanged, OutputFormatChanged{config_, deviceFormat});
    } else {
      spdlog::debug("output format unchanged; OutputFormatChanged suppressed");
    }
    if (!fillQueue()) {
      return;
    }

    spdlog::info("track loaded: {}Hz {}ch {}", currentTarget_.sampleRate,
                 currentTarget_.channelCount, sampleFormatName(currentTarget_.sampleFormat));
    stateMachine_.completeLoad();
    publishPosition();
  }

  void prepareNextOnWorker(const TrackPlaybackRequest& request, const PrepareNextMeta& kindMeta) {
    // T9/T10：作废旧槽前先撤除在途交叉面（未激活时 = 原 preloadSlot_.reset 语义；
    // 已激活时额外退第二源 + 打断源包络轨迹 + 复位窗口锁存——新槽 = 新臂）。
    cancelOverlapFaceOnWorker();
    preloadSlot_.reset();

    PreloadSlot slot{};
    slot.request = request;
    slot.source = std::make_unique<FfmpegAudioSource>();
    slot.pipeline = std::make_unique<FfmpegFilterPipeline>();
    slot.endPosition = endPositionFor(request);
    slot.kindMeta = kindMeta;

    if (const auto error = slot.source->open(request.filePath)) {
      emitPreloadError(error->code, error->message, error->detail);
      return;
    }
    if (request.offset.has_value() && request.offset->count() > 0) {
      if (const auto error = slot.source->seek(*request.offset)) {
        emitPreloadError(error->code, error->message, error->detail);
        return;
      }
    }

    slot.target = hasCurrentTarget_ ? currentTarget_ : requestedTarget(slot.source->streamInfo());
    const auto sampleBytes = bytesPerSample(slot.target.sampleFormat);
    if (slot.target.sampleRate == 0U || slot.target.channelCount == 0U || sampleBytes == 0U) {
      emitPreloadError(PlaybackErrorCode::FormatNegotiationFailed,
                       "failed to prepare next track output format",
                       describeTarget(config_.outputMode, slot.target));
      return;
    }

    if (const auto error = slot.pipeline->configure(slot.target)) {
      emitPreloadError(error->code, error->message, error->detail);
      return;
    }

    const auto capacityFrames = bufferFrameCount(slot.target.sampleRate, config_.bufferDuration);
    slot.queue = std::make_unique<PcmBufferQueue>(PcmBufferQueueConfig{capacityFrames, slot.target.channelCount * sampleBytes});
    if (!fillPreloadSlot(slot)) {
      return;
    }

    slot.ready = true;
    slot.seamlessEligible = config_.outputMode == AudioOutputMode::Mixed && device_.initialized() &&
                            device_.currentFormat().actualMode == AudioOutputMode::Mixed && hasCurrentTarget_ &&
                            sameTarget(slot.target, currentTarget_);
    preloadSlot_ = std::move(slot);
  }

  void playOnWorker() {
    spdlog::info("play");
    // T11：切换监督在途时 reducer 批尾的 Play/Resume 意图由交接自行 play——此处吞掉，
    // 避免 Playing 态重复 play 报非法迁移。覆盖三类在途：
    // 1) seek dip / 手动 dip 下行（finishingAction_）：归零点 adopt 自行 play；
    // 2) 档 3 真重叠激活（manualOverlapActive_，F3 扩展）：重叠入口不动状态机（机器
    //    保持 Playing），adopt（promote/drain-promote）自行 stateMachine_.play()——
    //    重叠期批尾 Play 恒冗余。F1 接管把在途 dip 清成 None 后旧吞掉条件失守：Play
    //    打上 Playing 即非法迁移 PlaybackError（伪错误 UI 全程交叉窗），故须显式覆盖。
    // resumeOnWorker 不扩展：重叠只在 Playing 激活（pause/stop 先撤重叠），reducer
    // 批尾恒 Play（selectTrack 无 Resume 尾），Playing 态无 Resume 来源 → Resume∧
    // manualOverlapActive_ 不可达。
    if (finishingAction_ == FinishingAction::SeekDipDown || finishingAction_ == FinishingAction::ManualDipDown ||
        manualOverlapActive_) {
      return;
    }
    if (!queue_ || !source_ || !pipeline_) {
      spdlog::error("play failed: no loaded track");
      fail(PlaybackErrorCode::OpenFailed, "play requires a loaded track", "missing playback pipeline");
      return;
    }

    const auto state = stateMachine_.state();
    if (state != PlaybackState::Ready && state != PlaybackState::Paused && state != PlaybackState::Stopped) {
      stateMachine_.play();
      return;
    }

    // T6：淡出收尾在途时的 play = "暂停中再播放"——中止物理收尾监督（不再于归零后停
    // 设备），设备保持运行；随后发布的 0→1 淡入由执行器在在途淡出结束后自动受理
    // （T5 账本：在途轨迹期间竞争发布被推迟，起点恒取回调 currentGain，连续无跳变）。
    const bool wasFinishing = abortFinishingForRestart();

    clock_.resume();
    // 冷启动淡入握手：先即时落 0.0（设备已停、包络已被 stop 复位到 1.0；0→1 轨迹须先
    // 经即时 0 把回调 currentGain 落到 0，否则执行器从 1.0 起跑 = 无淡入效果——T5-B2 前置）。
    if (transportFadeEnabled() && !wasFinishing) {
      armTransportFadeIn();
    }
    if (!device_.start()) {
      spdlog::error("play failed: device start returned false");
      clock_.pause();
      failWithDeviceError("failed to start audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.play();
    startProgressWorker();
    if (wasFinishing) {
      if (transportFadeEnabled()) {
        publishTransportFadeIn();
      } else {
        // fade 在淡出在途时被禁用（configureTransition 任意态合法）：收尾监督已中止、
        // 在途 1→0 轨迹仍会在回调里走完，若无后续发布增益将停在 0（逻辑 Playing 但
        // 永久静音）→ 即时发布回满增益。执行器受理：在途轨迹结束后当前块直落 target
        // （duration==0 跳变 = "无淡入"语义，一次电平阶跃，非平滑回升——正确）。
        publishMasterEnvelope(1.0F, std::chrono::milliseconds{0});
      }
    }
    publishPosition();
  }

  void pauseOnWorker() {
    spdlog::info("pause");
    // T11：重叠面激活时先撤副源（立即中止重叠——裁定打断即瞬时）再对主源执行 pause。
    if (device_.secondSourceActive()) {
      abortInFlightOverlapOnWorker();
    }
    if (finishingAction_ != FinishingAction::None) {
      if (finishingAction_ == FinishingAction::SeekDipDown || finishingAction_ == FinishingAction::ManualDipDown) {
        // T11：切换 dip 下行在途的 pause = 立即接管：撤 pending 交接目标，动作转为
        // PauseFreeze——在途下行腿继续走完（执行器不接受中途取消），归零点停设备定格。
        seekDip_.reset();
        manualDip_.reset();
        finishingAction_ = FinishingAction::PauseFreeze;
        stateMachine_.pause();
        return;
      }
      // 淡出在途的重复/迟到 pause：物理淡出不打断（单程无中途反转）；状态机处理
      // 幂等/非法迁移（Paused/Stopped 下 pause 报错与现状一致）。
      stateMachine_.pause();
      return;
    }
    // R1：逻辑 Paused 立即发出（先于任何物理动作/淡出——UI 即刻响应）。
    stateMachine_.pause();
    if (!transportFadeEnabled() || !device_.started()) {
      // —— fade 关 / transportFadeMs==0 / 设备未运行：现状瞬时路径，零行为变化 ——
      stopProgressWorker();
      stopDevice();
      updateClockFromQueue();
      clock_.pause();
      publishPosition();
      return;
    }
    beginFinishing(FinishingAction::PauseFreeze);
  }

  void resumeOnWorker() {
    spdlog::info("resume");
    if (finishingAction_ == FinishingAction::SeekDipDown || finishingAction_ == FinishingAction::ManualDipDown) {
      return;
    }
    if (!queue_) {
      spdlog::error("resume failed: no queue");
      fail(PlaybackErrorCode::OpenFailed, "resume requires a loaded track", "missing playback queue");
      return;
    }
    if (stateMachine_.state() != PlaybackState::Paused) {
      stateMachine_.resume();
      return;
    }

    // T6：暂停淡出在途的恢复 = "暂停中再播放"——中止收尾监督、不打断在途淡出；
    // 0→1 淡入随后由执行器自动受理（起点=回调 currentGain，从≈0 连续回升）。
    const bool wasFinishing = abortFinishingForRestart();

    clock_.resume();
    if (transportFadeEnabled() && !wasFinishing) {
      armTransportFadeIn();
    }
    if (!device_.start()) {
      spdlog::error("resume failed: device start returned false");
      clock_.pause();
      failWithDeviceError("failed to resume audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.resume();
    startProgressWorker();
    if (wasFinishing) {
      if (transportFadeEnabled()) {
        publishTransportFadeIn();
      } else {
        // 同 playOnWorker：fade 在淡出在途时被禁用 → 即时发布回满增益，防增益停在 0
        // （逻辑 Playing 永久静音）。duration==0 直落 target = "无淡入"语义。
        publishMasterEnvelope(1.0F, std::chrono::milliseconds{0});
      }
    }
    publishPosition();
  }

  void stopOnWorker() {
    spdlog::info("stop");
    // T11：重叠面激活时先撤副源再执行 stop（同 pause——立即中止重叠）。
    if (device_.secondSourceActive()) {
      abortInFlightOverlapOnWorker();
    }
    if (finishingAction_ != FinishingAction::None) {
      if (finishingAction_ == FinishingAction::SeekDipDown || finishingAction_ == FinishingAction::ManualDipDown) {
        // T11：切换 dip 下行在途的 stop = 立即接管：撤 pending 交接目标，动作转为
        // StopCleanup（归零点停设备并做 stop 队列清理，不执行换源）。
        seekDip_.reset();
        manualDip_.reset();
        finishingAction_ = FinishingAction::StopCleanup;
        stateMachine_.stop();
        return;
      }
      // 淡出在途的 stop：物理淡出不打断；收尾动作升级为 stop 清理（归零后清队列、
      // 复位挂起帧——位置清理照旧，只是推迟到归零点执行）。
      finishingAction_ = FinishingAction::StopCleanup;
      stateMachine_.stop();
      return;
    }
    // R2：逻辑 Stopped 立即发出（先于物理动作）。
    stateMachine_.stop();
    if (!transportFadeEnabled() || !device_.started()) {
      // —— fade 关 / transportFadeMs==0 / 设备未运行：现状瞬时路径，零行为变化 ——
      stopProgressWorker();
      stopDevice();
      updateClockFromQueue();
      clock_.pause();
      if (queue_) {
        queue_->clearForSeek();
      }
      pendingFrameWrite_.reset();
      publishPosition();
      return;
    }
    beginFinishing(FinishingAction::StopCleanup);
  }

  void seekOnWorker(std::chrono::milliseconds position) {
    spdlog::info("seek to {}ms", position.count());
    if (!source_ || !pipeline_ || !queue_) {
      spdlog::error("seek failed: no pipeline");
      fail(PlaybackErrorCode::SeekFailed, "seek requires a loaded track", "missing playback pipeline");
      return;
    }

    // T11：finishing（PauseFreeze 收尾淡出）期 seek = 仅更新定格位置——不打断淡出、
    // 不触发新 dip、不动状态机；物理 seek 推迟到归零点补做（设备已停安全，见 completeFinishing）。
    if (finishingAction_ == FinishingAction::PauseFreeze && stateMachine_.state() == PlaybackState::Paused) {
      pendingFrozenSeekPosition_ = position;
      spdlog::info("seek during finishing fade: frozen position will update to {}ms", position.count());
      return;
    }

    // T11：seek dip 下行在途（状态保持 Playing）→ 覆盖 pending 目标：在途下行腿继续走完
    // （进行中淡变按旧参数收尾），归零点按最新目标交接。
    if (finishingAction_ == FinishingAction::SeekDipDown && seekDip_) {
      seekDip_->target = position;
      spdlog::info("seek during seek dip: target updated to {}ms", position.count());
      return;
    }
    // 手动 dip 下行在途 + seek = 立即接管：撤换源 pending，转入 seek dip（复用下行腿）。
    if (finishingAction_ == FinishingAction::ManualDipDown) {
      manualDip_.reset();
      seekDip_ = PendingSeekDip{position, transitionConfig_.seekFadeMs - transitionConfig_.seekFadeMs / 2};
      finishingAction_ = FinishingAction::SeekDipDown;
      spdlog::info("manual dip interrupted by seek: target={}ms, up={}ms",
                   position.count(),
                   seekDip_->upMs.count());
      return;
    }

    if (stateMachine_.state() != PlaybackState::Ready && stateMachine_.state() != PlaybackState::Playing &&
        stateMachine_.state() != PlaybackState::Paused) {
      // 非法态 seek（逻辑 Stopped 等）：状态机只发错误事件。在途收尾监督必须保留——
      // 若在此清 finishing/停 ticker，淡出会在回调里走完而无人停设备（gain 停在 0 的
      // 僵尸设备）。淡出照常归零，completeFinishing 正常停设备（F2）。
      stateMachine_.seek(position);
      return;
    }

    // 重叠面激活时先撤副源（seek 属窗口失效操作，服务侧直达命令同样先撤再执行）。
    if (device_.secondSourceActive()) {
      abortInFlightOverlapOnWorker();
    }

    // 合法 seek：现在才中止任何在途收尾/淡入握手（stopDevice 会复位包络轨迹）。
    // finishing 期 seek 的"不打断淡出、仅更新定格位置"语义属任务 11 接线，本任务只防僵尸收尾。
    // 淡入握手状态须在清除前捕获——dip 判据要的是"命令到达时"是否处于握手期（见下）。
    const bool fadeHandshakePending = fadeInPending_;
    stopProgressWorker();
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;

    updateClockFromQueue();
    const bool shouldResume = stateMachine_.state() == PlaybackState::Playing;

    // T11：播放中 seek dip（对半分解：下 ½ + 上 ½，归零点即转向）。命令时刻只发下行腿并
    // 进入 SeekDipDown 监督——状态机保持 Playing（状态机 seek 事件整体推迟到归零点，见
    // completeSeekDip：beginSeek/completeSeek 背靠背发出，二次 seek 只覆盖 pending 目标）。
    // 冷启动淡入握手期（增益仍为 0）内容不可闻 → dip 无意义，走瞬时路径。
    if (shouldResume && transitionConfig_.fadeOnSeek && transitionConfig_.seekFadeMs.count() > 0 &&
        !fadeHandshakePending) {
      beginSeekDip(position);
      return;
    }

    // —— 瞬时 seek 路径（暂停/Ready 中 seek 不淡变：设备不重开事实保持；或 dip 未启用）——
    const auto seekGeneration = stateMachine_.beginSeek(position);

    stopDevice();
    if (const auto error = source_->seek(position)) {
      spdlog::error("seek failed (ffmpeg seek): {} - {}", error->message, error->detail);
      if (shouldResume) {
        clock_.resume();
        if (device_.start()) {
          startProgressWorker();
        } else {
          clock_.pause();
          failWithDeviceError("failed to restart audio output device after seek failure",
                              "AudioOutputDeviceBackend::start returned false");
          return;
        }
      }
      stateMachine_.cancelSeek(error->code, error->message, error->detail);
      publishPosition();
      return;
    }

    pipeline_->reset();
    queue_->clearForSeek();
    pendingFrameWrite_.reset();
    observedQueueCounters_ = queue_->counters();
    loadedToEnd_ = false;

    if (!fillQueue()) {
      spdlog::error("seek failed (fillQueue)");
      fail(PlaybackErrorCode::SeekFailed, "seek failed", "failed to fill PCM queue after seek");
      return;
    }

    observedQueueCounters_ = queue_->counters();
    clock_.seek(position);
    stateMachine_.completeSeek(seekGeneration);
    if (shouldResume) {
      clock_.resume();
      if (!device_.start()) {
        spdlog::error("seek failed: device restart returned false");
        clock_.pause();
        failWithDeviceError("failed to restart audio output device after seek", "AudioOutputDeviceBackend::start returned false");
        return;
      }
      startProgressWorker();
    } else {
      clock_.pause();
    }
    spdlog::debug("seek completed to {}ms", position.count());
    publishPosition();
  }
  struct PendingFrameWrite {
    FfmpegAudioFrame frame{};
    std::uint32_t writtenFrames{0};
  };

  struct PreloadSlot {
    TrackPlaybackRequest request{};
    FfmpegFilterTargetFormat target{};
    std::optional<std::chrono::milliseconds> endPosition{};
    std::optional<PendingFrameWrite> pendingFrameWrite{};
    std::unique_ptr<FfmpegAudioSource> source{};
    std::unique_ptr<FfmpegFilterPipeline> pipeline{};
    std::unique_ptr<PcmBufferQueue> queue{};
    bool loadedToEnd{false};
    bool ready{false};
    bool seamlessEligible{false};
    // T8：控制器选定的交接方式（SeamlessDirect=就绪直切副源；Crossfade=任务 9 重叠面源）。
    PrepareNextMeta kindMeta{};
  };

  // T11：seek dip 监督 pending（finishingAction_ == SeekDipDown 时有效）。target 可被在途
  // 期间的后续 seek 覆盖（进行中淡变按旧参数收尾，归零点按最新目标交接）。
  struct PendingSeekDip {
    std::chrono::milliseconds target{0};
    std::chrono::milliseconds upMs{0};
  };

  // T11：手动 dip 监督 pending（finishingAction_ == ManualDipDown 时有效）。slot = 归零点
  // 采纳的新主源（已预解码）；upMs = 上行腿时长（全长 − 下行腿，奇数毫秒归上行）。
  struct PendingManualDip {
    TrackPlaybackRequest request{};
    PreloadSlot slot{};
    std::chrono::milliseconds upMs{0};
  };

  struct OutputNegotiationCandidate {
    AudioOutputConfig config{};
    FfmpegFilterTargetFormat target{};
    std::string fallbackReason{};
  };

  struct OutputNegotiationResult {
    AudioOutputConfig effectiveConfig{};
    FfmpegFilterTargetFormat target{};
    AudioDeviceFormat deviceFormat{};
    std::string fallbackReason{};
  };

  struct QueueUnderrunDelta {
    std::uint64_t silenceFrames{0};
    std::uint64_t underrunCount{0};
  };

  using AudioCommand = std::function<void()>;

  void enqueueCommand(AudioCommand command) {
    {
      std::lock_guard lock{commandMutex_};
      if (audioWorkerStopping_) {
        return;
      }
      commands_.push_back(std::move(command));
    }
    commandAvailable_.notify_one();
  }

  void stopAudioWorker() {
    {
      std::lock_guard lock{commandMutex_};
      audioWorkerStopping_ = true;
    }
    commandAvailable_.notify_one();
    if (audioWorker_.joinable()) {
      audioWorker_.join();
    }
  }

  void runAudioWorker() {
    // Report the worker thread's scheduling state at startup (priority boosts
    // are disabled by design; see thread_priority.h). The helper only touches
    // the calling thread, so the miniaudio device callback thread is
    // unaffected.
    const auto priority = applyAudioWorkerThreadPriority();
    if (priority.outcome == ThreadPriorityOutcome::Denied) {
      spdlog::warn("audio worker thread priority: {}", priority.description);
    } else {
      spdlog::info("audio worker thread priority: {}", priority.description);
    }

    for (;;) {
      std::optional<AudioCommand> command;
      {
        std::unique_lock lock{commandMutex_};
        if (commands_.empty() && progressWorkerRunning_.load(std::memory_order_acquire) && !audioWorkerStopping_) {
          commandAvailable_.wait_for(lock, std::chrono::milliseconds{2});
        } else {
          commandAvailable_.wait(lock, [&] {
            return audioWorkerStopping_ || !commands_.empty() || progressWorkerRunning_.load(std::memory_order_acquire);
          });
        }
        if (!commands_.empty()) {
          command = std::move(commands_.front());
          commands_.pop_front();
        } else if (audioWorkerStopping_) {
          break;
        }
      }

      if (command) {
        (*command)();
      } else if (progressWorkerRunning_.load(std::memory_order_acquire)) {
        servicePlaybackProgress();
      }
    }

    stopProgressWorker();
    stopDevice();
    device_.uninitialize();
  }

  FfmpegFilterTargetFormat requestedTarget(const FfmpegAudioStreamInfo& streamInfo) const {
    return FfmpegFilterTargetFormat{config_.targetSampleRate.value_or(streamInfo.sampleRate),
                                    config_.targetSampleFormat.value_or(AudioSampleFormat::Float32),
                                    config_.targetChannelCount.value_or(streamInfo.channelCount)};
  }

  FfmpegFilterTargetFormat sourceTarget(const FfmpegAudioStreamInfo& streamInfo) const {
    return FfmpegFilterTargetFormat{streamInfo.sampleRate, streamInfo.sampleFormat, streamInfo.channelCount};
  }

  std::vector<OutputNegotiationCandidate> outputCandidates(const FfmpegAudioStreamInfo& streamInfo) const {
    const auto requested = requestedTarget(streamInfo);
    const auto source = sourceTarget(streamInfo);
    std::vector<OutputNegotiationCandidate> candidates;

    // 格式级降级链：在用户指定 target 基础上仅降位深（采样率/声道保持用户指定），
    // 依序 Int16 → Float32；已加入过的格式不重复；source 兜底在链尾单独判断。
    const auto pushFormatFallback = [&](AudioSampleFormat format) {
      const bool alreadyPresent = std::any_of(candidates.begin(), candidates.end(),
                                              [&](const OutputNegotiationCandidate& candidate) {
                                                return candidate.config.outputMode == AudioOutputMode::Mixed &&
                                                       candidate.target.sampleFormat == format;
                                              });
      if (alreadyPresent) {
        return;
      }
      auto target = requested;
      target.sampleFormat = format;
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Mixed, target),
                                                      target,
                                                      formatFallbackReason(requested.sampleFormat, format)});
    };

    const auto pushSourceFallback = [&]() {
      const bool sourceCovered = std::any_of(candidates.begin(), candidates.end(),
                                             [&](const OutputNegotiationCandidate& candidate) {
                                               return candidate.config.outputMode == AudioOutputMode::Mixed &&
                                                      sameTarget(candidate.target, source);
                                             });
      if (sourceCovered) {
        return;
      }
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Mixed, source),
                                                      source,
                                                      "requested mixed output format was unavailable; using source format"});
    };

    if (config_.outputMode == AudioOutputMode::Direct) {
      // 直接输出：设备按曲目原生参数初始化（采样率/声道/格式），
      // 用户配置的 target* 只作用于混合模式；Direct 不可用时降级 Mixed。
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Direct, source),
                                                      source,
                                                      {}});
      if (!config_.allowFallback) {
        return candidates;
      }
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Mixed, requested),
                                                      requested,
                                                      "direct output mode was unavailable; using mixed output mode"});
      pushFormatFallback(AudioSampleFormat::Int16);
      pushFormatFallback(AudioSampleFormat::Float32);
      pushSourceFallback();
      return candidates;
    }

    candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, config_.outputMode, requested), requested, {}});
    if (!config_.allowFallback) {
      return candidates;
    }

    pushFormatFallback(AudioSampleFormat::Int16);
    pushFormatFallback(AudioSampleFormat::Float32);
    pushSourceFallback();

    return candidates;
  }

  std::optional<OutputNegotiationResult> negotiateOutput(const FfmpegAudioStreamInfo& streamInfo,
                                                         std::string& failureDetail,
                                                         std::optional<AudioOutputDeviceError>& deviceError) {
    std::ostringstream failures;
    const auto candidates = outputCandidates(streamInfo);
    for (const auto& candidate : candidates) {
      const auto sampleBytes = bytesPerSample(candidate.target.sampleFormat);
      if (candidate.target.sampleRate == 0U || candidate.target.channelCount == 0U || sampleBytes == 0U) {
        failures << describeTarget(candidate.config.outputMode, candidate.target)
                 << " rejected because sample rate, channel count, and sample bytes must be nonzero; ";
        continue;
      }

      // T3：filter pipeline 枚举级验证（validateTarget）纳入协商循环——候选必须同时
      // 通过 pipeline 验证与设备打开才能被选中；configure 幂等，可对每候选重复调用。
      if (const auto error = pipeline_->configure(candidate.target)) {
        failures << describeTarget(candidate.config.outputMode, candidate.target)
                 << " rejected by filter pipeline validation: " << error->detail << "; ";
        continue;
      }

      const auto capacityFrames = bufferFrameCount(candidate.target.sampleRate, candidate.config.bufferDuration);
      auto candidateQueue = std::make_unique<PcmBufferQueue>(
          PcmBufferQueueConfig{capacityFrames, candidate.target.channelCount * sampleBytes});

      AudioOutputDeviceOpenRequest openRequest{};
      openRequest.config = candidate.config;
      openRequest.sampleFormat = candidate.target.sampleFormat;
      openRequest.sampleRate = candidate.target.sampleRate;
      openRequest.channelCount = candidate.target.channelCount;
      openRequest.bufferFrames = std::min<std::uint32_t>(capacityFrames, 512U);
      openRequest.pcmQueue = candidateQueue.get();

      if (!device_.initialize(openRequest)) {
        if (const auto error = device_.lastError(); error && error->code == PlaybackErrorCode::DeviceUnavailable) {
          deviceError = *error;
          return std::nullopt;
        }

        failures << describeTarget(candidate.config.outputMode, candidate.target)
                 << " rejected by audio output device; ";
        continue;
      }

      auto selectedFormat = device_.currentFormat();
      selectedFormat.fallbackApplied = !candidate.fallbackReason.empty();
      queue_ = std::move(candidateQueue);
      return OutputNegotiationResult{candidate.config, candidate.target, selectedFormat, candidate.fallbackReason};
    }

    failureDetail = failures.str();
    return std::nullopt;
  }

  // T7 免重开短路判据（D4，调用点保证设备已停）：outputMode==Mixed 且设备已初始化、
  // 驱动设备开启的配置未变（同上 sameOutputConfig：配置变更先于 LoadTrack 到达 worker）、
  // 设备当前实际模式仍为 Mixed，且本次协商将选出的目标（requestedTarget = 用户
  // target* 覆盖或流原生参数）与设备当前格式（采样率/格式/声道）一致。注意不能依赖
  // hasCurrentTarget_——loadTrackOnWorker 开头会把它复位，短路判定在复位之后执行；
  // activeDeviceConfig_（仅协商成功时写入）+ initialized 已等价表示"此前成功开过设备"。
  bool canReuseOutputDevice(const FfmpegAudioStreamInfo& streamInfo) const {
    if (config_.outputMode != AudioOutputMode::Mixed || !device_.initialized() ||
        !activeDeviceConfig_.has_value()) {
      return false;
    }
    if (!sameOutputConfig(config_, *activeDeviceConfig_)) {
      return false;
    }
    const auto& current = device_.currentFormat();
    if (current.sampleRate == 0U || current.actualMode != AudioOutputMode::Mixed) {
      return false;
    }
    const auto requested = requestedTarget(streamInfo);
    return requested.sampleRate == current.sampleRate && requested.sampleFormat == current.sampleFormat &&
           requested.channelCount == current.channelCount;
  }

  bool fillQueue() {
    if (!source_ || !pipeline_ || !queue_ || loadedToEnd_) {
      return true;
    }

    if (pendingFrameWrite_ && !writePendingFrame()) {
      return true;
    }

    if (trackEndPosition_.has_value()) {
      const auto currentPosition = clock_.snapshot().position;
      if (currentPosition >= *trackEndPosition_) {
        spdlog::debug("track boundary reached: current={}ms, end={}ms",
                      currentPosition.count(),
                      trackEndPosition_->count());
        loadedToEnd_ = true;
        return true;
      }
    }

    while (queue_->availableFrames() < queue_->capacityFrames()) {
      if (trackEndPosition_.has_value()) {
        const auto currentPosition = clock_.snapshot().position;
        if (currentPosition >= *trackEndPosition_) {
          spdlog::debug("track boundary reached during fill: current={}ms, end={}ms",
                        currentPosition.count(),
                        trackEndPosition_->count());
          loadedToEnd_ = true;
          return true;
        }
      }

      auto readResult = source_->readFrame();
      if (readResult.error) {
        fail(readResult.error->code, readResult.error->message, readResult.error->detail);
        return false;
      }
      if (readResult.endOfStream) {
        if (const auto error = pipeline_->signalEndOfInput()) {
          fail(error->code, error->message, error->detail);
          return false;
        }
        if (!drainPipeline(true)) {
          return false;
        }
        loadedToEnd_ = true;
        return true;
      }
      if (!readResult.frame) {
        continue;
      }
      if (const auto error = pipeline_->pushFrame(*readResult.frame)) {
        fail(error->code, error->message, error->detail);
        return false;
      }
      if (!drainPipeline(false)) {
        return false;
      }
      if (loadedToEnd_) {
        return true;
      }
    }

    return true;
  }

  bool fillPreloadSlot(PreloadSlot& slot) {
    if (!slot.source || !slot.pipeline || !slot.queue || slot.loadedToEnd) {
      return true;
    }

    if (slot.pendingFrameWrite && !writePendingPreloadFrame(slot)) {
      return true;
    }

    while (slot.queue->availableFrames() < slot.queue->capacityFrames()) {
      auto readResult = slot.source->readFrame();
      if (readResult.error) {
        emitPreloadError(readResult.error->code, readResult.error->message, readResult.error->detail);
        return false;
      }
      if (readResult.endOfStream) {
        if (const auto error = slot.pipeline->signalEndOfInput()) {
          emitPreloadError(error->code, error->message, error->detail);
          return false;
        }
        if (!drainPreloadPipeline(slot, true)) {
          return false;
        }
        slot.loadedToEnd = true;
        return true;
      }
      if (!readResult.frame) {
        continue;
      }
      if (const auto error = slot.pipeline->pushFrame(*readResult.frame)) {
        emitPreloadError(error->code, error->message, error->detail);
        return false;
      }
      if (!drainPreloadPipeline(slot, false)) {
        return false;
      }
      if (slot.loadedToEnd) {
        return true;
      }
    }

    return true;
  }

  bool drainPipeline(bool expectEnd) {
    for (int guard = 0; guard < 256; ++guard) {
      auto filterResult = pipeline_->readFrame();
      if (filterResult.error) {
        fail(filterResult.error->code, filterResult.error->message, filterResult.error->detail);
        return false;
      }
      if (filterResult.endOfStream) {
        return true;
      }
      if (!filterResult.frame) {
        return !expectEnd;
      }
      if (!writeFrame(*filterResult.frame)) {
        return true;
      }
    }

    fail(PlaybackErrorCode::DecodeFailed, "filter pipeline did not drain within guard limit", "guard exhausted while reading filtered frames");
    return false;
  }

  bool drainPreloadPipeline(PreloadSlot& slot, bool expectEnd) {
    for (int guard = 0; guard < 256; ++guard) {
      auto filterResult = slot.pipeline->readFrame();
      if (filterResult.error) {
        emitPreloadError(filterResult.error->code, filterResult.error->message, filterResult.error->detail);
        return false;
      }
      if (filterResult.endOfStream) {
        return true;
      }
      if (!filterResult.frame) {
        return !expectEnd;
      }
      if (!writePreloadFrame(slot, *filterResult.frame)) {
        return true;
      }
    }

    emitPreloadError(PlaybackErrorCode::DecodeFailed,
                     "preload filter pipeline did not drain within guard limit",
                     "guard exhausted while reading filtered frames");
    return false;
  }

  bool writeFrame(const FfmpegAudioFrame& frame) {
    const auto trimmed = trimFrameToBoundary(frame, trackEndPosition_);
    if (trimmed.reachedBoundary) {
      loadedToEnd_ = true;
    }
    if (!trimmed.frame.has_value()) {
      pendingFrameWrite_.reset();
      return true;
    }

    const auto& boundedFrame = *trimmed.frame;
    const auto bytesPerFrame = static_cast<std::size_t>(boundedFrame.channelCount) * bytesPerSample(boundedFrame.sampleFormat);
    if (bytesPerFrame == 0U || boundedFrame.sampleBytes.size() != static_cast<std::size_t>(boundedFrame.frameCount) * bytesPerFrame) {
      fail(PlaybackErrorCode::DecodeFailed, "filtered frame has invalid PCM payload", "sampleBytes size does not match frame shape");
      return false;
    }

    pendingFrameWrite_ = PendingFrameWrite{*trimmed.frame, 0U};
    return writePendingFrame();
  }

  bool writePendingFrame() {
    if (!pendingFrameWrite_) {
      return true;
    }

    auto& pending = *pendingFrameWrite_;
    const auto bytesPerFrame = static_cast<std::size_t>(pending.frame.channelCount) * bytesPerSample(pending.frame.sampleFormat);
    while (pending.writtenFrames < pending.frame.frameCount) {
      const auto availableCapacity = queue_->capacityFrames() - queue_->availableFrames();
      if (availableCapacity == 0U) {
        return false;
      }

      const auto chunkFrames = std::min(pending.frame.frameCount - pending.writtenFrames, availableCapacity);
      const auto byteOffset = static_cast<std::size_t>(pending.writtenFrames) * bytesPerFrame;
      if (!queue_->write(pending.frame.sampleBytes.data() + byteOffset, chunkFrames)) {
        return false;
      }
      pending.writtenFrames += chunkFrames;
      clock_.submitFrames(chunkFrames);
    }

    pendingFrameWrite_.reset();
    return true;
  }

  bool writePreloadFrame(PreloadSlot& slot, const FfmpegAudioFrame& frame) {
    const auto trimmed = trimFrameToBoundary(frame, slot.endPosition);
    if (trimmed.reachedBoundary) {
      slot.loadedToEnd = true;
    }
    if (!trimmed.frame.has_value()) {
      slot.pendingFrameWrite.reset();
      return true;
    }

    const auto& boundedFrame = *trimmed.frame;
    const auto bytesPerFrame = static_cast<std::size_t>(boundedFrame.channelCount) * bytesPerSample(boundedFrame.sampleFormat);
    if (bytesPerFrame == 0U || boundedFrame.sampleBytes.size() != static_cast<std::size_t>(boundedFrame.frameCount) * bytesPerFrame) {
      emitPreloadError(PlaybackErrorCode::DecodeFailed, "preloaded frame has invalid PCM payload", "sampleBytes size does not match frame shape");
      return false;
    }

    slot.pendingFrameWrite = PendingFrameWrite{*trimmed.frame, 0U};
    return writePendingPreloadFrame(slot);
  }

  bool writePendingPreloadFrame(PreloadSlot& slot) {
    if (!slot.pendingFrameWrite || !slot.queue) {
      return true;
    }

    auto& pending = *slot.pendingFrameWrite;
    const auto bytesPerFrame = static_cast<std::size_t>(pending.frame.channelCount) * bytesPerSample(pending.frame.sampleFormat);
    while (pending.writtenFrames < pending.frame.frameCount) {
      const auto availableCapacity = slot.queue->capacityFrames() - slot.queue->availableFrames();
      if (availableCapacity == 0U) {
        return false;
      }

      const auto chunkFrames = std::min(pending.frame.frameCount - pending.writtenFrames, availableCapacity);
      const auto byteOffset = static_cast<std::size_t>(pending.writtenFrames) * bytesPerFrame;
      if (!slot.queue->write(pending.frame.sampleBytes.data() + byteOffset, chunkFrames)) {
        return false;
      }
      pending.writtenFrames += chunkFrames;
    }

    slot.pendingFrameWrite.reset();
    return true;
  }

  void servicePlaybackProgress() {
    const auto deferredUnderrun = updateClockFromQueue(UnderrunReporting::Suppress);

    if (finishingAction_ != FinishingAction::None) {
      // —— T6 物理收尾 ticker：不受 Playing 门控（逻辑态已 Paused/Stopped）——
      tickFinishing(deferredUnderrun);
      return;  // 收尾完成后已停设备/定格发布；Playing 分支不属于收尾期
    }

    if (fadeInPending_ && device_.started() && device_.masterEnvelopeGain() <= kFadeCompleteGainEpsilon) {
      // 恢复淡入握手第二步：即时 0.0 已被回调执行（观测归零）→ 发布 0→1 单程淡入。
      publishTransportFadeIn();
    }

    if (stateMachine_.state() == PlaybackState::Playing && !loadedToEnd_) {
      const bool filled = fillQueue();
      if (filled && deferredUnderrun.silenceFrames > 0U && !loadedToEnd_) {
        emitBufferUnderrun(deferredUnderrun.silenceFrames, deferredUnderrun.underrunCount);
      }
      updateClockFromQueue(UnderrunReporting::Report);
    }
    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && pendingFrameWrite_) {
      static_cast<void>(writePendingFrame());
      updateClockFromQueue(UnderrunReporting::Report);
    }

    if (stateMachine_.state() == PlaybackState::Playing) {
      // —— T8 预解码接线：槽续解码驱动 + EndApproaching 预告 ——
      servicePreloadDecodeIfDue();
      maybeEmitEndApproaching();
      // —— T10 自动交叉重叠窗口调度（裁定⑦：窗内就绪即启；单发/臂裁决不延迟）——
      maybeStartAutoOverlap();
      tickOverlapRamp();
      // —— T11 手动交叉（档 3）：腿完采纳（无 AdvanceCompleted）——
      tickManualOverlapPromote();
    }

    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && !pendingFrameWrite_ && queue_ &&
        queue_->availableFrames() == 0U) {
      updateClockFromQueue(UnderrunReporting::Suppress);
      if (manualOverlapActive_) {
        // F2：手动交叉主源排空（当前曲先于交叉腿结束——剩余估算超前/钳制到剩余）
        // → 立即采纳（同下方自动路径排空提升语义）。双腿推进只随主源 copiedFrames
        // （设备双源 finalize 按主源计）——主源 EOF 后双腿冻结于残余值，promote 双条件
        // 永不达：不采纳 = 槽内容以冻结增益播至 EOF 后永久静音 + manualOverlapActive_
        // 卡死 + TrackChanged(B) 永不发（控制器已切 B → UI/后端漂移）。主源已空 =
        // source0 零贡献；source1 从冻结增益（≈1−数块/腿长）经采纳的源包络复位升至
        // 1.0，跳变 ≤ 残余量（数块增益，无半增益爆音——屏蔽注释的顾虑在排空态不成立）。
        completeManualOverlap();
        return;
      }
      // T10：主源排空 = 重叠期满 → 提升第二源为主源（AdvanceCompleted 先于新曲状态
      // 事件；见 completeOverlapHandoff）。未在重叠期时回落直切 handoff / 自然结束。
      if (completeOverlapHandoff()) {
        return;
      }
      if (handoffToPreparedNext()) {
        return;
      }

      stopDevice();
      clock_.pause();
      stateMachine_.naturalEnd();
      publishPosition();
      stopProgressWorkerAsync();
      return;
    }
    publishProgressIfDue();
  }

  void startProgressWorker() {
    if (progressWorkerRunning_.load(std::memory_order_acquire)) {
      return;
    }
    stopProgressWorker();
    progressWorkerRunning_.store(true, std::memory_order_release);
    commandAvailable_.notify_one();
  }

  void stopProgressWorker() { progressWorkerRunning_.store(false, std::memory_order_release); }

  void stopProgressWorkerAsync() { progressWorkerRunning_.store(false, std::memory_order_release); }

  // T8：预解码槽续解码驱动（仅 Playing tick 调用）。槽就绪且未解码到末尾时按需续解码：
  // fillPreloadSlot 只在队列有空位/有未决帧时真正工作——任务 9 双源回调面未启用时槽队列
  // 在 handoff 前无人消费 → 满队列零操作（维持"填满即停"，无交接目标的预载不空转）；
  // 任务 9 第二源启用后本驱动即"随消费持续解码"的供数点（分块喂双路防饿死）。
  void servicePreloadDecodeIfDue() {
    if (preloadSlot_ && preloadSlot_->ready && !preloadSlot_->loadedToEnd) {
      static_cast<void>(fillPreloadSlot(*preloadSlot_));
    }
  }

  // T8：剩余量估算——当前曲距自然终点的毫秒估计（EndApproaching 载荷来源）。
  // - CUE bounded 段：endPosition（offset+duration）− 时钟（精确，复用 trackEndPosition_）；
  // - 普通整轨（无 offset、带时长元数据）且未到 EOS：整轨时长 − 时钟（近似——仅作预解码
  //   提前量，绝不作硬停，时长元数据"非硬停"既有语义不变）；
  // - 已到 EOS：剩余 = 队列内 PCM（兜底，任何曲式可用）；
  // - 已到终点（loadedToEnd 且队列空）：无剩余——交由 naturalEnd 路径，EndApproaching
  //   不作 naturalEnd 的替代（裁定）。
  std::optional<std::chrono::milliseconds> estimateRemainingMs() const {
    if (!queue_) {
      return std::nullopt;
    }
    if (loadedToEnd_ && queue_->availableFrames() == 0U) {
      return std::nullopt;
    }
    const auto position = clock_.snapshot().position;
    if (trackEndPosition_.has_value()) {
      return *trackEndPosition_ - position;
    }
    if (!loadedToEnd_ && currentRequest_.duration.has_value() && !currentRequest_.offset.has_value() &&
        !currentRequest_.boundedSegment) {
      return *currentRequest_.duration - position;
    }
    if (loadedToEnd_) {
      const auto rate = currentTarget_.sampleRate != 0U ? currentTarget_.sampleRate : clock_.sampleRate();
      if (rate == 0U) {
        return std::nullopt;
      }
      return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(
          (static_cast<std::uint64_t>(queue_->availableFrames()) * 1000ULL) / rate)};
    }
    return std::nullopt;
  }

  // T8：进度轮询中的 EndApproaching 发射（armed 去重：一次性/臂）。
  // 触发阈值 = max(crossfadeMs, gaplessPreloadMs)（裁定）。仅当本臂存在"需要预解码"的
  // 语义才武装：Mixed +（自动档可交叉（长度>0）或 预加载>0）。Direct 与默认配置
  // （自动档=无 且 预加载=0）不武装 → 默认路径零新事件（Must Have 默认值等价回归）。
  void maybeEmitEndApproaching() {
    if (endApproachEmitted_) {
      return;
    }
    const auto threshold = std::max(transitionConfig_.crossfadeMs, transitionConfig_.gaplessPreloadMs);
    if (threshold.count() <= 0 || config_.outputMode != AudioOutputMode::Mixed) {
      return;
    }
    if (transitionConfig_.autoAdvanceFadeMode == AutoAdvanceFadeMode::Off &&
        transitionConfig_.gaplessPreloadMs.count() <= 0) {
      return;
    }
    const auto remaining = estimateRemainingMs();
    if (!remaining.has_value() || remaining->count() <= 0 || *remaining > threshold) {
      return;
    }
    endApproachEmitted_ = true;
    spdlog::debug("end approaching: remaining={}ms (threshold={}ms)", remaining->count(), threshold.count());
    dispatcher_.dispatch(BackendEventType::EndApproaching, EndApproaching{*remaining});
  }

  bool handoffToPreparedNext() {
    if (!preloadSlot_ || !preloadSlot_->ready || !preloadSlot_->seamlessEligible || !queue_ ||
        !sameTarget(preloadSlot_->target, currentTarget_) || device_.secondSourceActive()) {
      // T9 守卫：槽队列已作为第二源发布（交叉重叠中）时禁止 handoff——回调正在消费该
      // ring，排空移交会撕裂第二源且随后的槽析构触碰已发布队列。重叠期结束/退役由
      // 任务 10/11 调度；此刻无生产 activate 调用方，本守卫恒不生效（现状不变）。
      return false;
    }

    auto slot = std::move(*preloadSlot_);
    preloadSlot_.reset();
    // T10：无缝直切同为接管提交——AdvanceCompleted 最先发出（先于新曲 TrackChanged/
    // 状态事件；控制器据此校验 pendingAdvance 账本并提交，窗口内无 pending 时丢弃）。
    dispatcher_.dispatch(BackendEventType::AdvanceCompleted, AdvanceCompleted{slot.request.trackId});
    source_ = std::move(slot.source);
    pipeline_ = std::move(slot.pipeline);
    currentRequest_ = slot.request;
    currentTarget_ = slot.target;
    trackEndPosition_ = slot.endPosition;
    pendingFrameWrite_ = std::move(slot.pendingFrameWrite);
    hasCurrentTarget_ = true;
    loadedToEnd_ = false;

    clock_.reset(currentRequest_.trackId, currentTarget_.sampleRate, currentRequest_.offset.value_or(std::chrono::milliseconds{0}));
    clock_.resume();
    stateMachine_.loadTrack(currentRequest_);
    stateMachine_.completeLoad();
    stateMachine_.play();
    // T8：handoff = 新曲接管（等价新 loadTrack）——预解码预告重新武装，使 RepeatOne
    // 自身重播/自动前进链上的每首新曲都能再次触发 EndApproaching。
    endApproachEmitted_ = false;
    // T10：直切同样开启新臂（交叉窗口裁决锁存复位——declined 臂在直切兜底后不残留）。
    overlapWindowSettled_ = false;
    overlapRampPending_ = false;

    transferPreloadedPcm(slot);
    loadedToEnd_ = slot.loadedToEnd && slot.queue && slot.queue->availableFrames() == 0U;
    if (!loadedToEnd_) {
      static_cast<void>(fillQueue());
    }
    observedQueueCounters_ = queue_->counters();
    publishPosition();
    return true;
  }

  void transferPreloadedPcm(PreloadSlot& slot) {
    if (!slot.queue || !queue_) {
      return;
    }

    std::vector<std::uint8_t> pcm;
    while (slot.queue->availableFrames() > 0U && queue_->availableFrames() < queue_->capacityFrames()) {
      const auto writableFrames = queue_->capacityFrames() - queue_->availableFrames();
      const auto frames = std::min(slot.queue->availableFrames(), writableFrames);
      pcm.assign(static_cast<std::size_t>(frames) * queue_->bytesPerFrame(), 0U);
      const auto readResult = slot.queue->read(pcm.data(), frames);
      if (readResult.copiedFrames == 0U) {
        return;
      }
      if (!queue_->write(pcm.data(), readResult.copiedFrames)) {
        return;
      }
      clock_.submitFrames(readResult.copiedFrames);
    }
  }

  void publishPosition() {
    updateClockFromQueue(UnderrunReporting::Report);
    dispatchPosition(clock_.snapshot());
  }

  void publishProgressIfDue() {
    if (stateMachine_.state() != PlaybackState::Playing) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto snapshot = clock_.snapshot();
    if (lastPublishedPosition_.has_value() && snapshot.position == *lastPublishedPosition_) {
      return;
    }
    if (lastProgressPublish_ != std::chrono::steady_clock::time_point{} && now - lastProgressPublish_ < kProgressPublishInterval) {
      return;
    }
    dispatchPosition(snapshot);
  }

  void dispatchPosition(const PlaybackClockSnapshot& snapshot) {
    lastProgressPublish_ = std::chrono::steady_clock::now();
    lastPublishedPosition_ = snapshot.position;
    {
      std::lock_guard lock{snapshotMutex_};
      lastClockSnapshot_ = snapshot;
    }
    dispatcher_.dispatch(BackendEventType::PlaybackPositionUpdated, PlaybackPositionUpdated{snapshot});
  }

  enum class UnderrunReporting { Report, Suppress };

  QueueUnderrunDelta updateClockFromQueue(UnderrunReporting underrunReporting = UnderrunReporting::Report) {
    QueueUnderrunDelta underrun{};
    if (!queue_) {
      return underrun;
    }

    const auto counters = queue_->counters();
    if (counters.consumedFrames > observedQueueCounters_.consumedFrames) {
      clock_.consumeFrames(counters.consumedFrames - observedQueueCounters_.consumedFrames);
    }
    if (counters.silenceFrames > observedQueueCounters_.silenceFrames) {
      underrun.silenceFrames = counters.silenceFrames - observedQueueCounters_.silenceFrames;
      underrun.underrunCount = counters.underrunCount - observedQueueCounters_.underrunCount;
      clock_.reportUnderrun(underrun.silenceFrames);
      if (underrunReporting == UnderrunReporting::Report) {
        emitBufferUnderrun(underrun.silenceFrames, underrun.underrunCount);
      }
    }
    observedQueueCounters_ = counters;
    return underrun;
  }

  void stopDevice() {
    // 设备停止 = 任何未决恢复淡入握手作废（T6：握手只活在运行态）。
    fadeInPending_ = false;
    if (device_.started()) {
      static_cast<void>(device_.stop());
    }
    // T9/T10：退役 ring 的销毁点——设备已停（无活跃回调）才允许析构已发布过的队列
    // （见 retireSecondSourceOnWorker / completeOverlapHandoff / retiredQueue_）；stop
    // 失败时 device 仍 started → 不移交销毁。
    if (!device_.started()) {
      retiredQueue_.reset();
    }
  }

  // ---- T9/T10 第二源退役与自动交叉重叠窗口（worker 线程）----
  // 第二源 = 经 device_.activateSecondSource 发布到回调面的 preloadSlot_ 队列。撤销
  // 纪律：设备运行中 deactivate（淡交结束/中止/提升）后回调至多再持有一个 block 的旧
  // 指针 → ring 不可立即析构：先停入 retiredQueue_（延迟回收），销毁点 = 下一次
  // stopDevice（设备停 = 无活跃回调，同 T7「先停后换」纪律）。设备硬停路径自身安全：
  // AudioOutputDevice::stop/uninitialize 会把第二源回调面一并清空（active=false/代次
  // 递增/指针置空），停后 preloadSlot_.reset() 无竞态。
  void retireSecondSourceOnWorker() {
    if (!preloadSlot_ || !preloadSlot_->queue || !device_.secondSourceActive()) {
      return;
    }
    device_.deactivateSecondSource();
    retiredQueue_ = std::move(preloadSlot_->queue);
  }

  // T10：撤除在途交叉重叠面（幂等；不触碰 preloadSlot_ 本体与预解码预告武装——
  // 调用方按语义决定是否弃槽/重新武装）。步骤：退第二源（若激活）→ 两源包络轨迹
  // 打断（层复位到常量 1.0，version→0 使下臂发布直接可受理；打断即瞬时，裁定）→
  // 窗口锁存复位。configureTransition / prepareNextOnWorker / abortTransition 共用。
  void cancelOverlapFaceOnWorker() {
    retireSecondSourceOnWorker();
    device_.resetSourceEnvelope(0);
    device_.resetSourceEnvelope(1);
    overlapRampPending_ = false;
    overlapWindowSettled_ = false;
    // T11：面已撤 = 任何手动交叉随之死亡（锁存清除；否则主源排空后自然结束被
    // manualOverlapActive_ 误屏蔽——configureTransition/abort/新 loadTrack 都经本函数）。
    manualOverlapActive_ = false;
    overlapLegMs_ = std::chrono::milliseconds{0};
  }

  // T11：重叠面激活时中止在途交叉（pause/stop/seek 服务侧直达命令共用；与 abortTransition
  // 语义对齐 = 撤面 + 弃槽 + 预解码预告重新武装）。
  void abortInFlightOverlapOnWorker() {
    cancelOverlapFaceOnWorker();
    preloadSlot_.reset();
    endApproachEmitted_ = false;
  }

  // T11：手动交叉启动（档 3 复用已预解码槽；裁定⑤）。与自动 startOverlap 同典序
  // （槽 1 即时 0 激活 → source0 等功率下行腿；槽 1 上行腿由 tickOverlapRamp 观测归零
  // 后发布），差异：腿长 = min(crossfadeMs, 当前曲剩余)（裁定⑧ 临近曲末钳制）、preload
  // 槽由调用方按匹配选定（kindMeta 无关）、窗口锁存置位防自动调度干扰。返回 false =
  // 设备拒绝激活（N8 守卫；槽已废）——调用方按降级链转 dip/硬切，永不等待。
  bool startManualOverlap(std::chrono::milliseconds legMs) {
    overlapLegMs_ = legMs;
    sourceEnvelopeControllers_[1].syncCurrentGain(device_.sourceEnvelopeGain(1));
    const auto instantZero = sourceEnvelopeControllers_[1].makeRampSnapshot(
        0.0F, std::chrono::milliseconds{0}, GainEnvelopeCurve::Linear, envelopeSampleRate());
    device_.activateSecondSource(*preloadSlot_->queue, instantZero);
    if (!device_.secondSourceActive()) {
      spdlog::error("manual overlap start refused by device (second source inactive); "
                    "falling back to degraded transition");
      overlapLegMs_ = std::chrono::milliseconds{0};
      preloadSlot_.reset();
      return false;
    }
    overlapWindowSettled_ = true;
    manualOverlapActive_ = true;
    publishSourceEnvelope(0, 0.0F, legMs, GainEnvelopeCurve::EqualPowerPair);
    overlapRampPending_ = true;
    spdlog::debug("manual overlap started: legs={}ms", legMs.count());
    return true;
  }

  // T11：手动交叉腿完采纳（Playing tick 调用；裁定手动切换无 AC——控制器已自行选曲切换，
  // pendingAdvance 账本不存在，发射 AC 只会落 stale-drop，显式不发出）。复用自动提升主体
  // （含时钟基 = offset + 重叠期槽 ring 消费量），仅跳过 AdvanceCompleted 事件。
  void completeManualOverlap() {
    if (!manualOverlapActive_) {
      return;
    }
    manualOverlapActive_ = false;
    overlapLegMs_ = std::chrono::milliseconds{0};
    static_cast<void>(completeOverlapHandoff(/*emitAdvanceCompleted=*/false));
  }

  // T10：手动交叉腿完判定（Playing tick 调用，紧跟 tickOverlapRamp）：source0 下行腿归零
  // 且 source1 上行腿到顶 → 采纳。等功率对两腿同长、槽 1 腿约晚 1 块启动 → source1 到顶
  // 时 source0 必然已归零（双条件防提前 reset 源包络 = 半增益跳变爆音）。
  void tickManualOverlapPromote() {
    if (!manualOverlapActive_) {
      return;
    }
    if (device_.sourceEnvelopeGain(0) <= kFadeCompleteGainEpsilon &&
        device_.sourceEnvelopeGain(1) >= 1.0F - kFadeCompleteGainEpsilon) {
      completeManualOverlap();
    }
  }

  // T10：源层包络发布唯一入口（worker；纪律同 publishMasterEnvelope——账本同步读回
  // 回调真值 → 产出快照（版本递增）→ 发布）。slot < kActiveSourceEnvelopeSlots。
  void publishSourceEnvelope(std::size_t slot,
                             float targetGain,
                             std::chrono::milliseconds durationMs,
                             GainEnvelopeCurve curve) {
    auto& controller = sourceEnvelopeControllers_[slot];
    controller.syncCurrentGain(device_.sourceEnvelopeGain(slot));
    const auto snapshot = controller.makeRampSnapshot(targetGain, durationMs, curve, envelopeSampleRate());
    device_.setSourceEnvelope(slot, snapshot);
  }

  // T10：自动交叉重叠窗口调度（Playing tick 调用；裁定⑦ 时序、⑧ 就绪度不延迟）。
  // 窗口点 = 估剩 ≤ crossfadeMs 的首个 tick；单发/臂裁决（overlapWindowSettled_）：
  // - 槽尚未建立（控制器 PrepareNext 在途/未下发）：等待——不裁决（裁定⑧ 不延迟指
  //   不拖慢自然终点；prepare 已在管线中，非"未就绪硬等"，见 EA 阈值 == crossfadeMs
  //   时两者同 tick 的竞争场景）；
  // - 槽已建立但未就绪 / 交接方式非 Crossfade：本臂放弃交叉（declined——不等待不
  //   延迟，自然终点按直切/普通自然结束兜底，裁定⑧）；
  // - 槽就绪且交接方式 = Crossfade：启动交叉（就绪即启）。
  void maybeStartAutoOverlap() {
    if (overlapWindowSettled_ || device_.secondSourceActive()) {
      return;
    }
    const auto crossfadeMs = transitionConfig_.crossfadeMs;
    if (crossfadeMs.count() <= 0 || config_.outputMode != AudioOutputMode::Mixed) {
      return;
    }
    const auto remaining = estimateRemainingMs();
    if (!remaining.has_value() || *remaining > crossfadeMs) {
      return;
    }
    if (!preloadSlot_) {
      return;  // prepare 在途：等槽建立后再裁决（下个 tick 自然受理）。
    }
    // 窗口到点且槽已存在：一次性裁决（锁定——不重试；裁定⑧ 就绪即启/未就绪不延迟）。
    overlapWindowSettled_ = true;
    if (!preloadSlot_->ready || preloadSlot_->kindMeta.kind != PrepareNextKind::Crossfade) {
      spdlog::debug("overlap window declined for arm (slot ready={}, meta kind=crossfade={})",
                    preloadSlot_->ready,
                    preloadSlot_->kindMeta.kind == PrepareNextKind::Crossfade);
      return;
    }
    startOverlap(crossfadeMs);
  }

  // T10：启动交叉重叠。N7 典序：先以「即时 0」包络激活第二源（槽 1 层落零——B2 前置：
  // 0→1 EQ 轨迹须先经即时 0 把回调 currentGain 落到 0），激活成功后再发布主源 1→0
  // 腿（EQ 对、时长 = crossfadeMs）；槽 1 的 0→1 腿由 tickOverlapRamp 观测归零后发布
  // （同长 EQ 对）。设备拒绝激活（N8 格式守卫）→ 本臂已裁决、放弃交叉（兜底直切）。
  void startOverlap(std::chrono::milliseconds crossfadeMs) {
    overlapLegMs_ = crossfadeMs;
    sourceEnvelopeControllers_[1].syncCurrentGain(device_.sourceEnvelopeGain(1));
    const auto instantZero = sourceEnvelopeControllers_[1].makeRampSnapshot(
        0.0F, std::chrono::milliseconds{0}, GainEnvelopeCurve::Linear, envelopeSampleRate());
    device_.activateSecondSource(*preloadSlot_->queue, instantZero);
    if (!device_.secondSourceActive()) {
      spdlog::error("overlap start refused by device (second source inactive); arm declined");
      return;
    }
    publishSourceEnvelope(0, 0.0F, crossfadeMs, GainEnvelopeCurve::EqualPowerPair);
    overlapRampPending_ = true;
    spdlog::debug("overlap started: crossfade={}ms", crossfadeMs.count());
  }

  // T10：交叉第二腿发布（Playing tick 调用）：槽 1 回调增益归零（即时 0 已被执行）后
  // 发布 0→1 腿（时长 = overlapLegMs_ 的 EQ 对，与主源 1→0 腿功率互补）。腿长来源：
  // 自动 = crossfadeMs（startOverlap 设定）；手动 = min(crossfadeMs, 剩余)
  // （startManualOverlap 设定）——两腿必须同长，EQ 互补才成立。
  void tickOverlapRamp() {
    if (!overlapRampPending_) {
      return;
    }
    if (device_.sourceEnvelopeGain(1) <= kFadeCompleteGainEpsilon) {
      overlapRampPending_ = false;
      publishSourceEnvelope(1, 1.0F, overlapLegMs_, GainEnvelopeCurve::EqualPowerPair);
      spdlog::debug("overlap second leg published (0->1, {}ms)", overlapLegMs_.count());
    }
  }

  // T10：重叠期满（主源排空）→ 第二源提升为主源。时序：
  // 1) AdvanceCompleted 最先（先于新曲 TrackChanged/状态事件——控制器先提交后同步；
  //    仅自动交接发射——手动交叉（emitAdvanceCompleted=false）无 pendingAdvance 账本，
  //    控制器已自行选曲切换，AC 只会落 stale-drop）；
  // 2) 撤第二源面（回调至多再持一块旧指针）→ 主 face rebind 到槽 ring；
  // 3) 两源包络复位（打断在途交叉轨迹 → 常量 1.0，打断即瞬时，裁定）；
  // 4) 旧主 ring 停入 retiredQueue_（回调可能仍持有一块，延迟回收，见 stopDevice）。
  // 其余（时钟重置/状态机加载/重新武装/续解码/位置发布）与 handoffToPreparedNext 同构。
  bool completeOverlapHandoff(bool emitAdvanceCompleted = true) {
    if (!preloadSlot_ || !preloadSlot_->queue || !device_.secondSourceActive() || !queue_) {
      return false;
    }
    auto slot = std::move(*preloadSlot_);
    preloadSlot_.reset();
    overlapRampPending_ = false;
    overlapWindowSettled_ = false;
    // T10：第二源在重叠窗口期已被回调消费（主源排空前同步播放）——新曲时钟须从
    // 已消费帧对应的位置续走（而非 0），否则新曲总时长被重叠期截短（位置失真）。
    // 消费帧计数来自 ring 自身（prepare 期仅写不读、activate 前无人消费 → 计数值
    // 即激活后回调消费量；与 clock/observedQueueCounters_ 无关，无竞态）。
    const auto rate = slot.target.sampleRate != 0U ? slot.target.sampleRate : clock_.sampleRate();
    std::chrono::milliseconds consumedMs{0};
    if (rate != 0U) {
      consumedMs = std::chrono::milliseconds{
          (slot.queue->counters().consumedFrames * 1000ULL) / rate};
    }
    if (emitAdvanceCompleted) {
      dispatcher_.dispatch(BackendEventType::AdvanceCompleted, AdvanceCompleted{slot.request.trackId});
    }
    device_.deactivateSecondSource();
    device_.resetSourceEnvelope(0);
    device_.resetSourceEnvelope(1);
    retiredQueue_ = std::move(queue_);
    device_.rebindQueue(*slot.queue);
    queue_ = std::move(slot.queue);
    source_ = std::move(slot.source);
    pipeline_ = std::move(slot.pipeline);
    currentRequest_ = slot.request;
    currentTarget_ = slot.target;
    trackEndPosition_ = slot.endPosition;
    pendingFrameWrite_ = std::move(slot.pendingFrameWrite);
    hasCurrentTarget_ = true;

    const auto basePosition =
        currentRequest_.offset.value_or(std::chrono::milliseconds{0}) + consumedMs;
    clock_.reset(currentRequest_.trackId, currentTarget_.sampleRate, basePosition);
    clock_.resume();
    stateMachine_.loadTrack(currentRequest_);
    stateMachine_.completeLoad();
    stateMachine_.play();
    // 新曲臂重新武装（同 handoff 语义）。
    endApproachEmitted_ = false;
    loadedToEnd_ = slot.loadedToEnd && queue_->availableFrames() == 0U;
    if (!loadedToEnd_) {
      static_cast<void>(fillQueue());
    }
    observedQueueCounters_ = queue_->counters();
    publishPosition();
    return true;
  }

  // ================= T6 物理收尾（finishing）与传送淡变 =================
  // 逻辑态 vs 物理淡出（Metis 缺口 3）：pause/stop 命令立即翻转逻辑态（Paused/Stopped
  // 事件先出），finishing 期间设备继续运行、按 master 包络单程淡出到零；时钟随消费
  // 走到归零点定格。纯 worker 线程内部模式，不是状态机状态（对外枚举/事件零变化）。

  bool transportFadeEnabled() const {
    return transitionConfig_.fadeOnTransport && transitionConfig_.transportFadeMs.count() > 0;
  }

  std::uint32_t envelopeSampleRate() const {
    if (currentTarget_.sampleRate != 0U) {
      return currentTarget_.sampleRate;
    }
    return device_.currentFormat().sampleRate;
  }

  // worker 侧包络发布（T5 纪律）：经 GainEnvelopeController 账本产出快照（版本递增、
  // ms→帧换算、目标钳制），发布前用回调原子读回同步账本增益。只允许 worker 线程调用。
  void publishMasterEnvelope(float targetGain, std::chrono::milliseconds durationMs) {
    masterEnvelope_.syncCurrentGain(device_.masterEnvelopeGain());
    const auto snapshot = masterEnvelope_.makeRampSnapshot(targetGain,
                                                           durationMs,
                                                           GainEnvelopeCurve::Linear,
                                                           envelopeSampleRate());
    device_.setMasterEnvelope(snapshot);
  }

  // pause/stop 收尾入口（调用前逻辑态已翻转）：发布单程淡出（时长=transportFadeMs、
  // 线性、1.0→0），ticker 保持运行负责归零检测与解码续喂。
  void beginFinishing(FinishingAction action) {
    fadeInPending_ = false;  // 若有未决淡入握手，被本次淡出取代（握手即时 0 已落零）
    publishMasterEnvelope(0.0F, transitionConfig_.transportFadeMs);
    finishingAction_ = action;
    startProgressWorker();  // 收尾 ticker：2ms 轮询不 gate 于 Playing
    spdlog::info("finishing fade-out started ({}ms, action={})",
                 transitionConfig_.transportFadeMs.count(),
                 static_cast<int>(action));
  }

  // play/resume 冷恢复淡入握手第一步：先发布即时 0.0（durationFrames=0）。设备已停、
  // 包络已被 stop 复位到 1.0——T5-B2 前置：0→1 轨迹须先经即时 0 把回调 currentGain
  // 落到 0，ticker 观测归零后再发布淡入（第二步见 servicePlaybackProgress）。
  void armTransportFadeIn() {
    publishMasterEnvelope(0.0F, std::chrono::milliseconds{0});
    fadeInPending_ = true;
  }

  // 恢复淡入（0→1 单程，时长=transportFadeMs）。冷恢复由 ticker 在观测到即时 0 落零后
  // 调用；finishing 在途恢复由命令路径直接调用——执行器把竞争发布推迟到在途淡出结束，
  // 然后从回调 currentGain（≈0，块末粒度）自动受理，连续回升。
  void publishTransportFadeIn() {
    fadeInPending_ = false;
    publishMasterEnvelope(1.0F, transitionConfig_.transportFadeMs);
    spdlog::info("transport fade-in published ({}ms)", transitionConfig_.transportFadeMs.count());
  }

  // 中止收尾监督（play/resume 到达收尾淡出在途时）：finishing 不再于归零后停设备；
  // 在途淡出轨迹继续走完（执行器不接受中途取消），竞争淡入由执行器在轨迹结束后受理。
  // 返回 true 表示确实中止了一个在途收尾（调用方据此选择冷/热恢复淡入路径）。
  bool abortFinishingForRestart() {
    if (finishingAction_ == FinishingAction::None) {
      return false;
    }
    spdlog::info("finishing aborted by play/resume (action={})", static_cast<int>(finishingAction_));
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    // T11：finishing 期 seek 的定格目标随收尾取消一并作废（不会在下次收尾误用）。
    pendingFrozenSeekPosition_.reset();
    return true;
  }

  // ================= T11 seek dip 与手动切歌 dip（master 单腿对半监督） =================
  // 对半分解 dip（seek 与手动切换共用形状）：下行腿（1→0，线性，全长 ½）→ 归零点换数据/
  // 换源 → 即时 0（帧耗尽兜底时残余增益无声落零）+ 上行腿（0→1，全长 − ½）。两腿发布
  // 间隔内执行器自动延迟受理：下行在途时上行的竞争发布被拒（PENDING 保留），下行终点
  // 的下一块从回调 currentGain(=0) 受理上行 → 归零驻留 ≤1 块，即 dip 中点。设备全程不
  // 停止（stopDevice 复位包络会杀死下行腿），数据交换走 rebindQueue + retiredQueue_。

  void beginSeekDip(std::chrono::milliseconds position) {
    const auto totalMs = transitionConfig_.seekFadeMs;
    const auto downMs = totalMs / 2;
    const auto upMs = totalMs - downMs;
    seekDip_ = PendingSeekDip{position, upMs};
    finishingAction_ = FinishingAction::SeekDipDown;
    fadeInPending_ = false;
    publishMasterEnvelope(0.0F, downMs);
    startProgressWorker();
    spdlog::info("seek dip started: target={}ms, down={}ms, up={}ms",
                 position.count(), downMs.count(), upMs.count());
  }

  // SeekDipDown 归零点：物理 seek + 新 ring 换面 + 状态机 seek 事件 + 上行腿。
  void completeSeekDip() {
    if (!seekDip_) {
      finishingAction_ = FinishingAction::None;
      return;
    }
    const auto target = seekDip_->target;
    const auto upMs = seekDip_->upMs;
    seekDip_.reset();
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    spdlog::info("seek dip zero point: applying seek to {}ms", target.count());

    // 状态机 seek 先行（Loading + pendingSeek）——失败路径可 cancelSeek 回滚。
    const auto generation = stateMachine_.beginSeek(target);
    if (const auto error = source_->seek(target)) {
      spdlog::error("seek dip source seek failed: {} - {}", error->message, error->detail);
      // 源 seek 失败：放弃 dip（旧内容归零后仍可闻——即时回满增益恢复播放），
      // 状态机 cancelSeek 回滚（Loading → Playing + PlaybackError）。
      stateMachine_.cancelSeek(error->code, error->message, error->detail);
      publishMasterEnvelope(1.0F, std::chrono::milliseconds{0});
      publishPosition();
      return;
    }

    // 新 ring 构建并解码填满（设备运行中——回调只经代次读 face，worker 写 ring 安全；
    // 填满期间回调仍消费旧 ring（增益 0 静音），旧 ring 存活于本作用域直到换面或 fail）。
    auto previousRing = std::move(queue_);
    queue_ = std::make_unique<PcmBufferQueue>(PcmBufferQueueConfig{
        bufferFrameCount(currentTarget_.sampleRate, config_.bufferDuration),
        currentTarget_.channelCount * bytesPerSample(currentTarget_.sampleFormat)});
    pipeline_->reset();
    pendingFrameWrite_.reset();
    loadedToEnd_ = false;
    observedQueueCounters_ = queue_->counters();
    if (!fillQueue()) {
      return;  // fillQueue 已 fail()（含 stopDevice）；previousRing 于作用域末销毁，设备已停
    }
    observedQueueCounters_ = queue_->counters();
    clock_.seek(target);
    retiredQueue_ = std::move(previousRing);
    device_.rebindQueue(*queue_);
    stateMachine_.completeSeek(generation);
    publishMasterEnvelope(0.0F, std::chrono::milliseconds{0});
    publishMasterEnvelope(1.0F, upMs);
    publishPosition();
    spdlog::info("seek dip complete: target={}ms, up={}ms", target.count(), upMs.count());
  }

  // ManualDipDown 归零点：采纳已预解码的 staging 槽为新主源 + 上行腿（上行从首块起算——
  // 槽 ring 已满，换面即出声，裁定"不等待解码完成才出声"语义保持）。
  void completeManualDip() {
    if (!manualDip_) {
      finishingAction_ = FinishingAction::None;
      return;
    }
    auto dip = std::move(*manualDip_);
    manualDip_.reset();
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    spdlog::info("manual dip zero point: adopting '{}'", pathToUtf8(dip.request.filePath));
    adoptStagedSlotAsMain(dip);
    publishMasterEnvelope(0.0F, std::chrono::milliseconds{0});
    publishMasterEnvelope(1.0F, dip.upMs);
    publishPosition();
  }

  // 手动 dip 采纳：staging 槽对象（source/pipeline/queue/边界/挂起帧）整体接管为主源；
  // 单面 rebind 使槽 ring 成为主 face，旧主 ring 停入 retiredQueue_。状态机事件
  // （Loading→Ready→Playing + TrackChanged）在此背靠背发出（手动切换逻辑切换点 = 归零点，
  // dip 下行期间旧曲仍是逻辑当前曲）。时钟从新曲 offset 起算（设备运行，立即 resume）。
  void adoptStagedSlotAsMain(PendingManualDip& dip) {
    auto slot = std::move(dip.slot);
    source_ = std::move(slot.source);
    pipeline_ = std::move(slot.pipeline);
    currentRequest_ = dip.request;
    currentTarget_ = slot.target;
    trackEndPosition_ = slot.endPosition;
    pendingFrameWrite_ = std::move(slot.pendingFrameWrite);
    hasCurrentTarget_ = true;
    loadedToEnd_ = false;
    endApproachEmitted_ = false;
    overlapWindowSettled_ = false;
    overlapRampPending_ = false;

    clock_.reset(dip.request.trackId, slot.target.sampleRate,
                 dip.request.offset.value_or(std::chrono::milliseconds{0}));
    clock_.resume();
    retiredQueue_ = std::move(queue_);
    device_.rebindQueue(*slot.queue);
    queue_ = std::move(slot.queue);
    stateMachine_.loadTrack(dip.request);
    stateMachine_.completeLoad();
    stateMachine_.play();
    loadedToEnd_ = slot.loadedToEnd && queue_->availableFrames() == 0U;
    if (!loadedToEnd_) {
      static_cast<void>(fillQueue());
    }
    observedQueueCounters_ = queue_->counters();
  }

  // 收尾 ticker（servicePlaybackProgress 内、Playing 门控之前调用）：淡出期解码按需
  // 续喂（裁定④：淡出是否走完取决于缓冲/引擎策略——防淡出长于缓冲深度时欠载提前静音）；
  // 归零（或帧耗尽：曲目在淡出期播完）后按监督动作分派归零点处理（pause/stop=completeFinishing；
  // seek dip / 手动 dip = 各自换数据/换源续播）。
  void tickFinishing(const QueueUnderrunDelta& deferredUnderrun) {
    if (!loadedToEnd_) {
      if (!fillQueue()) {
        return;  // 解码错误：fillQueue 已 fail()——立即 stopDevice + 取消淡出 + Error（R6）
      }
      if (deferredUnderrun.silenceFrames > 0U && !loadedToEnd_) {
        emitBufferUnderrun(deferredUnderrun.silenceFrames, deferredUnderrun.underrunCount);
      }
      updateClockFromQueue(UnderrunReporting::Report);
    }
    if (loadedToEnd_ && pendingFrameWrite_) {
      static_cast<void>(writePendingFrame());
      updateClockFromQueue(UnderrunReporting::Report);
    }
    if (!finishingReachedZero()) {
      return;
    }
    switch (finishingAction_) {
    case FinishingAction::SeekDipDown:
      completeSeekDip();
      return;
    case FinishingAction::ManualDipDown:
      completeManualDip();
      return;
    case FinishingAction::PauseFreeze:
    case FinishingAction::StopCleanup:
    case FinishingAction::None:
      break;
    }
    completeFinishing();
  }

  bool finishingReachedZero() const {
    // 归零读回：包络轨迹结束后回调写回 0.0（块末粒度 1/duration≈0 由 ε 吸收）。
    if (device_.masterEnvelopeGain() <= kFadeCompleteGainEpsilon) {
      return true;
    }
    // 帧耗尽兜底：曲目已解码到末端且队列排空——无帧可淡，立即收尾（防欠载静音块
    // 不推进包络导致的悬挂）。
    return loadedToEnd_ && !pendingFrameWrite_ && queue_ && queue_->availableFrames() == 0U;
  }

  // 归零点收尾：停设备（包络复位）、时钟定格 = 归零点（R3：暂停定格位置，不得被淡出
  // 前旧位置覆盖——时钟在淡出期从未 pause，冻结只发生在此处）；StopCleanup 额外做
  // stop 的队列清理（位置清理照旧，推迟到归零点）。T11：finishing 期 seek 的定格目标
  // （pendingFrozenSeekPosition_）在此补做物理 seek（设备已停、无并发回调——paused-seek
  // 语义：无淡变、设备不重开，恢复时从定格位置淡入）。
  void completeFinishing() {
    const auto action = finishingAction_;
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    spdlog::info("finishing fade-out complete (action={})", static_cast<int>(action));
    stopProgressWorkerAsync();
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    if (pendingFrozenSeekPosition_.has_value()) {
      const auto target = *pendingFrozenSeekPosition_;
      pendingFrozenSeekPosition_.reset();
      if (queue_ && source_ && pipeline_) {
        if (const auto error = source_->seek(target)) {
          spdlog::error("frozen-position seek failed ({}ms): {} - {}",
                        target.count(), error->message, error->detail);
        } else {
          pipeline_->reset();
          queue_->clearForSeek();
          pendingFrameWrite_.reset();
          loadedToEnd_ = false;
          observedQueueCounters_ = queue_->counters();
          if (fillQueue()) {
            observedQueueCounters_ = queue_->counters();
            clock_.seek(target);
          }
          // fillQueue 失败已走 fail()（Error 态）；定格位置保持原样。
        }
      }
    }
    if (action == FinishingAction::StopCleanup) {
      if (queue_) {
        queue_->clearForSeek();
      }
      pendingFrameWrite_.reset();
    }
    publishPosition();
  }

  void fail(PlaybackErrorCode code, std::string message, std::string detail) {
    spdlog::error("playback error (code={}): {} - {}", static_cast<int>(code), message, detail);
    // T6（R6）：finishing 期间错误 → 立即 stopDevice（取消在途淡出）+ 转 Error，
    // 不留残余淡出窗口——收尾 ticker 随 stopProgressWorker 停止。
    finishingAction_ = FinishingAction::None;
    fadeInPending_ = false;
    // T11：切换监督 pending 随 Error 作废（含 finishing 期定格 seek——恢复前须重载曲目）。
    seekDip_.reset();
    manualDip_.reset();
    pendingFrozenSeekPosition_.reset();
    stopProgressWorker();
    stopDevice();
    stateMachine_.fail(code, std::move(message), std::move(detail));
  }

  void failWithDeviceError(std::string fallbackMessage, std::string fallbackDetail) {
    const auto error = device_.lastError();
    if (error) {
      spdlog::error("playback device error (code={}): {} - {}", static_cast<int>(error->code),
                    error->message, error->detail);
      fail(error->code, error->message, error->detail);
      return;
    }

    spdlog::error("playback device error: {} - {}", fallbackMessage, fallbackDetail);
    fail(PlaybackErrorCode::DeviceUnavailable, std::move(fallbackMessage), std::move(fallbackDetail));
  }

  void emitBufferUnderrun(std::uint64_t silenceFrames, std::uint64_t underrunCount) {
    spdlog::warn("buffer underrun: {} silence frames, {} underrun(s)",
                 silenceFrames, underrunCount);
    std::ostringstream detail;
    detail << "audio callback requested " << silenceFrames << " silence frames across " << underrunCount
           << " underrun read(s)";
    dispatcher_.dispatch(BackendEventType::PlaybackError,
                         PlaybackError{PlaybackErrorCode::BufferUnderrun,
                                       "audio output buffer underrun",
                                       detail.str(),
                                       clock_.snapshot()});
  }

  void emitPreloadError(PlaybackErrorCode code, std::string message, std::string detail) {
    std::optional<PlaybackClockSnapshot> clock;
    if (source_ || queue_) {
      updateClockFromQueue();
      clock = clock_.snapshot();
    }
    dispatcher_.dispatch(BackendEventType::PlaybackError,
                         PlaybackError{code, std::move(message), std::move(detail), std::move(clock)});
  }

  AudioOutputConfig config_{};
  // T7：最近一次成功开启设备时的 config_ 快照（免重开短路"设备目标未变"判据）。
  std::optional<AudioOutputConfig> activeDeviceConfig_{};
  TransitionConfig transitionConfig_{};
  // T8：EndApproaching 是否已在本臂（当前曲/当前配置）发出——一次性去重；
  // 由新 loadTrack / configureTransition / handoff 重新武装（裁定复位矩阵）。
  bool endApproachEmitted_{false};
  // T10：本臂交叉窗口裁决锁存——true = 已裁决（已启动交叉或已放弃，不重试；
  // 裁定⑧ 就绪即启/未就绪不延迟）。复位矩阵 = 新 loadTrack / configureTransition /
  // abortTransition / 两种 handoff（新曲臂）。
  bool overlapWindowSettled_{false};
  // T10：交叉第二腿（槽 1 0→1）待发——第二源已即时 0 激活、主源 1→0 腿已发布，
  // 等待槽 1 回调增益归零观测后发布 0→1 腿（B2：EQ 0→1 须先经即时 0 落底）。
  bool overlapRampPending_{false};
  // T10：源层包络发布账本（纪律同 masterEnvelope_；version 跨臂单调递增——与设备
  // resetSourceEnvelope 的版本清零并存：发布版本 > 0 恒被受理，A5 语义）。
  std::array<GainEnvelopeController, 2> sourceEnvelopeControllers_{
      GainEnvelopeController{1.0F}, GainEnvelopeController{1.0F}};
  // T11：交叉腿长（自动 = crossfadeMs；手动 = min(crossfadeMs, 当前曲剩余)）。tickOverlapRamp
  // 据此发布槽 1 上行腿——两腿必须同长，EQ 互补才成立。置位点 = startOverlap/startManualOverlap。
  std::chrono::milliseconds overlapLegMs_{0};
  // T11：手动交叉（档 3）在途锁存——source0/source1 腿完（或主源排空后腿完）时经
  // completeManualOverlap 采纳新主源（无 AdvanceCompleted）。撤面路径（cancelOverlapFace）
  // 清零（见其注释）；采纳路径清零。
  bool manualOverlapActive_{false};
  // T11：输出配置变更标志——configureOutput/selectOutputDevice 置位、下一次 LoadTrack
  // 消费。置位时该 LoadTrack = 配置整轨重载（非"改变音轨"），手动切歌档位对其不生效。
  bool outputConfigChangePending_{false};

  // ---- T6/T11 传送淡变与切换监督状态（均仅 audio worker 线程访问）----
  // finishingAction_ != None = 物理过渡监督中（pause/stop 已翻转逻辑态、设备运行淡出中；
  // T11：seek dip / 手动 dip 下行在途时逻辑态保持 Playing，归零点按动作分派交接）。
  FinishingAction finishingAction_{FinishingAction::None};
  // 恢复淡入握手进行中：冷恢复先发即时 0.0，ticker 观测归零后发布 0→1 淡入。
  bool fadeInPending_{false};
  // T11：seek dip pending（finishingAction_ == SeekDipDown 时有效；target 可被在途期间
  // 的后续 seek 覆盖，upMs = 上行腿长按首个命令配置快照）。
  std::optional<PendingSeekDip> seekDip_{};
  // T11：手动 dip pending（finishingAction_ == ManualDipDown 时有效；slot = 归零点采纳的
  // 已预解码新主源）。
  std::optional<PendingManualDip> manualDip_{};
  // T11：finishing（PauseFreeze）期 seek 的定格目标——淡出不打断，归零点补做物理 seek
  // 后时钟定格于此（completeFinishing 消费；abortFinishingForRestart/fail 作废）。
  std::optional<std::chrono::milliseconds> pendingFrozenSeekPosition_{};
  // T5 worker 侧包络账本（版本递增/ms→帧换算/增益钳制），发布唯一入口。
  GainEnvelopeController masterEnvelope_{1.0F};

  AudioOutputDevice device_;
  std::unique_ptr<DeviceFormatEnumerator> formatEnumerator_{};
  AudioEventDispatcher dispatcher_;
  PlaybackStateMachine stateMachine_;
  PlaybackClock clock_;
  PcmBufferQueueCounters observedQueueCounters_{};
  std::unique_ptr<PcmBufferQueue> queue_{};
  std::unique_ptr<FfmpegAudioSource> source_{};
  std::unique_ptr<FfmpegFilterPipeline> pipeline_{};
  TrackPlaybackRequest currentRequest_{};
  FfmpegFilterTargetFormat currentTarget_{};
  std::optional<PendingFrameWrite> pendingFrameWrite_{};
  std::optional<PreloadSlot> preloadSlot_{};
  // T9/T10：退役 ring 停放处（延迟回收，见 retireSecondSourceOnWorker / stopDevice）。
  // 两类 ring 停入：撤第二源面后的槽 ring（T9）、重叠提升后仍可能被回调持有一块的旧
  // 主 ring（T10）。持有期间设备已撤销对它的发布；析构只发生在设备停边界（无活跃回调）。
  std::unique_ptr<PcmBufferQueue> retiredQueue_{};
  bool loadedToEnd_{false};
  bool hasCurrentTarget_{false};
  std::optional<std::chrono::milliseconds> trackEndPosition_{};
  std::chrono::steady_clock::time_point lastProgressPublish_{};
  std::optional<std::chrono::milliseconds> lastPublishedPosition_{};
  mutable std::mutex snapshotMutex_{};
  PlaybackClockSnapshot lastClockSnapshot_{};
  std::mutex commandMutex_{};
  std::condition_variable commandAvailable_{};
  std::deque<AudioCommand> commands_{};
  bool audioWorkerStopping_{false};
  std::thread audioWorker_{};
  std::atomic<bool> progressWorkerRunning_{false};
};

std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend,
                                                               std::unique_ptr<DeviceFormatEnumerator> formatEnumerator) {
  return std::make_shared<SingleTrackAudioPlaybackService>(std::move(backend), std::move(formatEnumerator));
}

}
