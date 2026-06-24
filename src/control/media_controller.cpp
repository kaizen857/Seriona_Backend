#include "seriona/control/media_controller.h"

#include "control_event_loop.h"
#include "control_state_reducer.h"
#include "media_controller_module.h"
#include "subscription_store.h"

#include "seriona/metadata/metadata_contracts.h"

#include "spdlog/spdlog.h"

#include <future>
#include <mutex>
#include <type_traits>
#include <utility>

namespace seriona::control {
namespace {

[[nodiscard]] MediaControllerCommandResult stoppedResult() {
  return MediaControllerCommandResult{.accepted = false,
                                      .code = MediaControllerErrorCode::ControllerStopped,
                                      .message = "Media controller is stopped"};
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

    if (metadataCommandSubscription_.unsubscribe) {
      metadataCommandSubscription_.unsubscribe();
      metadataCommandSubscription_ = {};
    }
    if (shouldStopMetadata) {
      const auto stopResult = dependencies_.metadata->stop();
      (void)stopResult;
    }
    eventLoop_.stop();
    spdlog::info("media controller stopped");
  }

  MediaControllerCommandResult submitCommand(const MediaControlCommand& command) {
    return dispatch<MediaControllerCommandResult>([this, command] { return reduceCommand(command); });
  }

  MediaControllerCommandResult scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode) {
    if (!isRunning()) {
      return stoppedResult();
    }

    dependencies_.scanner->scan(roots, mode);
    dependencies_.scanner->startWatching(roots);
    return MediaControllerCommandResult{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}};
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

  [[nodiscard]] bool isRunning() const {
    std::lock_guard lock{mutex_};
    return started_ && !stopping_;
  }

  MediaControllerCommandResult reduceCommand(const MediaControlCommand& command) {
    auto reduction = reducer_.reduceCommand(command);
    commitReduction(reduction);
    executeIntents(reduction.intents);
    return reduction.result;
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

audio::BackendEventSink MediaController::audioEventSink() { return impl_->audioEventSink(); }

scanner::ScannerEventSink MediaController::scannerEventSink() { return impl_->scannerEventSink(); }

void MediaController::drainForTests() { impl_->drainForTests(); }

std::unique_ptr<MediaController> makeMediaController(MediaControllerDependencies dependencies, MediaControllerOptions options) {
  return std::make_unique<MediaController>(std::move(dependencies), options);
}

}
