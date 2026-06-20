#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

enum class LrcParseErrorCode {
  IoFailure,
  FileTooLarge,
  TooManyLines,
  InvalidTimestamp,
};

struct LrcParseOptions {
  std::size_t maxBytes{1024U * 1024U};
  std::size_t maxLines{10'000U};
};

struct LrcParseError {
  LrcParseErrorCode code{LrcParseErrorCode::InvalidTimestamp};
  std::filesystem::path path;
  std::size_t line{0};
  std::size_t column{0};
  std::string message;
  std::string detail;
};

struct LrcParseResult {
  std::vector<LyricLine> lines{};
  std::vector<LrcParseError> errors{};
};

[[nodiscard]] LrcParseResult parseLrcText(std::string text,
                                          const LrcParseOptions& options = {},
                                          std::optional<std::filesystem::path> path = std::nullopt);
[[nodiscard]] LrcParseResult parseLrcFile(const std::filesystem::path& path,
                                          const LrcParseOptions& options = {});

}
