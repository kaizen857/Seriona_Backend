#include "media_controller_module.h"

#include "seriona/audio/audio_playback_service.h"
#include "seriona/control/media_controller.h"
#include "seriona/metadata/metadata_contracts.h"
#include "seriona/scanner/file_scanner_service.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace seriona::control {
namespace {

class NoopAudioPlaybackService final : public audio::AudioPlaybackService {
public:
  void setEventSink(audio::BackendEventSink sink) override { sink_ = std::move(sink); }
  void configureOutput(const audio::AudioOutputConfig&) override {}
  void loadTrack(const audio::TrackPlaybackRequest& request) override { clock_.trackId = request.trackId; }
  void prepareNext(const audio::TrackPlaybackRequest&) override {}
  void play() override {}
  void pause() override {}
  void resume() override {}
  void stop() override { clock_ = {}; }
  void seek(std::chrono::milliseconds position) override { clock_.position = position; }
  void setVolume(float) override {}
  void setMuted(bool) override {}
  void selectOutputDevice(const std::string&) override {}
  [[nodiscard]] audio::PlaybackClockSnapshot queryPlaybackClock() const override { return clock_; }

private:
  audio::BackendEventSink sink_{};
  audio::PlaybackClockSnapshot clock_{};
};

[[nodiscard]] std::shared_ptr<audio::AudioPlaybackService> makeNoopAudioPlaybackService() {
  return std::make_shared<NoopAudioPlaybackService>();
}

[[nodiscard]] metadata::MetadataSharingOptions makeProductionMetadataOptions() {
  metadata::MetadataSharingOptions options{};
#if defined(__linux__) && !defined(__APPLE__)
  options.backendKind = metadata::MetadataBackendKind::Linux;
#endif
  return options;
}

}

MediaControllerDependencies makeDefaultMediaControllerDependencies() {
  MediaControllerDependencies dependencies{};
  dependencies.audio = makeNoopAudioPlaybackService();
  dependencies.scanner = scanner::makeFileScannerService();
  dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
  return dependencies;
}

MediaControllerDependencies makeProductionMediaControllerDependencies() {
  MediaControllerDependencies dependencies{};
  dependencies.audio = audio::makeAudioPlaybackService(audio::makeMiniaudioOutputDeviceBackend());
  dependencies.scanner = scanner::makeFileScannerService();
  dependencies.metadata = metadata::makeMetadataSharingService(makeProductionMetadataOptions());
  return dependencies;
}

std::unique_ptr<MediaController> makeProductionMediaController(MediaControllerOptions options) {
  return makeMediaController(makeProductionMediaControllerDependencies(), options);
}

void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies) {
  if (!dependencies.audio) {
    dependencies.audio = makeNoopAudioPlaybackService();
  }
  if (!dependencies.scanner) {
    dependencies.scanner = scanner::makeFileScannerService();
  }
  if (!dependencies.metadata) {
    dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
  }
}

}
