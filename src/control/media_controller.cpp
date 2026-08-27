#include "seriona/control/media_controller.h"

#include "control_event_loop.h"
#include "control_state_reducer.h"
#include "media_controller_module.h"
#include "subscription_store.h"

#include "seriona/control/app_settings_store.h"
#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/metadata/metadata_contracts.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <future>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

namespace seriona::control {
namespace {

// 路径文本恒为 UTF-8：generic_string() 在 Windows 按 CP_ACP 转换，字符不可表示时抛
// std::system_error（ERROR_NO_UNICODE_TRANSLATION）；generic_u8string() 永不抛，
// POSIX 上字节级不变。
[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

[[nodiscard]] MediaControllerCommandResult stoppedResult() {
  return MediaControllerCommandResult{.accepted = false,
                                      .code = MediaControllerErrorCode::ControllerStopped,
                                      .message = "Media controller is stopped"};
}

[[nodiscard]] MediaControllerCommandResult acceptedResult() {
  return MediaControllerCommandResult{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}};
}

[[nodiscard]] MediaControllerCommandResult rejectedResult(MediaControllerErrorCode code, std::string message) {
  return MediaControllerCommandResult{.accepted = false, .code = code, .message = std::move(message)};
}

[[nodiscard]] bool isSupportedFolderSortField(FolderSortField field) noexcept {
  switch (field) {
  case FolderSortField::Title:
  case FolderSortField::Artist:
  case FolderSortField::Album:
  case FolderSortField::Filename:
  case FolderSortField::Year:
  case FolderSortField::Duration:
  case FolderSortField::CreatedDate:
  case FolderSortField::DiscNumber:
  case FolderSortField::TrackNumber:
    return true;
  }
  return false;
}

[[nodiscard]] bool isSupportedFolderSortDirection(FolderSortDirection direction) noexcept {
  switch (direction) {
  case FolderSortDirection::Ascending:
  case FolderSortDirection::Descending:
    return true;
  }
  return false;
}

[[nodiscard]] bool isSupportedFolderSortMissingValuePolicy(FolderSortMissingValuePolicy policy) noexcept {
  switch (policy) {
  case FolderSortMissingValuePolicy::First:
  case FolderSortMissingValuePolicy::Last:
    return true;
  }
  return false;
}

[[nodiscard]] std::optional<MediaControllerCommandResult> validateFolderSortSetting(FolderSortSetting& setting) {
  if (setting.rootPath.empty()) {
    return rejectedResult(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules requires a root path");
  }
  try {
    setting.rootPath = std::filesystem::absolute(setting.rootPath).lexically_normal();
  } catch (const std::filesystem::filesystem_error& error) {
    return rejectedResult(MediaControllerErrorCode::InvalidCommand, error.what());
  }
  if (setting.folderNodeId.empty()) {
    return rejectedResult(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules requires a folder node id");
  }
  if (setting.rules.empty()) {
    return rejectedResult(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules requires at least one sort rule");
  }
  for (const auto& rule : setting.rules) {
    if (!isSupportedFolderSortField(rule.field) || !isSupportedFolderSortDirection(rule.direction) ||
        !isSupportedFolderSortMissingValuePolicy(rule.missingValuePolicy)) {
      return rejectedResult(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules contains an unsupported sort rule");
    }
  }
  return std::nullopt;
}

[[nodiscard]] MediaControllerErrorCode commandCodeFromStoreError(FolderSortSettingsErrorCode code) noexcept {
  switch (code) {
  case FolderSortSettingsErrorCode::InvalidRootPath:
  case FolderSortSettingsErrorCode::InvalidFolderNodeId:
  case FolderSortSettingsErrorCode::InvalidRulesJson:
    return MediaControllerErrorCode::InvalidCommand;
  case FolderSortSettingsErrorCode::StorageError:
    return MediaControllerErrorCode::BackendRejected;
  }
  return MediaControllerErrorCode::BackendRejected;
}

[[nodiscard]] ControlDomainNotification makeCommandRejectedNotification(MediaControllerErrorCode code, std::string message) {
  return ControlDomainNotification{.kind = ControlDomainNotificationKind::CommandRejected,
                                   .errorCode = code,
                                   .message = std::move(message),
                                   .scanStatus = std::nullopt,
                                   .folderSortSetting = std::nullopt};
}

[[nodiscard]] ControlDomainNotification makeFolderSortAppliedNotification(FolderSortSetting setting) {
  return ControlDomainNotification{.kind = ControlDomainNotificationKind::FolderSortRulesApplied,
                                   .errorCode = MediaControllerErrorCode::None,
                                   .message = "Folder sort rules applied",
                                   .scanStatus = std::nullopt,
                                   .folderSortSetting = std::move(setting)};
}

[[nodiscard]] bool canLoadSavedFolderRules(const PlaybackContextDescriptor& descriptor) {
  return descriptor.scope == PlaybackContextScope::Folder && descriptor.sortRules.empty() && !descriptor.rootPath.empty() &&
         !descriptor.folderNodeId.empty();
}

[[nodiscard]] metadata::PlatformMediaState platformStateFromSnapshot(const PlayerStateSnapshot& snapshot) {
  metadata::PlatformMediaState state{};
  state.controlState = snapshot;
  return state;
}

}

class MediaController::Impl {
public:
  Impl(MediaControllerDependencies dependencies, MediaControllerOptions options)
      : dependencies_(std::move(dependencies)), options_(options), eventLoop_(options), reducer_(options) {
    normalizeMediaControllerDependencies(dependencies_);
    installSinks();
    if (dependencies_.artworkResolver) {
      dependencies_.artworkResolver->setResultCallback([this](ArtworkResolveResultView view) {
        postArtworkResolved(std::move(view));
      });
    }
  }

  ~Impl() { shutdown(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void start() {
    {
      std::lock_guard lock{mutex_};
      if (started_) {
        return;
      }
      started_ = true;
      stopping_ = false;
    }

    spdlog::info("media controller starting");
    eventLoop_.start();
    metadataCommandSubscription_ = dependencies_.metadata->registerCommandCallback([this](const MediaControlCommand& command) {
      postMetadataCommand(command);
    });
    const auto startResult = dependencies_.metadata->start(platformStateFromSnapshot(playerStateSnapshot()));
    (void)startResult;
    spdlog::info("media controller started");
  }

  void shutdown() {
    bool shouldStopMetadata = false;
    {
      std::lock_guard lock{mutex_};
      shouldStopMetadata = started_ && !stopping_;
      stopping_ = true;
      started_ = false;
    }

    spdlog::info("media controller shutting down");
    dependencies_.audio->setEventSink({});
    dependencies_.scanner->setEventSink({});
    if (shouldStopMetadata) {
      stopScannerWatching("shutdown");
    }

    if (metadataCommandSubscription_.unsubscribe) {
      metadataCommandSubscription_.unsubscribe();
      metadataCommandSubscription_ = {};
    }
    if (shouldStopMetadata) {
      const auto stopResult = dependencies_.metadata->stop();
      (void)stopResult;
    }
    eventLoop_.stop();
    if (dependencies_.artworkResolver) {
      dependencies_.artworkResolver->stop();
    }
    spdlog::info("media controller stopped");
  }

  MediaControllerCommandResult submitCommand(const MediaControlCommand& command) {
    return dispatch<MediaControllerCommandResult>([this, command] { return reduceCommand(command); });
  }

  [[nodiscard]] std::optional<std::string> getAppSetting(const std::string& group, const std::string& key) {
    {
      std::lock_guard lock{mutex_};
      if (!started_ || stopping_) {
        return std::nullopt;
      }
    }

    auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
    auto future = promise->get_future();
    const auto work = [promise, this, group, key] {
      if (!dependencies_.appSettingsStore) {
        promise->set_value(std::nullopt);
        return;
      }
      try {
        promise->set_value(dependencies_.appSettingsStore->get(group, key));
      } catch (const std::exception&) {
        promise->set_value(std::nullopt);
      } catch (...) {
        promise->set_value(std::nullopt);
      }
    };
    if (!eventLoop_.post(work)) {
      spdlog::error("media controller dispatch failed: event loop post rejected");
      return std::nullopt;
    }
    if (options_.runInlineForTests) {
      eventLoop_.drainForTests();
    }
    return future.get();
  }

  MediaControllerCommandResult setAppSetting(std::string group, std::string key, std::string value) {
    return dispatch<MediaControllerCommandResult>([this,
                                                   group = std::move(group),
                                                   key = std::move(key),
                                                   value = std::move(value)] {
      if (group.empty() || key.empty()) {
        return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                             "SetAppSetting requires non-empty group and key");
      }
      if (!dependencies_.appSettingsStore) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, "app settings store is unavailable");
      }
      try {
        dependencies_.appSettingsStore->set(group, key, value);
      } catch (const AppSettingsError& error) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
      } catch (const std::exception& error) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
      }
      return acceptedResult();
    });
  }

  MediaControllerCommandResult removeAppSetting(std::string group, std::string key) {
    return dispatch<MediaControllerCommandResult>([this, group = std::move(group), key = std::move(key)] {
      if (group.empty() || key.empty()) {
        return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                             "RemoveAppSetting requires non-empty group and key");
      }
      if (!dependencies_.appSettingsStore) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, "app settings store is unavailable");
      }
      try {
        dependencies_.appSettingsStore->remove(group, key);
      } catch (const AppSettingsError& error) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
      } catch (const std::exception& error) {
        return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
      }
      return acceptedResult();
    });
  }

  MediaControllerCommandResult scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode) {
    if (!isRunning()) {
      return stoppedResult();
    }

    dependencies_.scanner->scan(roots, mode);
    try {
      dependencies_.scanner->startWatching(roots);
    } catch (const std::exception& error) {
      stopScannerWatching("watcher start failure");
      return rejectCommand(MediaControllerErrorCode::BackendRejected,
                           std::string{"Failed to start scanner watcher: "} + error.what());
    } catch (...) {
      stopScannerWatching("watcher start failure");
      return rejectCommand(MediaControllerErrorCode::BackendRejected, "Failed to start scanner watcher");
    }
    publishSavedFolderSortRulesForRoots(roots);
    return acceptedResult();
  }

  SubscriptionHandle subscribePlayerState(PlayerStateSnapshotCallback callback) {
    return playerSubscriptions_.subscribe(std::move(callback), playerStateSnapshot());
  }

  SubscriptionHandle subscribeLibraryState(LibraryStateSnapshotCallback callback) {
    return librarySubscriptions_.subscribe(std::move(callback), libraryStateSnapshot());
  }

  SubscriptionHandle subscribeDomainNotifications(ControlDomainNotificationCallback callback) {
    return notificationSubscriptions_.subscribe(std::move(callback));
  }

  PlayerStateSnapshot playerStateSnapshot() const {
    std::lock_guard lock{mutex_};
    return playerSnapshot_;
  }

  LibraryStateSnapshot libraryStateSnapshot() const {
    std::lock_guard lock{mutex_};
    return librarySnapshot_;
  }

  std::vector<audio::AudioDeviceFormat> enumeratePlaybackDevices() const {
    return dependencies_.audio->enumeratePlaybackDevices();
  }

  audio::BackendEventSink audioEventSink() {
    return [this](audio::BackendEvent event) { postAudioEvent(std::move(event)); };
  }

  scanner::ScannerEventSink scannerEventSink() {
    return [this](scanner::ScannerEvent event) { postScannerEvent(std::move(event)); };
  }

  void drainForTests() { eventLoop_.drainForTests(); }

