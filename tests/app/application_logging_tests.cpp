#include "seriona/app/application_logging.h"
#include "seriona/app/runtime_paths.h"

#include <doctest/doctest.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define open _open
#else
#include <unistd.h>
#endif

namespace {

std::filesystem::path uniqueTestRoot() {
  const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("seriona_application_logging_test_" + std::to_string(uniqueSuffix));
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string content;
  std::string line;
  while (std::getline(input, line)) {
    content += line;
    content += '\n';
  }
  return content;
}

class StdoutCapture {
public:
  explicit StdoutCapture(const std::filesystem::path& outputPath)
      : savedStdout_(::dup(STDOUT_FILENO)),
        captureFd_(::open(outputPath.string().c_str(), O_CREAT | O_TRUNC | O_WRONLY,
#if defined(_WIN32)
                          _S_IREAD | _S_IWRITE)) {
#else
                          S_IRUSR | S_IWUSR)) {
#endif
    std::fflush(stdout);
    if (savedStdout_ >= 0 && captureFd_ >= 0) {
      active_ = ::dup2(captureFd_, STDOUT_FILENO) >= 0;
    }
  }

  ~StdoutCapture() { restore(); }

  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  [[nodiscard]] bool active() const { return active_; }

  void restore() {
    if (active_) {
      std::fflush(stdout);
      static_cast<void>(::dup2(savedStdout_, STDOUT_FILENO));
      active_ = false;
    }
    if (captureFd_ >= 0) {
      static_cast<void>(::close(captureFd_));
      captureFd_ = -1;
    }
    if (savedStdout_ >= 0) {
      static_cast<void>(::close(savedStdout_));
      savedStdout_ = -1;
    }
  }

private:
  int savedStdout_ = -1;
  int captureFd_ = -1;
  bool active_ = false;
};

std::filesystem::path findApplicationLog(const std::filesystem::path& logDir) {
  const std::regex pattern(R"(seriona-\d{14}\.log)");
  std::filesystem::path logPath;
  for (const auto& entry : std::filesystem::directory_iterator(logDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (std::regex_match(entry.path().filename().string(), pattern)) {
      logPath = entry.path();
    }
  }
  return logPath;
}

}

TEST_CASE("application logging initializes a timestamped session log file") {
  constexpr const char* smokeMarker = "application dual sink smoke marker";
  const auto root = uniqueTestRoot();
  const auto executablePath = root / "bin" / "appSeriona";
  const auto runtimePaths = seriona::app::resolveRuntimePaths(executablePath);
  const auto stdoutPath = root / "stdout.log";

  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  StdoutCapture stdoutCapture(stdoutPath);
  REQUIRE(stdoutCapture.active());
  seriona::app::initializeApplicationLogging(runtimePaths);
  spdlog::info(smokeMarker);
  spdlog::default_logger()->flush();
  stdoutCapture.restore();

  const auto logFile = findApplicationLog(runtimePaths.logFile.parent_path());
  const bool logFileExists = !logFile.empty() && std::filesystem::exists(logFile);
  CHECK(logFileExists);

  const std::string content = logFileExists ? readFile(logFile) : std::string{};
  CHECK(content.find("seriona application logging initialized") != std::string::npos);
  CHECK(content.find(smokeMarker) != std::string::npos);
  CHECK(readFile(stdoutPath).find(smokeMarker) != std::string::npos);

  spdlog::shutdown();
  std::filesystem::remove_all(root);
}
