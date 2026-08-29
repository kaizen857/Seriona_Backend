#pragma once

#include <filesystem>
#include <string>

namespace seriona::app {

struct RuntimePaths {
  std::filesystem::path dataRoot;
  std::filesystem::path logFile;
  std::filesystem::path databasePath;
  std::filesystem::path artworkDir;

  void ensureDirectoriesExist() const;
};

// Resolves portable runtime paths relative to the executable directory.
// Uses /proc/self/exe on Linux; falls back to executablePath if absolute,
// then to std::filesystem::current_path().
[[nodiscard]] RuntimePaths resolvePortableRuntimePaths(const std::filesystem::path& executablePath);

// Resolves installed-mode runtime paths following the XDG Base Directory
// spec ($XDG_DATA_HOME / $XDG_STATE_HOME / $XDG_CACHE_HOME, falling back to
// $HOME-based defaults; relative XDG paths are ignored per spec). Always
// compiled so tests can exercise it directly with environment injection;
// production mode selection happens inside resolveRuntimePaths().
[[nodiscard]] RuntimePaths resolveInstalledRuntimePaths();

// Resolves runtime paths for the current build mode:
//   - SERIONA_INSTALLED_MODE defined  -> resolveInstalledRuntimePaths() (XDG)
//   - otherwise                        -> resolvePortableRuntimePaths() (exeDir/SerionaData)
[[nodiscard]] RuntimePaths resolveRuntimePaths(const std::filesystem::path& executablePath);

}  // namespace seriona::app
