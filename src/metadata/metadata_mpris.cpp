#include "seriona/metadata/metadata_contracts.h"

#include <optional>

namespace seriona::metadata {

MetadataSyncResult metadataMprisSmokeResult() {
  return MetadataSyncResult{.accepted = true, .changed = false, .state = {}, .errorCode = std::nullopt, .message = {}};
}

}
