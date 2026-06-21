#pragma once

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <filesystem>
#include <memory>

namespace seriona::scanner {

struct FileScannerServiceDependencies {
  std::shared_ptr<TagMetadataReader> metadataReader;
  std::filesystem::path databasePath;
  std::filesystem::path coverExportDir;
};

[[nodiscard]] std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies);

}
