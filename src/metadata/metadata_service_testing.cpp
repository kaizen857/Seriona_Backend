#include "metadata_service_testing.h"

namespace seriona::metadata {

std::shared_ptr<MetadataServiceTestHooks> makeMetadataServiceTestHooks() {
  return std::make_shared<MetadataServiceTestHooks>();
}

}
