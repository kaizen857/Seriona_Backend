#include "seriona/scanner/file_scanner_service.h"

#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

class NullFileScannerService final : public FileScannerService {
public:
  void setEventSink(ScannerEventSink sink) override { sink_ = std::move(sink); }
  void configure(const ScannerConfig& config) override { config_ = config; }
  void scan(const std::vector<ScannerRoot>& roots, ScanMode mode) override {
    roots_ = roots;
    mode_ = mode;
  }
  void stop() override { stopped_ = true; }
  [[nodiscard]] PlaylistTreeSnapshot snapshot() const override { return snapshot_; }

private:
  ScannerEventSink sink_{};
  ScannerConfig config_{};
  std::vector<ScannerRoot> roots_{};
  ScanMode mode_{ScanMode::Incremental};
  PlaylistTreeSnapshot snapshot_{};
  bool stopped_{false};
};

}

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
  return std::make_shared<NullFileScannerService>();
}

}
