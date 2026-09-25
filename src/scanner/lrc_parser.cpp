#include "seriona/scanner/lrc_parser.h"

#include "seriona/scanner/path_utils.h"

#include "path_utf8.h"

#include "spdlog/spdlog.h"

#include <unicode/ucnv.h>
#include <unicode/ustring.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// G3（增强 LRC 逐字标签剥离，设计文档 §8.9.3）：`<mm:ss.xx>` 的语义是**该字符的起始时间**。
// 本次只把标签从显示文本中剥离、**丢弃**逐字时间轴（**不**实现逐字/karaoke 高亮），整行沿用其
// 行首时间戳。正文里 `<` 可能是比较运算（如 `a < b`），故只在 `<` 后**严格匹配** `mm:ss.xx` 或
// `mm:ss.xxx` 形态时才当标签 —— 宁缺勿滥。
//
// 由本实现定死的保守文法（整段必须严格成立，不接受两侧/内部空格）：
//   `<` 1–3 位十进制分钟 `:` 恰好 2 位十进制秒且数值 ≤ 59 `.` 2–3 位十进制小数 `>`
// 理由：与 `parseTimestamp` 对秒 `< 60`、小数 2–3 位的既有判据一致；分钟上限 3 位（≤ 999）足够
// 真实歌词时长，同时收窄 `<0000:...>` 一类误命中。`<3:4>`（秒 1 位）、`<00:1.0>`、`<00:12>`
// （无小数）、`<00:12.3456>`（4 位小数）、`< 00:12.34 >`（含空格）、`<00:61.00>`（秒越界）等
// 一律**不是**标签，按正文逐字节保留。`tag` 参数是 `<` 与 `>` 之间的内容（不含两侧括号）。
//
// ★有意不对称（供 todo 38 回填文档）：参考实现 `tools/lyric_split_regression/lyric_split_tool.py`
// 不含任何 `<mm:ss.xx>` 处理（实测 `rg 'karaoke|<\d|逐字'` 0 命中）——本能力是**解析器侧新增、
// 参考实现不建模**，与 G1「内嵌歌词不覆盖」同属有意不对称，故对全库等价闸门零影响。
[[nodiscard]] bool matchesInlineTimestampTag(std::string_view tag) noexcept {
  const auto colon = tag.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon > 3U) {
    return false;
  }
  const auto dot = tag.find('.', colon + 1U);
  if (dot == std::string_view::npos) {
    return false;
  }
  const auto seconds = tag.substr(colon + 1U, dot - colon - 1U);
  const auto fraction = tag.substr(dot + 1U);
  if (seconds.size() != 2U || (fraction.size() != 2U && fraction.size() != 3U)) {
    return false;
  }
  const auto isDigit = [](const char ch) { return ch >= '0' && ch <= '9'; };
  if (!std::ranges::all_of(tag.substr(0, colon), isDigit) || !std::ranges::all_of(seconds, isDigit) ||
      !std::ranges::all_of(fraction, isDigit)) {
    return false;
  }
  // 秒数越界（如 `<00:61.00>`）保守视为正文；只有严格合法的 `mm:ss.xx` 才剥离。
  const auto secondsValue = static_cast<int>((seconds[0] - '0') * 10 + (seconds[1] - '0'));
  return secondsValue < 60;
}

// 删除正文中所有逐字标签，返回**按标签切分后剩余文字片段的原样拼接**（不额外 trim、不合并空格）：
// 一行内多个标签、标签紧贴文字（`你<00:01.00>好`）、连续标签（`<00:01.00><00:02.00>text`）均适用；
// 整行只有标签时结果为空串（调用处仍产出空文本 `LyricLine`，与既有「空文本行保留」一致）。
// 非标签的 `<...>`（含没有配对的 `>`）保持原样：只把 `<` 本身当普通字符继续扫描。
[[nodiscard]] std::string stripInlineTimestamps(std::string_view text) {
  std::string stripped;
  stripped.reserve(text.size());
  while (!text.empty()) {
    const auto open = text.find('<');
    if (open == std::string_view::npos) {
      stripped.append(text);
      break;
    }
    const auto close = text.find('>', open + 1U);
    if (close == std::string_view::npos) {
      stripped.append(text);
      break;
    }
    if (matchesInlineTimestampTag(text.substr(open + 1U, close - open - 1U))) {
      stripped.append(text.substr(0, open));
      text.remove_prefix(close + 1U);
      continue;
    }
    // 非标签：保留 `<` 与其前文，从其后的字符继续扫描（`>` 作为普通文本保留）。
    stripped.append(text.substr(0, open + 1U));
    text.remove_prefix(open + 1U);
  }
  return stripped;
}

// ── G6：行内元数据 / 制作人员行判据（镜像 src/control/lyric_split.cpp）────────────
//
// 本区块是算法侧 `src/control/lyric_split.cpp` 中 `removeInlineTags` / `matchMetadata`
// / `matchCredit` / `stripPython` 及其支撑谓词（`decodeUtf8At`、`isPythonWhitespace`
// 码点表、`isMetadataWordCodePoint`、`kCreditAlternatives`）的**逐行镜像**。参考实现是
// `tools/lyric_split_regression/lyric_split_tool.py` 的 `TIMESTAMP_RE`/`INLINE_TAG_RE`/
// `METADATA_RE`/`CREDIT_RE`（:34-43）与 `clean_line`（:125-138）。
//
// 为什么镜像而不是 include：scanner 层不得依赖 control 层（模块边界），且
// `seriona_scanner_paths_tests` 只编入 `lrc_parser.cpp` + `path_utils.cpp`，不能为此改
// `tests/CMakeLists.txt`。
//
// ★ 镜像有漂移风险，且**未纳入「全库逐行等价」闸门**（该闸门只覆盖 control 侧
// `lyric_split.cpp`）：若算法侧判据变更，必须同步本区块，否则两侧对同一行的归属会分叉
// （解析器丢的、切分侧当正文 ⇒ 漏行；反之 ⇒ 空行）。
//
// 支撑谓词：无效 UTF-8 退化为 U+FFFD 且只前进 1 字节（与 control 侧逐字节一致，
// 保证「按字节偏移迭代」在畸形输入上不死循环、不越界）。
struct G6DecodedCodePoint {
  std::uint32_t codePoint;
  std::size_t byteLength;
};

constexpr std::uint32_t kG6ReplacementCodePoint = 0xFFFDU;

[[nodiscard]] G6DecodedCodePoint decodeG6Utf8At(std::string_view text, std::size_t offset) noexcept {
  const auto lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80U) {
    return {lead, 1};
  }
  std::size_t length = 0;
  std::uint32_t codePoint = 0;
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    codePoint = static_cast<std::uint32_t>(lead & 0x1FU);
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    codePoint = static_cast<std::uint32_t>(lead & 0x0FU);
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    codePoint = static_cast<std::uint32_t>(lead & 0x07U);
  } else {
    return {kG6ReplacementCodePoint, 1};
  }
  if (offset + length > text.size()) {
    return {kG6ReplacementCodePoint, 1};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return {kG6ReplacementCodePoint, 1};
    }
    codePoint = (codePoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
  }
  const bool overlong = (length == 2 && codePoint < 0x80U) ||
                        (length == 3 && codePoint < 0x800U) ||
                        (length == 4 && codePoint < 0x10000U);
  const bool surrogate = codePoint >= 0xD800U && codePoint <= 0xDFFFU;
  if (overlong || surrogate || codePoint > 0x10FFFFU) {
    return {kG6ReplacementCodePoint, 1};
  }
  return {codePoint, length};
}

struct G6CodePointSpan {
  std::uint32_t first;
  std::uint32_t last;
};

template <std::size_t N>
[[nodiscard]] constexpr bool g6InSortedSpans(const std::array<G6CodePointSpan, N>& spans,
                                             std::uint32_t codePoint) noexcept {
  const auto upper = std::upper_bound(spans.begin(), spans.end(), codePoint,
                                      [](std::uint32_t value, const G6CodePointSpan& span) {
                                        return value < span.first;
                                      });
  if (upper == spans.begin()) {
    return false;
  }
  return codePoint <= (upper - 1)->last;
}

template <std::size_t N>
[[nodiscard]] constexpr bool g6SpansSortedAndDisjoint(const std::array<G6CodePointSpan, N>& spans) noexcept {
  for (std::size_t index = 1; index < N; ++index) {
    if (spans[index].first <= spans[index - 1].last) {
      return false;
    }
  }
  return true;
}

// Python `str.isspace()` / 正则 `\s`（str 模式）的精确集合：29 个码点、10 段。
// 逐字镜像 control 侧 `kPythonWhitespaceRanges`（U+3000 全角空格在内）。
constexpr std::array<G6CodePointSpan, 10> kG6PythonWhitespaceRanges{{
    {0x0009U, 0x000DU},
    {0x001CU, 0x0020U},
    {0x0085U, 0x0085U},
    {0x00A0U, 0x00A0U},
    {0x1680U, 0x1680U},
    {0x2000U, 0x200AU},
    {0x2028U, 0x2029U},
    {0x202FU, 0x202FU},
    {0x205FU, 0x205FU},
    {0x3000U, 0x3000U},
}};

static_assert(g6SpansSortedAndDisjoint(kG6PythonWhitespaceRanges),
              "G6 Python 空白表必须按起点有序且互不重叠，否则二分查表与参考实现语义不等价");

[[nodiscard]] constexpr bool isG6PythonWhitespace(std::uint32_t codePoint) noexcept {
  return g6InSortedSpans(kG6PythonWhitespaceRanges, codePoint);
}

