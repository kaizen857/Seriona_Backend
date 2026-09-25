#include "seriona/scanner/lyric_reference_pairing.h"
#include "seriona/scanner/lrc_parser.h"

#include <doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using seriona::scanner::groupLyricReferenceLines;
using seriona::scanner::LyricLine;
using seriona::scanner::parseLrcText;

LyricLine line(std::int64_t millis, std::string text) {
  return LyricLine{.timestamp = std::chrono::milliseconds{millis}, .text = std::move(text)};
}

}  // namespace

TEST_CASE("lyrics reference pairing takes the first of two same-timestamp lines as the original") {
  const std::vector<LyricLine> lines{line(1000, "原文"), line(1000, "译文")};

  const auto groups = groupLyricReferenceLines(lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(groups[0].originalIndex == 0U);
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(groups[0].translationIndexes[0] == 1U);
  CHECK_FALSE(groups[0].confidenceReduced);
}

TEST_CASE("lyrics reference pairing groups by timestamp and not by adjacent lines") {
  // 同 timestamp 的两行中间夹了另一个 timestamp：按相邻行切片会得到两个单行组（无参照），
  // 按 timestamp 分组才把 idx0 与 idx2 配成一组。
  const std::vector<LyricLine> lines{line(1000, "原文"), line(2000, "独行"), line(1000, "译文")};

  const auto groups = groupLyricReferenceLines(lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(groups[0].originalIndex == 0U);
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(groups[0].translationIndexes[0] == 2U);
}

TEST_CASE("lyrics reference pairing keeps every line of a three-line group") {
  const std::vector<LyricLine> lines{line(1000, "a"), line(1000, "b"), line(1000, "c")};

  const auto groups = groupLyricReferenceLines(lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].originalIndex == 0U);
  REQUIRE(groups[0].translationIndexes.size() == 2U);
  CHECK(groups[0].translationIndexes[0] == 1U);
  CHECK(groups[0].translationIndexes[1] == 2U);
  CHECK(groups[0].confidenceReduced);
}

TEST_CASE("lyrics reference pairing excludes the unsynced sentinel and keeps a real zero timestamp") {
  const std::vector<LyricLine> lines{line(0, "零毫秒原文"), line(0, "零毫秒译文"),
                                     line(-1, "哨兵一"), line(-1, "哨兵二")};

  const auto groups = groupLyricReferenceLines(lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{0});
  CHECK(groups[0].originalIndex == 0U);
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(groups[0].translationIndexes[0] == 1U);
  for (const auto& group : groups) {
    CHECK(group.originalIndex < 2U);
    for (const auto index : group.translationIndexes) {
      CHECK(index < 2U);
    }
  }

  const std::vector<LyricLine> sentinelsOnly{line(-1, "哨兵一"), line(-1, "哨兵二")};
  CHECK(groupLyricReferenceLines(sentinelsOnly).empty());
}

TEST_CASE("lyrics reference pairing keeps same-timestamp lines whose text differs") {
  // 守 `std::ranges::unique(..., (timestamp, text))` 的风险点：去重只去相邻的
  // (timestamp, text) 全等行，同刻不同文本的两行必须都还在，才能配成参照组。
  const auto parsed = parseLrcText("[00:01.00]aaa\n[00:01.00]bbb\n");

  REQUIRE(parsed.lines.size() == 2U);
  CHECK(parsed.lines[0].text == "aaa");
  CHECK(parsed.lines[1].text == "bbb");

  const auto groups = groupLyricReferenceLines(parsed.lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].originalIndex == 0U);
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(groups[0].translationIndexes[0] == 1U);
}

