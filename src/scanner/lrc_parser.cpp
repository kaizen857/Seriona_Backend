#include "seriona/scanner/lrc_parser.h"

#include "path_utf8.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
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

}
