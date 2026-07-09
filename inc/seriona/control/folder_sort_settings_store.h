#pragma once

#include "seriona/control/control_contracts.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace seriona::control {

enum class FolderSortSettingsErrorCode {
  InvalidRootPath,
  InvalidFolderNodeId,
  InvalidRulesJson,
  StorageError,
};

class FolderSortSettingsError : public std::runtime_error {
public:
  FolderSortSettingsError(FolderSortSettingsErrorCode code, std::string message);

  [[nodiscard]] FolderSortSettingsErrorCode code() const noexcept { return code_; }

private:
  FolderSortSettingsErrorCode code_;
};

struct FolderSortSettingsStoreConfig {
  std::filesystem::path databasePath;
};

class FolderSortSettingsStore {
public:
  virtual ~FolderSortSettingsStore() = default;

  virtual void upsert(FolderSortSetting setting) = 0;
  [[nodiscard]] virtual std::optional<FolderSortSetting> load(const std::filesystem::path& rootPath,
                                                              const std::string& folderNodeId) const = 0;
  virtual void remove(const std::filesystem::path& rootPath, const std::string& folderNodeId) = 0;
  [[nodiscard]] virtual std::vector<FolderSortSetting> list(const std::filesystem::path& rootPath) const = 0;
};

[[nodiscard]] std::unique_ptr<FolderSortSettingsStore> makeSQLiteFolderSortSettingsStore(
    FolderSortSettingsStoreConfig config);

}
