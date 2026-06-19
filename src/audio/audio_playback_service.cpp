#include "seriona/audio/audio_playback_service.h"

#include "seriona/audio/buffer/pcm_buffer_queue.h"
#include "seriona/audio/clock/playback_clock.h"
#include "seriona/audio/events/audio_event_dispatcher.h"
#include "seriona/audio/ffmpeg_audio_source.h"
#include "seriona/audio/ffmpeg_filter_pipeline.h"
#include "seriona/audio/playback_state_machine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace seriona::audio {
namespace {

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
  }

  ~SingleTrackAudioPlaybackService() override {
    dispatcher_.clearEventSink();
    stopDevice();
    device_.uninitialize();
  }

  void setEventSink(BackendEventSink sink) override { dispatcher_.setEventSink(std::move(sink)); }

  void configureOutput(const AudioOutputConfig& config) override { config_ = config; }

  void loadTrack(const TrackPlaybackRequest& request) override {
    stopDevice();
    device_.uninitialize();
    queue_.reset();
    source_ = std::make_unique<FfmpegAudioSource>();
    pipeline_ = std::make_unique<FfmpegFilterPipeline>();
    currentRequest_ = request;
    loadedToEnd_ = false;
    preloadSlot_.reset();

    stateMachine_.loadTrack(request);

    if (const auto error = source_->open(request.filePath)) {
      fail(error->code, error->message, error->detail);
      return;
    }

    const auto& streamInfo = source_->streamInfo();
    std::string negotiationFailure;
    const auto negotiation = negotiateOutput(streamInfo, negotiationFailure);
    if (!negotiation) {
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

  void prepareNext(const TrackPlaybackRequest& request) override {
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

  void play() override {
    if (!queue_ || !source_ || !pipeline_) {
      fail(PlaybackErrorCode::OpenFailed, "play requires a loaded track", "missing playback pipeline");
      return;
    }

    clock_.resume();
    if (!device_.start()) {
      clock_.pause();
      fail(PlaybackErrorCode::DeviceUnavailable, "failed to start audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.play();
    publishPosition();
  }

  void pause() override {
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    stateMachine_.pause();
    publishPosition();
  }

  void resume() override {
    if (!queue_) {
      fail(PlaybackErrorCode::OpenFailed, "resume requires a loaded track", "missing playback queue");
      return;
    }

    clock_.resume();
    if (!device_.start()) {
      clock_.pause();
      fail(PlaybackErrorCode::DeviceUnavailable, "failed to resume audio output device", "AudioOutputDeviceBackend::start returned false");
      return;
    }

    stateMachine_.resume();
    publishPosition();
  }

  void stop() override {
    stopDevice();
    updateClockFromQueue();
    clock_.pause();
    if (queue_) {
      queue_->clearForSeek();
    }
    stateMachine_.stop();
    publishPosition();
  }

  void seek(std::chrono::milliseconds position) override {
    if (!source_ || !pipeline_ || !queue_) {
      fail(PlaybackErrorCode::SeekFailed, "seek requires a loaded track", "missing playback pipeline");
      return;
    }

    stopDevice();
    updateClockFromQueue();
    const bool shouldResume = stateMachine_.state() == PlaybackState::Playing;
    stateMachine_.seek(position);
    if (const auto error = source_->seek(position)) {
      fail(error->code, error->message, error->detail);
      return;
    }

    pipeline_->reset();
    if (const auto error = pipeline_->configure(currentTarget_)) {
      fail(error->code, error->message, error->detail);
      return;
    }
    queue_->clearForSeek();
    observedQueueCounters_ = queue_->counters();
    clock_.seek(position);
    loadedToEnd_ = false;
    if (!fillQueue()) {
      return;
    }

    stateMachine_.completeSeek();
    if (shouldResume) {
      clock_.resume();
      if (!device_.start()) {
        clock_.pause();
        fail(PlaybackErrorCode::DeviceUnavailable, "failed to restart audio output device after seek", "AudioOutputDeviceBackend::start returned false");
        return;
      }
    } else {
      clock_.pause();
    }
    publishPosition();
  }

  void setVolume(float linearGain) override { volume_ = std::clamp(linearGain, 0.0F, 1.0F); }

  void setMuted(bool muted) override { muted_ = muted; }

  void selectOutputDevice(const std::string& deviceId) override { config_.preferredDeviceId = deviceId; }

  PlaybackClockSnapshot queryPlaybackClock() const override {
    const_cast<SingleTrackAudioPlaybackService*>(this)->servicePlaybackProgress();
    return clock_.snapshot();
  }

private:
  struct PreloadSlot {
    TrackPlaybackRequest request{};
    FfmpegFilterTargetFormat target{};
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
                                                         std::string& failureDetail) {
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
    if (!queue_->write(frame.sampleBytes.data(), frame.frameCount)) {
      return false;
    }
    clock_.submitFrames(frame.frameCount);
    return true;
  }

  bool writePreloadFrame(PreloadSlot& slot, const FfmpegAudioFrame& frame) {
    const auto bytesPerFrame = static_cast<std::size_t>(frame.channelCount) * bytesPerSample(frame.sampleFormat);
    if (bytesPerFrame == 0U || frame.sampleBytes.size() != static_cast<std::size_t>(frame.frameCount) * bytesPerFrame) {
      emitPreloadError(PlaybackErrorCode::DecodeFailed, "preloaded frame has invalid PCM payload", "sampleBytes size does not match frame shape");
      return false;
    }
    return slot.queue->write(frame.sampleBytes.data(), frame.frameCount);
  }

  void servicePlaybackProgress() {
    updateClockFromQueue();
    if (stateMachine_.state() == PlaybackState::Playing && !loadedToEnd_) {
      static_cast<void>(fillQueue());
      updateClockFromQueue();
    }

    if (stateMachine_.state() == PlaybackState::Playing && loadedToEnd_ && queue_ && queue_->availableFrames() == 0U) {
      if (handoffToPreparedNext()) {
        return;
      }

      stopDevice();
      clock_.pause();
      stateMachine_.naturalEnd();
      publishPosition();
    }
  }

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
    dispatcher_.dispatch(BackendEventType::PlaybackPositionUpdated, PlaybackPositionUpdated{clock_.snapshot()});
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
      clock_.reportUnderrun(counters.silenceFrames - observedQueueCounters_.silenceFrames);
    }
    observedQueueCounters_ = counters;
  }

  void stopDevice() {
    if (device_.started()) {
      static_cast<void>(device_.stop());
    }
  }

  void fail(PlaybackErrorCode code, std::string message, std::string detail) {
    stopDevice();
    stateMachine_.fail(code, std::move(message), std::move(detail));
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
  std::optional<PreloadSlot> preloadSlot_{};
  float volume_{1.0F};
  bool muted_{false};
  bool loadedToEnd_{false};
  bool hasCurrentTarget_{false};
};

std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend) {
  return std::make_shared<SingleTrackAudioPlaybackService>(std::move(backend));
}

}
