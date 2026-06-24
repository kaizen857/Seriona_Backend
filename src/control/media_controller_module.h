#pragma once

#include "seriona/control/control_contracts.h"

#include <filesystem>

namespace seriona::control {

[[nodiscard]] MediaControllerDependencies makeDefaultMediaControllerDependencies();
[[nodiscard]] MediaControllerDependencies makeProductionMediaControllerDependencies(
    std::filesystem::path databasePath = {},
    std::filesystem::path coverExportDir = {});
void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies);

}
