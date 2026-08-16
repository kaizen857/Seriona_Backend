#pragma once

#include "seriona/control/control_contracts.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace seriona::control {

class MediaController {
public:
  explicit MediaController(MediaControllerDependencies dependencies, MediaControllerOptions options = {});
  ~MediaController();

  MediaController(const MediaController&) = delete;
  MediaController& operator=(const MediaController&) = delete;
  MediaController(MediaController&&) noexcept;
  MediaController& operator=(MediaController&&) noexcept;

  void start();
  void shutdown();
  MediaControllerCommandResult submitCommand(const MediaControlCommand& command);
  MediaControllerCommandResult scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode);
  SubscriptionHandle subscribePlayerState(PlayerStateSnapshotCallback callback);
  SubscriptionHandle subscribeLibraryState(LibraryStateSnapshotCallback callback);
  SubscriptionHandle subscribeDomainNotifications(ControlDomainNotificationCallback callback);
  [[nodiscard]] PlayerStateSnapshot playerStateSnapshot() const;
  [[nodiscard]] LibraryStateSnapshot libraryStateSnapshot() const;
  [[nodiscard]] std::vector<audio::AudioDeviceFormat> enumeratePlaybackDevices() const;
  [[nodiscard]] audio::BackendEventSink audioEventSink();
  [[nodiscard]] scanner::ScannerEventSink scannerEventSink();
  void drainForTests();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<MediaController> makeMediaController(MediaControllerDependencies dependencies,
                                                                    MediaControllerOptions options = {});
[[nodiscard]] std::unique_ptr<MediaController> makeProductionMediaController(MediaControllerOptions options = {});
[[nodiscard]] std::unique_ptr<MediaController> makeProductionMediaController(
    MediaControllerOptions options,
    std::filesystem::path databasePath,
    std::filesystem::path coverExportDir);

}