// 等价 Python `str.strip()`：按 Python 空白语义剥离两侧。**不得**改用 `trimAscii`
// （后者只去 `' '`/`'\t'`/`'\r'`、保留 U+3000 —— 那是渲染文本的 G8 钉子，两者用途不同）。
[[nodiscard]] std::size_t skipG6PythonWhitespace(std::string_view text, std::size_t offset) noexcept {
  std::size_t cursor = offset;
  while (cursor < text.size()) {
    const auto decoded = decodeG6Utf8At(text, cursor);
    if (!isG6PythonWhitespace(decoded.codePoint)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  return cursor;
}

[[nodiscard]] std::string_view lstripG6Python(std::string_view text) noexcept {
  return text.substr(skipG6PythonWhitespace(text, 0));
}

[[nodiscard]] std::string_view rstripG6Python(std::string_view text) noexcept {
  std::size_t contentEnd = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeG6Utf8At(text, cursor);
    if (!isG6PythonWhitespace(decoded.codePoint)) {
      contentEnd = cursor + decoded.byteLength;
    }
    cursor += decoded.byteLength;
  }
  return text.substr(0, contentEnd);
}

[[nodiscard]] std::string_view stripG6Python(std::string_view text) noexcept {
  return rstripG6Python(lstripG6Python(text));
}

[[nodiscard]] bool isG6AsciiWordByte(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

// Python `re.IGNORECASE` 对 `[a-zA-Z_]` 的等价集合：ASCII 字母/下划线，外加简单大小写
// 折叠后落回该类内的 4 个码点（U+0130 İ / U+0131 ı / U+017F ſ / U+212A K）。
// **只有 METADATA_RE 带 IGNORECASE**；INLINE_TAG_RE 不带，故 `removeInlineTags` 只用
// `isG6AsciiWordByte`。把本谓词用到行内标签上会引入与参考实现的分歧。
[[nodiscard]] bool isG6MetadataWordCodePoint(std::uint32_t codePoint) noexcept {
  if (codePoint < 0x80U) {
    return isG6AsciiWordByte(static_cast<char>(codePoint));
  }
  return codePoint == 0x0130U || codePoint == 0x0131U || codePoint == 0x017FU || codePoint == 0x212AU;
}

// 全局删除全部 `\[[a-zA-Z_]+:[^\]]*\]`（自左向右、非重叠）：行内元数据标签（`[by:x]`、
// `[ar:x]`、`[tr:zh]` …）从正文中剥离。不匹配的 `[` 逐字节保留。
[[nodiscard]] std::string removeInlineTags(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    if (text[cursor] == '[') {
      std::size_t wordEnd = cursor + 1;
      while (wordEnd < text.size() && isG6AsciiWordByte(text[wordEnd])) {
        ++wordEnd;
      }
      if (wordEnd > cursor + 1 && wordEnd < text.size() && text[wordEnd] == ':') {
        std::size_t closing = wordEnd + 1;
        while (closing < text.size() && text[closing] != ']') {
          ++closing;
        }
        if (closing < text.size()) {
          cursor = closing + 1;
          continue;
        }
      }
    }
    result.push_back(text[cursor]);
    ++cursor;
  }
  return result;
}

// 制作人员行的关键词（逐字照抄 CREDIT_RE 的 alternation 顺序 —— 顺序即 Python 正则的
// 匹配顺序，不可重排；`歌`/`唱`/`词`/`曲` 等单字关键词也逐字保留）。
constexpr std::array<std::string_view, 31> kG6CreditAlternatives{
    "作词",  "作曲", "编曲", "歌",    "唱",   "原唱",    "原曲",  "発売日",
    "収録",  "ミックス", "歌詞", "翻译",   "訳",   "词",      "曲",    "演唱",
    "制作",  "混音", "录音", "母带",   "By",   "BY",      "by",    "Arr",
    "Arrange", "Lyrics", "Vocals", "Vocal", "Music", "Mix", "Master"};

static_assert(kG6CreditAlternatives.size() == 31, "G6 制作人员关键词表必须与参考实现同为 31 项");

// 等价 METADATA_RE.match：`^\s*\[[a-zA-Z_]+:`（IGNORECASE，见 isG6MetadataWordCodePoint）。
[[nodiscard]] bool matchMetadata(std::string_view text) noexcept {
  std::size_t cursor = skipG6PythonWhitespace(text, 0);
  if (cursor >= text.size() || text[cursor] != '[') {
    return false;
  }
  ++cursor;
  const std::size_t wordStart = cursor;
  while (cursor < text.size()) {
    const auto decoded = decodeG6Utf8At(text, cursor);
    if (!isG6MetadataWordCodePoint(decoded.codePoint)) {
      break;
    }
    cursor += decoded.byteLength;
  }
  if (cursor == wordStart) {
    return false;
  }
  return cursor < text.size() && text[cursor] == ':';
}

// 等价 CREDIT_RE.match：`^\s*(<31 个关键词>)\s*[:：]`。关键词逐个试、失败就换下一个 ——
// 正是 Python alternation「左起首个能接上 `\s*[:：]` 的分支」的语义。
[[nodiscard]] bool matchCredit(std::string_view text) noexcept {
  const std::size_t start = skipG6PythonWhitespace(text, 0);
  const std::string_view tail = text.substr(start);
  for (const auto alternative : kG6CreditAlternatives) {
    if (!tail.starts_with(alternative)) {
      continue;
    }
    const std::size_t colon = skipG6PythonWhitespace(text, start + alternative.size());
    // 半角 `:` 或全角 `：`（U+FF1A，多字节 ⇒ 按 UTF-8 字节前缀比较）。
    if (colon < text.size() && (text[colon] == ':' || text.substr(colon).starts_with("："))) {
      return true;
    }
  }
  return false;
}

// G6 判据（镜像 `clean_line` 阶段 A 的尾部）：给定「已剥时间戳、已剥行内 `[tag:value]`」
// 的 body 片段，返回该行是否应被丢弃。判据 = 按 Python 空白语义 strip 后为空 **或**
// 命中元数据 **或** 命中制作人员。
//
// ★ 检查点必须**早于**逐字标签（karaoke）剥离（调用处即如此）。todo 14 钉住
// `[00:01.00]<00:01.00><00:02.00>` 必须产出 1 行空文本：本判据此刻看到的 body 是
// `<00:01.00><00:02.00>`（非空 ⇒ 不丢），而被 karaoke 剥离后才是空串。若把「为空即丢」
// 的判据挪到 karaoke 剥离之后（对最终文本求值），该行会被误丢。允许在 cleanLine 的
// `body` 上查空，但**不得**对 karaoke 剥离后的最终文本查空。
[[nodiscard]] bool isDroppableLyricBody(std::string_view bodyWithoutInlineTags) noexcept {
  const auto body = stripG6Python(bodyWithoutInlineTags);
  return body.empty() || matchMetadata(body) || matchCredit(body);
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

// XML 1.0 §2.11 行尾归一（**TTML 专用**）：`#xD#xA` 与孤立 `#xD` 一律折成 `#xA`。
// 为什么不在 TTML 路径复用 `normalizeNewlines`：后者**删除**全部 `\r`，会让 CDATA/正文里的孤立 CR
// 凭空消失（`a\rb` → `ab`），而 XML 语义应为 `a\nb`、再按本实现的 `xml:space="default"` 折叠成
// `a b`（F3）。改动只落在 TTML 入口 ⇒ LRC/SRT/ASS/plain-text 四条路径的 `normalizeNewlines` 调用零变化。
void normalizeXmlLineEndings(std::string& text) {
  if (text.find('\r') == std::string::npos) {
    return;  // 最常见路径（无 CR）：零拷贝直接返回
  }
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '\r') {
      normalized.push_back(text[index]);
      continue;
    }
    normalized.push_back('\n');
    if (index + 1U < text.size() && text[index + 1U] == '\n') {
      ++index;  // `\r\n` 是单个行尾 ⇒ 连同它的 `\n` 一起消费，避免产出两个换行
    }
  }
  text = std::move(normalized);
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

// ── G5a：纯文本歌词的 6 步编码回退（ICU ucnv，严格）────────────────────────────
//
// 顺序逐字取自设计文档 §3.1 与参考实现 lyric_split_tool.py:610-621（`TEXT_ENCODINGS`）：
// utf-8-sig → utf-8 → gb18030 → big5 → shift_jis → utf-16，首个成功即采信。`utf-8-sig` 在首位
// ⇒ 无 BOM 的 UTF-8 也走该分支（BOM 剥离与无 BOM 命中同一路径，刻意的确定性口径）。
//
// ⚠️ 只对齐「顺序与集合」，不是编解码器的逐字节语义：ICU 的 gb18030 / Big5 / Shift_JIS 与
// Python 同名 codec 在部分字节序列上结果不同（已登记分歧）。不得声称等价。
inline constexpr std::array<std::string_view, 6> kPlainTextEncodings{
    "utf-8-sig", "utf-8", "gb18030", "big5", "shift_jis", "utf-16"};

// 与上表中除 `utf-8-sig`、`utf-16` 之外的 4 项一一对应（utf-8/gb18030/big5/shift_jis）；`utf-16` 需按
// BOM/本机序显式选 UTF-16LE / UTF-16BE，单列处理。
inline constexpr std::array<const char*, 4> kPlainTextIcuNames{"UTF-8", "gb18030", "Big5", "Shift_JIS"};

constexpr std::string_view kUtf8Bom("\xEF\xBB\xBF", 3);

// 用 UCNV_TO_U_CALLBACK_STOP 把 bytes 严格解码为 UTF-8；遇到非法/截断字节返回 false。
// 必须 STOP：ICU 默认的 SUBSTITUTE 会写 U+FFFD 并继续，等于把降级吞掉，与 Python 抛
// UnicodeDecodeError 才降级的语义不符。
[[nodiscard]] bool decodeStrictIcu(std::string_view bytes, const char* converterName, std::string& utf8Out) {
  UErrorCode status = U_ZERO_ERROR;
  UConverter* converter = ucnv_open(converterName, &status);
  if (U_FAILURE(status)) {
    return false;
  }
  status = U_ZERO_ERROR;
  ucnv_setToUCallBack(converter, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &status);
  if (U_FAILURE(status)) {
    ucnv_close(converter);
    return false;
  }

  const auto sourceLength = static_cast<std::int32_t>(bytes.size());
  status = U_ZERO_ERROR;
  const std::int32_t needed = ucnv_toUChars(converter, nullptr, 0, bytes.data(), sourceLength, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    ucnv_close(converter);
    return false;
  }
  status = U_ZERO_ERROR;
  std::vector<UChar> utf16(static_cast<std::size_t>(needed) + 1U);
  const std::int32_t written = ucnv_toUChars(converter, utf16.data(), static_cast<std::int32_t>(utf16.size()),
                                             bytes.data(), sourceLength, &status);
  ucnv_close(converter);
  if (U_FAILURE(status)) {
    return false;
  }

  status = U_ZERO_ERROR;
  std::int32_t utf8Length = 0;
  u_strToUTF8(nullptr, 0, &utf8Length, utf16.data(), written, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    return false;
  }
  status = U_ZERO_ERROR;
  utf8Out.assign(static_cast<std::size_t>(utf8Length), '\0');
  u_strToUTF8(utf8Out.data(), utf8Length, nullptr, utf16.data(), written, &status);
  return U_SUCCESS(status);
}

[[nodiscard]] bool hasBytesPrefix(std::string_view bytes, unsigned char first, unsigned char second) noexcept {
  return bytes.size() >= 2U && static_cast<unsigned char>(bytes[0]) == first &&
         static_cast<unsigned char>(bytes[1]) == second;
}

// utf-16 分支：Python `bytes.decode('utf-16')` 有 BOM 则按 BOM 判定并剥掉 BOM，无 BOM 则按
// 本机字节序。ICU 的 "UTF-16" 无 BOM 时默认大端（与 Python 不符），故显式选用
// UTF-16LE / UTF-16BE。BOM 前缀在 decodeLyricsBytes 入口直达本分支。
[[nodiscard]] bool decodePlainTextUtf16(std::string_view bytes, std::string& utf8Out) {
  std::string_view body = bytes;
  const char* converter = nullptr;
  if (hasBytesPrefix(body, 0xFFU, 0xFEU)) {
    body.remove_prefix(2U);
    converter = "UTF-16LE";
  } else if (hasBytesPrefix(body, 0xFEU, 0xFFU)) {
    body.remove_prefix(2U);
    converter = "UTF-16BE";
  } else if constexpr (std::endian::native == std::endian::big) {
    converter = "UTF-16BE";
  } else {
    converter = "UTF-16LE";
  }
  return decodeStrictIcu(body, converter, utf8Out);
}

// ── G5b：SubRip（.srt）解析判据 ─────────────────────────────────────────────
//
// 时间轴行的**严格**文法（自本实现定死；不使用 std::regex —— Python `\d`/`\s` 语义与 C++ 不同，
// 且既有 removeInlineTags/matchCredit 一律手写扫描）：
//   timecode = 小时(≥2 位十进制) ':' 分钟(恰好 2 位且 ≤ 59) ':' 秒(恰好 2 位且 ≤ 59) ',' 毫秒(恰好 3 位)
//   timeline = timecode 空白* '-->' 空白* timecode
// 逗号固定为毫秒分隔符；行两侧空白无意义。小时允许 >2 位（任务书点名的换算边界），其余字段位数
// 严格 —— `00:00:03.000`（点号）这类非标准写法不识别。仅**起始**时间被采用，结束时间只做合法性校验。
//
// 溢出界与 parseTimestamp 共用 kMaxTimestampMinutes：小时先被它约束（hours*60 无回绕），再校验
// 总分钟数，故下述乘法恒落在 uint64 且结果 < int64 上限（见 kMaxTimestampMinutes 的 static_assert）。
[[nodiscard]] std::optional<std::chrono::milliseconds> parseSrtTimecode(std::string_view text) noexcept {
  const auto firstColon = text.find(':');
  if (firstColon == std::string_view::npos || firstColon < 2U) {
    return std::nullopt;
  }
  const auto secondColon = text.find(':', firstColon + 1U);
  if (secondColon == std::string_view::npos) {
    return std::nullopt;
  }
  const auto comma = text.find(',', secondColon + 1U);
  if (comma == std::string_view::npos) {
    return std::nullopt;
  }
  const auto hoursText = text.substr(0, firstColon);
  const auto minutesText = text.substr(firstColon + 1U, secondColon - firstColon - 1U);
  const auto secondsText = text.substr(secondColon + 1U, comma - secondColon - 1U);
  const auto millisText = text.substr(comma + 1U);
  if (minutesText.size() != 2U || secondsText.size() != 2U || millisText.size() != 3U) {
    return std::nullopt;
  }

  std::uint64_t hours = 0;
  std::uint64_t minutes = 0;
  std::uint64_t seconds = 0;
  std::uint64_t millis = 0;
  if (!parseUnsigned(hoursText, hours) || !parseUnsigned(minutesText, minutes) ||
      !parseUnsigned(secondsText, seconds) || !parseUnsigned(millisText, millis)) {
    return std::nullopt;
  }
  if (hours > kMaxTimestampMinutes || minutes >= 60U || seconds >= 60U) {
    return std::nullopt;
  }
  const auto totalMinutes = (hours * 60U) + minutes;
  if (totalMinutes > kMaxTimestampMinutes) {
    return std::nullopt;
  }
  const auto totalMillis = ((totalMinutes * 60U) + seconds) * 1000U + millis;
  return std::chrono::milliseconds{static_cast<std::int64_t>(totalMillis)};
}

[[nodiscard]] std::optional<std::chrono::milliseconds> parseSrtTimingLine(std::string_view line) noexcept {
  const auto arrow = line.find("-->");
  if (arrow == std::string_view::npos) {
    return std::nullopt;
  }
  const auto start = parseSrtTimecode(trimAscii(line.substr(0, arrow)));
  if (!start.has_value()) {
    return std::nullopt;
  }
  if (!parseSrtTimecode(trimAscii(line.substr(arrow + 3U))).has_value()) {
    return std::nullopt;
  }
  return start;
}

// 序号行判据：trim 后非空且全部为 ASCII 十进制数字（无符号、无空白、无其它字符）。
// 纯数字行在本解析器里一律按结构行处理、永不产出正文 —— 这是任务书「序号行（纯数字行）不得成为
// 正文行，也不得作为 unsynced 兜底」的字面要求；代价是「整行只有数字」的歌词行会被一并丢弃，已在
// 证据 README 记录。不设位数上限：判据就是「非空且全为数字」，加任何位数上限都会与判据文字分叉。
[[nodiscard]] bool isSrtIndexLine(std::string_view line) noexcept {
  if (line.empty()) {
    return false;
  }
  return std::ranges::all_of(line, [](const char ch) { return ch >= '0' && ch <= '9'; });
}

// HTML 标签判据。`tag` 是 `<` 与**其后第一个** `>` 之间的内容（不含两侧括号），故不含 `>`、可能含 `<`。
// 文法（严格保守，宁可漏剥也不误伤）：
//   tag        = '/'? 名称 属性?
//   名称       = ASCII 字母 (ASCII 字母 | 数字)*     ← 必须以字母开头，`<3:4>`/`<00:12.34>` 因而被排除
//   属性       = (' ' | '\t') 若干字符，且**不得含 '<'**
// 任何不满足者（`< b`、`< 2 >`、未配对 `<`）一律按正文逐字节保留。边界决策：`<b>` 的 `b` 符合
// 名称文法（HTML 中本就是粗体标签）⇒ 被剥离；该行为由用例显式钉住，不是特例。
[[nodiscard]] bool matchesHtmlTag(std::string_view tag) noexcept {
  std::size_t cursor = 0;
  if (cursor < tag.size() && tag[cursor] == '/') {
    ++cursor;
  }
  if (cursor >= tag.size()) {
    return false;
  }
  const auto isAlpha = [](const char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); };
  const auto isAlphaNumeric = [&isAlpha](const char ch) { return isAlpha(ch) || (ch >= '0' && ch <= '9'); };
  if (!isAlpha(tag[cursor])) {
    return false;
  }
  ++cursor;
  while (cursor < tag.size() && isAlphaNumeric(tag[cursor])) {
    ++cursor;
  }
  if (cursor == tag.size()) {
    return true;
  }
  if (tag[cursor] != ' ' && tag[cursor] != '\t') {
    return false;
  }
  return tag.find('<', cursor) == std::string_view::npos;
}

