#include "seriona/metadata/metadata_contracts.h"

#include <optional>

#if defined(__linux__) && !defined(__APPLE__)
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace seriona::metadata {

MetadataSyncResult metadataMprisSmokeResult() {
  return MetadataSyncResult{.accepted = true, .changed = false, .state = {}, .errorCode = std::nullopt, .message = {}};
}

}
