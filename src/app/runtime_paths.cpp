#include "seriona/app/runtime_paths.h"

#include <array>
#include <filesystem>
#include <string_view>

#ifdef __linux__
#include <unistd.h>
#endif

namespace seriona::app {
namespace {

std::filesystem::path resolveExecutableDir(const std::filesystem::path& executablePath) {
  if (!executablePath.empty() && executablePath.is_absolute()) {
    return executablePath.parent_path();
  }

#ifdef __linux__
  std::array<char, 4096> buffer{};
  const auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len > 0) {
    buffer[static_cast<std::size_t>(len)] = '\0';
    return std::filesystem::path{buffer.data()}.parent_path();
  }
#endif

  return std::filesystem::current_path();
}

}  // namespace

void RuntimePaths::ensureDirectoriesExist() const {
  std::filesystem::create_directories(dataRoot);
  std::filesystem::create_directories(logFile.parent_path());
  std::filesystem::create_directories(artworkDir);
}

RuntimePaths resolveRuntimePaths(const std::filesystem::path& executablePath) {
  const auto exeDir = resolveExecutableDir(executablePath);
  const std::filesystem::path dataRoot = exeDir / "SerionaData";

  return {
      dataRoot,
      dataRoot / "logs" / "seriona.log",
      dataRoot / "library.sqlite",
      dataRoot / "artwork",
  };
}

}  // namespace seriona::app
