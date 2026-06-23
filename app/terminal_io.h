#pragma once

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#endif

#include <chrono>

namespace seriona::app {

enum class TerminalAction {
  None,
  TogglePlayPause,
  SkipNext,
  SkipPrevious,
  Stop,
  VolumeUp,
  VolumeDown,
  ToggleMuted,
  Quit,
};

class TerminalMode {
public:
  TerminalMode();
  ~TerminalMode();

  TerminalMode(const TerminalMode&) = delete;
  TerminalMode& operator=(const TerminalMode&) = delete;

  [[nodiscard]] bool enabled() const noexcept;

private:
#if defined(__unix__) || defined(__APPLE__)
  termios original_{};
#endif
  bool enabled_{false};
};

TerminalAction readTerminalAction(std::chrono::milliseconds timeout = std::chrono::hours{24});

}
