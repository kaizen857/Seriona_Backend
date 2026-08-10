#pragma once

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include "folder_thumbnail_resolver.h"

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
  // 文件夹缩略图导出 seam：null 时 orchestrator 装配生产 adapter（TagReader::ExportFolderCover，
  // ThumbnailOnly + Ignore）；测试注入 fake 断言接线。
  FolderThumbnailExportSeam folderThumbnailSeam;
  std::chrono::milliseconds watcherDebounce{50};
  std::chrono::milliseconds reconcileInterval{60000};
};

[[nodiscard]] std::shared_ptr<FileScannerService> makeFileScannerService(FileScannerServiceDependencies dependencies);

}
