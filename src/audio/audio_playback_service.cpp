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
#include <string>
#include <utility>

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

    stateMachine_.loadTrack(request);

    if (const auto error = source_->open(request.filePath)) {
      fail(error->code, error->message, error->detail);
      return;
    }

    const auto& streamInfo = source_->streamInfo();
    const auto target = targetFormat(streamInfo);
    const auto sampleBytes = bytesPerSample(target.sampleFormat);
    if (target.sampleRate == 0U || target.channelCount == 0U || sampleBytes == 0U) {
      fail(PlaybackErrorCode::FormatNegotiationFailed, "invalid output target format", "sample rate, channel count, and sample bytes must be nonzero");
      return;
    }

    if (const auto error = pipeline_->configure(target)) {
      fail(error->code, error->message, error->detail);
      return;
    }

    const auto capacityFrames = bufferFrameCount(target.sampleRate, config_.bufferDuration);
    queue_ = std::make_unique<PcmBufferQueue>(PcmBufferQueueConfig{capacityFrames, target.channelCount * sampleBytes});
    clock_.reset(request.trackId, target.sampleRate, request.offset.value_or(std::chrono::milliseconds{0}));
    observedQueueCounters_ = {};

    AudioOutputDeviceOpenRequest openRequest{};
    openRequest.config = config_;
    openRequest.sampleFormat = target.sampleFormat;
    openRequest.sampleRate = target.sampleRate;
    openRequest.channelCount = target.channelCount;
    openRequest.bufferFrames = std::min<std::uint32_t>(capacityFrames, 512U);
    openRequest.pcmQueue = queue_.get();

    if (!device_.initialize(openRequest)) {
      fail(PlaybackErrorCode::DeviceUnavailable, "failed to initialize audio output device", "AudioOutputDeviceBackend::initialize returned false");
      return;
    }

    dispatcher_.dispatch(BackendEventType::OutputFormatChanged, OutputFormatChanged{config_, device_.currentFormat()});
    if (!fillQueue()) {
      return;
    }

    stateMachine_.completeLoad();
    publishPosition();
  }

  void prepareNext(const TrackPlaybackRequest&) override {}

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
    const_cast<SingleTrackAudioPlaybackService*>(this)->updateClockFromQueue();
    return clock_.snapshot();
  }

private:
  FfmpegFilterTargetFormat targetFormat(const FfmpegAudioStreamInfo& streamInfo) {
    currentTarget_ = FfmpegFilterTargetFormat{config_.targetSampleRate.value_or(streamInfo.sampleRate),
                                             config_.targetSampleFormat.value_or(AudioSampleFormat::Float32),
                                             config_.targetChannelCount.value_or(streamInfo.channelCount)};
    return currentTarget_;
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
  float volume_{1.0F};
  bool muted_{false};
  bool loadedToEnd_{false};
};

std::shared_ptr<AudioPlaybackService> makeAudioPlaybackService(std::unique_ptr<AudioOutputDeviceBackend> backend) {
  return std::make_shared<SingleTrackAudioPlaybackService>(std::move(backend));
}

}