private:
  template <typename Result, typename Work>
  Result dispatch(Work work) {
    {
      std::lock_guard lock{mutex_};
      if (!started_ || stopping_) {
        return stoppedResult();
      }
    }

    if (options_.runInlineForTests) {
      auto promise = std::make_shared<std::promise<Result>>();
      auto future = promise->get_future();
      if (!eventLoop_.post([promise, work = std::move(work)]() mutable { completePromise(*promise, work); })) {
        spdlog::error("media controller dispatch failed: event loop post rejected");
        return stoppedResult();
      }
      eventLoop_.drainForTests();
      return future.get();
    }

    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    if (!eventLoop_.post([promise, work = std::move(work)]() mutable { completePromise(*promise, work); })) {
      spdlog::error("media controller dispatch failed: event loop post rejected");
      return stoppedResult();
    }
    return future.get();
  }

  template <typename Result, typename Work>
  static void completePromise(std::promise<Result>& promise, Work& work) noexcept {
    try {
      if constexpr (std::is_void_v<Result>) {
        work();
        promise.set_value();
      } else {
        promise.set_value(work());
      }
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
  }

  void installSinks() {
    dependencies_.audio->setEventSink(audioEventSink());
    dependencies_.scanner->setEventSink(scannerEventSink());
  }

  void postAudioEvent(audio::BackendEvent event) {
    if (!isRunning()) {
      return;
    }

    auto posted = eventLoop_.post([this, event = std::move(event)] {
      if (isRunning()) {
        handleAudioEvent(event);
      }
    });
    (void)posted;
  }

  void postScannerEvent(scanner::ScannerEvent event) {
    if (!isRunning()) {
      return;
    }

    auto posted = eventLoop_.post([this, event = std::move(event)] {
      if (isRunning()) {
        handleScannerEvent(event);
      }
    });
    (void)posted;
  }

  void postMetadataCommand(MediaControlCommand command) {
    if (!isRunning()) {
      return;
    }

    auto posted = eventLoop_.post([this, command = std::move(command)] {
      if (isRunning()) {
        reduceCommand(command);
      }
    });
    (void)posted;
  }

  void postArtworkResolved(ArtworkResolveResultView view) {
    if (!isRunning()) {
      return;
    }

    // The resolver callback runs on the resolver worker thread; serialize the
    // result through the control event loop before touching reducer state.
    auto posted = eventLoop_.post([this, view = std::move(view)] {
      if (isRunning()) {
        auto reduction = reducer_.reduceArtworkResolved(view);
        commitReduction(reduction);
      }
    });
    (void)posted;
  }

  [[nodiscard]] bool isRunning() const {
    std::lock_guard lock{mutex_};
    return started_ && !stopping_;
  }

  MediaControllerCommandResult reduceCommand(const MediaControlCommand& command) {
    if (command.kind == MediaControlCommandKind::ApplyFolderSortRules) {
      return applyFolderSortRules(command);
    }
    if (command.kind == MediaControlCommandKind::StartPlaybackFromContext) {
      return startPlaybackFromContext(command);
    }
    if (command.kind == MediaControlCommandKind::DeleteTrack || command.kind == MediaControlCommandKind::DeleteFolder) {
      return deleteTarget(command);
    }

    auto reduction = reducer_.reduceCommand(command);
    commitReduction(reduction);
    executeIntents(reduction.intents);
    return reduction.result;
  }

  MediaControllerCommandResult startPlaybackFromContext(const MediaControlCommand& command) {
    auto commandWithSavedRules = command;
    if (commandWithSavedRules.playbackContext.has_value()) {
      applySavedFolderSortRulesIfNeeded(*commandWithSavedRules.playbackContext);
    }

    auto reduction = reducer_.reduceCommand(commandWithSavedRules);
    commitReduction(reduction);
    executeIntents(reduction.intents);
    return reduction.result;
  }

  void applySavedFolderSortRulesIfNeeded(PlaybackContextDescriptor& descriptor) const {
    if (!canLoadSavedFolderRules(descriptor)) {
      return;
    }
    try {
      const auto saved = dependencies_.folderSortSettingsStore->load(descriptor.rootPath, descriptor.folderNodeId);
      if (saved.has_value()) {
        descriptor.sortRules = saved->rules;
      }
    } catch (const FolderSortSettingsError& error) {
      spdlog::warn("failed to load saved folder sort rules for root '{}' folder '{}': {}",
                   pathText(descriptor.rootPath),
                   descriptor.folderNodeId,
                   error.what());
    } catch (const std::exception& error) {
      spdlog::warn("failed to load saved folder sort rules for root '{}' folder '{}': {}",
                   pathText(descriptor.rootPath),
                   descriptor.folderNodeId,
                   error.what());
    }
  }

  MediaControllerCommandResult applyFolderSortRules(const MediaControlCommand& command) {
    if (!command.folderSortSetting.has_value()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand, "ApplyFolderSortRules requires folder sort settings");
    }

    auto setting = *command.folderSortSetting;
    if (auto validationError = validateFolderSortSetting(setting); validationError.has_value()) {
      return rejectCommand(validationError->code, validationError->message);
    }

    try {
      dependencies_.folderSortSettingsStore->upsert(setting);
    } catch (const FolderSortSettingsError& error) {
      return rejectCommand(commandCodeFromStoreError(error.code()), error.what());
    } catch (const std::exception& error) {
      return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
    }

    notificationSubscriptions_.publish(makeFolderSortAppliedNotification(setting));
    return acceptedResult();
  }

  MediaControllerCommandResult rejectCommand(MediaControllerErrorCode code, std::string message) {
    notificationSubscriptions_.publish(makeCommandRejectedNotification(code, message));
    return rejectedResult(code, std::move(message));
  }

  // 删除命令执行（worker 线程 = 控制事件循环）：若目标即在播/加载中曲目，先经
  // reducer 停止当前播放（audio worker FIFO 保证 stop 排在未完成的 loadTrackOnWorker
  // 之后，加载失败/成功均收敛为 Stopped），再经 scanner 服务删磁盘 + 同步缓存 +
  // 发布新快照（PlaylistSnapshotUpdated → 库快照通知）。
  MediaControllerCommandResult deleteTarget(const MediaControlCommand& command) {
    if (!command.targetPath.has_value() || command.targetPath->empty()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand, "DeleteTrack/DeleteFolder requires a target path");
    }
    const auto target = command.targetPath->lexically_normal();

    if (isPlaybackTarget(target)) {
      MediaControlCommand stopCommand{};
      stopCommand.kind = MediaControlCommandKind::Stop;
      auto reduction = reducer_.reduceCommand(stopCommand);
      commitReduction(reduction);
      executeIntents(reduction.intents);
    }

    bool removed = false;
    std::string failure;
    try {
      removed = dependencies_.scanner->removeLocation(target);
    } catch (const std::exception& error) {
      failure = error.what();
    } catch (...) {
      failure = "unknown scanner failure";
    }
    if (!removed) {
      const auto message = failure.empty() ? std::string{"Failed to remove target from disk or library"} : failure;
      return rejectCommand(MediaControllerErrorCode::BackendRejected, message);
    }
    return acceptedResult();
  }

  [[nodiscard]] bool isPlaybackTarget(const std::filesystem::path& target) const {
    switch (playerSnapshot_.playback.state) {
    case PlaybackStatus::Stopped:
    case PlaybackStatus::Error:
      return false;
    case PlaybackStatus::Playing:
    case PlaybackStatus::Paused:
    case PlaybackStatus::Loading:
    case PlaybackStatus::Seeking:
    case PlaybackStatus::Buffering:
      break;
    }
    if (!playerSnapshot_.currentTrack.has_value()) {
      return false;
    }
    return playerSnapshot_.currentTrack->filePath.lexically_normal() == target;
  }

  void publishSavedFolderSortRulesForRoots(const std::vector<scanner::ScannerRoot>& roots) {
    for (const auto& root : roots) {
      try {
        for (auto setting : dependencies_.folderSortSettingsStore->list(root.path)) {
          notificationSubscriptions_.publish(makeFolderSortAppliedNotification(std::move(setting)));
        }
      } catch (const FolderSortSettingsError& error) {
        spdlog::warn("failed to list saved folder sort rules for root '{}': {}", pathText(root.path), error.what());
      } catch (const std::exception& error) {
        spdlog::warn("failed to list saved folder sort rules for root '{}': {}", pathText(root.path), error.what());
      }
    }
  }

  void stopScannerWatching(std::string_view reason) noexcept {
    try {
      dependencies_.scanner->stopWatching();
    } catch (const std::exception& error) {
      spdlog::warn("failed to stop scanner watcher during {}: {}", reason, error.what());
    } catch (...) {
      spdlog::warn("failed to stop scanner watcher during {}", reason);
    }
  }

  void handleAudioEvent(const audio::BackendEvent& event) {
    auto reduction = reducer_.reduceAudioEvent(event);
    commitReduction(reduction);
    executeIntents(reduction.intents);
  }

  void handleScannerEvent(const scanner::ScannerEvent& event) {
    auto reduction = reducer_.reduceScannerEvent(event);
    commitReduction(reduction);
    executeIntents(reduction.intents);
  }

  void commitReduction(const ControlReduction& reduction) {
    std::optional<PlayerStateSnapshot> playerSnapshot;
    std::optional<LibraryStateSnapshot> librarySnapshot;

    {
      std::lock_guard lock{mutex_};
      if (reduction.playerStateChanged) {
        playerSnapshot_ = reducer_.playerState();
        playerSnapshot = playerSnapshot_;
      }
      if (reduction.libraryStateChanged) {
        librarySnapshot_ = reducer_.libraryState();
        librarySnapshot = librarySnapshot_;
      }
    }

    if (playerSnapshot.has_value()) {
      publishPlayerSnapshot(*playerSnapshot);
    }
    if (librarySnapshot.has_value()) {
      librarySubscriptions_.publish(*librarySnapshot);
    }
    for (const auto& notification : reduction.notifications) {
      notificationSubscriptions_.publish(notification);
    }
  }

  void publishPlayerSnapshot(const PlayerStateSnapshot& snapshot) {
    playerSubscriptions_.publish(snapshot);
    if (isRunning()) {
      const auto updateResult = dependencies_.metadata->update(platformStateFromSnapshot(snapshot));
      (void)updateResult;
    }
  }

  void executeIntents(const std::vector<ControlIntent>& intents) {
    for (const auto& intent : intents) {
      switch (intent.kind) {
      case ControlIntentKind::LoadTrack:
        if (intent.track.has_value()) {
          dependencies_.audio->loadTrack(*intent.track);
        }
        break;
      case ControlIntentKind::Play:
        dependencies_.audio->play();
        break;
      case ControlIntentKind::Pause:
        dependencies_.audio->pause();
        break;
      case ControlIntentKind::Resume:
        dependencies_.audio->resume();
        break;
      case ControlIntentKind::Stop:
        dependencies_.audio->stop();
        break;
      case ControlIntentKind::Seek:
        if (intent.position.has_value()) {
          dependencies_.audio->seek(*intent.position);
        }
        break;
      case ControlIntentKind::SetVolume:
        if (intent.volume.has_value()) {
          dependencies_.audio->setVolume(*intent.volume);
        }
        break;
      case ControlIntentKind::SetMuted:
        if (intent.muted.has_value()) {
          dependencies_.audio->setMuted(*intent.muted);
        }
        break;
      case ControlIntentKind::ResolveArtwork:
        if (intent.artworkRequest.has_value() && dependencies_.artworkResolver) {
          dependencies_.artworkResolver->request(*intent.artworkRequest);
        }
        break;
      case ControlIntentKind::ConfigureOutput:
        if (intent.outputConfig.has_value()) {
          dependencies_.audio->configureOutput(*intent.outputConfig);
        }
        break;
      }
    }
  }

  MediaControllerDependencies dependencies_{};
  MediaControllerOptions options_{};
  ControlEventLoop eventLoop_;
  ControlStateReducer reducer_;
  PlayerStateSubscriptionStore playerSubscriptions_{};
  LibraryStateSubscriptionStore librarySubscriptions_{};
  DomainNotificationSubscriptionStore notificationSubscriptions_{};
  mutable std::mutex mutex_{};
  PlayerStateSnapshot playerSnapshot_{};
  LibraryStateSnapshot librarySnapshot_{};
  SubscriptionHandle metadataCommandSubscription_{};
  bool started_{false};
  bool stopping_{false};
};

