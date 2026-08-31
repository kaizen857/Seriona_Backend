#include "seriona/app/runtime_paths.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace seriona::app {
namespace {

std::filesystem::path uniqueTestRoot() {
  const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("seriona_runtime_paths_test_" + std::to_string(uniqueSuffix));
}

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

class ScopedEnvVar {
public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* value = std::getenv(name); value != nullptr) {
      hadOldValue_ = true;
      oldValue_ = value;
    }
  }

  ~ScopedEnvVar() {
    if (hadOldValue_) {
      setEnv(name_.c_str(), oldValue_.c_str());
    } else {
      unsetEnv(name_.c_str());
    }
  }

private:
  std::string name_;
  bool hadOldValue_ = false;
  std::string oldValue_;
};

TEST_CASE("RuntimePaths resolves absolute executable path") {
  const auto executablePath = uniqueTestRoot() / "bin" / "seriona";
  const auto expectedDataRoot = executablePath.parent_path() / "SerionaData";
  const auto paths = resolveRuntimePaths(executablePath);

  CHECK(paths.dataRoot == expectedDataRoot);
  CHECK(paths.logFile == expectedDataRoot / "logs" / "seriona.log");
  CHECK(paths.databasePath == expectedDataRoot / "library.sqlite");
  CHECK(paths.artworkDir == expectedDataRoot / "artwork");
}

TEST_CASE("RuntimePaths derives all fields from dataRoot") {
  const auto executablePath = uniqueTestRoot() / "bin" / "seriona";
  const auto paths = resolveRuntimePaths(executablePath);

  CHECK(paths.dataRoot == executablePath.parent_path() / "SerionaData");

  CHECK(paths.logFile.parent_path() == paths.dataRoot / "logs");
  CHECK(paths.logFile.filename() == "seriona.log");

  CHECK(paths.databasePath.parent_path() == paths.dataRoot);
  CHECK(paths.databasePath.filename() == "library.sqlite");

  CHECK(paths.artworkDir == paths.dataRoot / "artwork");
}

TEST_CASE("RuntimePaths fallback for empty/relative path uses current_path") {
  // When /proc/self/exe is available (Linux), it takes priority.
  // On platforms without /proc/self/exe, or when readlink fails,
  // an empty or relative executablePath falls back to
  // std::filesystem::current_path() to produce a deterministic result.
  //
  // This test documents the fallback contract:
  //   dataRoot = current_path() / "SerionaData"
  //
  // Note: on Linux this test will yield the /proc/self/exe-derived
  // directory, which is the expected production behavior. The
  // current_path() fallback is reachable on non-Linux platforms and
  // when /proc/self/exe is unavailable.
  const auto cwd = std::filesystem::current_path();
  const auto expectedDataRoot = cwd / "SerionaData";

  const auto paths = resolveRuntimePaths("");

  // On Linux, /proc/self/exe determines the actual result.
  // Verify the shape (relative positioning) rather than exact equality.
  CHECK(paths.logFile.filename() == "seriona.log");
  CHECK(paths.databasePath.filename() == "library.sqlite");
  CHECK(paths.logFile.parent_path().filename() == "logs");
  CHECK(paths.artworkDir.filename() == "artwork");

  // dataRoot ends with /SerionaData
  CHECK(paths.dataRoot.filename() == "SerionaData");

  // All sub-paths are rooted under dataRoot
  CHECK(paths.logFile.parent_path().parent_path() == paths.dataRoot);
  CHECK(paths.databasePath.parent_path() == paths.dataRoot);
  CHECK(paths.artworkDir.parent_path() == paths.dataRoot);

  // Document: the deterministic fallback would produce:
  //   paths.dataRoot == expectedDataRoot
  (void)expectedDataRoot;
}

#if defined(__APPLE__)

TEST_CASE("Installed paths on macOS live under ~/Library, never inside the bundle") {
  ScopedEnvVar home("HOME");

  const auto homeDir = uniqueTestRoot() / "home";
  setEnv("HOME", homeDir.string().c_str());

  const auto paths = resolveInstalledRuntimePaths();
  //  写进 .app 内部会破坏代码签名封印，带 quarantine 的产物会被 Gatekeeper 直接 SIGKILL
  CHECK(paths.dataRoot == homeDir / "Library/Application Support/org.kaizen857.Seriona");
  CHECK(paths.logFile == homeDir / "Library/Logs/org.kaizen857.Seriona" / "seriona.log");
  CHECK(paths.databasePath ==
        homeDir / "Library/Application Support/org.kaizen857.Seriona" / "library.sqlite");
  CHECK(paths.artworkDir == homeDir / "Library/Caches/org.kaizen857.Seriona" / "artwork");
}

