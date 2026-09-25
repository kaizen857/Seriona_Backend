#include "seriona/control/media_controller.h"

#include "control_event_loop.h"
#include "control_state_reducer.h"
#include "media_controller_module.h"
#include "subscription_store.h"
#include "track_lyrics_split.h"

#include "seriona/control/app_settings_store.h"
#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/control/lyric_split_store.h"
#include "seriona/metadata/metadata_contracts.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
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

namespace {

[[nodiscard]] SubscriptionDeliveryMode deliveryModeFor(const MediaControllerOptions& options) {
  // inline 测试模式（无事件循环 worker 线程）下同步投递订阅回调：
  // 回调可见性可预测，测试无需轮询等待异步投递（2026-09-05 修复：异步投递
  // 偶发调度延迟曾让 1s-5s 等待预算的断言随机失败）。生产线程模式保持异步。
  return options.runInlineForTests ? SubscriptionDeliveryMode::Sync : SubscriptionDeliveryMode::Async;
}

}

class MediaController::Impl {
public:
  // 控制层内部 seam（定义在私有头 media_controller_module.h）访问点。
  friend struct MediaControllerInternalAccess;

  Impl(MediaControllerDependencies dependencies, MediaControllerOptions options)
      : dependencies_(std::move(dependencies)),
        options_(options),
        eventLoop_(options),
        reducer_(options),
        playerSubscriptions_({}, deliveryModeFor(options)),
        librarySubscriptions_({}, deliveryModeFor(options)),
        notificationSubscriptions_({}, deliveryModeFor(options)),
        equalizerSubscriptions_({}, deliveryModeFor(options)),
        spectrumSubscriptions_({}, deliveryModeFor(options)),
        trackLyricsSubscriptions_({}, deliveryModeFor(options)) {
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
    try {
      const auto startResult = dependencies_.metadata->start(platformStateFromSnapshot(playerStateSnapshot()));
      (void)startResult;
    } catch (const std::exception& error) {
      // metadata（MPRIS/SMTC 等平台控制面）启动异常不得影响控制器可用性：记录后继续，
      // 与 backend 返回 !accepted 的既有降级语义一致（播放/扫描均不依赖它）。
      spdlog::warn("metadata backend start failed: {}", error.what());
    }
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

  SubscriptionHandle subscribeEqualizerState(EqualizerStateSnapshotCallback callback) {
    return equalizerSubscriptions_.subscribe(std::move(callback), equalizerStateSnapshot());
  }

  SubscriptionHandle subscribeSpectrum(SpectrumSnapshotCallback callback) {
    return spectrumSubscriptions_.subscribe(std::move(callback), spectrumSnapshot());
  }

  SubscriptionHandle subscribeTrackLyrics(TrackLyricsSnapshotCallback callback) {
    if (!callback) {
      return {};
    }
    // 单调投递包装（A）：publish 捕获投递列表与其实际投递之间不是原子的（发布在锁外，
    // 且 Sync 模式在调用线程内执行回调），因此投递回调可能重入并触发一次更新的发布，
    // 使「较旧快照的回调」在「较新快照的回调」之后才返回、订阅面以较旧版本收尾。
    // 这里为每个订阅维护一个门：投递进行中时，把新到的投递登记为「待投递（只保留最新）」，
    // 由外层投递循环在回调返回后继续投递；已完成投递过的最高版本之下的投递直接丢弃。
    // 结果：投递给该订阅的 version 严格递增，订阅面最终收敛到最新快照。
    auto gate = std::make_shared<LyricsDeliveryGate>();
    auto guarded = [callback = std::move(callback), gate](const TrackLyricsSnapshot& snapshot) {
      try {
        std::optional<TrackLyricsSnapshot> next{snapshot};
        // 入口（只判定一次）：较旧即丢弃；已有投递在进行则只登记最新待投递快照并返回。
        {
          std::lock_guard lock{gate->mutex};
          if (next->freshness.version < gate->highestDelivered) {
            return;  // 已被更新的投递取代
          }
          if (gate->inProgress) {
            // 同步重入：登记最新待投递快照，交由正在投递的那个循环继续投递。
            if (!gate->pending.has_value() || gate->pending->freshness.version < next->freshness.version) {
              gate->pending = std::move(next);
            }
            return;
          }
          gate->inProgress = true;
        }
        // 本调用成为该订阅的投递者：依次投递 next，并把重入期间登记的更新快照投递完。
        for (;;) {
          {
            std::lock_guard lock{gate->mutex};
            gate->highestDelivered = next->freshness.version;
          }
          callback(*next);
          std::optional<TrackLyricsSnapshot> followUp;
          {
            std::lock_guard lock{gate->mutex};
            if (gate->pending.has_value()) {
              followUp = std::move(gate->pending);
              gate->pending.reset();
            } else {
              // 取出登记项与结束投递在同一临界区内判定，避免漏掉并发登记项而永久卡住。
              gate->inProgress = false;
            }
          }
          if (!followUp.has_value()) {
            return;
          }
          next = std::move(followUp);
        }
      } catch (...) {
        // 回调抛出：复位门状态（并丢弃未投递的登记项），再把异常交给上游记账，
        // 避免门永久停在「投递中」而让该订阅此后收不到任何投递。
        std::lock_guard lock{gate->mutex};
        gate->inProgress = false;
        gate->pending.reset();
        throw;
      }
    };
    // 注册与初投同样不外泄异常（C）：初投快照拷贝失败（如分配失败）时改为不初投、
    // 仍完成注册；注册本身失败时返回空句柄。二者都不把异常抛给调用方。
    std::optional<TrackLyricsSnapshot> initial;
    try {
      initial = trackLyricsSnapshot();
    } catch (...) {
      initial = std::nullopt;
    }
    try {
      return trackLyricsSubscriptions_.subscribe(std::move(guarded), std::move(initial));
    } catch (...) {
      return {};
    }
  }

  // 每订阅投递门（A）：`inProgress` 表示该订阅正处于一次投递回调内部；`pending` 是
  // 重入期间登记的最新待投递快照；`highestDelivered` 是已投递过的最高 version。
  struct LyricsDeliveryGate {
    std::mutex mutex{};
    std::optional<TrackLyricsSnapshot> pending{};
    std::uint64_t highestDelivered{0};
    bool inProgress{false};
  };

  TrackLyricsSnapshot trackLyricsSnapshot() const {
    std::lock_guard lock{mutex_};
    return trackLyricsSnapshot_;
  }

  std::string lyricsTargetLanguage() const {
    std::lock_guard lock{mutex_};
    return lyricsTargetLanguage_;
  }

  PlayerStateSnapshot playerStateSnapshot() const {
    std::lock_guard lock{mutex_};
    return playerSnapshot_;
  }

  LibraryStateSnapshot libraryStateSnapshot() const {
    std::lock_guard lock{mutex_};
    return librarySnapshot_;
  }

  audio::EqualizerStateSnapshot equalizerStateSnapshot() const {
    std::lock_guard lock{mutex_};
    return equalizerSnapshot_;
  }

  audio::SpectrumSnapshot spectrumSnapshot() const {
    std::lock_guard lock{mutex_};
    return spectrumSnapshot_;
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
    if (isLyricsCommand(command.kind)) {
      return reduceLyricsCommand(command);
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

    // 落库成功后让播放顺序立即跟随新排序（reducer 不做 I/O，只接收已校验的纯数据规则）：
    // 否则"界面已按新序、后端仍按起播时冻结的旧序推进"，表现为顺序播放跳过曲目。
    auto reduction = reducer_.applyContextSortRules(setting.rootPath, setting.folderNodeId, setting.rules);
    commitReduction(reduction);
    executeIntents(reduction.intents);

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
    // 已存规则由上一次进程落库（本进程无对应 ApplyFolderSortRules 命令）：先注入 reducer，
    // 使随后到达的快照即按用户设置排序；再逐条通知前端回填其排序规则缓存。
    std::vector<FolderSortSetting> savedSettings;
    for (const auto& root : roots) {
      try {
        for (auto setting : dependencies_.folderSortSettingsStore->list(root.path)) {
          savedSettings.push_back(setting);
          notificationSubscriptions_.publish(makeFolderSortAppliedNotification(std::move(setting)));
        }
      } catch (const FolderSortSettingsError& error) {
        spdlog::warn("failed to list saved folder sort rules for root '{}': {}", pathText(root.path), error.what());
      } catch (const std::exception& error) {
        spdlog::warn("failed to list saved folder sort rules for root '{}': {}", pathText(root.path), error.what());
      }
    }

    if (!savedSettings.empty()) {
      auto reduction = reducer_.injectSavedContextSortRules(std::move(savedSettings));
      commitReduction(reduction);
      executeIntents(reduction.intents);
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
    // R2 频谱显示链路：SpectrumUpdated 不进入 reducer 状态面——reducer 无频谱镜像
    // （服务原子位为开关最终态、事件载荷即完整快照），控制器 impl 层独立槽直写
    // 后发布（同订阅初始投递面 spectrumSnapshot()）。其余事件照旧经 reducer。
    if (event.type == audio::BackendEventType::SpectrumUpdated) {
      handleSpectrumUpdated(event);
      return;
    }
    auto reduction = reducer_.reduceAudioEvent(event);
    commitReduction(reduction);
    executeIntents(reduction.intents);
  }

  // 频谱快照事件落槽 + 订阅推送（事件循环线程；mutex_ 仅护成员写，publish 在锁外，
  // 同 commitReduction 的锁纪律）。载荷非法（type/payload 失配）时静默丢弃。
  void handleSpectrumUpdated(const audio::BackendEvent& event) {
    const auto* payload = std::get_if<audio::SpectrumUpdated>(&event.payload);
    if (payload == nullptr) {
      return;
    }
    audio::SpectrumSnapshot snapshot;
    {
      std::lock_guard lock{mutex_};
      spectrumSnapshot_ = payload->snapshot;
      snapshot = spectrumSnapshot_;
    }
    spectrumSubscriptions_.publish(snapshot);
  }

  void handleScannerEvent(const scanner::ScannerEvent& event) {
    auto reduction = reducer_.reduceScannerEvent(event);
    commitReduction(reduction);
    executeIntents(reduction.intents);
  }

  void commitReduction(const ControlReduction& reduction) {
    std::optional<PlayerStateSnapshot> playerSnapshot;
    std::optional<LibraryStateSnapshot> librarySnapshot;
    std::optional<audio::EqualizerStateSnapshot> equalizerSnapshot;

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
      if (reduction.equalizerStateChanged) {
        equalizerSnapshot_ = reducer_.equalizerState();
        equalizerSnapshot = equalizerSnapshot_;
      }
    }

    if (playerSnapshot.has_value()) {
      publishPlayerSnapshot(*playerSnapshot);
    }
    if (librarySnapshot.has_value()) {
      librarySubscriptions_.publish(*librarySnapshot);
    }
    if (equalizerSnapshot.has_value()) {
      equalizerSubscriptions_.publish(*equalizerSnapshot);
    }
    for (const auto& notification : reduction.notifications) {
      notificationSubscriptions_.publish(notification);
    }

    // 歌词切分快照重发布判据（发布时机①④）：
    // ① 当前曲目变更 —— 用「当前 trackId 是否与上次发布时不同」判定，而不是
    //    `playerStateChanged`（后者每个播放位置 tick 都为真，会造成逐 tick 重算）；
    // ④ 曲库变更 —— 只看【曲库树版本】是否变化，而不是 `libraryStateChanged`：
    //    后者在扫描期由进度 tick（默认每 250ms）置真，判定过宽会造成 ≈4×/s 重算。
    // ②（目标语言变更）与 ③（手工纠错增删）不经此路径，由调用方显式触发重算。
    if (shouldRefreshTrackLyrics(reduction)) {
      refreshTrackLyrics();
    }
  }

  [[nodiscard]] std::optional<std::uint64_t> libraryTreeVersionLocked() const {
    return librarySnapshot_.libraryTree.has_value()
               ? std::optional<std::uint64_t>{librarySnapshot_.libraryTree->version}
               : std::nullopt;
  }

  [[nodiscard]] std::string currentTrackIdLocked() const {
    return playerSnapshot_.currentTrack ? playerSnapshot_.currentTrack->trackId : std::string{};
  }

  [[nodiscard]] bool shouldRefreshTrackLyrics(const ControlReduction& reduction) {
    std::lock_guard lock{mutex_};
    // 曲库树版本变化：立即重算（不受失败重试的限频影响）。
    if (reduction.libraryStateChanged && libraryTreeVersionLocked() != publishedTrackLyricsTreeVersion_) {
      return true;
    }
    if (!reduction.playerStateChanged) {
      return false;
    }
    // 当前曲目变化：立即重算。
    if (currentTrackIdLocked() != publishedTrackLyricsTrackId_) {
      return true;
    }
    // 同曲目、同树版本：仅当上次刷新失败且已到重试时机时才重试。失败后的前
    // kLyricsRefreshRapidRetries 次重试不等待（下一次触发即重试，使 store 恢复后
    // 单次触发即可回正）；之后按 kLyricsRefreshRetryInterval 限频，使持续失败时的
    // store 尝试次数有上界。健康路径下 consecutiveFailures_ 为 0，故 B2 的
    // 「不因普通 tick 反复重算」不受影响。
    return lyricsRefreshConsecutiveFailures_ > 0
        && std::chrono::steady_clock::now() >= lyricsRefreshRetryNotBefore_;
  }

  // 失败后的重试策略：前 kLyricsRefreshRapidRetries 次重试不等待（下一次触发即重试），
  // 覆盖「store 短暂 BUSY 后立即恢复」这一常见情形；之后的连续失败按
  // kLyricsRefreshRetryInterval 限频。间隔取 1s：比扫描进度 tick（默认 250ms）与播放
  // 位置 tick 都长，使持续失败时的 store 尝试次数落在约 1 次/秒的量级。
  static constexpr int kLyricsRefreshRapidRetries{2};
  static constexpr std::chrono::milliseconds kLyricsRefreshRetryInterval{1000};

  // 重算并重发布一次【全量】当前曲目歌词快照（不做增量）。发布内容由 todo 26 的
  // buildTrackLyricsSnapshot 组装；本函数只负责取数时机、freshness 推进与发布。
  //
  // 对外不抛出（B1/F2）：store 交互失败、publish 失败与任何意外异常都只记录，不交给
  // 调用方。异常面的差别是：曲库/播放事件路径由 ControlEventLoop 的 worker/drain 捕获
  // （control_event_loop.cpp:64-70/131-135）；submitCommand 与媒体控制器内部 seam
  // 没有该捕获层，故本函数是它们不抛出的保证来源。
  // 一次歌词刷新所基于的输入身份。第 4 项是 store 内容修订号（B）：store 的 manual 行
  // 只能经 callLyricStore 的三条命令改写（auto 行由刷新自身写入、属同一次 build），
  // 故该计数变化即代表本次 build 读到的 store 内容已被超越。
  struct LyricsBuildIdentity {
    std::string trackId{};
    std::optional<std::uint64_t> treeVersion{};
    std::string language{};
    std::uint64_t storeRevision{0};

    [[nodiscard]] bool operator==(const LyricsBuildIdentity& other) const {
      return trackId == other.trackId && treeVersion == other.treeVersion && language == other.language
          && storeRevision == other.storeRevision;
    }
  };

  [[nodiscard]] LyricsBuildIdentity currentLyricsBuildIdentityLocked() const {
    return LyricsBuildIdentity{.trackId = currentTrackIdLocked(),
                               .treeVersion = libraryTreeVersionLocked(),
                               .language = lyricsTargetLanguage_,
                               .storeRevision = lyricSplitStoreRevision_};
  }

  // 快照提交的唯一入口（调用方必须持有 mutex_）：身份复核 → 记账 → 空行回退 → freshness 推进。
  // `identity` 非空时与实时状态比对，不一致返回 nullopt（丢弃：不发布、不改写
  // trackLyricsSnapshot_ / publishedTrackLyrics* / 失败记账）；为空表示以实时状态为准
  // （异常收尾路径专用，无需复核）。校验与写入同处调用方的 `mutex_` 临界区。
  [[nodiscard]] std::optional<TrackLyricsSnapshot> commitLyricsSnapshotLocked(
      TrackLyricsSnapshot snapshot, const bool refreshed, const std::optional<LyricsBuildIdentity> identity) {
    if (identity.has_value() && !(*identity == currentLyricsBuildIdentityLocked())) {
      return std::nullopt;
    }
    const auto current = currentLyricsBuildIdentityLocked();
    // 已发布身份：成功与失败回退都记 —— 两种情况都已把订阅面更新为【当前曲目】。
    publishedTrackLyricsTrackId_ = current.trackId;
    publishedTrackLyricsTreeVersion_ = current.treeVersion;
    // 失败后的重试时机：成功即清零；失败则前 kLyricsRefreshRapidRetries 次立即可重试，
    // 之后的连续失败推到 now + 间隔（持续失败时的尝试上界）。
    if (refreshed) {
      lyricsRefreshConsecutiveFailures_ = 0;
      lyricsRefreshRetryNotBefore_ = std::chrono::steady_clock::time_point{};
    } else {
      ++lyricsRefreshConsecutiveFailures_;
      lyricsRefreshRetryNotBefore_ = lyricsRefreshConsecutiveFailures_ <= kLyricsRefreshRapidRetries
                                         ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::now() + kLyricsRefreshRetryInterval;
    }
    // 无当前曲目/找不到该曲目/清洗后无正文行（以及 store 失败回退）时发布空行快照：
    // 仍推进 freshness，让订阅者能用 version 区分「重发布过」与「从未发布」；
    // trackId 仍带上当前曲目，订阅者才能判断「这首没有切分歌词」而不是「不知道是哪首」。
    // 回退空快照（而非保留上一首的快照）使订阅面的 trackId 始终等于播放器当前曲目。
    snapshot.freshness.version = ++trackLyricsVersion_;
    snapshot.freshness.sampledAt = std::chrono::steady_clock::now();
    if (snapshot.trackId.empty()) {
      snapshot.trackId = current.trackId;
    }
    if (snapshot.targetLanguage.empty()) {
      snapshot.targetLanguage = current.language;
    }
    trackLyricsSnapshot_ = std::move(snapshot);
    return trackLyricsSnapshot_;
  }

  void refreshTrackLyrics() {
    try {
      refreshTrackLyricsImpl();
    } catch (...) {
      // 与内层 store 失败同语义（D）：任何未在 impl 内收束的异常（含非 std 类型）都落到
      // 同一收尾 —— 发布当前曲目的空行回退快照并做失败记账/限频，使「订阅面 trackId 恒等
      // 当前曲目」与「store 尝试次数有上界」不随异常类型变化。
      reportLyricsRefreshFailure();
      commitLyricsRefreshFailure();
    }
  }

  // 异常收尾：以实时状态为准提交一次失败回退（复用 commitLyricsSnapshotLocked）。
  void commitLyricsRefreshFailure() noexcept {
    try {
      std::optional<TrackLyricsSnapshot> toPublish;
      {
        std::lock_guard lock{mutex_};
        toPublish = commitLyricsSnapshotLocked(TrackLyricsSnapshot{}, /*refreshed=*/false, std::nullopt);
      }
      if (toPublish.has_value()) {
        publishTrackLyrics(*toPublish);
      }
    } catch (...) {
      reportLyricsRefreshFailure();
    }
  }

  void reportLyricsRefreshFailure() noexcept {
    try {
      spdlog::error("track lyrics refresh aborted by an unexpected exception (suppressed)");
    } catch (...) {
    }
  }

  void refreshTrackLyricsImpl() {
    // 锁内只做状态快照拷贝（F3）：store I/O 与切分计算在锁外进行，使
    // playerStateSnapshot()/libraryStateSnapshot()/lyricsTargetLanguage() 等公共读取口
    // 不被整个刷新期间挡住。
    //
    // 拷贝到提交之间存在窗口，期间这些输入【可以】被改写：inline 测试模式下控制事件
    // 循环会在调用者线程上重入执行被投递的工作，而投递进来的用户回调也可能再提交命令
    // （见 subscribeTrackLyrics 的单调投递包装）。复核因而是【纵深防御】——它让提交时刻
    // 已过期的输入被丢弃。既有面中 `Impl::scanLibrary` 也在调用者线程改 reducer/快照
    // 状态（属后续任务），但它当前不触发本函数；本复核不依赖该路径成立。
    PlayerStateSnapshot player;
    LibraryStateSnapshot library;
    std::string language;
    std::string currentTrackId;
    std::optional<std::uint64_t> treeVersion;
    std::uint64_t storeRevision = 0;
    {
      std::lock_guard lock{mutex_};
      player = playerSnapshot_;
      library = librarySnapshot_;
      language = lyricsTargetLanguage_;
      currentTrackId = currentTrackIdLocked();
      treeVersion = libraryTreeVersionLocked();
      storeRevision = lyricSplitStoreRevision_;
    }

    TrackLyricsSnapshot snapshot;
    bool refreshed = false;
    if (dependencies_.lyricSplitStore) {
      try {
        if (auto built = buildTrackLyricsSnapshot(player, library, language, *dependencies_.lyricSplitStore);
            built.has_value()) {
          snapshot = std::move(*built);
        }
        refreshed = true;
      } catch (const LyricSplitStoreError& error) {
        spdlog::error("lyric split refresh failed, publishing an empty snapshot for the current track: {}",
                      error.what());
      } catch (const std::exception& error) {
        spdlog::error("lyric split refresh failed, publishing an empty snapshot for the current track: {}",
                      error.what());
      }
    } else {
      // 同上的纵深防御分支：装配期已注入 Noop，故此处不可达；若真为空则视为
      // 「无 store 可查、按自动结果发布」，与 Noop 的 load 恒未命中同义。
      refreshed = true;
    }

    // 提交：身份复核（4 个输入）不通过即丢弃。丢弃不会造成「无人发布」——把输入变陈旧
    // 的写入（commitReduction 末、applyLyricsTargetLanguage、callLyricStore）随后都
    // 自行发起一次基于当时状态的刷新。
    std::optional<TrackLyricsSnapshot> toPublish;
    {
      std::lock_guard lock{mutex_};
      toPublish = commitLyricsSnapshotLocked(std::move(snapshot), refreshed,
                                             LyricsBuildIdentity{.trackId = currentTrackId,
                                                                 .treeVersion = treeVersion,
                                                                 .language = language,
                                                                 .storeRevision = storeRevision});
    }
    if (toPublish.has_value()) {
      publishTrackLyrics(*toPublish);
    }
  }

  // publish 会拷贝订阅回调（subscription_store.cpp 的 deliveries 装配）并可能起投递
  // 线程，这些都在 store 防护区之外；此处再包一层，使「refreshTrackLyrics 不抛」覆盖
  // publish（F2）。记录失败本身也在保护范围内，避免日志分配失败把异常带出去。
  [[nodiscard]] bool isSnapshotSuperseded(const std::uint64_t version) const {
    std::lock_guard lock{mutex_};
    return version < trackLyricsVersion_;
  }

  void publishTrackLyrics(const TrackLyricsSnapshot& snapshot) noexcept {
    try {
      // 机会性检查（A 的纵深防御）：若本次提交在发布前已被更新的提交取代，跳过发布。
      // 它覆盖「提交 → 发布」之间被超越的窗口，但不覆盖「某次 publish 已捕获投递列表
      // 之后才被超越」的窗口 —— 后者由 subscribeTrackLyrics 的逐订阅单调包装闭合。
      if (isSnapshotSuperseded(snapshot.freshness.version)) {
        return;
      }
      trackLyricsSubscriptions_.publish(snapshot);
    } catch (...) {
      try {
        spdlog::error("track lyrics publish failed (exception suppressed)");
      } catch (...) {
      }
    }
  }

  // 目标语言白名单。只用短语言码、不引入区域变体（如 zh-Hans）。
  [[nodiscard]] static bool isSupportedLyricsTargetLanguage(const std::string_view language) {
    return language == "zh" || language == "ja" || language == "ko" || language == "en";
  }

  [[nodiscard]] static MediaControllerErrorCode commandCodeFromLyricStoreError(const LyricSplitStoreErrorCode code) noexcept {
    switch (code) {
    case LyricSplitStoreErrorCode::InvalidEntry:
      return MediaControllerErrorCode::InvalidCommand;
    case LyricSplitStoreErrorCode::StorageError:
      return MediaControllerErrorCode::BackendRejected;
    }
    return MediaControllerErrorCode::BackendRejected;
  }

  MediaControllerCommandResult applyLyricsTargetLanguage(const std::string& targetLanguage) {
    if (!isSupportedLyricsTargetLanguage(targetLanguage)) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                           "SetLyricsTargetLanguage requires one of zh/ja/ko/en");
    }
    bool changed = false;
    {
      std::lock_guard lock{mutex_};
      changed = lyricsTargetLanguage_ != targetLanguage;
      lyricsTargetLanguage_ = targetLanguage;
    }
    // 目标语言未变时不重发布（幂等接受）；变了则立即按时机②重算并重发布。
    if (changed) {
      refreshTrackLyrics();
    }
    return acceptedResult();
  }

