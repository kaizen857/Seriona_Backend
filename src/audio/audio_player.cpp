#include "seriona/audio/audio_contracts.h"

#include "seriona/audio/audio_playback_service.h"

#include <utility>

namespace seriona::audio {

AudioPlayer::AudioPlayer() : service_(makeAudioPlaybackService()) {}

AudioPlayer::AudioPlayer(std::shared_ptr<AudioPlaybackService> service) : service_(std::move(service)) {}

void AudioPlayer::setPlaybackService(std::shared_ptr<AudioPlaybackService> service) { service_ = std::move(service); }

void AudioPlayer::setEventSink(BackendEventSink sink) {
  if (service_) {
    service_->setEventSink(std::move(sink));
  }
}

void AudioPlayer::configureOutput(const AudioOutputConfig& config) {
  if (service_) {
    service_->configureOutput(config);
  }
}

void AudioPlayer::loadTrack(const TrackPlaybackRequest& request) {
  if (service_) {
    service_->loadTrack(request);
  }
}

void AudioPlayer::prepareNext(const TrackPlaybackRequest& request) {
  if (service_) {
    service_->prepareNext(request);
  }
}

void AudioPlayer::play() {
  if (service_) {
    service_->play();
  }
}

void AudioPlayer::pause() {
  if (service_) {
    service_->pause();
  }
}

void AudioPlayer::resume() {
  if (service_) {
    service_->resume();
  }
}

void AudioPlayer::stop() {
  if (service_) {
    service_->stop();
  }
}

void AudioPlayer::seek(std::chrono::milliseconds position) {
  if (service_) {
    service_->seek(position);
  }
}

void AudioPlayer::setVolume(float linearGain) {
  if (service_) {
    service_->setVolume(linearGain);
  }
}

void AudioPlayer::setMuted(bool muted) {
  if (service_) {
    service_->setMuted(muted);
  }
}

void AudioPlayer::selectOutputDevice(const std::string& deviceId) {
  if (service_) {
    service_->selectOutputDevice(deviceId);
  }
}

PlaybackClockSnapshot AudioPlayer::queryPlaybackClock() const {
  if (!service_) {
    return {};
  }

  return service_->queryPlaybackClock();
}

}