// 删除正文中所有 HTML 标签，返回剩余文字片段的原样拼接（不额外 trim、不合并空格）。
// 结构与 stripInlineTimestamps 同构：非标签的 `<` 保留、从其后的字符继续扫描；未配对的 `<` 原样保留。
[[nodiscard]] std::string stripHtmlTags(std::string_view text) {
  std::string stripped;
  stripped.reserve(text.size());
  while (!text.empty()) {
    const auto open = text.find('<');
    if (open == std::string_view::npos) {
      stripped.append(text);
      break;
    }
    const auto close = text.find('>', open + 1U);
    if (close == std::string_view::npos) {
      stripped.append(text);
      break;
    }
    if (matchesHtmlTag(text.substr(open + 1U, close - open - 1U))) {
      stripped.append(text.substr(0, open));
      text.remove_prefix(close + 1U);
      continue;
    }
    stripped.append(text.substr(0, open + 1U));
    text.remove_prefix(open + 1U);
  }
  return stripped;
}

// ── G5c：Advanced SubStation Alpha（.ass / .ssa）解析判据 ─────────────────────
//
// 字段顺序的**唯一权威**是 `[Events]` 段内的 `Format:` 行（逗号分隔的字段名）。本实现据此**按名**
// 解析 `Start` / `Text` 的列下标，**不硬编码任何列号** —— `[V4+ Styles]`（ASS）/ `[V4 Styles]`（SSA）
// 段里的 `Format:` 定义的是样式列，必须一并忽略。`Dialogue:` 按 Format 的**字段数**做 N-1 次逗号
// 切分、**末列吃下剩余全部内容**：ASS 约定文本字段位于末列（它是唯一可含逗号的字段），故 `Text`
// 内部的逗号不会被截断。
//
// 段名 / 键名 / 字段名一律按 **ASCII 大小写不敏感**比较（`[EVENTS]`、`dialogue:`、`START`/`TEXT`
// 均可）。选择这一单一口径而非「部分敏感」是为了不产生分叉；依据与夹具见证据 README。
//
// 只有 `[Events]` 段内的 `Dialogue:` 才产出正文；`Comment:`（不显示）与其他段（含段外的同名行）
// 一律跳过。时间戳是 `H:MM:SS.cc`（centisecond，2 位；`:`/`.` 固定；小时 **1–2 位**），与 SRT 的
// `hh:mm:ss,mmm` **不同** —— 本实现**不接受**逗号毫秒。只解析并取用 **`Start`**（`End` 列**不被读取**）。
[[nodiscard]] constexpr char asciiLower(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32) : value;
}

[[nodiscard]] bool asciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (asciiLower(lhs[index]) != asciiLower(rhs[index])) {
      return false;
    }
  }
  return true;
}

// `[Events]` 形态的段头：trim 后以 `[` 开头、以 `]` 结尾；名称取两侧括号之间的内容（再 trim）。
// 不满足者返回 nullopt（例如索引号/正文/键值行），调用方按非段头处理。
[[nodiscard]] std::optional<std::string_view> assSectionName(std::string_view trimmedLine) noexcept {
  if (trimmedLine.size() < 2U || trimmedLine.front() != '[' || trimmedLine.back() != ']') {
    return std::nullopt;
  }
  return trimAscii(trimmedLine.substr(1U, trimmedLine.size() - 2U));
}

// ASS 时间戳文法（centisecond）：
//   timecode = 小时(1–2 位十进制) ':' 分钟(恰好 2 位且 ≤ 59) ':' 秒(恰好 2 位且 ≤ 59) '.' 厘秒(恰好 2 位)
// 换算 ms = (((h*60 + m)*60 + s)*1000) + cc*10。
//
// 溢出：1–2 位小时 ⇒ h ≤ 99；配合 mm/ss ≤ 59 与 cc 恰 2 位（≤ 99），时间戳上界为
// 359,999,990 ms（见 kAssMaxTimestampMillis 的 static_assert），远在 int64 内，且**不可能**发生
// uint64 乘法回绕 ⇒ 本函数**不**照搬 parseSrtTimecode 的 kMaxTimestampMinutes 运行时上界：那份检查
// 只对「小时位数不受限」的 SRT 文法有意义，照搬到 ASS 会得到一处**在位数字法下不可达**的死检查。
// 本实现改为用编译期 static_assert 证明「无需运行时上界」，该结论因此是可验证的而非仅注释断言。
constexpr std::uint64_t kAssMaxTimestampMillis =
    ((99ULL * 60ULL + 59ULL) * 60ULL + 59ULL) * 1000ULL + 99ULL * 10ULL;
static_assert(kAssMaxTimestampMillis <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
              "ass timestamp grammar must bound the value below int64 max without a runtime overflow guard");

[[nodiscard]] std::optional<std::chrono::milliseconds> parseAssTimecode(std::string_view text) noexcept {
  const auto firstColon = text.find(':');
  if (firstColon == std::string_view::npos || firstColon < 1U || firstColon > 2U) {
    return std::nullopt;
  }
  const auto secondColon = text.find(':', firstColon + 1U);
  if (secondColon == std::string_view::npos) {
    return std::nullopt;
  }
  const auto dot = text.find('.', secondColon + 1U);
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }
  const auto hoursText = text.substr(0, firstColon);
  const auto minutesText = text.substr(firstColon + 1U, secondColon - firstColon - 1U);
  const auto secondsText = text.substr(secondColon + 1U, dot - secondColon - 1U);
  const auto centisText = text.substr(dot + 1U);
  if (minutesText.size() != 2U || secondsText.size() != 2U || centisText.size() != 2U) {
    return std::nullopt;
  }

  std::uint64_t hours = 0;
  std::uint64_t minutes = 0;
  std::uint64_t seconds = 0;
  std::uint64_t centis = 0;
  if (!parseUnsigned(hoursText, hours) || !parseUnsigned(minutesText, minutes) ||
      !parseUnsigned(secondsText, seconds) || !parseUnsigned(centisText, centis)) {
    return std::nullopt;
  }
  if (minutes >= 60U || seconds >= 60U) {
    return std::nullopt;
  }
  // 小时 1–2 位 ⇒ ≤ 99、厘秒恰好 2 位 ⇒ ≤ 99，两者都不需要显式上界检查（见 kAssMaxTimestampMillis）。
  const auto totalMillis = (((hours * 60U) + minutes) * 60U + seconds) * 1000U + (centis * 10U);
  return std::chrono::milliseconds{static_cast<std::int64_t>(totalMillis)};
}

// libass `ass_parse_tags` 在 `\` 之后只跳过 `' '` 与 `'\t'`（`skip_spaces`，`ass_utils.h:123-128`）。
// 该集合与 `strtoll` 的前导空白集合（C 标准 `isspace`）**不同**，两者在本文件里各司其职、不可混用。
[[nodiscard]] constexpr bool isAssTagSpace(char value) noexcept {
  return value == ' ' || value == '\t';
}

// `strtoll` 的前导空白集合（C 标准 `isspace`）。块内实际不可能出现 `'\n'`（解析按行进行），
// 收录全部 6 个只为与 `strtoll` 完全一致。
[[nodiscard]] constexpr bool isCAssIntSpace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\v' || value == '\f' || value == '\r';
}

// 数字实参 → 绘图缩放。镜像 libass `argtoi32`（`ass_parse.c:39-43`）→ `mystrtoi32` → `strtoll`
// （`ass_utils.h:257-262`）：跳过前导 `isspace`、接受**一个**可选 `+`/`-`、再取十进制数字。
// `magnitude` 只区分「0」与「≥1」（`>1` 钳为 1）以避免超长数字串的无界增长。
struct AssDrawingScale {
  bool negative = false;
  std::uint64_t magnitude = 0;
};

[[nodiscard]] AssDrawingScale parseAssDrawingScale(std::string_view text) noexcept {
  AssDrawingScale scale;
  std::size_t index = 0;
  while (index < text.size() && isCAssIntSpace(text[index])) {
    ++index;
  }
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    scale.negative = text[index] == '-';
    ++index;
  }
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    if (scale.magnitude < 1U) {
      scale.magnitude = scale.magnitude * 10U + static_cast<std::uint64_t>(text[index] - '0');
      if (scale.magnitude > 1U) {
        scale.magnitude = 1U;
      }
    }
    ++index;
  }
  return scale;
}

// libass 在匹配标签名**之前**先把**括号参数表**解析进 `args`（`ass_parse.c:310-341`），随后
// `tag("p")` 的 `push_arg` 因 `name_end` 正好在 `(` 处而 `end > start` 不成立、**不 push**
// （`:53-60`）⇒ `argtoi32(*args)` 读到的正是**括号内第一个参数**（故 `\p1(0)` 读 `0`，不是 `1`）。
//
// 切分规则（逐行核对 `:318-341`）：`(` 之后先 `skip_spaces`，再扫到首个 `,` / `\` / `)` / 块尾；
// 若先遇到 `\`，则该「反斜杠实参」吞到**匹配的 `)`**（`:329-338`）—— 故 `\p(1\p0)` 的 `args[0]`
// 是 `1\p0`（`strtoll` 读到 `1` ⇒ 开）。`first` 保持原始区间（不去空白；`strtoll` 自会跳前导空白）。
// libass `push_arg`（`:53-60`）在 `*nargs > MAX_VALID_NARGS`（= 7，`:31`）时**完全不再计数**
// ⇒ `nargs` 最大 8。此处镜像该上限（本实现只用到「`nargs` 是否 > 4」这一区分，而该上限不改变它）。
constexpr std::size_t kAssMaxValidNargs = 7;

struct AssBracketArgs {
  std::string_view first;   // args[0] 的原始区间
  std::string_view inside;  // 括号内全部文本（供 `\t` 递归解析）
  std::size_t afterClose = 0;  // 闭括号之后的位置（无闭括号 ⇒ 块尾）
  std::size_t nargs = 0;       // push_arg 计入的字段个数（镜像 `:53-60`：去尾后非空才 ++）
  bool hasBackslashArg = false;  // 括号内出现 `\`（镜像 `:334`）
};

// `noCloseParen` 断言 `block` 内**不含** `)`（`\t` 括号内文本按构造必然满足，见 `applyAssDrawingOverride`）
// ⇒ 省去 `find(')')` 扫描，逐字节等价（结论都是「无闭括号」）。它只消除闭括号扫描一项；整体线性还
// 依赖压栈前的帧文本右去尾（见 `applyAssDrawingOverride`），二者缺一会退化（详见该函数复杂度说明）。
[[nodiscard]] AssBracketArgs parseAssBracketArgs(std::string_view block, std::size_t openParen,
                                                bool noCloseParen = false) noexcept {
  AssBracketArgs args;
  const auto closeParen = noCloseParen ? std::string_view::npos : block.find(')', openParen + 1U);
  args.afterClose = closeParen == std::string_view::npos ? block.size() : closeParen + 1U;
  const auto insideEnd = closeParen == std::string_view::npos ? block.size() : closeParen;
  args.inside = block.substr(openParen + 1U, insideEnd - (openParen + 1U));

  // 逐字段镜像 `:318-341` + `push_arg`（`:53-60`）：每字段先 `skip_spaces`，再扫到 `,`/`\`/`)`/块尾，
  // 最后 `rskip_spaces` 去尾空白。`push_arg` **只 push 去尾后仍非空的字段** ⇒ `args[0]` 是**第一个
  // 非空字段**（故 `\p(,1)` 的 `args[0]` 是 `1`，不是空串）。遇到 `\` 则本字段吞到闭括号并终止切分。
  std::size_t cursor = openParen + 1U;
  while (cursor < args.afterClose) {
    while (cursor < block.size() && isAssTagSpace(block[cursor])) {
      ++cursor;
    }
    std::size_t fieldEnd = cursor;
    while (fieldEnd < block.size() && block[fieldEnd] != ',' && block[fieldEnd] != '\\' &&
           block[fieldEnd] != ')') {
      ++fieldEnd;
    }
    const bool backslashArg = fieldEnd < block.size() && block[fieldEnd] == '\\';
    if (backslashArg) {
      args.hasBackslashArg = true;  // `:334`
      fieldEnd = insideEnd;         // 反斜杠实参吞到闭括号（`:329-338`）
    }
    std::size_t trimmed = fieldEnd;  // rskip_spaces：只去 `' '`/`'\t'`
    while (trimmed > cursor && isAssTagSpace(block[trimmed - 1U])) {
      --trimmed;
    }
    // 镜像 push_arg（`:53-60`）：超过上限时不再计数，且**只对去尾后非空的字段** `++nargs`。
    // `args[0]` 即首个被计入者（故 `\p(,1)` 的 `args[0]` 是 `1`）。
    if (trimmed > cursor && args.nargs <= kAssMaxValidNargs) {
      if (args.nargs == 0) {
        args.first = block.substr(cursor, trimmed - cursor);
      }
      ++args.nargs;
    }
    if (!backslashArg && fieldEnd < block.size() && block[fieldEnd] == ',') {
      cursor = fieldEnd + 1U;  // 空字段：继续取下一个（push_arg 不 push 空字段）
      continue;
    }
    break;  // `)` / 块尾 / 反斜杠字段之后 ⇒ 切分结束
  }
  return args;
}

