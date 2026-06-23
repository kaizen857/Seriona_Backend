#include "terminal_io.h"

#include <chrono>
#include <optional>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/select.h>
#include <unistd.h>
#endif

namespace seriona::app {
namespace {

TerminalAction actionFromByte(unsigned char value) noexcept {
  switch (value) {
  case ' ':
  case 'p':
  case 'P':
    return TerminalAction::TogglePlayPause;
  case 'n':
  case 'N':
    return TerminalAction::SkipNext;
  case 'b':
  case 'B':
    return TerminalAction::SkipPrevious;
  case 's':
  case 'S':
    return TerminalAction::Stop;
  case '+':
  case '=':
    return TerminalAction::VolumeUp;
  case '-':
  case '_':
    return TerminalAction::VolumeDown;
  case 'm':
  case 'M':
    return TerminalAction::ToggleMuted;
  case 'q':
  case 'Q':
    return TerminalAction::Quit;
  default:
    return TerminalAction::None;
  }
}

#if defined(__unix__) || defined(__APPLE__)
std::optional<unsigned char> readByteWithTimeout(std::chrono::milliseconds timeout) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(STDIN_FILENO, &set);
  timeval wait{timeout.count() / 1000, static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
  const auto ready = ::select(STDIN_FILENO + 1, &set, nullptr, nullptr, &wait);
  if (ready <= 0) {
    return std::nullopt;
  }
  unsigned char value = 0;
  return ::read(STDIN_FILENO, &value, 1) == 1 ? std::optional<unsigned char>{value} : std::nullopt;
}
#endif

}

TerminalMode::TerminalMode() {
#if defined(__unix__) || defined(__APPLE__)
  if (!::isatty(STDIN_FILENO) || ::tcgetattr(STDIN_FILENO, &original_) != 0) {
    return;
  }
  auto raw = original_;
  raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  enabled_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
#endif
}

TerminalMode::~TerminalMode() {
#if defined(__unix__) || defined(__APPLE__)
  if (enabled_) {
    static_cast<void>(::tcsetattr(STDIN_FILENO, TCSANOW, &original_));
  }
#endif
}

bool TerminalMode::enabled() const noexcept { return enabled_; }

TerminalAction readTerminalAction(std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
  const auto first = readByteWithTimeout(timeout);
  if (!first.has_value()) {
    return TerminalAction::None;
  }
  if (*first != '\x1B') {
    return actionFromByte(*first);
  }
  const auto bracket = readByteWithTimeout(std::chrono::milliseconds{20});
  const auto code = readByteWithTimeout(std::chrono::milliseconds{20});
  if (!bracket.has_value() || !code.has_value() || *bracket != '[') {
    return TerminalAction::None;
  }
  return *code == 'C' ? TerminalAction::SkipNext : (*code == 'D' ? TerminalAction::SkipPrevious : TerminalAction::None);
#else
  return TerminalAction::None;
#endif
}

}
