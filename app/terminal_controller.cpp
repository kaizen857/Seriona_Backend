#include "terminal_controller.h"

#include "terminal_io.h"

#include "seriona/app/runtime_paths.h"
#include "seriona/control/media_controller.h"

#include "logging/logging.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>

#ifdef SERIONA_RELEASE_BUILD
extern "C" {
#include <libavutil/log.h>
}
#endif

#include <optional>
#include <string>
#include <utility>

namespace seriona::app {
namespace {

namespace control = seriona::control;
namespace scanner = seriona::scanner;
using namespace std::chrono_literals;

constexpr auto kIdleRefreshInterval = 100ms;

using TerminalActionReader = std::function<TerminalAction(std::chrono::milliseconds)>;

struct TerminalState {
  control::PlayerStateSnapshot player{};
  control::LibraryStateSnapshot library{};
  std::optional<control::ControlDomainNotification> notification{};
  std::mutex mutex{};
};

std::string playbackStatusName(control::PlaybackStatus status) {
  switch (status) {
  case control::PlaybackStatus::Stopped:
    return "stopped";
  case control::PlaybackStatus::Playing:
    return "playing";
  case control::PlaybackStatus::Paused:
    return "paused";
  case control::PlaybackStatus::Loading:
    return "loading";
  case control::PlaybackStatus::Seeking:
    return "seeking";
  case control::PlaybackStatus::Buffering:
    return "buffering";
  case control::PlaybackStatus::Error:
    return "error";
  }
  return "unknown";
}

std::string scanStatusName(control::LibraryScanStatus status) {
  switch (status) {
  case control::LibraryScanStatus::Idle:
    return "idle";
  case control::LibraryScanStatus::Scanning:
    return "scanning";
  case control::LibraryScanStatus::Completed:
    return "completed";
  case control::LibraryScanStatus::Stopped:
    return "stopped";
  case control::LibraryScanStatus::Error:
    return "error";
  }
  return "unknown";
}

std::string currentTrackLabel(const control::PlayerStateSnapshot& player) {
  if (player.display.has_value() && !player.display->title.empty()) {
    return player.display->artist.empty() ? player.display->title : player.display->artist + " - " + player.display->title;
  }
  if (player.currentTrack.has_value()) {
    return player.currentTrack->filePath.filename().string();
  }
  return "no track";
}

void renderStatus(TerminalState& state, std::mutex& outputMutex, std::ostream& output) {
  control::PlayerStateSnapshot player;
  control::LibraryStateSnapshot library;
  std::optional<control::ControlDomainNotification> notification;
  {
    std::scoped_lock lock{state.mutex};
    player = state.player;
    library = state.library;
    notification = state.notification;
  }

  std::scoped_lock outputLock{outputMutex};
  output << "\r\033[K[" << playbackStatusName(player.playback.state) << "] " << currentTrackLabel(player) << " | "
         << player.timeline.position.count() << " ms | volume " << static_cast<int>(player.volume * 100.0F) << "%"
         << (player.muted ? " muted" : "") << " | scan " << scanStatusName(library.scanStatus);
  if (notification.has_value() && !notification->message.empty()) {
    output << " | " << notification->message;
  }
  output << std::flush;
}

control::MediaControlCommand commandForAction(TerminalAction action, const control::PlayerStateSnapshot& player) {
  control::MediaControlCommand command{};
  switch (action) {
  case TerminalAction::TogglePlayPause:
    command.kind = control::MediaControlCommandKind::TogglePlayPause;
    return command;
  case TerminalAction::SkipNext:
    command.kind = control::MediaControlCommandKind::SkipNext;
    return command;
  case TerminalAction::SkipPrevious:
    command.kind = control::MediaControlCommandKind::SkipPrevious;
    return command;
  case TerminalAction::Stop:
    command.kind = control::MediaControlCommandKind::Stop;
    return command;
  case TerminalAction::VolumeUp:
    command.kind = control::MediaControlCommandKind::SetVolume;
    command.volume = std::min(player.volume + 0.05F, 1.0F);
    return command;
  case TerminalAction::VolumeDown:
    command.kind = control::MediaControlCommandKind::SetVolume;
    command.volume = std::max(player.volume - 0.05F, 0.0F);
    return command;
  case TerminalAction::ToggleMuted:
    command.kind = control::MediaControlCommandKind::SetMuted;
    command.muted = !player.muted;
    return command;
  case TerminalAction::None:
  case TerminalAction::Quit:
    command.kind = control::MediaControlCommandKind::Stop;
    return command;
  }
  command.kind = control::MediaControlCommandKind::Stop;
  return command;
}

control::PlayerStateSnapshot playerSnapshot(TerminalState& state) {
  std::scoped_lock lock{state.mutex};
  return state.player;
}

void printControls(std::ostream& output) {
  output << "Controls: space/p play-pause, n/right next, b/left previous, s stop, +/- volume, m mute, q quit\n";
}

void updatePlayerState(TerminalState& state,
                       std::mutex& outputMutex,
                       std::ostream& output,
                       const control::PlayerStateSnapshot& snapshot) {
  {
    std::scoped_lock lock{state.mutex};
    state.player = snapshot;
  }
  renderStatus(state, outputMutex, output);
}

void updateLibraryState(TerminalState& state,
                        std::mutex& outputMutex,
                        std::ostream& output,
                        const control::LibraryStateSnapshot& snapshot) {
  {
    std::scoped_lock lock{state.mutex};
    state.library = snapshot;
  }
  renderStatus(state, outputMutex, output);
}

void updateNotification(TerminalState& state,
                        std::mutex& outputMutex,
                        std::ostream& output,
                        const control::ControlDomainNotification& notification) {
  {
    std::scoped_lock lock{state.mutex};
    state.notification = notification;
  }
  renderStatus(state, outputMutex, output);
}

void refreshPlayerState(TerminalState& state,
                        std::mutex& outputMutex,
                        std::ostream& output,
                        const control::MediaController& controller) {
  const auto snapshot = controller.playerStateSnapshot();
  {
    std::scoped_lock lock{state.mutex};
    if (state.player.freshness.version == snapshot.freshness.version) {
      return;
    }
    state.player = snapshot;
  }
  renderStatus(state, outputMutex, output);
}

int runTerminalControllerSession(const std::filesystem::path& musicPath,
                                 control::MediaController& controller,
                                 const TerminalActionReader& readAction,
                                 std::ostream& output,
                                 std::ostream& error,
                                 bool keyboardControlAvailable) {
  TerminalState state{};
  std::mutex outputMutex;
  auto playerSubscription = controller.subscribePlayerState([&](const control::PlayerStateSnapshot& snapshot) {
    updatePlayerState(state, outputMutex, output, snapshot);
  });
  auto librarySubscription = controller.subscribeLibraryState([&](const control::LibraryStateSnapshot& snapshot) {
    updateLibraryState(state, outputMutex, output, snapshot);
  });
  auto notificationSubscription = controller.subscribeDomainNotifications([&](const control::ControlDomainNotification& notification) {
    updateNotification(state, outputMutex, output, notification);
  });

  auto unsubscribeAll = [&] {
    playerSubscription.unsubscribe();
    librarySubscription.unsubscribe();
    notificationSubscription.unsubscribe();
  };

  try {
    controller.start();
  } catch (const std::exception& e) {
    spdlog::critical("media controller failed to start: {}", e.what());
    unsubscribeAll();
    return 1;
  }
  spdlog::info("media controller started");
  printControls(output);
  const auto scanResult = controller.scanLibrary({scanner::ScannerRoot{.path = musicPath, .recursive = true}}, scanner::ScanMode::Full);
  if (!scanResult.accepted) {
    spdlog::warn("library scan rejected: {}", scanResult.message);
    error << "\nseriona: failed to scan library: " << scanResult.message << '\n';
    spdlog::info("seriona shutting down");
    controller.shutdown();
    unsubscribeAll();
    return 1;
  }
  spdlog::info("library scan accepted: {}", scanResult.message);
  renderStatus(state, outputMutex, output);

  try {
    if (keyboardControlAvailable) {
      bool running = true;
      while (running) {
        const auto action = readAction(kIdleRefreshInterval);
        if (action == TerminalAction::None) {
          refreshPlayerState(state, outputMutex, output, controller);
          continue;
        }
        if (action == TerminalAction::Quit) {
          running = false;
          continue;
        }
        const auto command = commandForAction(action, playerSnapshot(state));
        if (command.kind == control::MediaControlCommandKind::TogglePlayPause ||
            command.kind == control::MediaControlCommandKind::SkipNext ||
            command.kind == control::MediaControlCommandKind::SkipPrevious ||
            command.kind == control::MediaControlCommandKind::Stop) {
          spdlog::info("user command submitted: kind={}", static_cast<int>(command.kind));
        }
        const auto result = controller.submitCommand(command);
        if (!result.accepted && !result.message.empty()) {
          std::scoped_lock outputLock{outputMutex};
          output << "\nseriona: command rejected: " << result.message << '\n';
        }
      }
    } else {
      error << "\nseriona: terminal keyboard control is not implemented on this platform\n";
    }
  } catch (const std::exception& e) {
    spdlog::critical("unrecoverable runtime failure: {}", e.what());
  } catch (...) {
    spdlog::critical("unrecoverable unknown runtime failure");
  }

  control::MediaControlCommand stopCommand{};
  stopCommand.kind = control::MediaControlCommandKind::Stop;
  static_cast<void>(controller.submitCommand(stopCommand));
  controller.shutdown();
  spdlog::info("seriona shutting down");
  unsubscribeAll();
  output << "\nseriona: stopped\n";
  return 0;
}

}

#ifdef SERIONA_TERMINAL_CONTROLLER_TESTING
namespace testing {

int runTerminalControllerForTest(const std::filesystem::path& musicPath,
                                 control::MediaController& controller,
                                 const TerminalActionReader& readAction,
                                 std::ostream& output,
                                 std::ostream& error) {
  return runTerminalControllerSession(musicPath, controller, readAction, output, error, true);
}

}
#else
int runTerminalController(const std::filesystem::path& musicPath) {
  TerminalMode terminalMode;
  if (!terminalMode.enabled()) {
    std::cerr << "seriona: interactive terminal input is required\n";
    return 1;
  }

  const auto runtimePaths = resolveRuntimePaths({});
  runtimePaths.ensureDirectoriesExist();

  std::cerr << "seriona: data root: " << runtimePaths.dataRoot.string() << '\n';

#ifdef SERIONA_RELEASE_BUILD
  av_log_set_level(AV_LOG_QUIET);
#endif

  const auto timestampedLogPath = seriona::logging::prepareLogFile(runtimePaths.dataRoot / "logs");

  try {
    seriona::logging::initialize(spdlog::level::off, timestampedLogPath.string()
#ifdef SERIONA_RELEASE_BUILD
                                 , spdlog::level::info
#endif
    );
  } catch (const std::exception& e) {
    std::cerr << "seriona: logging initialization failed: " << e.what() << '\n';
  }

  spdlog::info("seriona starting");
  spdlog::info("  executable root: {}", runtimePaths.dataRoot.string());
  spdlog::info("  log file:        {}", timestampedLogPath.string());
  spdlog::info("  database:        {}", runtimePaths.databasePath.string());
  spdlog::info("  artwork dir:     {}", runtimePaths.artworkDir.string());
  spdlog::info("  music scan root: {}", musicPath.string());

  std::unique_ptr<control::MediaController> controller;
  try {
    controller = control::makeProductionMediaController(control::MediaControllerOptions{},
                                                         runtimePaths.databasePath,
                                                         runtimePaths.artworkDir);
  } catch (const std::exception& e) {
    spdlog::critical("failed to create media controller: {}", e.what());
    spdlog::shutdown();
    return 1;
  }
#if defined(__unix__) || defined(__APPLE__)
  constexpr bool keyboardControlAvailable = true;
#else
  constexpr bool keyboardControlAvailable = false;
#endif
  const auto exitCode = runTerminalControllerSession(
      musicPath,
      *controller,
      [](std::chrono::milliseconds timeout) { return readTerminalAction(timeout); },
      std::cout,
      std::cerr,
      keyboardControlAvailable);
  controller.reset();
  spdlog::shutdown();
  return exitCode;
}
#endif

}