// 在**一个覆盖块的内容**里按 `\p<n>` 切换绘图态（`drawing` 由调用方持有、逐块累积）。
//
// 词法镜像 libass `ass_parse_tags`（逐行核对 `libass/ass_parse.c`、`libass/ass_utils.h`）：
//   * `\` 之后先 `skip_spaces`（`:293`，只跳 `' '`/`'\t'`）⇒ `\ p1` 的标签名就是 `p1`。
//   * 标签名终点 `name_end` = 跳过空白后**首个** `(` / `\` / 块尾（`:290-297`）。
//   * 命中 `p` 时按 libass 取值：`\pos`（`complex_tag`，`:606`）与 `\pbo`（`tag`，`:898`）排在
//     `\p`（`tag`，`:901`）**之前**匹配，且 `mystrcmp` 是**前缀**匹配（`:68-78`，不要求分隔符）
//     ⇒ 标签名以 `os`/`bo` 开头者一律**中性**（不触碰 `drawing_scale`）。
//   * 其余 `p<...>` 形态经 `tag("p")` → `argtoi32`（`:39-43`）→ `mystrtoi32`/`strtoll`
//     （`ass_utils.h:257-262`）解析：**≥1 ⇒ 开、==0 ⇒ 关**；负值由 `val = (val < 0) ? 0 : val`
//     （`:903`）钳成 0 ⇒ 关。**无参 / 非数字**时结果为 **0 ⇒ 关闭**：空参数在 `push_arg` 里因
//     `end > start` 不成立而**不 push**（`:53-60`），`args`（声明 `:304`、`""` 初值 `:307-308`），
//     故 `strtoll("") == 0`。实参来源：**括号表优先**（`:310-341` 先于 `:351` 的 tag 宏），
//     否则取 `p` 之后到 `name_end` 的文本。
//   * 其余标签的括号实参只作数据消费、**不**递归 ⇒ 其中的 `\p` 不生效（如 `\fs(\p1)`）；libass
//     唯一的递归点是 `\t`（`:718`），**且仅当 `cnt = nargs-1 ∈ [0,3]` 且括号实参内含 `\`**（`:709`
//     /`:713`，否则 `continue`）⇒ `\t(\p1)` / `\t(0,100,\p1)` 打开绘图态，而 `\t(0,1,2,3,\p1)`
//     （`nargs=5` ⇒ `cnt=4`）**不**打开（libass 整段跳过）。
//
// 本函数只接收**已配对**块的内容（调用方仅在找到 `}` 时以 `substr(index+1, close-index-1)` 调用），
// 故未闭合的 `{` 段按字面保留、其中的 `\p1` 不会生效。
//
// `\t` 的括号内部用**显式帧栈迭代展开**（非 C++ 递归）⇒ 处理深度无上限，调用栈不随输入深度增长。
// 命中 `\t(...)` 时先把「`)` 之后」记为当前帧的续扫位置，再把括号内文本压为新帧（新栈顶 ⇒ 内层
// **先**被处理，之后才回到 `)` 之后）；`drawing` 是跨帧共享的可变状态，效果按出现顺序累积（故
// `\t(\p1\p0)` ⇒ 关、`\t(\p0\p1)` ⇒ 开）。非 `t` 标签的括号只消费、不压栈。
//
// **复杂度**：每帧线性扫描自身文本，故总代价 = 各帧文本长度之和。`noCloseParen` 省掉闭括号扫描，
// 压栈前右去尾省掉尾随空白重扫；**二者齐备时**该和为 O(输入长度)。若去掉右去尾，则 `\t(`×N +
// `\p1` + 空格×M + `)`×N 的每帧都要回扫那 M 个空格 ⇒ O(N·M)（N=M=60000 时量级约 1.5s，
// 去尾后约 2ms；实测数据见 `qa/deep-sweep-r8.txt`）。
void applyAssDrawingOverride(std::string_view block, bool& drawing) {
  // 帧 = 一段待扫文本 + 当前下标。`noCloseParen` 表示本帧文本**不含 `)`**：`\t` 括号内文本是
  // 从 `(` 到**首个** `)` 的区间（两端都不含 `)`）⇒ 按构造必不含 `)`。据此可跳过闭括号扫描。
  struct Frame {
    std::string_view text;
    std::size_t index = 0;
    bool noCloseParen = false;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{block, 0, false});
  while (!stack.empty()) {
    // 值拷贝 text/index：下方 push_back 可能使 vector 重分配，不得长期持有 back() 的引用。
    const std::string_view text = stack.back().text;
    const std::size_t index = stack.back().index;
    if (index >= text.size()) {
      stack.pop_back();
      continue;
    }
    const auto backslash = text.find('\\', index);
    if (backslash == std::string_view::npos) {
      stack.pop_back();
      continue;
    }
    // `:293` —— `\` 之后先 `skip_spaces`（仅 `' '`/`'\t'`）。
    std::size_t cursor = backslash + 1U;
    while (cursor < text.size() && isAssTagSpace(text[cursor])) {
      ++cursor;
    }
    // `:290-297` —— 标签名终点 = 首个 `(` / `\` / 块尾。
    std::size_t nameEnd = cursor;
    while (nameEnd < text.size() && text[nameEnd] != '(' && text[nameEnd] != '\\') {
      ++nameEnd;
    }
    if (nameEnd == cursor) {
      // 空标签名（如 `\\`）：libass `:298` continue（不消费任何实参）。`cursor > index` ⇒ 必前进。
      stack.back().index = cursor;
      continue;
    }
    const auto name = text.substr(cursor, nameEnd - cursor);
    const bool hasBracket = nameEnd < text.size() && text[nameEnd] == '(';
    AssBracketArgs bracketArgs;
    if (hasBracket) {
      bracketArgs = parseAssBracketArgs(text, nameEnd, stack.back().noCloseParen);
    }
    if (name[0] == 'p') {
      // `\pos` / `\pbo`：libass 在 `\p` 之前匹配二者 ⇒ 不改变绘图态。
      const auto rest = name.substr(1U);
      if (!(rest.starts_with("os") || rest.starts_with("bo"))) {
        // 实参来源（libass 顺序：括号表**先** push，非括号文本**后** push；`args[0]` 取先者）。
        // 括号表可能一个都没 push（全空字段，如 `\p1()` / `\p1( )` / `\p1(,)`）—— 此时
        // `args[0]` 落到**标签名之后**的文本（故 `\p1()` 的 `args[0]` 是 `1` ⇒ 开）。
        const auto argument = hasBracket && !bracketArgs.first.empty() ? bracketArgs.first : rest;
        const auto scale = parseAssDrawingScale(argument);
        // 镜像 `:901-903`：`drawing = !negative && scale >= 1`。
        drawing = !scale.negative && scale.magnitude >= 1U;
      }
      stack.back().index = hasBracket ? bracketArgs.afterClose : nameEnd;
      continue;
    }
    if (name[0] == 't') {
      // 先写父帧续扫位置，再压栈 —— 顺序不可颠倒：push_back 后 `back()` 已是**新**帧。
      stack.back().index = hasBracket ? bracketArgs.afterClose : nameEnd;
      // `:672`/`:709`/`:713` —— libass 先算 `cnt = nargs - 1`；**仅当 `cnt ∈ [0,3]` 且括号实参内
      // 含 `\`** 时才递归解析（`:718` 是唯一的递归点），否则 `continue`（内层标签一律不生效）。
      const auto cnt = static_cast<std::ptrdiff_t>(bracketArgs.nargs) - 1;
      if (hasBracket && cnt >= 0 && cnt <= 3 && bracketArgs.hasBackslashArg) {
        // 压栈前按 `push_arg` 的 `rskip_spaces`（`:53-60`，只去 `' '`/`'\t'`）对帧文本右去尾：
        // 否则深嵌套 `\t` + 长尾随空白会因每帧重扫同一段空白而退化为 O(N·M)。
        auto inner = bracketArgs.inside;
        while (!inner.empty() && isAssTagSpace(inner.back())) {
          inner.remove_suffix(1U);
        }
        stack.push_back(Frame{inner, 0, true});
      }
      continue;
    }
    // 其余标签：括号实参作为数据消费、不递归；无括号则只消费标签名。
    stack.back().index = hasBracket ? bracketArgs.afterClose : nameEnd;
  }
}

// 删除正文中所有 ASS 覆盖块 `{\...}`，同时按 `\p<n>` 跟踪绘图态并**丢弃绘图态内的负载**。
// 覆盖块承载特效/定位标签，Aegisub 规格明确也可作行内注释，故整块丢弃；而 `\p1` 之后的内容是矢量
// 图形指令（不渲染为可见文字），故在下一个 `\p0`（或行尾）之前一律丢弃，避免把坐标串当歌词。
//
// 转义感知：`\{` / `\}` 是**字面花括号**（libass `ass_parse.c:1139-1146`），**不是**覆盖块边界；
// 本函数把它们原样保留，交由 `replaceAssEscapes` 还原（故 `\{a\}` 不会被误当覆盖块删掉）。
// 未转义的 `{` 与其后**最近**的 `}` 配对（块内内容整体丢弃）；找不到 `}` 时从该 `{` 起的其余内容
// 按字面保留。未转义的孤立 `}` 亦按字面保留。结构与 stripInlineTimestamps / stripHtmlTags 同构。
[[nodiscard]] std::string stripAssOverrideBlocks(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool drawing = false;
  std::size_t index = 0;
  while (index < text.size()) {
    const char current = text[index];
    if (current == '\\' && index + 1U < text.size() &&
        (text[index + 1U] == '{' || text[index + 1U] == '}')) {
      if (!drawing) {
        out.push_back(current);
        out.push_back(text[index + 1U]);
      }
      index += 2U;
      continue;
    }
    if (current == '{') {
      const auto close = text.find('}', index + 1U);
      if (close == std::string_view::npos) {
        if (!drawing) {
          out.append(text.substr(index));
        }
        index = text.size();
        continue;
      }
      applyAssDrawingOverride(text.substr(index + 1U, close - index - 1U), drawing);
      index = close + 1U;
      continue;
    }
    if (!drawing) {
      out.push_back(current);
    }
    ++index;
  }
  return out;
}

// ASS 的**字符级**转义（不在覆盖块内；`\N` 已由调用方更早切分成多行，故不在此处理）：
//   `\n` 软换行：Aegisub 规格 —— 仅在 wrap style 2 下断行，其余模式**被替换为一个普通空格**。
//        本实现取**空格**：它正是多数 wrap style（0/1/3）下渲染器显示的字符，且不会凭空造出会被
//        D22 误当译文的「同时间戳第二行」。
//   `\h` 硬空格：定义为不换行空格。libass 的 `ass_parse.c` 对 `\h` 返回 `NBSP`（同文件 `#define NBSP 0xa0`）
//        ⇒ 本实现输出 U+00A0 本身（且 trimAscii 不剥 U+00A0，不会被后续 trim 吃掉）。
//   `\{` / `\}` 字面花括号：libass `ass_parse.c:1139-1146` 把二者分别还原为 `{` / `}`（该二条为
//        libass 扩展，Aegisub 页的 Special characters 只列 `\n`/`\N`/`\h`）；本实现按 libass 处理，
//        `\{` 已由 stripAssOverrideBlocks 原样保留至此，故 `\{a\}` ⇒ `{a}` 而非被误当覆盖块删掉。
//   其余 `\x` 一律原样保留 —— libass `ass_get_next_char` 对未列出的 `\x` 不消费转义，返回字面 `\`
//        并只前进 1 字节（`ass_parse.c:1116-1149`）。
[[nodiscard]] std::string replaceAssEscapes(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\\' && index + 1U < text.size()) {
      const char next = text[index + 1U];
      if (next == 'n') {
        out.push_back(' ');
        ++index;
        continue;
      }
      if (next == 'h') {
        out.append("\xC2\xA0");
        ++index;
        continue;
      }
      if (next == '{') {
        out.push_back('{');
        ++index;
        continue;
      }
      if (next == '}') {
        out.push_back('}');
        ++index;
        continue;
      }
    }
    out.push_back(text[index]);
  }
  return out;
}

// 按 `Format:` 的字段名（ASCII 大小写不敏感）解析 `Start` / `Text` 的列下标。字段数决定 `Dialogue:`
// 的切分次数（N-1），下标决定取值位置，故两者都要。同名重复时**取首次出现**的下标。缺 `Start` 或
// 缺 `Text` ⇒ 该 Format 不可用（调用方记一条错误并跳过其 `Dialogue:`）。
struct AssEventFormat {
  std::size_t fieldCount{0};
  std::size_t startIndex{0};
  std::size_t textIndex{0};
  bool usable{false};
};

[[nodiscard]] AssEventFormat resolveAssEventFormat(std::string_view fieldList) {
  AssEventFormat format;
  std::size_t begin = 0;
  bool haveStart = false;
  bool haveText = false;
  while (true) {
    const auto comma = fieldList.find(',', begin);
    const auto field = trimAscii(fieldList.substr(begin, comma == std::string_view::npos ? std::string_view::npos
                                                                                        : comma - begin));
    if (!haveStart && asciiEqualsIgnoreCase(field, "Start")) {
      format.startIndex = format.fieldCount;
      haveStart = true;
    } else if (!haveText && asciiEqualsIgnoreCase(field, "Text")) {
      format.textIndex = format.fieldCount;
      haveText = true;
    }
    ++format.fieldCount;
    if (comma == std::string_view::npos) {
      break;
    }
    begin = comma + 1U;
  }
  format.usable = haveStart && haveText;
  return format;
}

// ── G5d：TTML（.ttml / .dfxp）解析判据 ─────────────────────────────────────
//
// TTML 是 XML 方言（W3C TTML2，2018-11-08）。**仓库没有任何 XML 依赖**
// （`pugixml|tinyxml|libxml|expat|rapidxml|xml2` 在 CMakeLists 中 0 命中），且计划把范围钉死为
// 「只覆盖 `p`/`span` 与 `begin`」⇒ 这里是一个**严格子集的手写扫描器**，**不是**通用 XML 处理器：
// 不做 DTD、不做命名空间解析、不做结构校验。完整行为契约（含逐条 accept/reject 与依据）见
// 证据 `README.md` §2；下面每条注释都可在那里找到出处。
//
// **无参考实现 / 无渲染真值的显式不对称**：`lyric_split_tool.py` 不建模 TTML，也没有 libass 式的
// 真值源（语料 `.ttml`/`.dfxp` = 0）⇒ 唯一对齐对象是 W3C 规格本身；**不得**声称与任何实现或
// 播放器一致（交 todo 38 回填，见 README §0/§3）。

[[nodiscard]] constexpr bool isXmlWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] constexpr bool isAsciiDigit(char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] bool isAllAsciiDigits(std::string_view text) noexcept {
  return !text.empty() && std::ranges::all_of(text, [](char value) { return isAsciiDigit(value); });
}

// XML Name 的**实用近似**：只需切出标签名/属性名，不需要完整 XML Name 字符表。起始字符取
// ASCII 字母 / `_` / `:`，另接受**任何非 ASCII 字节**（XML Name 允许大量 Unicode 字符）；后续
// 另加 数字 / `-` / `.`。非 ASCII 名字不会等于本子集比较的 `p`/`span`/`br`/`begin`（`xmlLocalName`
// 后仍是含非 ASCII 的串）⇒ 按未知元素/未知属性忽略。（R4-F1：此前非 ASCII 不参与名字，会让
// 含非 ASCII 属性名的良构标签走进畸形恢复、把标签尾部泄漏进歌词。）
[[nodiscard]] constexpr bool isXmlNameStart(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_' || value == ':' ||
         static_cast<unsigned char>(value) >= 0x80U;
}

