#include <wtr/watcher.hpp>

#include <windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace fs = std::filesystem;

constexpr std::chrono::seconds kEventTimeout{10};
constexpr std::chrono::seconds kHandshakeTimeout{2};
constexpr std::chrono::milliseconds kRearmWindow{100};

void phase(const char* marker) {
  std::cout << marker << '\n' << std::flush;
}

class ProbeEvents {
public:
  ProbeEvents() = default;

  explicit ProbeEvents(fs::path childPath)
      : childPath_{std::move(childPath)} {}

  void expectHandshake(fs::path handshakePath) {
    std::scoped_lock lock{mutex_};
    handshakePath_ = std::move(handshakePath);
    handshakeSeen_ = false;
  }

  void observe(const wtr::watcher::event& event) {
    std::unique_lock lock{mutex_};
    const auto pathName = event.path_name;
    const auto effectType = event.effect_type;
    const auto pathType = event.path_type;

    liveSeen_ = liveSeen_ || (effectType == wtr::watcher::event::effect_type::create
                              && pathType == wtr::watcher::event::path_type::watcher);
    destroyedSeen_ = destroyedSeen_ || (effectType == wtr::watcher::event::effect_type::destroy
                                        && pathType == wtr::watcher::event::path_type::watcher);
    handshakeSeen_ = handshakeSeen_ || (!handshakePath_.empty() && pathName == handshakePath_);
    childSeen_ = childSeen_ || (!childPath_.empty() && pathName == childPath_);

    lock.unlock();
    changed_.notify_all();
  }

  [[nodiscard]] bool waitForLive() {
    return waitUntil(liveSeen_, std::chrono::steady_clock::now() + kEventTimeout);
  }

  [[nodiscard]] bool waitForDestroyed() {
    return waitUntil(destroyedSeen_, std::chrono::steady_clock::now() + kEventTimeout);
  }

  [[nodiscard]] bool waitForHandshake() {
    return waitUntil(handshakeSeen_, std::chrono::steady_clock::now() + kHandshakeTimeout);
  }

  [[nodiscard]] bool waitForChild() {
    return waitUntil(childSeen_, std::chrono::steady_clock::now() + kEventTimeout);
  }

  void waitForRearmWindow() {
    const auto deadline = std::chrono::steady_clock::now() + kRearmWindow;
    std::unique_lock lock{mutex_};
    (void)changed_.wait_until(lock, deadline, [deadline] { return std::chrono::steady_clock::now() >= deadline; });
  }

private:
  [[nodiscard]] bool waitUntil(const bool& observed, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock{mutex_};
    return changed_.wait_until(lock, deadline, [&observed] { return observed; });
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  fs::path handshakePath_;
  fs::path childPath_;
  bool liveSeen_{false};
  bool destroyedSeen_{false};
  bool handshakeSeen_{false};
  bool childSeen_{false};
};

void prepareRoot(const fs::path& root) {
  (void)fs::remove_all(root);
  (void)fs::create_directories(root);
}

void cleanupRoot(const fs::path& root) {
  (void)fs::remove_all(root);
}

[[nodiscard]] int closeAndVerify(wtr::watch& watcher, ProbeEvents& events) {
  phase("phase: watcher-close-requested");
  const bool closeSucceeded = watcher.close();
  phase("phase: watcher-close-returned");

  const bool destroyed = events.waitForDestroyed();
  if (!destroyed) {
    phase("phase: watcher-destroy-timeout");
    return 4;
  }
  if (!closeSucceeded) {
    phase("phase: watcher-close-failed");
    return 5;
  }

  phase("phase: watcher-destroyed");
  return 0;
}

[[nodiscard]] int runRootLifecycle(const fs::path& root) {
  ProbeEvents events;
  int result = 0;

  {
    wtr::watch watcher{root, [&events](const wtr::watcher::event& event) { events.observe(event); }};
    phase("phase: watcher-constructed");

    if (!events.waitForLive()) {
      phase("phase: watcher-live-timeout");
      result = 2;
    } else {
      phase("phase: watcher-live");
    }

    const int closeResult = closeAndVerify(watcher, events);
    if (result == 0) {
      result = closeResult;
    }
  }

  // This only prints after the destructor's internal second close has returned.
  phase("phase: watcher-scope-destroyed");
  return result;
}

[[nodiscard]] int runUnicodeChildLifecycle(const fs::path& root) {
  const fs::path childPath = root / fs::path{L"child-\U00020BB7"};
  const std::array handshakePaths{
      root / "watcher-ready-a",
      root / "watcher-ready-b",
      root / "watcher-ready-c",
  };
  ProbeEvents events{childPath};
  int result = 0;

  {
    wtr::watch watcher{root, [&events](const wtr::watcher::event& event) { events.observe(event); }};
    phase("phase: watcher-constructed");

    if (!events.waitForLive()) {
      phase("phase: watcher-live-timeout");
      result = 2;
    } else {
      phase("phase: watcher-live");
      bool handshakeSeen = false;
      for (const fs::path& handshakePath : handshakePaths) {
        events.expectHandshake(handshakePath);
        if (!fs::create_directory(handshakePath)) {
          phase("phase: handshake-create-failed");
          result = 6;
          break;
        }
        if (events.waitForHandshake()) {
          handshakeSeen = true;
          break;
        }
      }
      if (!handshakeSeen && result == 0) {
        phase("phase: handshake-timeout");
        result = 7;
      }
      if (handshakeSeen) {
        phase("phase: watcher-handshake");
        events.waitForRearmWindow();
        phase("phase: watcher-rearmed");
        std::ofstream childFile{childPath};
        if (!childFile) {
          phase("phase: unicode-child-create-failed");
          result = 8;
        } else {
          childFile.close();
        }
        if (result == 0 && !events.waitForChild()) {
          phase("phase: unicode-child-timeout");
          result = 9;
        } else if (result == 0) {
          phase("phase: unicode-child-observed");
        }
      }
    }

    const int closeResult = closeAndVerify(watcher, events);
    if (result == 0) {
      result = closeResult;
    }
  }

  // This only prints after the destructor's internal second close has returned.
  phase("phase: watcher-scope-destroyed");
  return result;
}

[[nodiscard]] int runRootScenario(const fs::path& root) {
  prepareRoot(root);
  const int result = runRootLifecycle(root);
  cleanupRoot(root);
  phase("phase: cleanup-complete");
  return result;
}

[[nodiscard]] int runUnicodeChildScenario(const fs::path& root) {
  prepareRoot(root);
  const int result = runUnicodeChildLifecycle(root);
  cleanupRoot(root);
  phase("phase: cleanup-complete");
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  const UINT previousErrorMode = SetErrorMode(0);
  (void)SetErrorMode(previousErrorMode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

  if (argc != 3 || std::string_view{argv[1]} != "--mode") {
    phase("phase: invalid-arguments");
    return 64;
  }

  const fs::path baseRoot = fs::temp_directory_path()
                            / "seriona-wtr-unicode-path-probe"
                            / fs::path{std::to_wstring(GetCurrentProcessId())};
  const std::string_view mode{argv[2]};

  if (mode == "ascii-root") {
    phase("phase: mode-ascii-root");
    return runRootScenario(baseRoot / "ascii-root");
  }
  if (mode == "unicode-root") {
    phase("phase: mode-unicode-root");
    return runRootScenario(baseRoot / fs::path{L"unicode-root-\U00020BB7"});
  }
  if (mode == "unicode-child") {
    phase("phase: mode-unicode-child");
    return runUnicodeChildScenario(baseRoot / "unicode-child-root");
  }

  phase("phase: invalid-mode");
  return 64;
}
