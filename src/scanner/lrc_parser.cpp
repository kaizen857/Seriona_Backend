#include "seriona/scanner/lrc_parser.h"

#include "path_utf8.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
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

// F1/F2 时间轴数值安全上界：时间戳与 offset 都以 int64 毫秒参与 `ts − offset`，两个上界须成对成立，
// 该减法才在任何方向都无有符号溢出（UB）。真实语料远低于此（最大时间戳 99:00、最大 offset 幅值 0ms），
// 界只用于拒绝畸形/恶意输入，不改变任何真实文件的行为。
//
// F1：offset 幅值上界 24h —— `[offset:]` 语义是毫秒级全局微调，超界按无效标签跳过（同 `[offset:abc]`，不报错）。
constexpr std::int64_t kMaxOffsetMagnitudeMs = 86'400'000;
// F2：minutes 上界 = (INT64_MAX − kMaxOffsetMagnitudeMs − 59999) / 60000 的整数下界，其中
// total = minutes*60000 + seconds*1000 + fractionMs（seconds<60、fractionMs<1000）。minutes 超界 ⇒ 畸形
// 时间戳（既有 InvalidTimestamp 路径）。该检查令随后的 uint64 乘法不回绕 ⇒ 被接受的合法时间戳恒 ≥ 0，
// 「< 0 ⟺ unsynced 哨兵」成为真实不变式；同时给 offset 应用预留 ±24h 余量（见 static_assert）。
constexpr std::uint64_t kMaxTimestampMinutes = 153'722'867'279'471ULL;
static_assert(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                      static_cast<std::uint64_t>(kMaxOffsetMagnitudeMs) >=
                  kMaxTimestampMinutes * 60'000ULL + 59'999ULL,
              "timestamp upper bound must leave kMaxOffsetMagnitudeMs of int64 headroom for ts - offset");

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
  if (minutes > kMaxTimestampMinutes) {
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

// `[offset:<±ms>]`：按权威文档 §8.9.1 解析毫秒偏移；符号方向是 ts_new = ts_parsed − offset，
// 即正 offset = 歌词提前显示 = 时间戳变小（勿凭直觉写反）。`+`/`-` 可缺省，缺省视为正值。
// 无效值（如 `[offset:abc]`、空值、超出 uint64 的数字、幅值 > kMaxOffsetMagnitudeMs、双符号）返回
// nullopt —— 调用方沿用既有 isMetadataTag 元数据跳过路径，不报错，以免改变既有文件行为。
//
// 两条不变式共同保证 `ts − offset` 在正负两个方向都不发生有符号溢出（F1），且**整类符号回绕**被消除：
// 1) 幅值解析进**无符号** uint64 —— `std::from_chars` 对无符号类型**不接受任何符号**，故手动剥离至多
//    一个前导 `+`/`-` 后，`+-100`/`--100`/`++500`/`-+500` 等剩余以符号开头的输入必然 `from_chars` 失败
//    ⇒ 不存在「第二个符号把 magnitude 变成负数再绕过上界」的回绕路径（此处**不是**特判，而是无符号解析的
//    固有性质：整类符号回绕一并消除）；
// 2) 上界 kMaxOffsetMagnitudeMs（24h）在无符号域上比大小；与 parseTimestamp 的 kMaxTimestampMinutes
//    成对成立（static_assert 锁定）⇒ 合法 offset 幅值远小于 int64 上限，`−magnitude`（转 int64 后取负）
//    与 `ts − offset` 都不会溢出。
[[nodiscard]] std::optional<std::chrono::milliseconds> parseOffsetTag(std::string_view tag) noexcept {
  constexpr std::string_view prefix = "offset:";
  if (!tag.starts_with(prefix)) {
    return std::nullopt;
  }
  auto magnitudeText = tag.substr(prefix.size());
  bool negative = false;
  if (!magnitudeText.empty() && (magnitudeText.front() == '+' || magnitudeText.front() == '-')) {
    negative = magnitudeText.front() == '-';
    magnitudeText.remove_prefix(1);
  }
  if (magnitudeText.empty()) {
    return std::nullopt;
  }
  std::uint64_t magnitude = 0;
  const auto* begin = magnitudeText.data();
  const auto* end = begin + magnitudeText.size();
  const auto parsed = std::from_chars(begin, end, magnitude);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  if (magnitude > static_cast<std::uint64_t>(kMaxOffsetMagnitudeMs)) {
    return std::nullopt;
  }
  const auto signedMagnitude = static_cast<std::int64_t>(magnitude);
  return std::chrono::milliseconds{negative ? -signedMagnitude : signedMagnitude};
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

// G4（未同步行保留）：整首无时间戳的纯文本 `.lrc` 中「无行首 `[...]` 的正文行」以此哨兵时间戳保留。
// 值为 -1ms，是**独立哨兵**，**不复用 0**：语料实测 `[00:00.xxx]` 是合法时间戳（1,945 行 / 1,257 文件
// ≈78%），用 0 当标记会把每个文件真实的首行误判为 unsynced 并排除出 D22 配对——若该行存在同 ts 译文行，
// 会静默丢失。哨兵行语义 = **不参与 D22 分组**：todo 21（自动切分）必须按「`< 0`」排除，**不得**按
// 「`== 0`」排除。F2 修复后 `parseTimestamp` 保证被接受的合法时间戳恒 ≥ 0，故「`< 0`」与哨兵一一对应
// （此前 uint64→int64 回绕可让合法标签产出 -1，与哨兵碰撞）。只作用于后端解析的 `.lrc` 侧车文件；
// 内嵌歌词由 TagReader 解析、不经本函数，故该保留不覆盖内嵌歌词（与 G1 同源的、有意的不对称）。
// 哨兵不得进入公共契约头。
constexpr std::chrono::milliseconds kUnsyncedTimestamp{std::chrono::milliseconds{-1}};

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
  // `[offset:<±ms>]` 全文件单值语义，可出现在任意位置；多次出现按 LRC 惯例后者覆盖前者
  // （AIMP/Walaoke 一致）。这里在单遍解析里记录最后见到的 offset，循环后再统一加减，
  // 避免两遍重扫破坏 maxLines 计数语义。
  std::chrono::milliseconds lastOffset{0};
  std::vector<std::string> pendingUnsynced;
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
      if (const auto tagOffset = parseOffsetTag(tag); tagOffset.has_value()) {
        lastOffset = *tagOffset;
      } else if (const auto timestamp = parseTimestamp(tag); timestamp.has_value()) {
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

    if (!sawBracket) {
      // G4：行首无任何 `[...]` 的非空正文行先收进候选。仅当「整首无任何时间戳行」时才提升为
      // unsynced 歌词；混合形态按 §8.9.4 丢弃（这类行通常是文件尾制作者署名，不是歌词）。
      pendingUnsynced.emplace_back(trimAscii(line));
      continue;
    }
    if (timestamps.empty()) {
      // 带 `[...]` 但无有效时间戳（元数据 / offset 标签）或该行时间戳畸形：保持既有跳过语义，不收集。
      continue;
    }

    const auto lyricText = trimAscii(line.substr(cursor));
    for (const auto timestamp : timestamps) {
      result.lines.push_back({.timestamp = timestamp, .text = std::string{lyricText}});
    }
  }

  // G1（`[offset:<±ms>]`）只作用于后端解析的 LRC（本函数解析的 .lrc 侧车文件）。内嵌歌词由
  // TagReader 解析、不经过本函数，故「内嵌歌词不覆盖」是有意的不对称，不在 G1 范围内。
  // 方向：ts_new = ts_parsed − offset（正数 = 提前显示 = 时间戳变小）；结果一律夹到 ≥ 0，
  // 只有正 offset 才可能压到负数。应用置于既有 stable_sort 之前，max(0,x) 单调非减 ⇒ 稳定序不变。
  if (lastOffset != std::chrono::milliseconds{0}) {
    for (auto& line : result.lines) {
      line.timestamp = std::max(std::chrono::milliseconds{0}, line.timestamp - lastOffset);
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

  // G4：整首无任何时间戳行（`result.lines` 为空 ⇒ 上面一条带时间戳的行都没有）时，把主循环收集的
  // 纯文本行以 unsynced 哨兵时间戳保留。提升放在 offset 应用与 stable_sort/unique 之后，理由有两条：
  // （1）必须晚于 offset——offset 的 `max(0,·)` 会把负哨兵夹成 0 而失去哨兵语义；（2）晚于 unique ⇒
  // unsynced 行按文档顺序、不去重地原样保留（G4 的目标是「不丢数据」）。混合形态下这里不执行，
  // `pendingUnsynced` 随作用域丢弃，与既有行为逐字节一致。
  if (result.lines.empty() && !pendingUnsynced.empty()) {
    for (auto& text : pendingUnsynced) {
      result.lines.push_back({.timestamp = kUnsyncedTimestamp, .text = std::move(text)});
    }
  }
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