TEST_CASE("lyrics reference pairing keeps the file order of same-timestamp lines after the stable sort") {
  // 文件顺序 2000 → 1000(bbb) → 1000(aaa)：按 (timestamp, text) 排序会把 aaa 排到 bbb 前面、
  // 让 aaa 成为原文；仅按 timestamp 的稳定序保留文件顺序 ⇒ 原文是 bbb。
  const auto parsed = parseLrcText("[00:02.00]zzz\n[00:01.00]bbb\n[00:01.00]aaa\n");

  REQUIRE(parsed.lines.size() == 3U);
  CHECK(parsed.lines[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(parsed.lines[0].text == "bbb");
  CHECK(parsed.lines[1].text == "aaa");

  const auto groups = groupLyricReferenceLines(parsed.lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(parsed.lines[groups[0].originalIndex].text == "bbb");
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(parsed.lines[groups[0].translationIndexes[0]].text == "aaa");
}

TEST_CASE("lyrics reference pairing skips a group whose first line is not a lyrics body") {
  const std::vector<LyricLine> blank{line(1000, ""), line(1000, "译文")};
  CHECK(groupLyricReferenceLines(blank).empty());

  const std::vector<LyricLine> whitespace{line(1000, "   "), line(1000, "译文")};
  CHECK(groupLyricReferenceLines(whitespace).empty());

  const std::vector<LyricLine> tagOnly{line(1000, "[ar:someone]"), line(1000, "译文")};
  CHECK(groupLyricReferenceLines(tagOnly).empty());

  const std::vector<LyricLine> tagOnlyMultiple{line(1000, "[ar:someone][by:them]"), line(1000, "译文")};
  CHECK(groupLyricReferenceLines(tagOnlyMultiple).empty());

  const std::vector<LyricLine> normal{line(1000, "原文"), line(1000, "译文")};
  CHECK(groupLyricReferenceLines(normal).size() == 1U);
}

TEST_CASE("lyrics reference pairing treats Python whitespace and prefixed tags as content") {
  // 空白判据用 Python `str.isspace()` 语义（与 lrc_parser.cpp 的 stripG6Python 同义），
  // 不是仅 ASCII：这些串单独成行时都没有正文。
  const std::vector<std::string> blankOnly{
      "\x0A",              // U+000A
      "\x0C",              // U+000C
      "\x0B",              // U+000B
      "\xE3\x80\x80",      // U+3000
      "\xC2\xA0",          // U+00A0
      "\xE2\x80\x80",      // U+2000
      "\xE2\x80\xA8",      // U+2028
      "\xE2\x81\x9F",      // U+205F
      "\xE3\x80\x80\xE3\x80\x80",
  };
  for (const auto& text : blankOnly) {
    CAPTURE(text);
    const std::vector<LyricLine> lines{line(1000, text), line(1000, "译文")};
    CHECK(groupLyricReferenceLines(lines).empty());
  }

  // 空白 + 正文、标签 + 正文都仍算有正文：标签先剥离，再判剩余是否空白。
  const std::vector<std::string> withBody{
      " \xE3\x80\x80\xE5\x8E\x9F\xE6\x96\x87",                 // U+3000 + 原文
      "[ar:someone]\xE5\x8E\x9F\xE6\x96\x87",                  // 标签 + 原文
  };
  for (const auto& text : withBody) {
    CAPTURE(text);
    const std::vector<LyricLine> lines{line(1000, text), line(1000, "译文")};
    const auto groups = groupLyricReferenceLines(lines);
    REQUIRE(groups.size() == 1U);
    CHECK(groups[0].originalIndex == 0U);
  }
}

TEST_CASE("lyrics reference pairing does not emit a single-line group") {
  const std::vector<LyricLine> lines{line(1000, "独行"), line(2000, "另一行")};

  CHECK(groupLyricReferenceLines(lines).empty());
}

TEST_CASE("lyrics reference pairing gives each expanded timestamp its own group") {
  // 一行带两个时间戳（副歌复用）在解析期展开成两条同文本行：每个时间戳各自成组，
  // 参照行只作用于它所属的时间戳组。
  const auto parsed = parseLrcText("[00:01.00][00:02.00]奥\n[00:02.00]译\n");

  REQUIRE(parsed.lines.size() == 3U);

  const auto groups = groupLyricReferenceLines(parsed.lines);

  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{2000});
  CHECK(parsed.lines[groups[0].originalIndex].text == "奥");
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(parsed.lines[groups[0].translationIndexes[0]].text == "译");
}
