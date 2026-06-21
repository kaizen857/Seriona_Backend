#pragma once

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace seriona::scanner {

enum class WatchPathKind {
  File,
  Directory,
  Watcher,
  Other,
};

enum class WatchEffectKind {
  Created,
  Modified,
  Destroyed,
  Renamed,
  OwnerChanged,
  Other,
};

struct WatchEvent {
  std::filesystem::path path;
  WatchPathKind pathKind{WatchPathKind::Other};
  WatchEffectKind effectKind{WatchEffectKind::Other};
  std::vector<WatchEvent> associated;
};

using WatchEventCallback = std::function<void(const WatchEvent&)>;

class FolderWatcher {
public:
  virtual ~FolderWatcher() = default;
  virtual void close() noexcept = 0;
};

class FolderWatcherFactory {
public:
  virtual ~FolderWatcherFactory() = default;
  [[nodiscard]] virtual std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                             WatchEventCallback callback) = 0;
};

struct FileScannerServiceDependencies {
  std::shared_ptr<TagMetadataReader> metadataReader;
  std::shared_ptr<FolderWatcherFactory> watcherFactory;
  std::filesystem::path databasePath;
  std::filesystem::path coverExportDir;
  std::chrono::milliseconds watcherDebounce{50};
};

[[nodiscard]] std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies);

}