  // 单条纠错的公共校验。约定缺失【必须】拒绝：约定是内容寻址键的组成部分，回退成
  // 控制层自行推断的当前曲目约定会静默写错键（同一 rawText 在不同约定下是不同的行）。
  [[nodiscard]] std::optional<MediaControllerCommandResult> validateSingleCorrection(
      const MediaControlCommand& command, const std::string_view commandName) {
    if (!command.lyricRawText.has_value() || command.lyricRawText->empty()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                           std::string{commandName} + " requires a non-empty lyricRawText");
    }
    if (!command.lyricConvention.has_value()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                           std::string{commandName} + " requires lyricConvention");
    }
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<LyricSplitConvention> correctionConvention(const MediaControlCommand& command) {
    return conventionFromToken(*command.lyricConvention);
  }

  MediaControllerCommandResult callLyricStore(const std::function<void(LyricSplitStore&)>& work) {
    // 纵深防御分支：正常装配下不可达 —— normalizeMediaControllerDependencies 在
    // dependencies_.lyricSplitStore 为空时注入 Noop 实现（每次 load 均未命中、不落库），
    // 故真实语义是 noop 降级而非拒绝。保留此判断只为防「绕过装配直接构造」的调用方。
    if (!dependencies_.lyricSplitStore) {
      return rejectCommand(MediaControllerErrorCode::BackendRejected, "lyric split store is unavailable");
    }
    try {
      work(*dependencies_.lyricSplitStore);
    } catch (const LyricSplitStoreError& error) {
      return rejectCommand(commandCodeFromLyricStoreError(error.code()), error.what());
    } catch (const std::exception& error) {
      return rejectCommand(MediaControllerErrorCode::BackendRejected, error.what());
    } catch (...) {
      // 自定义 store 可能抛非 std 类型：同样收敛为拒绝，不让它穿出公共 API。
      return rejectCommand(MediaControllerErrorCode::BackendRejected,
                           "lyric split store operation failed with a non-std exception");
    }
    // 写入成功：登记 store 内容修订号（B）。刷新自身写入的 auto 行属同一次 build、不计入；
    // 外部能改写 store 内容的只有这三条经本函数的命令（manual 行）。
    {
      std::lock_guard lock{mutex_};
      ++lyricSplitStoreRevision_;
    }
    // refreshTrackLyrics() 自身不抛（B1/D）：store 失败只会让它走失败收尾（发布当前曲目的
    // 空行回退并记账），因此这里不再需要第二层 try/catch，命令结果由上面的 store 写入决定。
    refreshTrackLyrics();
    return acceptedResult();
  }

  MediaControllerCommandResult upsertLyricSplitCorrection(const MediaControlCommand& command) {
    constexpr std::string_view kName{"UpsertLyricSplitCorrection"};
    if (auto rejection = validateSingleCorrection(command, kName); rejection.has_value()) {
      return *rejection;
    }
    const auto convention = correctionConvention(command);
    if (!convention.has_value()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                           std::string{kName} + " has an unknown lyricConvention token");
    }
    const std::string language = lyricsTargetLanguage();
    return callLyricStore([&](LyricSplitStore& store) {
      store.upsertManual(LyricSplitEntry{.rawText = *command.lyricRawText,
                                        .targetLanguage = language,
                                        .convention = *convention,
                                        .original = command.lyricOriginal.value_or(std::string{}),
                                        .translation = command.lyricTranslation.value_or(std::string{})});
    });
  }

  MediaControllerCommandResult removeLyricSplitCorrection(const MediaControlCommand& command) {
    constexpr std::string_view kName{"RemoveLyricSplitCorrection"};
    if (auto rejection = validateSingleCorrection(command, kName); rejection.has_value()) {
      return *rejection;
    }
    const auto convention = correctionConvention(command);
    if (!convention.has_value()) {
      return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                           std::string{kName} + " has an unknown lyricConvention token");
    }
    const std::string language = lyricsTargetLanguage();
    // removeManual 的接口是 void、无「命中行数」回报，故未命中（没有对应 manual 行）时
    // 该命令同样返回 accepted —— 语义是幂等空操作（状态与执行前一致），而不是「已撤销」
    // 的强承诺。前端不应据此断言纠错一定曾存在。
    return callLyricStore([&](LyricSplitStore& store) {
      store.removeManual(*command.lyricRawText, language, *convention);
    });
  }

  // 全库语义：清空全部手工纠错（前端禁用，见契约注释；供 store 层单测与运维复位）。
  MediaControllerCommandResult clearLyricSplitCorrections() {
    return callLyricStore([](LyricSplitStore& store) { store.clearManual(); });
  }

  MediaControllerCommandResult reduceLyricsCommand(const MediaControlCommand& command) {
    switch (command.kind) {
    case MediaControlCommandKind::SetLyricsTargetLanguage:
      if (!command.lyricsTargetLanguage.has_value()) {
        return rejectCommand(MediaControllerErrorCode::InvalidCommand,
                             "SetLyricsTargetLanguage requires lyricsTargetLanguage");
      }
      return applyLyricsTargetLanguage(*command.lyricsTargetLanguage);
    case MediaControlCommandKind::UpsertLyricSplitCorrection:
      return upsertLyricSplitCorrection(command);
    case MediaControlCommandKind::RemoveLyricSplitCorrection:
      return removeLyricSplitCorrection(command);
    case MediaControlCommandKind::ClearLyricSplitCorrections:
      return clearLyricSplitCorrections();
    default:
      return rejectCommand(MediaControllerErrorCode::InvalidCommand, "Unsupported lyrics command");
    }
  }

  [[nodiscard]] static bool isLyricsCommand(const MediaControlCommandKind kind) noexcept {
    switch (kind) {
    case MediaControlCommandKind::SetLyricsTargetLanguage:
    case MediaControlCommandKind::UpsertLyricSplitCorrection:
    case MediaControlCommandKind::RemoveLyricSplitCorrection:
    case MediaControlCommandKind::ClearLyricSplitCorrections:
      return true;
    default:
      return false;
    }
  }

  void runOnControlThread(std::function<void()> work) {
    if (options_.runInlineForTests) {
      if (!eventLoop_.post(std::move(work))) {
        spdlog::error("media controller dispatch failed: event loop post rejected");
        return;
      }
      eventLoop_.drainForTests();
      return;
    }
    if (!eventLoop_.post(std::move(work))) {
      spdlog::error("media controller dispatch failed: event loop post rejected");
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
      case ControlIntentKind::SetTransitionConfig:
        if (intent.transitionConfig.has_value()) {
          dependencies_.audio->configureTransition(*intent.transitionConfig);
        }
        break;
      case ControlIntentKind::PrepareNext:
        if (intent.track.has_value()) {
          dependencies_.audio->prepareNext(*intent.track,
                                           intent.prepareNextMeta.value_or(audio::PrepareNextMeta{}));
        }
        break;
      case ControlIntentKind::AbortTransition:
        dependencies_.audio->abortTransition();
        break;
      // 接口默认空实现暂不生效 DSP（B2 生效路径以实际输出率重建曲线并重发布）。
      case ControlIntentKind::SetEqualizerConfig:
        if (intent.equalizerConfig.has_value()) {
          dependencies_.audio->setEqualizer(*intent.equalizerConfig);
        }
        break;
      // R2 频谱显示链路：开关命令落地服务原子位（Noop/Fake 走接口默认空实现）。
      case ControlIntentKind::SetSpectrumEnabled:
        if (intent.spectrumEnabled.has_value()) {
          dependencies_.audio->setSpectrumEnabled(*intent.spectrumEnabled);
        }
        break;
      }
    }
  }

  MediaControllerDependencies dependencies_{};
  MediaControllerOptions options_{};
  ControlEventLoop eventLoop_;
  ControlStateReducer reducer_;
  PlayerStateSubscriptionStore playerSubscriptions_;
  LibraryStateSubscriptionStore librarySubscriptions_;
  DomainNotificationSubscriptionStore notificationSubscriptions_;
  EqualizerStateSubscriptionStore equalizerSubscriptions_;
  SpectrumSubscriptionStore spectrumSubscriptions_;
  TrackLyricsSnapshotSubscriptionStore trackLyricsSubscriptions_;
  mutable std::mutex mutex_{};
  PlayerStateSnapshot playerSnapshot_{};
  LibraryStateSnapshot librarySnapshot_{};
  audio::EqualizerStateSnapshot equalizerSnapshot_{};
  audio::SpectrumSnapshot spectrumSnapshot_{};
  TrackLyricsSnapshot trackLyricsSnapshot_{};
  std::string lyricsTargetLanguage_{"zh"};
  std::uint64_t trackLyricsVersion_{0};
  // store 内容修订号（B）：每次经 callLyricStore 成功写入 manual 行后自增；
  // refreshTrackLyricsImpl 在拷贝块取值、提交复核时比对，用于发现「build 期间 store
  // 内容被命令改写」的过期输入。
  std::uint64_t lyricSplitStoreRevision_{0};
  std::string publishedTrackLyricsTrackId_{};
  std::optional<std::uint64_t> publishedTrackLyricsTreeVersion_{};
  std::chrono::steady_clock::time_point lyricsRefreshRetryNotBefore_{};
  int lyricsRefreshConsecutiveFailures_{0};
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