[[nodiscard]] constexpr bool isXmlNameChar(char value) noexcept {
  return isXmlNameStart(value) || isAsciiDigit(value) || value == '-' || value == '.';
}

// 取 local name：**忽略前缀**（README §2.2）⇒ `tt:p` / `ns0:p` / `p` 归为 `p`。
// 理由：正确解命名空间需要 `xmlns` 作用域栈 + URI 解析（＝通用 XML 实现，被计划 Must-NOT 禁止），
// 而本子集只需区分 3 个元素；按 local name 匹配对「有前缀/无前缀/默认命名空间」行为一致且可判负。
[[nodiscard]] std::string_view xmlLocalName(std::string_view name) noexcept {
  const auto colon = name.rfind(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1U);
}

// 命名空间声明（`xmlns` / `xmlns:*`）是**声明**而非内容属性，不参与 `begin` 匹配（R3-F2）：
// 否则 `xmlns:begin="<uri>"` 剥前缀后误命中 `begin`，把 URI 当时间值 ⇒ 整段歌词被跳过。
[[nodiscard]] bool isXmlnsDeclaration(std::string_view name) noexcept {
  return xmlLocalName(name) == "xmlns" || name.starts_with("xmlns:");
}

[[nodiscard]] bool startsWithAt(std::string_view text, std::size_t offset, std::string_view needle) noexcept {
  return offset <= text.size() && text.substr(offset, needle.size()) == needle;
}

[[nodiscard]] std::size_t countNewlines(std::string_view text) noexcept {
  return static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

void appendUtf8CodePoint(std::uint32_t codePoint, std::string& out) {
  if (codePoint < 0x80U) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else if (codePoint < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
  }
}

// 十六进制数字字符引用 `&#x26;`：**不限长度**（XML 允许任意前导零，如 `&#x00000026;` ≡ `&#x26;`，
// 见第 2 轮 N1 ⇒ 不得按长度设上界），仅在**数值溢出 64 bit** 或出现非十六进制字符时返回 false。
// 前导零不推进 `accumulator`，故任意长前导零仍是 O(len) 且不误判。
[[nodiscard]] bool parseHexUnsigned(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) {
    return false;
  }
  constexpr std::uint64_t kShiftOverflowGuard = 0x0FFFFFFFFFFFFFFFULL;  // UINT64_MAX >> 4
  std::uint64_t accumulator = 0;
  for (const char ch : text) {
    std::uint64_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<std::uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<std::uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<std::uint64_t>(ch - 'A' + 10);
    } else {
      return false;
    }
    if (accumulator > kShiftOverflowGuard) {
      return false;  // 再左移 4 位必溢出
    }
    accumulator = (accumulator << 4U) | digit;
  }
  value = accumulator;
  return true;
}

// 值来源：正文（字面空白原样保留，随后由 collapseXmlWhitespace 折叠）或属性值
// （字面 `#x9`/`#xA`/`#xD` 按 XML 1.0 §3.3.3 属性值归一化折成空格 `#x20`）。
enum class XmlValueKind { TextContent, AttributeValue };

// XML 实体还原（README §2.5）：5 个预定义实体 + 十进制/十六进制数字字符引用。
// **未定义实体**（`&nbsp;`）与**不构成实体的 `&`**（`a & b`、`&;`、孤立 `&`）按**原样保留**处置：
// 报错会丢整行文本（违反 Must-NOT「不丢弃文本」）。U+0000 与代理区间（U+D800..DFFF）同样原样保留。
// 属性值另按 XML §3.3.3 归一**字面**空白；字符引用（`&#10;`）产生的字符不归一（XML 同款豁免）。
//
// 实体名不设长度上界（第 2 轮 N1）：XML 允许前导零（`&#000000038;` ≡ `&#38;`），故按「第一个 `;`」
// 定位名字，与长度无关。复杂度 O(N)：唯一慢点是 `find(';')` 回扫，故缓存 `nextSemi`（仅当已越过
// `index+1` 才重算）；`index` 单调不减、重扫各自从上次命中之后开始 ⇒ 各次 `find` 区间互不重叠。
void appendXmlDecoded(std::string_view raw, std::string& out, XmlValueKind kind) {
  std::size_t index = 0;
  std::size_t nextSemi = raw.find(';');
  while (index < raw.size()) {
    const char ch = raw[index];
    if (ch != '&') {
      out.push_back(kind == XmlValueKind::AttributeValue && isXmlWhitespace(ch) ? ' ' : ch);
      ++index;
      continue;
    }
    if (nextSemi != std::string_view::npos && nextSemi < index + 1U) {
      nextSemi = raw.find(';', index + 1U);  // 缓存已越过 ⇒ 单调重扫（各次区间互不重叠）
    }
    if (nextSemi == std::string_view::npos) {
      out.push_back('&');  // 其后已无 `;` ⇒ 不构成任何实体
      ++index;
      continue;
    }
    const auto semicolon = nextSemi;
    const auto entity = raw.substr(index + 1U, semicolon - (index + 1U));
    if (entity == "amp") {
      out.push_back('&');
      index = semicolon + 1U;
      continue;
    }
    if (entity == "lt") {
      out.push_back('<');
      index = semicolon + 1U;
      continue;
    }
    if (entity == "gt") {
      out.push_back('>');
      index = semicolon + 1U;
      continue;
    }
    if (entity == "quot") {
      out.push_back('"');
      index = semicolon + 1U;
      continue;
    }
    if (entity == "apos") {
      out.push_back('\'');
      index = semicolon + 1U;
      continue;
    }
    if (entity.size() > 1U && entity.front() == '#') {
      const bool hexadecimal = entity[1] == 'x' || entity[1] == 'X';
      const auto digits = entity.substr(hexadecimal ? 2U : 1U);
      std::uint64_t codePoint = 0;
      const bool parsed = hexadecimal ? parseHexUnsigned(digits, codePoint) : parseUnsigned(digits, codePoint);
      if (parsed && codePoint != 0U && codePoint <= 0x10FFFFU &&
          !(codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
        appendUtf8CodePoint(static_cast<std::uint32_t>(codePoint), out);
        index = semicolon + 1U;
        continue;
      }
    }
    out.push_back('&');  // 未定义 / 非法 ⇒ 只消费 `&`，其后字符在后续迭代按字面复制
    ++index;
  }
}

// ASCII 空白折叠（README §2.4）：把连续 `' '`/`'\t'`/`'\n'`/`'\r'` 折成**一个** `' '`，并去首尾。
// 依据 TTML2 默认 `xml:space="default"`（呈现时折叠空白）；同时消除「美化缩进换行」被误当歌词换行。
// 无 `pendingSpace` 在 `out` 为空时不落空格 ⇒ 前导空白自然被去掉；末尾 pending 不落 ⇒ 尾部也去掉。
// **残余差异已登记**：不区分 `xml:space="preserve"`（折叠）。
[[nodiscard]] std::string collapseXmlWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (const char ch : text) {
    if (isXmlWhitespace(ch)) {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty()) {
      out.push_back(' ');
    }
    pendingSpace = false;
    out.push_back(ch);
  }
  return out;
}

// 时间上界：与 `parseTimestamp` 的 kMaxTimestampMinutes 成对（F2 不变式）⇒ 被接受的合法时间戳
// 落在 [0, int64 上限)；「< 0 ⟺ unsynced 哨兵」据此成立。
constexpr std::uint64_t kMaxTtmlTimestampMs = kMaxTimestampMinutes * 60'000ULL + 59'999ULL;

// `fraction` 的毫秒值：取前 3 位十进制、右补零（`12.5` ⇒ 500、`12.50` ⇒ 500、`12.5001` ⇒ 500，
// 即第 4 位起截断）。对 offset-time 而言返回的是「一个度量单位的小数千分位」，调用方再乘 unit/1000。
[[nodiscard]] std::uint64_t fractionToMillis(std::string_view fraction) noexcept {
  std::uint64_t digits = 0;
  std::size_t taken = 0;
  for (; taken < 3U && taken < fraction.size(); ++taken) {
    digits = digits * 10U + static_cast<std::uint64_t>(fraction[taken] - '0');
  }
  for (; taken < 3U; ++taken) {
    digits *= 10U;
  }
  return digits;
}

// clock-time（README §2.7 接受表）：`hours ":" minutes ":" seconds [ "." fraction ]`
//   hours   ：≥2 位十进制（TTML2 的 `hours` 是「≥2 位」，非「恰好 2 位」⇒ `100:00:00.000` 合法）
//   minutes ：恰好 2 位且 ≤ 59 ；seconds ：恰好 2 位且 ≤ 59
//   fraction：`.` + ≥1 位十进制（十进制秒的小数，**不是**帧）
// **拒绝**：两段式 `mm:ss`（TTML2 §12.3.1 的 clock-time 产生式要求 h:m:s）、帧形式
// `h:m:s:ff[.sf]`（帧→毫秒需要 `ttp:frameRate`/`ttp:subFrameRate`，本子集不建模参数 ⇒ 猜帧率会
// **静默给出错误时间戳**，那是字幕解析最坏的失效模式 ⇒ 拒绝好于猜）。
[[nodiscard]] std::optional<std::chrono::milliseconds> parseTtmlClockTime(std::string_view text) noexcept {
  const auto first = text.find(':');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  const auto second = text.find(':', first + 1U);
  if (second == std::string_view::npos) {
    return std::nullopt;
  }
  const auto hoursText = text.substr(0, first);
  const auto minutesText = text.substr(first + 1U, second - first - 1U);
  const auto rest = text.substr(second + 1U);
  const auto dot = rest.find('.');
  const auto secondsText = rest.substr(0, dot == std::string_view::npos ? std::string_view::npos : dot);
  if (hoursText.size() < 2U || minutesText.size() != 2U || secondsText.size() != 2U) {
    return std::nullopt;
  }
  if (!isAllAsciiDigits(hoursText) || !isAllAsciiDigits(minutesText) || !isAllAsciiDigits(secondsText)) {
    return std::nullopt;
  }
  std::uint64_t hours = 0;
  std::uint64_t minutes = 0;
  std::uint64_t seconds = 0;
  if (!parseUnsigned(hoursText, hours) || !parseUnsigned(minutesText, minutes) ||
      !parseUnsigned(secondsText, seconds)) {
    return std::nullopt;
  }
  if (minutes >= 60U || seconds >= 60U) {
    return std::nullopt;
  }
  std::uint64_t fractionMs = 0;
  if (dot != std::string_view::npos) {
    const auto fraction = rest.substr(dot + 1U);
    if (!isAllAsciiDigits(fraction)) {
      return std::nullopt;  // 空 fraction 与含非数字者都在此被拒
    }
    fractionMs = fractionToMillis(fraction);
  }
  if (hours > kMaxTimestampMinutes / 60U) {
    return std::nullopt;  // 先挡 `hours*60` 回绕，再过总分钟上界
  }
  const std::uint64_t totalMinutes = hours * 60U + minutes;
  if (totalMinutes > kMaxTimestampMinutes) {
    return std::nullopt;
  }
  const std::uint64_t totalMs = (totalMinutes * 60U + seconds) * 1000U + fractionMs;
  return std::chrono::milliseconds{static_cast<std::int64_t>(totalMs)};
}

// offset-time（README §2.7 接受表）：`count [ "." fraction ] metric`，metric ∈ {h, m, s, ms}。
// **拒绝**：`f`/`t`（需 `ttp:frameRate`/`ttp:tickRate`）、无度量（TTML2 §12.3.1 要求度量）、
// 任何符号（结果须 ≥ 0；带符号的相对时间需要参照系，与帧形式同类理由）。
// 度量匹配**先 `ms` 再单字符**，否则 `12500ms` 会被读成 `m` 度量 + 尾随 `s`。
[[nodiscard]] std::optional<std::chrono::milliseconds> parseTtmlOffsetTime(std::string_view text) noexcept {
  std::uint64_t unitMs = 0;
  if (text.ends_with("ms")) {
    unitMs = 1U;
    text.remove_suffix(2U);
  } else if (text.ends_with("h")) {
    unitMs = 3'600'000U;
    text.remove_suffix(1U);
  } else if (text.ends_with("m")) {
    unitMs = 60'000U;
    text.remove_suffix(1U);
  } else if (text.ends_with("s")) {
    unitMs = 1'000U;
    text.remove_suffix(1U);
  } else {
    return std::nullopt;
  }
  const auto dot = text.find('.');
  const auto countText = text.substr(0, dot == std::string_view::npos ? std::string_view::npos : dot);
  if (!isAllAsciiDigits(countText)) {
    return std::nullopt;  // 空 count 与含符号/非数字者都在此被拒
  }
  std::uint64_t count = 0;
  if (!parseUnsigned(countText, count)) {
    return std::nullopt;
  }
  std::uint64_t thousandths = 0;
  if (dot != std::string_view::npos) {
    const auto fraction = text.substr(dot + 1U);
    if (!isAllAsciiDigits(fraction)) {
      return std::nullopt;
    }
    thousandths = fractionToMillis(fraction);
  }
  if (count > kMaxTtmlTimestampMs / unitMs) {
    return std::nullopt;  // 挡 `count*unitMs` 回绕
  }
  const std::uint64_t base = count * unitMs;
  const std::uint64_t fractionMs = thousandths * unitMs / 1000U;
  if (base > kMaxTtmlTimestampMs - fractionMs) {
    return std::nullopt;
  }
  return std::chrono::milliseconds{static_cast<std::int64_t>(base + fractionMs)};
}

