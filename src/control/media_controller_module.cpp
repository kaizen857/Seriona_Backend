#include "media_controller_module.h"

#include "seriona/audio/audio_playback_service.h"
#include "seriona/metadata/metadata_contracts.h"
#include "seriona/scanner/file_scanner_service.h"

#include <memory>

namespace seriona::control {

MediaControllerDependencies makeDefaultMediaControllerDependencies() {
  MediaControllerDependencies dependencies{};
  dependencies.audio = audio::makeAudioPlaybackService();
  dependencies.scanner = scanner::makeFileScannerService();
  dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
  return dependencies;
}

void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies) {
  if (!dependencies.audio) {
    dependencies.audio = audio::makeAudioPlaybackService();
  }
  if (!dependencies.scanner) {
    dependencies.scanner = scanner::makeFileScannerService();
  }
  if (!dependencies.metadata) {
    dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
  }
}

}
