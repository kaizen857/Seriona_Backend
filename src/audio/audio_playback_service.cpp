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
    enqueueCommand([this, config] { config_ = config; });
  }

  void loadTrack(const TrackPlaybackRequest& request) override {
    enqueueCommand([this, request] { loadTrackOnWorker(request); });
  }

  void prepareNext(const TrackPlaybackRequest& request) override {
    enqueueCommand([this, request] { prepareNextOnWorker(request); });
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
    enqueueCommand([this, deviceId] { config_.preferredDeviceId = deviceId; });
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
    stopProgressWorker();
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

  void prepareNextOnWorker(const TrackPlaybackRequest& request) {
    preloadSlot_.reset();

    PreloadSlot slot{};
    slot.request = request;
    slot.source = std::make_unique<FfmpegAudioSource>();
    slot.pipeline = std::make_unique<FfmpegFilterPipeline>();
    slot.endPosition = endPositionFor(request);

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

    clock_.resume();
    if (!device_.start()) {
      spdlog::error("play failed: device start returned false");
      clock_.pause();
      failWithDeviceError("failed to start audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.play();
    startProgressWorker();
    publishPosition();
  }

  void pauseOnWorker() {
    spdlog::info("pause");
    stopProgressWorker();
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    stateMachine_.pause();
    publishPosition();
  }

  void resumeOnWorker() {
    spdlog::info("resume");
    if (!queue_) {
      spdlog::error("resume failed: no queue");
      fail(PlaybackErrorCode::OpenFailed, "resume requires a loaded track", "missing playback queue");
      return;
    }
    if (stateMachine_.state() != PlaybackState::Paused) {
      stateMachine_.resume();
      return;
    }

    clock_.resume();
    if (!device_.start()) {
      spdlog::error("resume failed: device start returned false");
      clock_.pause();
      failWithDeviceError("failed to resume audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.resume();
    startProgressWorker();
    publishPosition();
  }

  void stopOnWorker() {
    spdlog::info("stop");
    stopProgressWorker();
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    if (queue_) {
      queue_->clearForSeek();
    }
    pendingFrameWrite_.reset();
    stateMachine_.stop();
    publishPosition();
  }

  void seekOnWorker(std::chrono::milliseconds position) {
    spdlog::info("seek to {}ms", position.count());
    stopProgressWorker();
    if (!source_ || !pipeline_ || !queue_) {
      spdlog::error("seek failed: no pipeline");
      fail(PlaybackErrorCode::SeekFailed, "seek requires a loaded track", "missing playback pipeline");
      return;
    }
    if (stateMachine_.state() != PlaybackState::Ready && stateMachine_.state() != PlaybackState::Playing &&
        stateMachine_.state() != PlaybackState::Paused) {
      stateMachine_.seek(position);
      return;
    }

    updateClockFromQueue();
    const bool shouldResume = stateMachine_.state() == PlaybackState::Playing;
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

    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && !pendingFrameWrite_ && queue_ &&
        queue_->availableFrames() == 0U) {
      updateClockFromQueue(UnderrunReporting::Suppress);
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

  bool handoffToPreparedNext() {
    if (!preloadSlot_ || !preloadSlot_->ready || !preloadSlot_->seamlessEligible || !queue_ ||
        !sameTarget(preloadSlot_->target, currentTarget_)) {
      return false;
    }

    auto slot = std::move(*preloadSlot_);
    preloadSlot_.reset();
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
    if (device_.started()) {
      static_cast<void>(device_.stop());
    }
  }

  void fail(PlaybackErrorCode code, std::string message, std::string detail) {
    spdlog::error("playback error (code={}): {} - {}", static_cast<int>(code), message, detail);
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
