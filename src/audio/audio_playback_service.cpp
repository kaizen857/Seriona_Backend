#include "seriona/audio/audio_playback_service.h"

#include "seriona/audio/buffer/pcm_buffer_queue.h"
#include "seriona/audio/clock/playback_clock.h"
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
  explicit SingleTrackAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend)
      : device_(std::move(backend)), dispatcher_(BackendSourceModule::AudioPlaybackService) {
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

private:
  void loadTrackOnWorker(const TrackPlaybackRequest& request) {
    stopProgressWorker();
    stopDevice();
    device_.uninitialize();
    queue_.reset();
    source_ = std::make_unique<FfmpegAudioSource>();
    pipeline_ = std::make_unique<FfmpegFilterPipeline>();
    currentRequest_ = request;
    loadedToEnd_ = false;
    hasCurrentTarget_ = false;
    currentTarget_ = {};
    pendingFrameWrite_.reset();
    preloadSlot_.reset();

    stateMachine_.loadTrack(request);

    if (const auto error = source_->open(request.filePath)) {
      fail(error->code, error->message, error->detail);
      return;
    }

    const auto& streamInfo = source_->streamInfo();
    std::string negotiationFailure;
    std::optional<AudioOutputDeviceError> negotiationDeviceError;
    const auto negotiation = negotiateOutput(streamInfo, negotiationFailure, negotiationDeviceError);
    if (!negotiation) {
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

    if (const auto error = pipeline_->configure(currentTarget_)) {
      device_.uninitialize();
      queue_.reset();
      fail(error->code, error->message, error->detail);
      return;
    }

    clock_.reset(request.trackId, currentTarget_.sampleRate, request.offset.value_or(std::chrono::milliseconds{0}));
    observedQueueCounters_ = {};

    if (!negotiation->fallbackReason.empty()) {
      dispatcher_.dispatch(BackendEventType::OutputModeFallback,
                           OutputModeFallback{config_,
                                              negotiation->effectiveConfig,
                                              negotiation->deviceFormat,
                                              negotiation->fallbackReason});
    }

    dispatcher_.dispatch(BackendEventType::OutputFormatChanged,
                         OutputFormatChanged{config_, negotiation->deviceFormat});
    if (!fillQueue()) {
      return;
    }

    stateMachine_.completeLoad();
    publishPosition();
  }

  void prepareNextOnWorker(const TrackPlaybackRequest& request) {
    preloadSlot_.reset();

    PreloadSlot slot{};
    slot.request = request;
    slot.source = std::make_unique<FfmpegAudioSource>();
    slot.pipeline = std::make_unique<FfmpegFilterPipeline>();

    if (const auto error = slot.source->open(request.filePath)) {
      emitPreloadError(error->code, error->message, error->detail);
      return;
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
    if (!queue_ || !source_ || !pipeline_) {
      fail(PlaybackErrorCode::OpenFailed, "play requires a loaded track", "missing playback pipeline");
      return;
    }

    clock_.resume();
    if (!device_.start()) {
      clock_.pause();
      failWithDeviceError("failed to start audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.play();
    startProgressWorker();
    publishPosition();
  }

  void pauseOnWorker() {
    stopProgressWorker();
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    stateMachine_.pause();
    publishPosition();
  }

  void resumeOnWorker() {
    if (!queue_) {
      fail(PlaybackErrorCode::OpenFailed, "resume requires a loaded track", "missing playback queue");
      return;
    }
    if (stateMachine_.state() != PlaybackState::Paused) {
      stateMachine_.resume();
      return;
    }

    clock_.resume();
    if (!device_.start()) {
      clock_.pause();
      failWithDeviceError("failed to resume audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.resume();
    startProgressWorker();
    publishPosition();
  }

  void stopOnWorker() {
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
    stopProgressWorker();
    if (!source_ || !pipeline_ || !queue_) {
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

    auto nextSource = std::make_unique<FfmpegAudioSource>();
    if (const auto error = nextSource->open(currentRequest_.filePath)) {
      stateMachine_.cancelSeek(error->code, error->message, error->detail);
      publishPosition();
      return;
    }
    if (const auto error = nextSource->seek(position)) {
      stateMachine_.cancelSeek(error->code, error->message, error->detail);
      publishPosition();
      return;
    }

    auto nextPipeline = std::make_unique<FfmpegFilterPipeline>();
    if (const auto error = nextPipeline->configure(currentTarget_)) {
      stateMachine_.cancelSeek(error->code, error->message, error->detail);
      publishPosition();
      return;
    }

    const auto sampleBytes = bytesPerSample(currentTarget_.sampleFormat);
    auto nextQueue = std::make_unique<PcmBufferQueue>(
        PcmBufferQueueConfig{bufferFrameCount(currentTarget_.sampleRate, config_.bufferDuration),
                             currentTarget_.channelCount * sampleBytes});

    auto previousSource = std::move(source_);
    auto previousPipeline = std::move(pipeline_);
    auto previousQueue = std::move(queue_);
    auto previousPendingFrameWrite = std::move(pendingFrameWrite_);
    const bool previousLoadedToEnd = loadedToEnd_;
    source_ = std::move(nextSource);
    pipeline_ = std::move(nextPipeline);
    queue_ = std::move(nextQueue);
    pendingFrameWrite_.reset();
    observedQueueCounters_ = queue_->counters();
    loadedToEnd_ = false;

    if (!fillQueue()) {
      source_ = std::move(previousSource);
      pipeline_ = std::move(previousPipeline);
      queue_ = std::move(previousQueue);
      pendingFrameWrite_ = std::move(previousPendingFrameWrite);
      loadedToEnd_ = previousLoadedToEnd;
      observedQueueCounters_ = queue_ ? queue_->counters() : PcmBufferQueueCounters{};
      stateMachine_.cancelSeek(PlaybackErrorCode::SeekFailed, "seek failed", "failed to fill PCM queue after seek");
      publishPosition();
      return;
    }

    stopDevice();
    previousQueue.reset();
    previousPipeline.reset();
    previousSource.reset();

    observedQueueCounters_ = queue_->counters();
    clock_.seek(position);
    stateMachine_.completeSeek(seekGeneration);
    if (shouldResume) {
      clock_.resume();
      if (!device_.start()) {
        clock_.pause();
        failWithDeviceError("failed to restart audio output device after seek", "AudioOutputDeviceBackend::start returned false");
        return;
      }
      startProgressWorker();
    } else {
      clock_.pause();
    }
    publishPosition();
  }
  struct PendingFrameWrite {
    FfmpegAudioFrame frame{};
    std::uint32_t writtenFrames{0};
  };

  struct PreloadSlot {
    TrackPlaybackRequest request{};
    FfmpegFilterTargetFormat target{};
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
    candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, config_.outputMode, requested), requested, {}});

    if (!config_.allowFallback) {
      return candidates;
    }

    if (config_.outputMode == AudioOutputMode::Direct) {
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Mixed, requested),
                                                      requested,
                                                      "direct output mode was unavailable; using mixed output mode"});
    }

    if (!sameTarget(requested, source)) {
      candidates.push_back(OutputNegotiationCandidate{explicitConfig(config_, AudioOutputMode::Mixed, source),
                                                      source,
                                                      "requested mixed output format was unavailable; using source format"});
    }

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

  bool fillQueue() {
    if (!source_ || !pipeline_ || !queue_ || loadedToEnd_) {
      return true;
    }

    if (pendingFrameWrite_ && !writePendingFrame()) {
      return true;
    }

    while (queue_->availableFrames() < queue_->capacityFrames()) {
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
    const auto bytesPerFrame = static_cast<std::size_t>(frame.channelCount) * bytesPerSample(frame.sampleFormat);
    if (bytesPerFrame == 0U || frame.sampleBytes.size() != static_cast<std::size_t>(frame.frameCount) * bytesPerFrame) {
      fail(PlaybackErrorCode::DecodeFailed, "filtered frame has invalid PCM payload", "sampleBytes size does not match frame shape");
      return false;
    }

    pendingFrameWrite_ = PendingFrameWrite{frame, 0U};
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
    const auto bytesPerFrame = static_cast<std::size_t>(frame.channelCount) * bytesPerSample(frame.sampleFormat);
    if (bytesPerFrame == 0U || frame.sampleBytes.size() != static_cast<std::size_t>(frame.frameCount) * bytesPerFrame) {
      emitPreloadError(PlaybackErrorCode::DecodeFailed, "preloaded frame has invalid PCM payload", "sampleBytes size does not match frame shape");
      return false;
    }

    slot.pendingFrameWrite = PendingFrameWrite{frame, 0U};
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
    updateClockFromQueue();
    if (stateMachine_.state() == PlaybackState::Playing && !loadedToEnd_) {
      static_cast<void>(fillQueue());
      updateClockFromQueue();
    }
    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && pendingFrameWrite_) {
      static_cast<void>(writePendingFrame());
      updateClockFromQueue();
    }

    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && !pendingFrameWrite_ && queue_ &&
        queue_->availableFrames() == 0U) {
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

    const auto endedClock = clock_.snapshot();
    dispatcher_.dispatch(BackendEventType::PlaybackEnded, PlaybackEnded{currentRequest_, endedClock});

    auto slot = std::move(*preloadSlot_);
    preloadSlot_.reset();
    source_ = std::move(slot.source);
    pipeline_ = std::move(slot.pipeline);
    currentRequest_ = slot.request;
    currentTarget_ = slot.target;
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
    updateClockFromQueue();
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

  void updateClockFromQueue() {
    if (!queue_) {
      return;
    }

    const auto counters = queue_->counters();
    if (counters.consumedFrames > observedQueueCounters_.consumedFrames) {
      clock_.consumeFrames(counters.consumedFrames - observedQueueCounters_.consumedFrames);
    }
    if (counters.silenceFrames > observedQueueCounters_.silenceFrames) {
      const auto silenceDelta = counters.silenceFrames - observedQueueCounters_.silenceFrames;
      clock_.reportUnderrun(silenceDelta);
      emitBufferUnderrun(silenceDelta, counters.underrunCount - observedQueueCounters_.underrunCount);
    }
    observedQueueCounters_ = counters;
  }

  void stopDevice() {
    if (device_.started()) {
      static_cast<void>(device_.stop());
    }
  }

  void fail(PlaybackErrorCode code, std::string message, std::string detail) {
    stopProgressWorker();
    stopDevice();
    stateMachine_.fail(code, std::move(message), std::move(detail));
  }

  void failWithDeviceError(std::string fallbackMessage, std::string fallbackDetail) {
    const auto error = device_.lastError();
    if (error) {
      fail(error->code, error->message, error->detail);
      return;
    }

    fail(PlaybackErrorCode::DeviceUnavailable, std::move(fallbackMessage), std::move(fallbackDetail));
  }

  void emitBufferUnderrun(std::uint64_t silenceFrames, std::uint64_t underrunCount) {
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
  AudioOutputDevice device_;
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

std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend) {
  return std::make_shared<SingleTrackAudioPlaybackService>(std::move(backend));
}

}
