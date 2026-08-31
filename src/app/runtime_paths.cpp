#include "seriona/app/runtime_paths.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string_view>

#if defined(__linux__) || defined(__APPLE__)
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

std::filesystem::path homeDirectory() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path{home};
  }
#if defined(__linux__) || defined(__APPLE__)
  if (const auto* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) {
    return std::filesystem::path{pw->pw_dir};
  }
#endif
  return std::filesystem::current_path();
}

#if !defined(__APPLE__)
//  macOS 走 ~/Library，不使用 XDG；此处按平台裁掉，避免 -Wunused-function
std::filesystem::path xdgBase(const char* envName, const char* fallbackSuffix) {
  if (const char* value = std::getenv(envName); value != nullptr && *value != '\0') {
    std::filesystem::path candidate{value};
    if (candidate.is_absolute()) {
      return candidate;
    }
  }

  return homeDirectory() / fallbackSuffix;
}
#endif  // !__APPLE__

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
#if defined(__APPLE__)
  //  macOS 不用 XDG，走系统约定的 ~/Library。更要紧的是：.app bundle 绝不能
  //  被写入 —— 向 bundle 内写文件会破坏代码签名的封印，带 quarantine 的分发产物
  //  会直接被 Gatekeeper SIGKILL；/Applications 下通常也不可写。
  const auto home = homeDirectory();
  const auto dataRoot = home / "Library" / "Application Support" / kInstalledAppId;

  return {
      dataRoot,
      home / "Library" / "Logs" / kInstalledAppId / "seriona.log",
      dataRoot / "library.sqlite",
      home / "Library" / "Caches" / kInstalledAppId / "artwork",
  };
#else
  const auto dataRoot = xdgBase("XDG_DATA_HOME", ".local/share") / kInstalledAppId;
  const auto stateRoot = xdgBase("XDG_STATE_HOME", ".local/state") / kInstalledAppId;
  const auto cacheRoot = xdgBase("XDG_CACHE_HOME", ".cache") / kInstalledAppId;

  return {
      dataRoot,
      stateRoot / "logs" / "seriona.log",
      dataRoot / "library.sqlite",
      cacheRoot / "artwork",
  };
#endif
}

//  installed 模式下不看可执行文件位置，故参数可能未使用
RuntimePaths resolveRuntimePaths([[maybe_unused]] const std::filesystem::path& executablePath) {
#ifdef SERIONA_INSTALLED_MODE
  return resolveInstalledRuntimePaths();
#else
  return resolvePortableRuntimePaths(executablePath);
#endif
}

}  // namespace seriona::app