// 剥除首尾 ASCII 空白（`' '`/`\t`/`\n`/`\r`）。**TTML 专用**：不复用也不改动全局
// trimAscii —— 后者不含 `\n`，且被 LRC/SRT/ASS/plain-text 四条路径共用，动它会把这处修复的
// blast radius 扩到四条无关路径（F1 明确要求只作用于属性值/属性解析这一侧）。
[[nodiscard]] std::string_view trimXmlWhitespace(std::string_view value) noexcept {
  while (!value.empty() && isXmlWhitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && isXmlWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

// `begin` 值文法分派（README §2.7）：含 `:` ⇒ clock-time，否则 offset-time。
// 首尾 ASCII 空白在本子集中无意义：**字面**空白已由 scanTtmlTagTail 按 XML 1.0 §3.3.3 归一为
// 空格，这里再剥掉首尾 ASCII 空白（含字符引用产生的 `\n`，如 `begin="1s&#10;"`）。XML §3.3.3
// 对字符引用有豁免，本子集有意放宽（宁可接受，不因一个空白丢整段歌词）；该放宽已登记于 README §10。
[[nodiscard]] std::optional<std::chrono::milliseconds> parseTtmlBegin(std::string_view text) noexcept {
  const auto value = trimXmlWhitespace(text);
  if (value.empty()) {
    return std::nullopt;
  }
  return value.find(':') == std::string_view::npos ? parseTtmlOffsetTime(value) : parseTtmlClockTime(value);
}

struct TtmlTagScan {
  bool closed{false};
  bool selfClosing{false};
  bool hasBegin{false};
  // `closed` 但标签**畸形**（属性扫描在某个 `>` 之前失败）：调用方消费到该 `>` 并记一条错误，
  // 而不是当作文档截断。与 `!closed`（其后已无 `>`，真截断）区分，见 scanTtmlTagTail。
  bool malformed{false};
  std::string beginValue;   // 已做实体还原的 `begin` 值（README §2.6）
  std::size_t end{0};       // 越过 `>` 的下标；未闭合时无意义（调用方必须先看 closed）
};

// ── 共享「构造终止」判定（R7-F1 / R8-F1 / R8-F3）──────────────────────────────
//
// 每类 XML 构造都有一个正常终止符：标签与 `<!` 声明是 `>`，注释 `-->`，CDATA `]]>`，PI `?>`。
// 本函数是各构造分支**唯一**的终止判定，分支只传自己的规则，不再各写一遍扫描。
//
// 单字符 `>` 规则（标签 / `<!` 声明）：构造在「引号外 `>`」与「引号外裸 `<`」中先到者处结束；
// 标签内部不允许出现裸 `<`（XML 语法）⇒ 遇到它即说明本构造已结束、该 `<` 属于下一个构造。
// `<!` 另加一条（R8-F3）：裸 `</`
// 终止，**不受引号与 `[ ... ]` 影响** —— `</` 是关闭 `<p>` 的标签形式，被 `"..."` 或内部
// 子集跳过就会吞掉段边界，后段并入前段。
//
// 多字符规则（注释 / CDATA / PI）：XML 语义下正文里的 `<`、`>`、引号、`</` 都只是字符，只有
// 终止符结束构造 ⇒ **终止符优先**（R8-F1）；仅当终止符在 `from` 之后确实不存在时，才退回第一个
// 裸 `<`，让其后文档（含 `</p>`）重新可见。
//
// 终止符存在性的判断必须是摊还常数时间：某次向前查找失败后记下「自此之后不存在」（`XmlEndMemo`），
// 后续调用直接回退 —— 若每次构造都扫到文档尾，重复的未闭合构造会退化成 Θ(N²)。
//
// `end` 语义：`Terminated` ⇒ 越过终止符；`RecoveredAtLt` ⇒ 该裸 `<` 的下标（**不越过**，调用方
// 从它继续，`</p>` 才重新可见）；`Unterminated` ⇒ 等于 `from`，调用方按文档截断处理。
enum class XmlEndKind { Terminated, RecoveredAtLt, Unterminated };

struct XmlEnd {
  XmlEndKind kind = XmlEndKind::Unterminated;
  std::size_t end = 0U;
};

// 多字符终止符的「缺失下界」：某次向前查找失败后置为该起点，表示该终止符自此之后不再出现
// （起点随文档推进单调递增，故后一次查找的起点不小于它 ⇒ 可直接回退）。
struct XmlEndMemo {
  std::size_t commentAbsent{std::string_view::npos};  // `-->`
  std::size_t cdataAbsent{std::string_view::npos};    // `]]>`
  std::size_t piAbsent{std::string_view::npos};       // `?>`
};

struct XmlEndRule {
  std::string_view closer;      // 正常终止符
  bool quotes{false};           // 跳过成对引号内的内容（标签/声明的属性值与字面量可为引号包裹）
  bool brackets{false};         // 跳过 `[ ... ]` 内部子集（`<!DOCTYPE` 的 `[...]`）
  bool breaksAtLtSlash{false};  // 裸 `</` 终止，且不受引号/子集影响（`<!`，R8-F3）
  std::size_t XmlEndMemo::* absent{nullptr};  // 多字符终止符的缺失下界槽位（单字符规则为 nullptr）
};

[[nodiscard]] XmlEnd scanXmlEnd(std::string_view text, std::size_t from, XmlEndRule rule,
                                XmlEndMemo& memo) {
  if (rule.closer != ">") {
    if (rule.absent != nullptr && from >= memo.*rule.absent) {
      // 已知该终止符自此之后不存在 ⇒ 不必再扫。
    } else {
      const auto close = text.find(rule.closer, from);
      if (close != std::string_view::npos) {
        return {XmlEndKind::Terminated, close + rule.closer.size()};
      }
      if (rule.absent != nullptr) {
        memo.*rule.absent = from;
      }
    }
    const auto lt = text.find('<', from);
    if (lt != std::string_view::npos) {
      return {XmlEndKind::RecoveredAtLt, lt};
    }
    return {XmlEndKind::Unterminated, from};
  }

  char quote = '\0';
  std::size_t bracketDepth = 0U;
  for (std::size_t i = from; i < text.size(); ++i) {
    const char ch = text[i];
    if (rule.breaksAtLtSlash && ch == '<' && i + 1U < text.size() && text[i + 1U] == '/') {
      return {XmlEndKind::RecoveredAtLt, i};  // `</` 终止，不看引号/子集（R8-F3）
    }
    if (quote != '\0') {
      if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (rule.quotes && (ch == '"' || ch == '\'')) {
      quote = ch;
      continue;
    }
    if (rule.brackets) {
      if (ch == '[') {
        ++bracketDepth;
      } else if (ch == ']' && bracketDepth > 0U) {
        --bracketDepth;
      } else if (bracketDepth > 0U) {
        continue;  // 内部子集内的 `>`/`<` 不结束声明
      }
    }
    if (ch == '>') {
      return {XmlEndKind::Terminated, i + 1U};
    }
    if (ch == '<') {
      return {XmlEndKind::RecoveredAtLt, i};
    }
  }
  return {XmlEndKind::Unterminated, from};
}

// 从 `cursor`（标签名之后）扫描属性直到 `>` / `/>`。**引号内的 `>` 不结束标签**（XML 允许属性值含 `>`）。
// 只对 `begin` 做处理，其余属性不解析。`begin` 区分大小写、重复出现取**第一个**。
// 属性名排除命名空间声明（`xmlns` / `xmlns:*`）（R3-F2）；对其余属性按 local name 比较（忽略前缀）
// ⇒ `tt:begin` 与无前缀写法等价。边界：本子集不做命名空间解析（`xmlns` 作用域 + URI 对照＝通用 XML
// 实现，被计划 Must-NOT 禁止），故 `任意前缀:begin`（非 `xmlns:*`）等同 `begin`；若某文档用其它
// 命名空间定义同名属性则会误读，属已登记的不对称（README §2.6/§10）。
//
// 返回语义（R3-F1/R5-F1）：属性扫描失败时，用共享判定找标签终点（`closer == ">"` ⇒ 取「引号外 `>`」
// 与「引号外裸 `<`」先到者）。已终止 ⇒ `closed=true, malformed=true`；未终止 ⇒ `closed=false`
// （调用方按文档截断处理）。
[[nodiscard]] TtmlTagScan scanTtmlTagTail(std::string_view text, std::size_t cursor,
                                          XmlEndMemo& memo) {
  TtmlTagScan scan;
  const auto malformedAt = [&text, &memo](std::size_t from) {
    TtmlTagScan out;
    const auto found = scanXmlEnd(text, from, XmlEndRule{.closer = ">", .quotes = true}, memo);
    if (found.kind != XmlEndKind::Unterminated) {
      out.closed = true;
      out.malformed = true;
      out.end = found.end;
    } else {
      out.end = from;
    }
    return out;
  };
  while (cursor < text.size()) {
    while (cursor < text.size() && isXmlWhitespace(text[cursor])) {
      ++cursor;
    }
    if (cursor >= text.size()) {
      break;
    }
    if (text[cursor] == '>') {
      scan.closed = true;
      scan.end = cursor + 1U;
      return scan;
    }
    if (text[cursor] == '/') {
      ++cursor;
      while (cursor < text.size() && isXmlWhitespace(text[cursor])) {
        ++cursor;
      }
      if (cursor < text.size() && text[cursor] == '>') {
        scan.closed = true;
        scan.selfClosing = true;
        scan.end = cursor + 1U;
        return scan;
      }
      return malformedAt(cursor);
    }
    if (!isXmlNameStart(text[cursor])) {
      return malformedAt(cursor);  // 属性名首字符非法 ⇒ 畸形
    }
    const auto nameStart = cursor;
    while (cursor < text.size() && isXmlNameChar(text[cursor])) {
      ++cursor;
    }
    const auto name = text.substr(nameStart, cursor - nameStart);
    while (cursor < text.size() && isXmlWhitespace(text[cursor])) {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '=') {
      continue;  // 无值属性（XML 非法）：跳过它，继续解析后续属性
    }
    ++cursor;
    while (cursor < text.size() && isXmlWhitespace(text[cursor])) {
      ++cursor;
    }
    if (cursor >= text.size() || (text[cursor] != '"' && text[cursor] != '\'')) {
      return malformedAt(cursor);  // 缺引号 ⇒ 畸形
    }
    const char quote = text[cursor];
    ++cursor;
    const auto valueStart = cursor;
    while (cursor < text.size() && text[cursor] != quote) {
      ++cursor;
    }
    if (cursor >= text.size()) {
      // 引号未闭合到末尾 ⇒ 未终止（截断）：属性值在 XML 里延伸到下一个同种引号，缺之即文档未闭合。
      scan.end = text.size();
      return scan;
    }
    if (!isXmlnsDeclaration(name) && xmlLocalName(name) == "begin" && !scan.hasBegin) {
      scan.hasBegin = true;
      scan.beginValue.clear();
      // XML 1.0 §3.3.3 属性值归一化：**字面** `#x9`/`#xA`/`#xD` 折成空格（F1：否则良构 XML 的
      // `begin="1s\n"` 会残留 `\n` 而被判定畸形、整段歌词被丢弃）。字符引用产生的字符不归一。
      appendXmlDecoded(text.substr(valueStart, cursor - valueStart), scan.beginValue,
                       XmlValueKind::AttributeValue);
    }
    ++cursor;
  }
  scan.end = cursor;
  return scan;
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
      // G6：无时间戳行同样过元数据/制作人员判据（否则与 `cleanLine` 分叉）；只有命中判据的
      // 才丢，整首纯文本歌词不被误杀。同样剥离行内 `[tag:value]`。
      const auto unsyncedBody = removeInlineTags(trimAscii(line));
      if (!isDroppableLyricBody(unsyncedBody)) {
        pendingUnsynced.emplace_back(trimAscii(unsyncedBody));
      }
      continue;
    }
    if (timestamps.empty()) {
      // 带 `[...]` 但无有效时间戳（元数据 / offset 标签）或该行时间戳畸形：保持既有跳过语义，不收集。
      continue;
    }

    // 剥离顺序遵循设计文档 §6.3（P1 陷阱）：先时间戳（上方行首 `[...]` 循环）→ 再行内
    // `[tag:value]`（removeInlineTags）→ 最后判制作人员/元数据（isDroppableLyricBody）。
    // 顺序颠倒会让 `[00:00.000][by:随遇不安_]` / `[00:00.000]作词 : みゅー [by:x]` 漏过过滤。
    const auto bodyWithoutInlineTags = removeInlineTags(trimAscii(line.substr(cursor)));
    if (isDroppableLyricBody(bodyWithoutInlineTags)) {
      continue;
    }
    // 逐字标签剥离在 G6 判据**之后**：只作用于真正产出的正文（见 isDroppableLyricBody 注释）。
    const auto lyricText = stripInlineTimestamps(trimAscii(bodyWithoutInlineTags));
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

std::optional<std::string> decodeLyricsBytes(std::string_view bytes) {
  // S10 F3：ICU 的 sourceLength 是 int32_t，负值语义 = **NUL 终止扫描**；故 > INT32_MAX 的输入
  // 会因截断成负长度而越过 bytes 末尾（string_view 不保证终止 NUL）⇒ 越界读。此处显式拒绝。
  // 对任何 ≤ 2 GiB 的输入行为不变（既有断言与语义均不受影响）。
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }

  // utf-16 BOM 前缀直达：Python 的五个前序 codec 对 FF FE / FE FF 前缀一律拒绝（末步性质），
  // 而 ICU 的 Big5 视 0xFF 为合法前导字节 ⇒ 不预判则 `FF FE` + ASCII 载荷会被 Big5 抢先解出、
  // utf-16 分支永不抵达，与 Python 分歧。此逻辑与 tools/lyric_split_dump/lyric_split_dump.cpp
  // 的 decodeWithFallback 一致。
  if (hasBytesPrefix(bytes, 0xFFU, 0xFEU) || hasBytesPrefix(bytes, 0xFEU, 0xFFU)) {
    std::string decoded;
    if (decodePlainTextUtf16(bytes, decoded)) {
      return decoded;
    }
    return std::nullopt;
  }

  for (std::size_t index = 0; index < kPlainTextEncodings.size(); ++index) {
    std::string decoded;
    if (index == 0U) {
      std::string_view body = bytes;
      if (body.size() >= kUtf8Bom.size() && body.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        body.remove_prefix(kUtf8Bom.size());
      }
      if (decodeStrictIcu(body, kPlainTextIcuNames[0], decoded)) {
        return decoded;
      }
    } else if (index <= kPlainTextIcuNames.size()) {
      if (decodeStrictIcu(bytes, kPlainTextIcuNames[index - 1U], decoded)) {
        return decoded;
      }
    } else {
      // utf-16：无 BOM（有 BOM 已在函数开头直达并返回）。
      if (decodePlainTextUtf16(bytes, decoded)) {
        return decoded;
      }
    }
  }
  return std::nullopt;
}

LrcParseResult parsePlainTextLyrics(std::string text, const LrcParseOptions& options,
                                    std::optional<std::filesystem::path> path) {
  LrcParseResult result;
  if (text.size() > options.maxBytes) {
    result.errors.push_back(makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                      "plain text lyrics exceed scanner limit"));
    spdlog::warn("plain text parse: content exceeds size limit ({})",
                 path.has_value() ? pathToUtf8(*path) : "<text>");
    return result;
  }

  normalizeNewlines(text);
  std::size_t lineNumber = 0;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    if (lineNumber >= options.maxLines) {
      result.errors.push_back(makeError(LrcParseErrorCode::TooManyLines, path, lineNumber + 1U, 0U,
                                        "plain text lyrics exceed scanner line limit"));
      break;
    }

    const auto next = text.find('\n', offset);
    const auto line = std::string_view{text}.substr(offset, next == std::string_view::npos ? std::string_view::npos
                                                                                          : next - offset);
    ++lineNumber;
    offset = next == std::string_view::npos ? text.size() + 1U : next + 1U;
    if (trimAscii(line).empty()) {
      continue;
    }

    // 与 parseLrcText 的 G4 无时间戳行同一判据（剥行内 `[tag:value]` → G6 空/元数据/制作人员），
    // 使纯文本侧车与 `.lrc` 的整首无时间戳路径同构；不额外做逐字标签剥离（G4 路径亦不做）。
    const auto body = removeInlineTags(trimAscii(line));
    if (isDroppableLyricBody(body)) {
      continue;
    }
    result.lines.push_back({.timestamp = kUnsyncedTimestamp, .text = std::string{trimAscii(body)}});
  }
  return result;
}

