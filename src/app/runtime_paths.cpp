#include "seriona/app/runtime_paths.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string_view>

#ifdef __linux__
#include <pwd.h>
#include <unistd.h>
#endif

namespace seriona::app {
namespace {

constexpr std::string_view kInstalledAppId = "org.kaizen857.Seriona";

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

std::filesystem::path xdgBase(const char* envName, const char* fallbackSuffix) {
  if (const char* value = std::getenv(envName); value != nullptr && *value != '\0') {
    std::filesystem::path candidate{value};
    if (candidate.is_absolute()) {
      return candidate;
    }
  }

  std::filesystem::path homeBase;
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    homeBase = std::filesystem::path{home};
  } else {
#ifdef __linux__
    if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
      homeBase = std::filesystem::path{pw->pw_dir};
    }
#endif
    if (homeBase.empty()) {
      homeBase = std::filesystem::current_path();
    }
  }
  return homeBase / fallbackSuffix;
}

}  // namespace

void RuntimePaths::ensureDirectoriesExist() const {
  std::filesystem::create_directories(dataRoot);
  std::filesystem::create_directories(logFile.parent_path());
  std::filesystem::create_directories(artworkDir);
}

RuntimePaths resolvePortableRuntimePaths(const std::filesystem::path& executablePath) {
  const auto exeDir = resolveExecutableDir(executablePath);
  const std::filesystem::path dataRoot = exeDir / "SerionaData";

  return {
      dataRoot,
      dataRoot / "logs" / "seriona.log",
      dataRoot / "library.sqlite",
      dataRoot / "artwork",
  };
}

RuntimePaths resolveInstalledRuntimePaths() {
  const auto dataRoot = xdgBase("XDG_DATA_HOME", ".local/share") / kInstalledAppId;
  const auto stateRoot = xdgBase("XDG_STATE_HOME", ".local/state") / kInstalledAppId;
  const auto cacheRoot = xdgBase("XDG_CACHE_HOME", ".cache") / kInstalledAppId;

  return {
      dataRoot,
      stateRoot / "logs" / "seriona.log",
      dataRoot / "library.sqlite",
      cacheRoot / "artwork",
  };
}

RuntimePaths resolveRuntimePaths(const std::filesystem::path& executablePath) {
#ifdef SERIONA_INSTALLED_MODE
  return resolveInstalledRuntimePaths();
#else
  return resolvePortableRuntimePaths(executablePath);
#endif
}

}  // namespace seriona::app
