#include "terminal_controller.h"

#include "terminal_io.h"

#include "seriona/app/runtime_paths.h"
#include "seriona/control/media_controller.h"

#include "logging/logging.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace seriona::app {
namespace {

namespace control = seriona::control;
namespace scanner = seriona::scanner;
using namespace std::chrono_literals;

constexpr auto kIdleRefreshInterval = 100ms;

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

void renderStatus(TerminalState& state, std::mutex& outputMutex) {
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
  std::cout << "\r\033[K[" << playbackStatusName(player.playback.state) << "] " << currentTrackLabel(player) << " | "
            << player.timeline.position.count() << " ms | volume " << static_cast<int>(player.volume * 100.0F) << "%"
            << (player.muted ? " muted" : "") << " | scan " << scanStatusName(library.scanStatus);
  if (notification.has_value() && !notification->message.empty()) {
    std::cout << " | " << notification->message;
  }
  std::cout << std::flush;
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

void printControls() {
  std::cout << "Controls: space/p play-pause, n/right next, b/left previous, s stop, +/- volume, m mute, q quit\n";
}

void updatePlayerState(TerminalState& state, std::mutex& outputMutex, const control::PlayerStateSnapshot& snapshot) {
  {
    std::scoped_lock lock{state.mutex};
    state.player = snapshot;
  }
  renderStatus(state, outputMutex);
}

void updateLibraryState(TerminalState& state, std::mutex& outputMutex, const control::LibraryStateSnapshot& snapshot) {
  {
    std::scoped_lock lock{state.mutex};
    state.library = snapshot;
  }
  renderStatus(state, outputMutex);
}

void updateNotification(TerminalState& state, std::mutex& outputMutex, const control::ControlDomainNotification& notification) {
  {
    std::scoped_lock lock{state.mutex};
    state.notification = notification;
  }
  renderStatus(state, outputMutex);
}

void refreshPlayerState(TerminalState& state, std::mutex& outputMutex, const control::MediaController& controller) {
  const auto snapshot = controller.playerStateSnapshot();
  {
    std::scoped_lock lock{state.mutex};
    if (state.player.freshness.version == snapshot.freshness.version) {
      return;
    }
    state.player = snapshot;
  }
  renderStatus(state, outputMutex);
}

}

int runTerminalController(const std::filesystem::path& musicPath) {
  TerminalMode terminalMode;
  if (!terminalMode.enabled()) {
    std::cerr << "seriona: interactive terminal input is required\n";
    return 1;
  }

  const auto runtimePaths = resolveRuntimePaths({});
  runtimePaths.ensureDirectoriesExist();

  std::cerr << "seriona: data root: " << runtimePaths.dataRoot.string() << '\n';

  const auto timestampedLogPath = seriona::logging::prepareLogFile(runtimePaths.dataRoot / "logs");

  try {
    seriona::logging::initialize(spdlog::level::off, timestampedLogPath.string());
  } catch (const std::exception& e) {
    std::cerr << "seriona: logging initialization failed: " << e.what() << '\n';
  }

  spdlog::info("seriona starting");
  spdlog::info("  executable root: {}", runtimePaths.dataRoot.string());
  spdlog::info("  log file:        {}", timestampedLogPath.string());
  spdlog::info("  database:        {}", runtimePaths.databasePath.string());
  spdlog::info("  artwork dir:     {}", runtimePaths.artworkDir.string());
  spdlog::info("  music scan root: {}", musicPath.string());

  TerminalState state{};
  std::mutex outputMutex;
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
  auto playerSubscription = controller->subscribePlayerState([&](const control::PlayerStateSnapshot& snapshot) {
    updatePlayerState(state, outputMutex, snapshot);
  });
  auto librarySubscription = controller->subscribeLibraryState([&](const control::LibraryStateSnapshot& snapshot) {
    updateLibraryState(state, outputMutex, snapshot);
  });
  auto notificationSubscription = controller->subscribeDomainNotifications([&](const control::ControlDomainNotification& notification) {
    updateNotification(state, outputMutex, notification);
  });

  try {
    controller->start();
  } catch (const std::exception& e) {
    spdlog::critical("media controller failed to start: {}", e.what());
    spdlog::shutdown();
    return 1;
  }
  spdlog::info("media controller started");
  printControls();
  const auto scanResult = controller->scanLibrary({scanner::ScannerRoot{.path = musicPath, .recursive = true}}, scanner::ScanMode::Full);
  if (!scanResult.accepted) {
    spdlog::warn("library scan rejected: {}", scanResult.message);
    std::cerr << "\nseriona: failed to scan library: " << scanResult.message << '\n';
    spdlog::info("seriona shutting down");
    spdlog::shutdown();
    controller->shutdown();
    return 1;
  }
  spdlog::info("library scan accepted: {}", scanResult.message);
  renderStatus(state, outputMutex);

  try {
#if defined(__unix__) || defined(__APPLE__)
  bool running = true;
  while (running) {
    const auto action = readTerminalAction(kIdleRefreshInterval);
    if (action == TerminalAction::None) {
      refreshPlayerState(state, outputMutex, *controller);
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
    const auto result = controller->submitCommand(command);
    if (!result.accepted && !result.message.empty()) {
      std::scoped_lock outputLock{outputMutex};
      std::cout << "\nseriona: command rejected: " << result.message << '\n';
    }
  }
#else
  std::cerr << "\nseriona: terminal keyboard control is not implemented on this platform\n";
#endif
  } catch (const std::exception& e) {
    spdlog::critical("unrecoverable runtime failure: {}", e.what());
  } catch (...) {
    spdlog::critical("unrecoverable unknown runtime failure");
  }

  control::MediaControlCommand stopCommand{};
  stopCommand.kind = control::MediaControlCommandKind::Stop;
  static_cast<void>(controller->submitCommand(stopCommand));
  {
    controller->shutdown();
    spdlog::info("seriona shutting down");
    controller.reset();
  }
  spdlog::shutdown();
  playerSubscription.unsubscribe();
  librarySubscription.unsubscribe();
  notificationSubscription.unsubscribe();
  std::cout << "\nseriona: stopped\n";
  return 0;
}

}