LrcParseResult parsePlainTextLyricsFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    spdlog::warn("plain text parse: failed to stat {} ({})", pathToUtf8(path), error.message());
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U,
                                 "failed to stat plain text lyrics file", error.message())}};
  }
  if (size > options.maxBytes) {
    spdlog::warn("plain text parse: file exceeds size limit ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                 "plain text lyrics exceed scanner limit")}};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    spdlog::warn("plain text parse: failed to open {}", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U,
                                 "failed to open plain text lyrics file")}};
  }

  std::string bytes;
  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

  auto decoded = decodeLyricsBytes(bytes);
  if (!decoded.has_value()) {
    spdlog::warn("plain text parse: unsupported encoding, skipping ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::UnsupportedEncoding, path, 0U, 0U,
                                 "plain text lyrics encoding is not in the supported fallback set",
                                 "utf-8-sig/utf-8/gb18030/big5/shift_jis/utf-16 all failed")}};
  }
  return parsePlainTextLyrics(std::move(*decoded), options, path);
}

LrcParseResult parseSrtLyrics(std::string text, const LrcParseOptions& options,
                              std::optional<std::filesystem::path> path) {
  LrcParseResult result;
  if (text.size() > options.maxBytes) {
    result.errors.push_back(makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                      "srt lyrics exceed scanner limit"));
    spdlog::warn("srt parse: content exceeds size limit ({})", path.has_value() ? pathToUtf8(*path) : "<text>");
    return result;
  }

  normalizeNewlines(text);
  std::size_t lineNumber = 0;
  std::size_t offset = 0;

  // 逻辑块状态：blockStart 有值 ⇒ 已见到本块时间轴，blockTexts 是其后的原始文本行（trim 后）。
  // 未见到时间轴却先出现正文 ⇒ malformedTextBeforeTiming（畸形块，flush 时记一条错误且不产出）。
  // 块终结：**空行**与**时间轴行**各自调用 flushBlock（见下方两处），故两者都终结当前块；**序号行不终结**
  // （只跳过，见下方分支），块已开时其后的正文行仍归入该块。仅凭「时间轴行必终结」就足以让「缺空行
  // 分隔」的非标准文件正确分块；且时间轴行在其分支里一律 continue，绝不会被收集为正文。
  std::optional<std::chrono::milliseconds> blockStart;
  std::vector<std::string_view> blockTexts;
  std::size_t blockLine = 0;
  bool malformedTextBeforeTiming = false;
  std::string_view malformedFirstText;

  const auto flushBlock = [&]() {
    if (blockStart.has_value()) {
      for (const auto raw : blockTexts) {
        // 必须先绑定 stripHtmlTags 的返回（std::string 临时量），再取 trimAscii 的 view；
        // 直接写 `trimAscii(stripHtmlTags(raw))` 会让 view 指向语句结束即析构的临时量（悬垂）。
        const auto strippedText = stripHtmlTags(raw);
        const auto stripped = trimAscii(strippedText);
        // HTML 标签剥离后为空的行不产出（空文本块语义；不制造空 LyricLine）。
        if (stripped.empty()) {
          continue;
        }
        result.lines.push_back({.timestamp = *blockStart, .text = std::string{stripped}});
      }
    } else if (malformedTextBeforeTiming) {
      result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, blockLine, 0U,
                                        "srt block is missing a timeline", std::string{malformedFirstText}));
      spdlog::debug("srt parse: block without timeline at line {}", blockLine);
    }
    blockStart.reset();
    blockTexts.clear();
    blockLine = 0;
    malformedTextBeforeTiming = false;
    malformedFirstText = {};
  };

  while (offset <= text.size()) {
    if (lineNumber >= options.maxLines) {
      result.errors.push_back(makeError(LrcParseErrorCode::TooManyLines, path, lineNumber + 1U, 0U,
                                        "srt lyrics exceed scanner line limit"));
      break;
    }

    const auto next = text.find('\n', offset);
    const auto line = std::string_view{text}.substr(offset, next == std::string_view::npos ? std::string_view::npos
                                                                                          : next - offset);
    ++lineNumber;
    offset = next == std::string_view::npos ? text.size() + 1U : next + 1U;
    const auto trimmed = trimAscii(line);

    if (trimmed.empty()) {
      flushBlock();
      continue;
    }

    if (trimmed.find("-->") != std::string_view::npos) {
      // 含 `-->` 的行一律视为结构行：先校验时间轴（纯计算，不读块状态），再终结当前块。畸形时间轴
      // （如点号分隔）记一条错误并丢弃其后直到下一个块边界的内容，绝不把该行本身收集为正文。
      const auto start = parseSrtTimingLine(trimmed);
      flushBlock();
      if (start.has_value()) {
        blockStart = *start;
        blockLine = lineNumber;
      } else {
        result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, 0U,
                                          "srt timeline is malformed", std::string{trimmed}));
        spdlog::debug("srt parse: malformed timeline at line {}", lineNumber);
      }
      continue;
    }

    if (isSrtIndexLine(trimmed)) {
      // 序号行：跳过且不终结当前块（缺空行时下一个块的时间轴行会终结它）。永不产出正文。
      continue;
    }

    if (blockStart.has_value()) {
      blockTexts.push_back(trimmed);
    } else {
      if (!malformedTextBeforeTiming) {
        blockLine = lineNumber;
        malformedFirstText = trimmed;
      }
      malformedTextBeforeTiming = true;
    }
  }
  flushBlock();

  // 与 parseLrcText 同构：只按时间戳稳定排序。同一多行块的各行共享起始时间戳 ⇒ 保持块内文件顺序，
  // 下游 D22 配对（组内第 1 行 = 原文）在乱序输入下仍可确定。
  std::ranges::stable_sort(result.lines, [](const LyricLine& lhs, const LyricLine& rhs) {
    return lhs.timestamp < rhs.timestamp;
  });
  return result;
}

LrcParseResult parseSrtLyricsFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    spdlog::warn("srt parse: failed to stat {} ({})", pathToUtf8(path), error.message());
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to stat srt lyrics file",
                                 error.message())}};
  }
  if (size > options.maxBytes) {
    spdlog::warn("srt parse: file exceeds size limit ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                 "srt lyrics exceed scanner limit")}};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    spdlog::warn("srt parse: failed to open {}", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to open srt lyrics file")}};
  }

  std::string bytes;
  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

  auto decoded = decodeLyricsBytes(bytes);
  if (!decoded.has_value()) {
    spdlog::warn("srt parse: unsupported encoding, skipping ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::UnsupportedEncoding, path, 0U, 0U,
                                 "srt lyrics encoding is not in the supported fallback set",
                                 "utf-8-sig/utf-8/gb18030/big5/shift_jis/utf-16 all failed")}};
  }
  return parseSrtLyrics(std::move(*decoded), options, path);
}

LrcParseResult parseAssLyrics(std::string text, const LrcParseOptions& options,
                              std::optional<std::filesystem::path> path) {
  LrcParseResult result;
  if (text.size() > options.maxBytes) {
    result.errors.push_back(makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                      "ass lyrics exceed scanner limit"));
    spdlog::warn("ass parse: content exceeds size limit ({})", path.has_value() ? pathToUtf8(*path) : "<text>");
    return result;
  }

  normalizeNewlines(text);
  bool inEvents = false;
  bool haveFormat = false;
  AssEventFormat format;
  std::size_t lineNumber = 0;
  std::size_t offset = 0;
  std::vector<std::string_view> fields;

  while (offset <= text.size()) {
    if (lineNumber >= options.maxLines) {
      result.errors.push_back(makeError(LrcParseErrorCode::TooManyLines, path, lineNumber + 1U, 0U,
                                        "ass lyrics exceed scanner line limit"));
      break;
    }

    const auto next = text.find('\n', offset);
    const auto line = std::string_view{text}.substr(offset, next == std::string_view::npos ? std::string_view::npos
                                                                                          : next - offset);
    ++lineNumber;
    offset = next == std::string_view::npos ? text.size() + 1U : next + 1U;
    const auto trimmed = trimAscii(line);
    if (trimmed.empty()) {
      continue;
    }

    if (const auto section = assSectionName(trimmed); section.has_value()) {
      // 进入新段：只有 `[Events]` 段内的行才参与解析；同时丢弃上一段的 `Format:`（每个段的 Format
      // 只对本段有效，`[V4+ Styles]` 的样式列定义绝不能用于 `Dialogue:`）。
      inEvents = asciiEqualsIgnoreCase(*section, "Events");
      haveFormat = false;
      continue;
    }

    const auto colon = trimmed.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const auto key = trimAscii(trimmed.substr(0, colon));
    if (!inEvents) {
      // 段外同名行（如 `[Script Info]` 里的 `Dialogue:`）一律不解析。
      continue;
    }
    const auto value = trimAscii(trimmed.substr(colon + 1U));

    if (asciiEqualsIgnoreCase(key, "Format")) {
      format = resolveAssEventFormat(value);
      haveFormat = true;
      if (!format.usable) {
        result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, 0U,
                                          "ass events Format lacks a Start or Text column", std::string{value}));
        spdlog::debug("ass parse: unusable Format at line {}", lineNumber);
      }
      continue;
    }

    if (asciiEqualsIgnoreCase(key, "Comment")) {
      // 注释行不显示，跳过且不报错（其 Start 是否合法无关紧要）。
      continue;
    }

    if (!asciiEqualsIgnoreCase(key, "Dialogue")) {
      continue;
    }

    if (!haveFormat || !format.usable) {
      result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, 0U,
                                        "ass dialogue appears before a usable Format", std::string{value}));
      spdlog::debug("ass parse: dialogue without usable Format at line {}", lineNumber);
      continue;
    }

    // 按 Format 的字段数做 N-1 次逗号切分：末列吃下剩余全部内容（其内部逗号不再切分）。字段不足
    // 时把剩余部分绑定到**截断处**那个下标（而非声明的 Text 下标）：若声明的 Start/Text 落在截断点
    // 之后，其值仍不可用 —— Start 不可用记一条错误并跳过，Text 不可用则静默跳过（不产出、不报错）。
    // 故该兜底只保住截断点之前的列，并非"保证不丢 Start/Text 之一"。
    fields.clear();
    std::size_t begin = 0;
    bool truncated = false;
    for (std::size_t index = 0; index + 1U < format.fieldCount; ++index) {
      const auto comma = value.find(',', begin);
      if (comma == std::string_view::npos) {
        truncated = true;
        break;
      }
      fields.push_back(trimAscii(value.substr(begin, comma - begin)));
      begin = comma + 1U;
    }
    fields.push_back(truncated ? trimAscii(value.substr(begin)) : value.substr(begin));

    if (format.startIndex >= fields.size()) {
      result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, 0U,
                                        "ass dialogue is missing the Start field", std::string{value}));
      spdlog::debug("ass parse: dialogue missing Start at line {}", lineNumber);
      continue;
    }
    const auto start = parseAssTimecode(fields[format.startIndex]);
    if (!start.has_value()) {
      result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, lineNumber, 0U,
                                        "ass dialogue Start timestamp is malformed",
                                        std::string{fields[format.startIndex]}));
      spdlog::debug("ass parse: malformed Start at line {}", lineNumber);
      continue;
    }
    if (format.textIndex >= fields.size()) {
      // 无文本可用 ⇒ 空文本，不产出、不报错（与「空文本不产出」一致）。
      continue;
    }

    // 剥离顺序：覆盖块 `{\...}`（含 `\p` 绘图负载丢弃）→ `\N` 硬换行切成多行 → 每行做字符级
    // 转义还原（含 `\{` / `\}`）→ HTML 形态标签剥离 → trim。`\N` 必须在切片前完成，字符转义必须
    // 逐个切片内完成。
    const auto stripped = stripAssOverrideBlocks(fields[format.textIndex]);
    std::size_t partBegin = 0;
    while (true) {
      const auto hardBreak = stripped.find("\\N", partBegin);
      const auto part = std::string_view{stripped}.substr(
          partBegin, hardBreak == std::string::npos ? std::string_view::npos : hardBreak - partBegin);
      // 三段各自绑定 owning std::string 后再取其 view，避免 view 指向语句结束即析构的临时量（悬垂）。
      const auto unescaped = replaceAssEscapes(part);
      const auto htmlStripped = stripHtmlTags(unescaped);
      const auto lyric = trimAscii(htmlStripped);
      if (!lyric.empty()) {
        result.lines.push_back({.timestamp = *start, .text = std::string{lyric}});
      }
      if (hardBreak == std::string::npos) {
        break;
      }
      partBegin = hardBreak + 2U;
    }
  }

  // 与 parseSrtLyrics 同构：只按时间戳稳定排序、**不去重**。**与 parseLrcText 不同**：后者在排序后
  // 仍执行 `std::ranges::unique`（排序键只剩时间戳后它退化为「只去相邻 (timestamp,text) 重复」），
  // 本解析器与 parseSrtLyrics 均**无**该去重步。同一 `\N` 多行的各行共享 Start 时间戳 ⇒ 保持切片
  // 顺序，下游 D22 配对（组内第 1 行 = 原文）在乱序输入下仍可确定。
  std::ranges::stable_sort(result.lines, [](const LyricLine& lhs, const LyricLine& rhs) {
    return lhs.timestamp < rhs.timestamp;
  });
  return result;
}

