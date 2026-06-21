#include "seriona/scanner/file_scanner_service.h"

#include "file_scanner_service_internal.h"

#include <utility>

namespace seriona::scanner {

FileScanner::FileScanner() : service_(makeFileScannerService()) {}

FileScanner::FileScanner(std::shared_ptr<FileScannerService> service) : service_(std::move(service)) {}

void FileScanner::setScannerService(std::shared_ptr<FileScannerService> service) { service_ = std::move(service); }

void FileScanner::setEventSink(ScannerEventSink sink) {
  if (service_) {
    service_->setEventSink(std::move(sink));
  }
}

void FileScanner::configure(const ScannerConfig& config) {
  if (service_) {
    service_->configure(config);
  }
}

void FileScanner::scan(const std::vector<ScannerRoot>& roots, ScanMode mode) {
  if (service_) {
    service_->scan(roots, mode);
  }
}

void FileScanner::startWatching(const std::vector<ScannerRoot>& roots) {
  if (service_) {
    service_->startWatching(roots);
  }
}

void FileScanner::stopWatching() {
  if (service_) {
    service_->stopWatching();
  }
}

void FileScanner::stop() {
  if (service_) {
    service_->stop();
  }
}

PlaylistTreeSnapshot FileScanner::snapshot() const {
  if (!service_) {
    return {};
  }

  return service_->snapshot();
}

std::shared_ptr<FileScannerService> makeFileScannerService() {
  return makeFileScannerService(FileScannerServiceDependencies{});
}

}
