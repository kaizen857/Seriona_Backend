#include "seriona/scanner/lyric_reference_pairing.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace seriona::scanner {
namespace {

// ── 「组内首行没有正文」的防御性判据 ────────────────────────────────────────────
//
// §8.8 要求参照行配对前已完成 §6.2 阶段 A 的清洗，故进本函数的行**本应已是正文**；下面这些
// 谓词只是防御。它们与 `lrc_parser.cpp` 匿名命名空间里的同名谓词**同义镜像**（scanner 层不得
// 依赖 control 层的清洗函数，且那些谓词是内部链接、无法 include）：
//
//   · `decodeUtf8At` / `isPythonWhitespace` / `kPythonWhitespaceRanges` / `inSortedSpans`
//     镜像该文件的 `decodeG6Utf8At` / `isG6PythonWhitespace` / `kG6PythonWhitespaceRanges` /
//     `g6InSortedSpans`（Python `str.isspace()` 的 29 码点、10 段）。
//   · `removeInlineTags` 镜像该文件的同名函数：全局删除 `\[[A-Za-z_]+:[^\]]*\]`，自左向右、
//     非重叠；不匹配的 `[` 逐字节保留。
//
// 同步义务：本 TU 与 `lrc_parser.cpp` 的这几处镜像必须同步变更，由 todo 22/S14 复核。
// 本仓库的 Python 空白表因此有 3 份（`lyric_split.cpp`、`lrc_parser.cpp`、本文件）；
// 去重需要 scanner 内部头，本轮为不动 `lrc_parser.cpp` 的字节而未做。
//
// ★有意不镜像 `matchMetadata`/`matchCredit`（`isDroppableLyricBody` 的后两个析取项）：那是
// **解析期**职责，五个解析路径都已在产出 `LyricLine` 之前调用 `isDroppableLyricBody` 丢弃这类行。
// 在此重复判定会造出第二份 G6 语义（`lrc_parser.cpp` 的 G6 区块头已就此警告过漂移风险）。

struct CodePointSpan {
  std::uint32_t first;
  std::uint32_t last;
};

constexpr std::uint32_t kReplacementCodePoint = 0xFFFDU;

constexpr std::array<CodePointSpan, 10> kPythonWhitespaceRanges{{
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

[[nodiscard]] constexpr bool spansSortedAndDisjoint(const std::array<CodePointSpan, 10>& spans) noexcept {
  for (std::size_t index = 1; index < spans.size(); ++index) {
    if (spans[index].first <= spans[index - 1].last) {
      return false;
    }
  }
  return true;
}

static_assert(spansSortedAndDisjoint(kPythonWhitespaceRanges),
              "Python 空白表必须按起点有序且互不重叠，否则二分查表与参考实现语义不等价");

[[nodiscard]] constexpr bool inSortedSpans(const std::array<CodePointSpan, 10>& spans,
                                           std::uint32_t codePoint) noexcept {
  const auto upper = std::upper_bound(spans.begin(), spans.end(), codePoint,
                                      [](std::uint32_t value, const CodePointSpan& span) {
                                        return value < span.first;
                                      });
  if (upper == spans.begin()) {
    return false;
  }
  return codePoint <= (upper - 1)->last;
}

struct DecodedCodePoint {
  std::uint32_t codePoint;
  std::size_t byteLength;
};

// 无效 UTF-8 退化为 U+FFFD 且只前进 1 字节（与 `lrc_parser.cpp` 逐字节一致，保证按字节偏移
// 迭代在畸形输入上不死循环、不越界）。U+FFFD 不是空白 ⇒ 畸形字节算作正文，与解析器同向。
[[nodiscard]] DecodedCodePoint decodeUtf8At(std::string_view text, std::size_t offset) noexcept {
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
    return {kReplacementCodePoint, 1};
  }
  if (offset + length > text.size()) {
    return {kReplacementCodePoint, 1};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return {kReplacementCodePoint, 1};
    }
    codePoint = (codePoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
  }
  const bool overlong = (length == 2 && codePoint < 0x80U) ||
                        (length == 3 && codePoint < 0x800U) ||
                        (length == 4 && codePoint < 0x10000U);
  const bool surrogate = codePoint >= 0xD800U && codePoint <= 0xDFFFU;
  if (overlong || surrogate || codePoint > 0x10FFFFU) {
    return {kReplacementCodePoint, 1};
  }
  return {codePoint, length};
}

[[nodiscard]] bool isPythonWhitespace(std::uint32_t codePoint) noexcept {
  return inSortedSpans(kPythonWhitespaceRanges, codePoint);
}

// 整串是否只由 Python 空白组成（等价 `stripPython(text).empty()`，无中间分配）。
[[nodiscard]] bool isBlankPythonWhitespace(std::string_view text) noexcept {
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto decoded = decodeUtf8At(text, cursor);
    if (!isPythonWhitespace(decoded.codePoint)) {
      return false;
    }
    cursor += decoded.byteLength;
  }
  return true;
}

[[nodiscard]] bool isAsciiWordByte(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
}

[[nodiscard]] std::string removeInlineTags(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    if (text[cursor] == '[') {
      std::size_t wordEnd = cursor + 1;
      while (wordEnd < text.size() && isAsciiWordByte(text[wordEnd])) {
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

[[nodiscard]] bool hasLyricsBody(std::string_view text) {
  return !isBlankPythonWhitespace(removeInlineTags(text));
}

}  // namespace

std::vector<LyricReferenceGroup> groupLyricReferenceLines(const std::vector<LyricLine>& lines) {
  // 分组的键只有 timestamp：unordered_map 会丢掉「按 timestamp 升序」的输出顺序，
  // 按相邻行切片则会把「同 timestamp 但中间夹了别的 timestamp」的行拆成两段。用有序 map。
  std::map<std::chrono::milliseconds, std::vector<std::size_t>> byTimestamp;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (lines[index].timestamp < std::chrono::milliseconds{0}) {
      continue;  // unsynced 哨兵（< 0）不属于任何时间戳；0ms 是合法时间戳，不在此列
    }
    byTimestamp[lines[index].timestamp].push_back(index);
  }

  std::vector<LyricReferenceGroup> groups;
  for (const auto& [timestamp, indexes] : byTimestamp) {
    if (indexes.size() < 2U) {
      continue;  // 单行组没有参照关系
    }
    if (!hasLyricsBody(lines[indexes.front()].text)) {
      continue;  // §8.8：组内第 1 行本身即空白/占位 ⇒ 跳过该组
    }

    std::vector<std::string_view> texts;
    texts.reserve(indexes.size());
    for (const auto index : indexes) {
      texts.push_back(lines[index].text);
    }
    std::ranges::sort(texts);
    const bool duplicated = std::ranges::adjacent_find(texts) != texts.end();

    LyricReferenceGroup group;
    group.timestamp = timestamp;
    group.originalIndex = indexes.front();
    group.translationIndexes.assign(indexes.begin() + 1, indexes.end());
    group.confidenceReduced = indexes.size() > 2U || duplicated;
    groups.push_back(std::move(group));
  }
  return groups;
}

}  // namespace seriona::scanner
