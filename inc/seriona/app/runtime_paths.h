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
[[nodiscard]] RuntimePaths resolveRuntimePaths(const std::filesystem::path& executablePath);

}  // namespace seriona::app
