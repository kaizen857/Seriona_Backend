#include "seriona/scanner/scanner_module.h"

#include <efsw/efsw.hpp>

namespace seriona::scanner {

bool scannerModuleLinked() noexcept {
  [[maybe_unused]] constexpr auto watchedAction = efsw::Actions::Modified;
  return true;
}

}