LrcParseResult parseAssLyricsFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    spdlog::warn("ass parse: failed to stat {} ({})", pathToUtf8(path), error.message());
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to stat ass lyrics file",
                                 error.message())}};
  }
  if (size > options.maxBytes) {
    spdlog::warn("ass parse: file exceeds size limit ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                 "ass lyrics exceed scanner limit")}};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    spdlog::warn("ass parse: failed to open {}", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to open ass lyrics file")}};
  }

  std::string bytes;
  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

  auto decoded = decodeLyricsBytes(bytes);
  if (!decoded.has_value()) {
    spdlog::warn("ass parse: unsupported encoding, skipping ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::UnsupportedEncoding, path, 0U, 0U,
                                 "ass lyrics encoding is not in the supported fallback set",
                                 "utf-8-sig/utf-8/gb18030/big5/shift_jis/utf-16 all failed")}};
  }
  return parseAssLyrics(std::move(*decoded), options, path);
}

LrcParseResult parseTtmlLyrics(std::string text, const LrcParseOptions& options,
                               std::optional<std::filesystem::path> path) {
  LrcParseResult result;
  if (text.size() > options.maxBytes) {
    result.errors.push_back(makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                      "ttml lyrics exceed scanner limit"));
    spdlog::warn("ttml parse: content exceeds size limit ({})", path.has_value() ? pathToUtf8(*path) : "<text>");
    return result;
  }

  normalizeXmlLineEndings(text);
  const auto document = std::string_view{text};
  std::size_t cursor = 0;
  std::size_t lineNumber = 1U;
  XmlEndMemo memo;  // 多字符终止符的缺失下界，全程共享 ⇒ 每类终止符至多一次向前失败查找

  std::size_t paragraphDepth = 0U;
  std::vector<std::string> segments(1U);
  bool beginPresent = false;
  std::string beginValue;
  std::chrono::milliseconds timestamp = kUnsyncedTimestamp;
  std::size_t paragraphLine = 0U;

  const auto flushParagraph = [&]() {
    if (beginPresent && !parseTtmlBegin(beginValue).has_value()) {
      // `begin` 存在但畸形 ⇒ 记一条错误并**跳过该 `<p>`**（与 SRT 畸形时间轴、ASS 畸形 Start 同处置）。
      result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, paragraphLine, 0U,
                                        "ttml p begin timestamp is malformed", beginValue));
      spdlog::debug("ttml parse: malformed begin at line {}", paragraphLine);
    } else {
      // `begin` 缺失 ⇒ 按 unsynced 哨兵产出、文本照常保留（README §2.8）。
      for (const auto& segment : segments) {
        // collapseXmlWhitespace 返回 owning std::string ⇒ 不存在「view 指向临时量」的悬垂（todo 17 曾捕获）。
        const auto lyric = collapseXmlWhitespace(segment);
        if (!lyric.empty()) {
          result.lines.push_back({.timestamp = timestamp, .text = lyric});
        }
      }
    }
    segments.assign(1U, std::string{});
    beginPresent = false;
    beginValue.clear();
    timestamp = kUnsyncedTimestamp;
    paragraphLine = 0U;
  };

  while (cursor < document.size()) {
    if (lineNumber > options.maxLines) {
      result.errors.push_back(makeError(LrcParseErrorCode::TooManyLines, path, lineNumber, 0U,
                                        "ttml lyrics exceed scanner line limit"));
      break;
    }

    if (document[cursor] != '<') {
      const auto next = document.find('<', cursor);
      const auto end = next == std::string_view::npos ? document.size() : next;
      const auto chunk = document.substr(cursor, end - cursor);
      if (paragraphDepth > 0U) {
        appendXmlDecoded(chunk, segments.back(), XmlValueKind::TextContent);
      }
      lineNumber += countNewlines(chunk);
      cursor = end;
      continue;
    }

    if (startsWithAt(document, cursor, "<!--")) {
      // 注释：正常终止符 `-->`；未终止则退回裸 `<` 或按文档截断（R7-F1）。
      const auto found = scanXmlEnd(document, cursor + 4U,
                                   XmlEndRule{.closer = "-->", .absent = &XmlEndMemo::commentAbsent}, memo);
      if (found.kind == XmlEndKind::Unterminated) {
        cursor = document.size();
        break;
      }
      lineNumber += countNewlines(document.substr(cursor, found.end - cursor));
      cursor = found.end;
      continue;
    }

    if (startsWithAt(document, cursor, "<![CDATA[")) {
      // CDATA：内容**原样**拼接（不做实体还原）。正常终止符 `]]>`；未终止则只取到裸 `<` 之前，
      // 丢弃 `<![CDATA[` 起始标记并让主循环重新看到该 `<`（R7-F1）——不得把其后标签文本当字面内容吞入。
      const auto found = scanXmlEnd(document, cursor + 9U,
                                   XmlEndRule{.closer = "]]>", .absent = &XmlEndMemo::cdataAbsent}, memo);
      if (found.kind == XmlEndKind::Unterminated) {
        cursor = document.size();
        break;
      }
      const auto contentEnd = found.kind == XmlEndKind::Terminated ? found.end - 3U : found.end;
      const auto content = document.substr(cursor + 9U, contentEnd - (cursor + 9U));
      if (paragraphDepth > 0U) {
        segments.back().append(content);
      }
      lineNumber += countNewlines(content);
      cursor = found.end;
      continue;
    }

    if (startsWithAt(document, cursor, "<!")) {
      // DOCTYPE 或其它声明：整体跳过（**不**解析 DTD ⇒ DOCTYPE 里自定义的实体按「未定义实体」处置）。
      // 共享判定（R7-F1）：正常终止符 `>`；跳过成对引号与内部子集 `[ ... ]`（避免被 SYSTEM 字面量或
      // DTD 内的 `>` 提前截断）；终止符缺席时退回引号外裸 `<`。
      const auto found = scanXmlEnd(
          document, cursor + 2U,
          XmlEndRule{.closer = ">", .quotes = true, .brackets = true, .breaksAtLtSlash = true}, memo);
      if (found.kind == XmlEndKind::Unterminated) {
        cursor = document.size();
        break;
      }
      lineNumber += countNewlines(document.substr(cursor, found.end - cursor));
      cursor = found.end;
      continue;
    }

    if (startsWithAt(document, cursor, "<?")) {
      // 处理指令（含 `<?xml ...?>` 声明）：跳过。编码在字节层由 decodeLyricsBytes 决定，与此无关。
      // 正常终止符 `?>`；未终止则退回裸 `<` 或按文档截断（R7-F1）。
      const auto found = scanXmlEnd(document, cursor + 2U,
                                   XmlEndRule{.closer = "?>", .absent = &XmlEndMemo::piAbsent}, memo);
      if (found.kind == XmlEndKind::Unterminated) {
        cursor = document.size();
        break;
      }
      lineNumber += countNewlines(document.substr(cursor, found.end - cursor));
      cursor = found.end;
      continue;
    }

    if (startsWithAt(document, cursor, "</")) {
      std::size_t scan = cursor + 2U;
      const auto nameStart = scan;
      while (scan < document.size() && isXmlNameChar(document[scan])) {
        ++scan;
      }
      const auto name = xmlLocalName(document.substr(nameStart, scan - nameStart));
      // 收尾标签不带属性值（无引号语义）：正常终止符 `>`，终止符缺席时退回裸 `<`（R7-F1）——否则
      // `</span` 会吃掉所在段的 `</p>`，后段被并入前段、其时间戳被吞。
      const auto found = scanXmlEnd(document, scan, XmlEndRule{.closer = ">"}, memo);
      if (found.kind == XmlEndKind::Unterminated) {
        cursor = document.size();
        break;
      }
      lineNumber += countNewlines(document.substr(cursor, found.end - cursor));
      cursor = found.end;
      // 只有 `</p>` 关闭产出单位；`</span>`/`</br>`/未知元素的闭合标签忽略（透明容器语义）。
      if (name == "p" && paragraphDepth > 0U) {
        --paragraphDepth;
        if (paragraphDepth == 0U) {
          flushParagraph();
        }
      }
      continue;
    }

    if (cursor + 1U >= document.size() || !isXmlNameStart(document[cursor + 1U])) {
      // 不构成标签（正文里的 `a < b`）⇒ `<` 按普通字符保留（与 SRT 侧的保守 `<` 处置同向）。
      if (paragraphDepth > 0U) {
        segments.back().push_back('<');
      }
      ++cursor;
      continue;
    }

    std::size_t scan = cursor + 1U;
    const auto nameStart = scan;
    while (scan < document.size() && isXmlNameChar(document[scan])) {
      ++scan;
    }
    const auto name = xmlLocalName(document.substr(nameStart, scan - nameStart));
    const auto tagLine = lineNumber;
    const auto tag = scanTtmlTagTail(document, scan, memo);
    if (!tag.closed) {
      cursor = document.size();  // 其后已无 `>` ⇒ 文档截断（README §2.9）
      break;
    }
    lineNumber += countNewlines(document.substr(cursor, tag.end - cursor));
    cursor = tag.end;

    if (tag.malformed) {
      // 标签畸形但**终止于 `>`**（R3-F1）：`cursor` 已越过它，后续文档继续解析。
      // 对 `<p>` 按公共头契约记一条 InvalidTimestamp 并跳过该段（不产出其文本）；
      // 其余元素的畸形开标签按透明容器忽略。**不**改动 paragraphDepth —— 该段整体被跳过，
      // 其对应的 `</p>` 在深度 0 时是空操作（见闭合标签分支）。
      if (name == "p") {
        result.errors.push_back(makeError(LrcParseErrorCode::InvalidTimestamp, path, tagLine, 0U,
                                          "ttml p tag is malformed"));
      }
      continue;
    }

    if (name == "p") {
      if (paragraphDepth == 0U) {
        segments.assign(1U, std::string{});
        beginPresent = tag.hasBegin;
        beginValue = tag.beginValue;
        timestamp = kUnsyncedTimestamp;
        if (tag.hasBegin) {
          if (const auto parsed = parseTtmlBegin(tag.beginValue); parsed.has_value()) {
            timestamp = *parsed;
          }
        }
        paragraphLine = tagLine;
      }
      if (!tag.selfClosing) {
        ++paragraphDepth;
      } else if (paragraphDepth == 0U) {
        flushParagraph();  // `<p .../>`：无文本 ⇒ 不产出（与 SRT/ASS「空文本不产出」一致）
      }
      continue;
    }

    // `br` 是**空元素**：`<br/>` 与 `<br>` 必须**同样**切分 ⇒ 此判断必须在 selfClosing 提前 continue 之前。
    if (name == "br") {
      if (paragraphDepth > 0U) {
        segments.emplace_back();
      }
      continue;
    }
    if (tag.selfClosing) {
      continue;
    }
    // `span` 与其它内联元素：透明容器（README §2.4）—— 无状态，其文本自然落入当前片段。
  }

  if (paragraphDepth > 0U) {
    flushParagraph();  // 文档在 `<p>` 内结束（未闭合）⇒ 按已收文本 flush，不丢文本
  }

  // 与 parseSrtLyrics / parseAssLyrics 同构：只按 timestamp 稳定排序、**不去重**。同一 `<p>` 的
  // 多个 `<br>` 片段共享 timestamp ⇒ 保持片段顺序，下游 D22 配对（组内第 1 行 = 原文）仍可确定；
  // unsynced 哨兵（-1ms）排在所有合法时间戳之前。
  std::ranges::stable_sort(result.lines, [](const LyricLine& lhs, const LyricLine& rhs) {
    return lhs.timestamp < rhs.timestamp;
  });
  return result;
}

LrcParseResult parseTtmlLyricsFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    spdlog::warn("ttml parse: failed to stat {} ({})", pathToUtf8(path), error.message());
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to stat ttml lyrics file",
                                 error.message())}};
  }
  if (size > options.maxBytes) {
    spdlog::warn("ttml parse: file exceeds size limit ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::FileTooLarge, path, 0U, 0U,
                                 "ttml lyrics exceed scanner limit")}};
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    spdlog::warn("ttml parse: failed to open {}", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::IoFailure, path, 0U, 0U, "failed to open ttml lyrics file")}};
  }

  std::string bytes;
  bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

  auto decoded = decodeLyricsBytes(bytes);
  if (!decoded.has_value()) {
    spdlog::warn("ttml parse: unsupported encoding, skipping ({})", pathToUtf8(path));
    return {.errors = {makeError(LrcParseErrorCode::UnsupportedEncoding, path, 0U, 0U,
                                 "ttml lyrics encoding is not in the supported fallback set",
                                 "utf-8-sig/utf-8/gb18030/big5/shift_jis/utf-16 all failed")}};
  }
  return parseTtmlLyrics(std::move(*decoded), options, path);
}

LrcParseResult parseLyricsSidecarFile(const std::filesystem::path& path, const LrcParseOptions& options) {
  switch (lyricsSourceForSidecarPath(path)) {
    case LyricsSource::ExternalLrc:
      return parseLrcFile(path, options);
    case LyricsSource::ExternalSrt:
      return parseSrtLyricsFile(path, options);
    case LyricsSource::ExternalAss:
      return parseAssLyricsFile(path, options);
    case LyricsSource::ExternalTtml:
      return parseTtmlLyricsFile(path, options);
    case LyricsSource::ExternalText:
      return parsePlainTextLyricsFile(path, options);
    case LyricsSource::None:
    case LyricsSource::EmbeddedTag:
      break;
  }
  return {};
}

}