MediaController::MediaController(MediaControllerDependencies dependencies, MediaControllerOptions options)
    : impl_(std::make_unique<Impl>(std::move(dependencies), options)) {}

MediaController::~MediaController() = default;

MediaController::MediaController(MediaController&&) noexcept = default;

MediaController& MediaController::operator=(MediaController&&) noexcept = default;

void MediaController::start() { impl_->start(); }

void MediaController::shutdown() { impl_->shutdown(); }

MediaControllerCommandResult MediaController::submitCommand(const MediaControlCommand& command) { return impl_->submitCommand(command); }

std::optional<std::string> MediaController::getAppSetting(const std::string& group, const std::string& key) {
  return impl_->getAppSetting(group, key);
}

MediaControllerCommandResult MediaController::setAppSetting(std::string group, std::string key, std::string value) {
  return impl_->setAppSetting(std::move(group), std::move(key), std::move(value));
}

MediaControllerCommandResult MediaController::removeAppSetting(std::string group, std::string key) {
  return impl_->removeAppSetting(std::move(group), std::move(key));
}

MediaControllerCommandResult MediaController::scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode) {
  return impl_->scanLibrary(std::move(roots), mode);
}

SubscriptionHandle MediaController::subscribePlayerState(PlayerStateSnapshotCallback callback) {
  return impl_->subscribePlayerState(std::move(callback));
}

SubscriptionHandle MediaController::subscribeLibraryState(LibraryStateSnapshotCallback callback) {
  return impl_->subscribeLibraryState(std::move(callback));
}

SubscriptionHandle MediaController::subscribeDomainNotifications(ControlDomainNotificationCallback callback) {
  return impl_->subscribeDomainNotifications(std::move(callback));
}

PlayerStateSnapshot MediaController::playerStateSnapshot() const { return impl_->playerStateSnapshot(); }

LibraryStateSnapshot MediaController::libraryStateSnapshot() const { return impl_->libraryStateSnapshot(); }

std::vector<audio::AudioDeviceFormat> MediaController::enumeratePlaybackDevices() const {
  return impl_->enumeratePlaybackDevices();
}

audio::BackendEventSink MediaController::audioEventSink() { return impl_->audioEventSink(); }

scanner::ScannerEventSink MediaController::scannerEventSink() { return impl_->scannerEventSink(); }

void MediaController::drainForTests() { impl_->drainForTests(); }

std::unique_ptr<MediaController> makeMediaController(MediaControllerDependencies dependencies, MediaControllerOptions options) {
  return std::make_unique<MediaController>(std::move(dependencies), options);
}

}
