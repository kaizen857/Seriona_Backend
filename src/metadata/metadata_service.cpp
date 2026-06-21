#include "seriona/metadata/metadata_contracts.h"

#include "metadata_synchronizer.h"

namespace seriona::metadata {

MetadataSyncResult metadataServiceDefaultResult() {
  return MetadataSyncResult{};
}

MetadataSyncPlan metadataServiceSynchronize(const control::PlayerStateSnapshot& snapshot) {
  static MetadataSynchronizer synchronizer{};
  return synchronizer.synchronize(snapshot);
}

}
