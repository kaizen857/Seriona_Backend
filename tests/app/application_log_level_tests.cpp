// T5【后端】日志等级 API：setLogLevel 运行时设置日志等级。
// 断言 spdlog::should_log 随等级变化（debug→error 各档）、named logger 同步、
// sink 级别同步与无效枚举值拒绝。
#include "seriona/app/application_logging.h"
#include "seriona/app/runtime_paths.h"

#include <doctest/doctest.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

std::filesystem::path uniqueTestRoot() {
  const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("seriona_application_log_level_test_" + std::to_string(uniqueSuffix));
}

seriona::app::RuntimePaths makeRuntimePaths(const std::filesystem::path& root) {
  return seriona::app::resolveRuntimePaths(root / "bin" / "appSeriona");
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

// dup2 重定向 stdout 到文件，用于端到端验证控制台 sink 的实际可见性。
class StdoutCapture {
public:
  explicit StdoutCapture(const std::filesystem::path& outputPath)
      : savedStdout_(::dup(STDOUT_FILENO)),
        captureFd_(::open(outputPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR)) {
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

}  // namespace

TEST_CASE("setLogLevel toggles should_log from debug through error") {
  const auto root = uniqueTestRoot();
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  seriona::app::initializeApplicationLogging(makeRuntimePaths(root));

  // debug：debug 及更高级别全部放行
  seriona::app::setLogLevel(spdlog::level::debug);
  CHECK(spdlog::should_log(spdlog::level::debug));
  CHECK(spdlog::should_log(spdlog::level::info));
  CHECK(spdlog::should_log(spdlog::level::warn));
  CHECK(spdlog::should_log(spdlog::level::err));
  CHECK(spdlog::should_log(spdlog::level::critical));

  // named logger（initialize 注册的 "seriona"，同时是默认 logger）同步生效
  const auto named = spdlog::get("seriona");
  REQUIRE(named != nullptr);
  CHECK(named->should_log(spdlog::level::debug));

  // info：debug 被过滤
  seriona::app::setLogLevel(spdlog::level::info);
  CHECK_FALSE(spdlog::should_log(spdlog::level::debug));
  CHECK(spdlog::should_log(spdlog::level::info));
  CHECK(spdlog::should_log(spdlog::level::warn));

  // warn：info 被过滤
  seriona::app::setLogLevel(spdlog::level::warn);
  CHECK_FALSE(spdlog::should_log(spdlog::level::info));
  CHECK(spdlog::should_log(spdlog::level::warn));
  CHECK(spdlog::should_log(spdlog::level::err));

  // err：warn 被过滤
  seriona::app::setLogLevel(spdlog::level::err);
  CHECK_FALSE(spdlog::should_log(spdlog::level::warn));
  CHECK(spdlog::should_log(spdlog::level::err));
  CHECK(spdlog::should_log(spdlog::level::critical));

  // off：全部关闭
  seriona::app::setLogLevel(spdlog::level::off);
  CHECK_FALSE(spdlog::should_log(spdlog::level::critical));
  CHECK_FALSE(named->should_log(spdlog::level::critical));

  spdlog::shutdown();
  std::filesystem::remove_all(root);
}

TEST_CASE("setLogLevel rejects invalid level values") {
  const auto root = uniqueTestRoot();
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  seriona::app::initializeApplicationLogging(makeRuntimePaths(root));

  seriona::app::setLogLevel(spdlog::level::err);
  REQUIRE(spdlog::should_log(spdlog::level::err));
  REQUIRE_FALSE(spdlog::should_log(spdlog::level::warn));

  // 上界越界值被拒绝，级别保持不变
  seriona::app::setLogLevel(static_cast<spdlog::level::level_enum>(999));
  CHECK(spdlog::should_log(spdlog::level::err));
  CHECK_FALSE(spdlog::should_log(spdlog::level::warn));

  // 下界越界值同样被拒绝
  seriona::app::setLogLevel(static_cast<spdlog::level::level_enum>(-1));
  CHECK(spdlog::should_log(spdlog::level::err));
  CHECK_FALSE(spdlog::should_log(spdlog::level::warn));

  spdlog::shutdown();
  std::filesystem::remove_all(root);
}

TEST_CASE("setLogLevel syncs sinks and makes debug visible on console") {
  const auto root = uniqueTestRoot();
  const auto stdoutPath = root / "stdout.log";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  seriona::app::initializeApplicationLogging(makeRuntimePaths(root));

  constexpr const char* filteredMarker = "setLogLevel filtered debug marker";
  constexpr const char* visibleMarker = "setLogLevel visible debug marker";

  // 先收紧到 err：debug 在 logger 级与 sink 级都被过滤
  seriona::app::setLogLevel(spdlog::level::err);
  {
    StdoutCapture capture(stdoutPath);
    REQUIRE(capture.active());
    spdlog::debug(filteredMarker);
    spdlog::default_logger()->flush();
    capture.restore();
  }
  CHECK(readFile(stdoutPath).find(filteredMarker) == std::string::npos);

  // 再放开到 debug：控制台 sink 同步后 debug 实际可见
  seriona::app::setLogLevel(spdlog::level::debug);
  {
    StdoutCapture capture(stdoutPath);
    REQUIRE(capture.active());
    spdlog::debug(visibleMarker);
    spdlog::default_logger()->flush();
    capture.restore();
  }
  CHECK(readFile(stdoutPath).find(visibleMarker) != std::string::npos);

  // 默认 logger 的全部 sink（控制台 + 文件）级别跟随新等级
  const auto logger = spdlog::default_logger();
  REQUIRE(logger != nullptr);
  REQUIRE_FALSE(logger->sinks().empty());
  for (const auto& sink : logger->sinks()) {
    CHECK(sink->level() == spdlog::level::debug);
  }

  spdlog::shutdown();
  std::filesystem::remove_all(root);
}
