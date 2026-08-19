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
                               logFile.string(),
                               loggerLevel);
  spdlog::info("seriona application logging initialized");
  spdlog::info("  data root:   {}", runtimePaths.dataRoot.string());
  spdlog::info("  log file:    {}", logFile.string());
  spdlog::info("  database:    {}", runtimePaths.databasePath.string());
  spdlog::info("  artwork dir: {}", runtimePaths.artworkDir.string());
  spdlog::default_logger()->flush();
}

void setLogLevel(spdlog::level::level_enum level) {
  seriona::logging::setLogLevel(level);
}

}