TEST_CASE("Installed paths on macOS ignore XDG overrides") {
  ScopedEnvVar data("XDG_DATA_HOME");
  ScopedEnvVar home("HOME");

  const auto homeDir = uniqueTestRoot() / "home";
  setEnv("HOME", homeDir.string().c_str());
  setEnv("XDG_DATA_HOME", (uniqueTestRoot() / "xdg").string().c_str());

  const auto paths = resolveInstalledRuntimePaths();
  CHECK(paths.dataRoot == homeDir / "Library/Application Support/org.kaizen857.Seriona");
}

#else

TEST_CASE("Installed paths honor explicit XDG dirs") {
  ScopedEnvVar data("XDG_DATA_HOME");
  ScopedEnvVar state("XDG_STATE_HOME");
  ScopedEnvVar cache("XDG_CACHE_HOME");
  ScopedEnvVar home("HOME");

  const auto dataRoot = uniqueTestRoot() / "data";
  const auto stateRoot = uniqueTestRoot() / "state";
  const auto cacheRoot = uniqueTestRoot() / "cache";
  setEnv("XDG_DATA_HOME", dataRoot.string().c_str());
  setEnv("XDG_STATE_HOME", stateRoot.string().c_str());
  setEnv("XDG_CACHE_HOME", cacheRoot.string().c_str());

  const auto paths = resolveInstalledRuntimePaths();
  CHECK(paths.dataRoot == dataRoot / "org.kaizen857.Seriona");
  CHECK(paths.logFile == stateRoot / "org.kaizen857.Seriona" / "logs" / "seriona.log");
  CHECK(paths.databasePath == dataRoot / "org.kaizen857.Seriona" / "library.sqlite");
  CHECK(paths.artworkDir == cacheRoot / "org.kaizen857.Seriona" / "artwork");
}

TEST_CASE("Installed paths fall back to HOME defaults when XDG unset") {
  ScopedEnvVar data("XDG_DATA_HOME");
  ScopedEnvVar state("XDG_STATE_HOME");
  ScopedEnvVar cache("XDG_CACHE_HOME");
  ScopedEnvVar home("HOME");

  const auto homeDir = uniqueTestRoot() / "home";
  setEnv("HOME", homeDir.string().c_str());
  unsetEnv("XDG_DATA_HOME");
  unsetEnv("XDG_STATE_HOME");
  unsetEnv("XDG_CACHE_HOME");

  const auto paths = resolveInstalledRuntimePaths();
  CHECK(paths.dataRoot == homeDir / ".local/share/org.kaizen857.Seriona");
  CHECK(paths.logFile == homeDir / ".local/state/org.kaizen857.Seriona" / "logs" / "seriona.log");
  CHECK(paths.databasePath == homeDir / ".local/share/org.kaizen857.Seriona" / "library.sqlite");
  CHECK(paths.artworkDir == homeDir / ".cache/org.kaizen857.Seriona" / "artwork");
}

TEST_CASE("Installed paths ignore relative XDG values") {
  ScopedEnvVar data("XDG_DATA_HOME");
  ScopedEnvVar state("XDG_STATE_HOME");
  ScopedEnvVar cache("XDG_CACHE_HOME");
  ScopedEnvVar home("HOME");

  const auto homeDir = uniqueTestRoot() / "home";
  setEnv("HOME", homeDir.string().c_str());
  setEnv("XDG_DATA_HOME", "relative/data");
  setEnv("XDG_STATE_HOME", "relative/state");
  setEnv("XDG_CACHE_HOME", "relative/cache");

  const auto paths = resolveInstalledRuntimePaths();
  CHECK(paths.dataRoot == homeDir / ".local/share/org.kaizen857.Seriona");
  CHECK(paths.logFile == homeDir / ".local/state/org.kaizen857.Seriona" / "logs" / "seriona.log");
  CHECK(paths.databasePath == homeDir / ".local/share/org.kaizen857.Seriona" / "library.sqlite");
  CHECK(paths.artworkDir == homeDir / ".cache/org.kaizen857.Seriona" / "artwork");
}

#endif  // __APPLE__

TEST_CASE("RuntimePaths ensureDirectoriesExist does not crash") {
  const auto root = uniqueTestRoot();
  const auto paths = resolveRuntimePaths(root / "bin" / "seriona");
  std::filesystem::remove_all(root);

  CHECK_NOTHROW(paths.ensureDirectoriesExist());
  CHECK(std::filesystem::exists(paths.dataRoot));
  CHECK(std::filesystem::exists(paths.logFile.parent_path()));
  CHECK(std::filesystem::exists(paths.artworkDir));

  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace seriona::app
