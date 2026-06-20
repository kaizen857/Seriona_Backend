#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <memory>
#include <vector>

namespace seriona::scanner {

class FileScanner {
public:
  FileScanner();
  explicit FileScanner(std::shared_ptr<FileScannerService> service);

  void setScannerService(std::shared_ptr<FileScannerService> service);
  void setEventSink(ScannerEventSink sink);
  void configure(const ScannerConfig& config);
  void scan(const std::vector<ScannerRoot>& roots, ScanMode mode = ScanMode::Incremental);
  void stop();
  [[nodiscard]] PlaylistTreeSnapshot snapshot() const;

private:
  std::shared_ptr<FileScannerService> service_;
};

std::shared_ptr<FileScannerService> makeFileScannerService();

}
