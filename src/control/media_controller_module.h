#pragma once

#include "seriona/control/control_contracts.h"

namespace seriona::control {

[[nodiscard]] MediaControllerDependencies makeDefaultMediaControllerDependencies();
void normalizeMediaControllerDependencies(MediaControllerDependencies& dependencies);

}
