#include "media_controller_module.h"

#include "artwork_resolver.h"
#include "seriona/audio/audio_playback_service.h"
#include "seriona/control/app_settings_store.h"
#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/control/media_controller.h"
#include "seriona/metadata/metadata_contracts.h"
#include "scanner/file_scanner_service_internal.h"
#include "seriona/scanner/file_scanner_service.h"

#include "spdlog/spdlog.h"

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

class NoopFolderSortSettingsStore final : public FolderSortSettingsStore {
public:
  void upsert(FolderSortSetting) override {}
  [[nodiscard]] std::optional<FolderSortSetting> load(const std::filesystem::path&, const std::string&) const override { return std::nullopt; }
  void remove(const std::filesystem::path&, const std::string&) override {}
  [[nodiscard]] std::vector<FolderSortSetting> list(const std::filesystem::path&) const override { return {}; }
};

[[nodiscard]] std::shared_ptr<audio::AudioPlaybackService> makeNoopAudioPlaybackService() {
  return std::make_shared<NoopAudioPlaybackService>();
}

[[nodiscard]] std::shared_ptr<FolderSortSettingsStore> makeNoopFolderSortSettingsStore() {
  return std::make_shared<NoopFolderSortSettingsStore>();
}

[[nodiscard]] std::shared_ptr<FolderSortSettingsStore> makeFolderSortSettingsStore(std::filesystem::path databasePath) {
  if (databasePath.empty()) {
    return makeNoopFolderSortSettingsStore();
  }
  return std::shared_ptr<FolderSortSettingsStore>{makeSQLiteFolderSortSettingsStore(
      FolderSortSettingsStoreConfig{.databasePath = std::move(databasePath)})};
}

class NoopAppSettingsStore final : public AppSettingsStore {
public:
  void set(std::string, std::string, std::string) override {}
  [[nodiscard]] std::optional<std::string> get(const std::string&, const std::string&) const override {
    return std::nullopt;
  }
  void remove(const std::string&, const std::string&) override {}
  [[nodiscard]] std::vector<AppSettingsEntry> listByGroup(const std::string&) const override { return {}; }
};

[[nodiscard]] std::shared_ptr<AppSettingsStore> makeNoopAppSettingsStore() {
  return std::make_shared<NoopAppSettingsStore>();
}

[[nodiscard]] std::shared_ptr<AppSettingsStore> makeAppSettingsStore(std::filesystem::path databasePath) {
  if (databasePath.empty()) {
    return makeNoopAppSettingsStore();
  }
  return std::shared_ptr<AppSettingsStore>{makeSQLiteAppSettingsStore(
      AppSettingsStoreConfig{.databasePath = std::move(databasePath)})};
}

[[nodiscard]] const char* backendKindText(metadata::MetadataBackendKind kind) {
  switch (kind) {
  case metadata::MetadataBackendKind::Noop:
    return "noop";
  case metadata::MetadataBackendKind::Linux:
    return "linux";
  case metadata::MetadataBackendKind::Windows:
    return "windows";
  }
  return "unknown";
}

[[nodiscard]] metadata::MetadataSharingOptions makeProductionMetadataOptions() {
  metadata::MetadataSharingOptions options{};
#if defined(__linux__) && !defined(__APPLE__)
  options.backendKind = metadata::MetadataBackendKind::Linux;
#endif
  spdlog::info("selected {} metadata backend", backendKindText(options.backendKind));
  return options;
}

}

MediaControllerDependencies makeDefaultMediaControllerDependencies() {
  spdlog::info("selected noop audio backend (default)");
  MediaControllerDependencies dependencies{};
  dependencies.audio = makeNoopAudioPlaybackService();
  dependencies.scanner = scanner::makeFileScannerService();
  dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
  dependencies.folderSortSettingsStore = makeNoopFolderSortSettingsStore();
  dependencies.appSettingsStore = makeNoopAppSettingsStore();
  return dependencies;
}

MediaControllerDependencies makeProductionMediaControllerDependencies(
    std::filesystem::path databasePath,
    std::filesystem::path coverExportDir) {
  spdlog::info("selected production audio backend (miniaudio)");
  MediaControllerDependencies dependencies{};
  dependencies.audio = audio::makeAudioPlaybackService(audio::makeMiniaudioOutputDeviceBackend());
  scanner::FileScannerServiceDependencies scannerDeps{};
  auto settingsDatabasePath = databasePath;
  scannerDeps.databasePath = std::move(databasePath);
  if (!coverExportDir.empty()) {
    dependencies.artworkResolver = std::make_shared<ArtworkResolver>(coverExportDir, ArtworkResolverCompletion{});
  }
  scannerDeps.coverExportDir = std::move(coverExportDir);
  dependencies.scanner = scanner::makeFileScannerService(std::move(scannerDeps));
  dependencies.metadata = metadata::makeMetadataSharingService(makeProductionMetadataOptions());
  dependencies.folderSortSettingsStore = makeFolderSortSettingsStore(settingsDatabasePath);
  dependencies.appSettingsStore = makeAppSettingsStore(std::move(settingsDatabasePath));
  return dependencies;
}

std::unique_ptr<MediaController> makeProductionMediaController(MediaControllerOptions options) {
  return makeMediaController(makeProductionMediaControllerDependencies(), options);
}

std::unique_ptr<MediaController> makeProductionMediaController(
    MediaControllerOptions options,
    std::filesystem::path databasePath,
    std::filesystem::path coverExportDir) {
  return makeMediaController(makeProductionMediaControllerDependencies(std::move(databasePath), std::move(coverExportDir)), options);
}

void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies) {
  if (!dependencies.audio) {
    dependencies.audio = makeNoopAudioPlaybackService();
    spdlog::info("selected noop audio backend (missing dependency)");
  }
  if (!dependencies.scanner) {
    dependencies.scanner = scanner::makeFileScannerService();
  }
  if (!dependencies.metadata) {
    dependencies.metadata = metadata::makeMetadataSharingService(metadata::MetadataSharingOptions{});
    spdlog::info("selected noop metadata backend (missing dependency)");
  }
  if (!dependencies.folderSortSettingsStore) {
    dependencies.folderSortSettingsStore = makeNoopFolderSortSettingsStore();
  }
  if (!dependencies.appSettingsStore) {
    dependencies.appSettingsStore = makeNoopAppSettingsStore();
  }
}

}
