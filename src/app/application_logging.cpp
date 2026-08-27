#include "seriona/app/application_logging.h"

#include "logging/logging.h"

#include <spdlog/spdlog.h>

namespace seriona::app {

void initializeApplicationLogging(const RuntimePaths& runtimePaths) {
  runtimePaths.ensureDirectoriesExist();

#ifdef SERIONA_RELEASE_BUILD
  constexpr auto loggerLevel = spdlog::level::info;
#else
  constexpr auto loggerLevel = spdlog::level::trace;
#endif

  const auto logFile = seriona::logging::prepareLogFile(runtimePaths.logFile.parent_path());
  seriona::logging::initialize(loggerLevel,
                               seriona::logging::pathText(logFile),
                               loggerLevel);
  spdlog::info("seriona application logging initialized");
  spdlog::info("  data root:   {}", seriona::logging::pathText(runtimePaths.dataRoot));
  spdlog::info("  log file:    {}", seriona::logging::pathText(logFile));
  spdlog::info("  database:    {}", seriona::logging::pathText(runtimePaths.databasePath));
  spdlog::info("  artwork dir: {}", seriona::logging::pathText(runtimePaths.artworkDir));
  spdlog::default_logger()->flush();
}

void setLogLevel(spdlog::level::level_enum level) {
  seriona::logging::setLogLevel(level);
}

}
