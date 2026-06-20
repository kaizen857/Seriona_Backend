#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

enum class HashErrorCode {
  IoFailure,
  Cancelled,
  UnsupportedPath,
};

struct HashOptions {
  std::size_t chunkBytes{64U * 1024U};
  const std::atomic_bool* cancellationRequested{nullptr};
};

struct HashError {
  HashErrorCode code{HashErrorCode::IoFailure};
  ScannerError scannerError{};
};

struct FileHashResult {
  std::optional<std::string> hash;
  std::vector<HashError> errors{};
};

struct DirectoryHashResult {
  std::optional<std::string> hash;
  std::vector<HashError> errors{};
};

[[nodiscard]] FileHashResult hashFileContent(const std::filesystem::path& path,
                                             const HashOptions& options = {});
[[nodiscard]] FileHashResult hashLyricsSidecar(const std::filesystem::path& path,
                                               const HashOptions& options = {});
[[nodiscard]] DirectoryHashResult hashDirectoryMerkle(const std::filesystem::path& root,
                                                      const HashOptions& options = {});

}
