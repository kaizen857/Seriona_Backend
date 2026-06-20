#include "seriona/scanner/scanner_module.h"

#include "wtr/watcher.hpp"

namespace seriona::scanner {

bool scannerModuleLinked() noexcept {
  [[maybe_unused]] constexpr auto watcherPathType = wtr::event::path_type::watcher;
  return true;
}

}
