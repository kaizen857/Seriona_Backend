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
  // 目录树哈希的库相关性过滤（默认开启）：只统计受支持音频、.cue、封面侧车；`.lrc` 仍无条件
  // 排除（每次扫描都会重读）。无关条目既不改变哈希，也就不会让周期探测升级为 Full 对账。
  // 置 false 恢复「哈希一切条目」。
  bool libraryRelevanceFilter{true};
  // 音频扩展名集合；空 = defaultAudioExtensions()。
  std::vector<std::string> allowedExtensions{};
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
