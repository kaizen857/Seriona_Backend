#pragma once

#include "seriona/control/control_contracts.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::control {

enum class LyricSplitStoreErrorCode {
  InvalidEntry,
  StorageError,
};

class LyricSplitStoreError : public std::runtime_error {
public:
  LyricSplitStoreError(LyricSplitStoreErrorCode code, std::string message);

  [[nodiscard]] LyricSplitStoreErrorCode code() const noexcept { return code_; }

private:
  LyricSplitStoreErrorCode code_;
};

struct LyricSplitStoreConfig {
  std::filesystem::path databasePath;
};

class LyricSplitStore {
public:
  virtual ~LyricSplitStore() = default;

  virtual void putAuto(LyricSplitEntry entry) = 0;
  virtual void upsertManual(LyricSplitEntry entry) = 0;
  [[nodiscard]] virtual std::optional<LyricSplitEntry> load(std::string_view rawText,
                                                            std::string_view targetLanguage,
                                                            LyricSplitConvention convention) const = 0;
  virtual void removeManual(std::string_view rawText, std::string_view targetLanguage,
                            LyricSplitConvention convention) = 0;
  virtual void clearManual() = 0;
  [[nodiscard]] virtual std::vector<LyricSplitEntry> listManual() const = 0;
  [[nodiscard]] virtual std::string algoVersion() const = 0;
};

[[nodiscard]] std::unique_ptr<LyricSplitStore> makeSQLiteLyricSplitStore(LyricSplitStoreConfig config);

}
