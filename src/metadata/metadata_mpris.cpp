#include "seriona/metadata/metadata_contracts.h"

#include "spdlog/spdlog.h"

#include <optional>

namespace seriona::metadata {

MetadataSyncResult metadataMprisSmokeResult() {
  spdlog::debug("metadata mpris smoke: noop backend active");
  return MetadataSyncResult{.accepted = true, .changed = false, .state = {}, .errorCode = std::nullopt, .message = {}};
}

}
