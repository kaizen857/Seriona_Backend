#include "seriona/scanner/lrc_parser.h"

#include "path_utf8.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::string_view trimAscii(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool parseUnsigned(std::string_view text, std::uint64_t& value) noexcept {
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] std::optional<std::chrono::milliseconds> parseTimestamp(std::string_view text) noexcept {
  const auto colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return std::nullopt;
  }

  const auto secondsEnd = text.find_first_of(".", colon + 1);
  const auto secondsText = text.substr(colon + 1, secondsEnd == std::string_view::npos ? std::string_view::npos
                                                                                       : secondsEnd - colon - 1);
  if (secondsText.size() != 2) {
    return std::nullopt;
  }

  std::uint64_t minutes = 0;
  std::uint64_t seconds = 0;
  if (!parseUnsigned(text.substr(0, colon), minutes) || !parseUnsigned(secondsText, seconds) || seconds >= 60U) {
    return std::nullopt;
  }

  std::uint64_t fractionMs = 0;
  if (secondsEnd != std::string_view::npos) {
    const auto fraction = text.substr(secondsEnd + 1);
    if (fraction.empty() || fraction.size() > 3U) {
      return std::nullopt;
    }
    std::uint64_t parsedFraction = 0;
    if (!parseUnsigned(fraction, parsedFraction)) {
      return std::nullopt;
    }
    if (fraction.size() == 1U) {
      fractionMs = parsedFraction * 100U;
    } else if (fraction.size() == 2U) {
      fractionMs = parsedFraction * 10U;
    } else {
      fractionMs = parsedFraction;
    }
  }

  return std::chrono::milliseconds{static_cast<std::int64_t>(((minutes * 60U) + seconds) * 1000U + fractionMs)};
}

[[nodiscard]] bool isMetadataTag(std::string_view tag) noexcept {
  const auto colon = tag.find(':');
  if (colon == std::string_view::npos || colon == 0 || parseTimestamp(tag).has_value()) {
    return false;
  }
  return std::ranges::all_of(tag.substr(0, colon), [](const char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  });
}

[[nodiscard]] LrcParseError makeError(LrcParseErrorCode code, const std::optional<std::filesystem::path>& path,
                                      std::size_t line, std::size_t column, std::string message,
                                      std::string detail = {}) {
  return {.code = code,
          .path = path.value_or(std::filesystem::path{}),
          .line = line,
          .column = column,
          .message = std::move(message),
          .detail = std::move(detail)};
}

void normalizeNewlines(std::string& text) {
  text.erase(std::ranges::remove(text, '\r').begin(), text.end());
}

}

LrcParseResult parseLrcText(std::string text, const LrcParseOptions& options,
                            std::optional<std::filesystem::path> path) {
  LrcParseResult result;
  if (text.size() > options.maxBytes) {
    result.errors.push_back(makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U, "lrc file exceeds scanner limit"));
    spdlog::warn("lrc parse: file exceeds size limit ({})", path.has_value() ? pathToUtf8(*path) : "<text>");
    return result;
  }

  normalizeNewlines(text);
  std::size_t lineNumber = 0;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    if (lineNumber >= options.maxLines) {
      result.errors.push_back(makeError(LrcParseErrorCode::TooManyLines, path, lineNumber + 1U, 0U,
                                        "lrc file exceeds scanner line limit"));
      break;
    }

    const auto next = text.find('\n', offset);
    const auto line = std::string_view{text}.substr(offset, next == std::string::npos ? std::string_view::npos
                                                                                     : next - offset);
    ++lineNumber;
    offset = next == std::string::npos ? text.size() + 1U : next + 1U;
    if (trimAscii(line).empty()) {
      continue;
    }

    std::vector<std::chrono::milliseconds> timestamps;
    std::size_t cursor = 0;
    bool sawBracket = false;
    while (cursor < line.size() && line[cursor] == '[') {
      sawBracket = true;
      const auto close = line.find(']', cursor + 1U);
      if (close == std::string_view::npos) {
        result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, cursor + 1U,
                                          "lrc timestamp is missing closing bracket", std::string{line}));
        spdlog::debug("lrc parse: malformed line {} (missing closing bracket)", lineNumber);
        timestamps.clear();
        break;
      }
      const auto tag = line.substr(cursor + 1U, close - cursor - 1U);
      if (const auto timestamp = parseTimestamp(tag); timestamp.has_value()) {
        timestamps.push_back(*timestamp);
      } else if (!isMetadataTag(tag)) {
        result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, cursor + 1U,
                                          "lrc timestamp is malformed", std::string{tag}));
        spdlog::debug("lrc parse: malformed timestamp at line {}", lineNumber);
        timestamps.clear();
        break;
      }
      cursor = close + 1U;
    }

    if (!sawBracket || timestamps.empty()) {
      continue;
    }

    const auto lyricText = trimAscii(line.substr(cursor));
    for (const auto timestamp : timestamps) {
      result.lines.push_back({.timestamp = timestamp, .text = std::string{lyricText}});
    }
  }

  // 稳定排序，且只按时间戳：同时间戳的多行保持输入（出现）顺序，使下游「组内第 1 行 = 原文」
  // 的配对约定（D22）成立，并与 TagReader 的 NormalizeLyrics 保持同构。此前按（时间戳, 文本）
  // 排序会把同组行按文本重排，「第 1 行」不再可确定。
  std::ranges::stable_sort(result.lines, [](const LyricLine& lhs, const LyricLine& rhs) {
    return lhs.timestamp < rhs.timestamp;
  });
  // 去重谓词保持（时间戳, 文本）不变，这是有意的决定：排序键只剩时间戳后，std::ranges::unique
  // 的语义从「去全部重复」退化为「只去相邻重复」。不改去重实现是最小改动（不引入额外状态/
  // 复杂度），且 D22 只要求「组内第 1 行 = 原文」——非相邻重复不改变组内首行。
  result.lines.erase(std::ranges::unique(result.lines, {}, [](const LyricLine& line) {
                       return std::pair{line.timestamp, line.text};
                     }).begin(),
                     result.lines.end());
  return result;
}

LrcParseResult parseLrcFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    spdlog::warn("lrc parse: failed to stat {} ({})", pathToUtf8(path), error.message());
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to stat lrc file", error.message())}};
  }
  if (size > options.maxBytes) {
    spdlog::warn("lrc parse: file exceeds size limit ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U, "lrc file exceeds scanner limit")}};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    spdlog::warn("lrc parse: failed to open {}", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to open lrc file")}};
  }

  std::string text;
  text.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
  return parseLrcText(std::move(text), options, path);
}

}