SubscriptionHandle MediaController::subscribeEqualizerState(EqualizerStateSnapshotCallback callback) {
  return impl_->subscribeEqualizerState(std::move(callback));
}

SubscriptionHandle MediaController::subscribeSpectrum(SpectrumSnapshotCallback callback) {
  return impl_->subscribeSpectrum(std::move(callback));
}

SubscriptionHandle MediaController::subscribeTrackLyrics(TrackLyricsSnapshotCallback callback) {
  return impl_->subscribeTrackLyrics(std::move(callback));
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

void MediaControllerInternalAccess::setLyricsTargetLanguage(MediaController& controller, std::string targetLanguage) {
  controller.impl_->runOnControlThread(
      [impl = controller.impl_.get(), language = std::move(targetLanguage)]() mutable {
        if (impl->isRunning()) {
          (void)impl->applyLyricsTargetLanguage(language);
        }
      });
}

void MediaControllerInternalAccess::refreshTrackLyrics(MediaController& controller) {
  controller.impl_->runOnControlThread([impl = controller.impl_.get()] {
    if (impl->isRunning()) {
      impl->refreshTrackLyrics();
    }
  });
}

std::string MediaControllerInternalAccess::lyricsTargetLanguage(const MediaController& controller) {
  return controller.impl_->lyricsTargetLanguage();
}

}
