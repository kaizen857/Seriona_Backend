#include "scanner_test_harness.h"

#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"

#include <doctest.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output.is_open());
  output << text;
}

[[nodiscard]] const ClassifiedPath& requireRelativePath(const std::vector<ClassifiedPath>& entries,
                                                        const std::string_view relativePath) {
  const auto entry = std::ranges::find(entries, relativePath, &ClassifiedPath::relativeUtf8);
  REQUIRE(entry != entries.end());
  return *entry;
}

TEST_CASE("scanner path classification covers roots extensions cue lrc and stable order") {
  test::TempScannerRoot root("scanner-paths");
  writeTextFile(root.path() / "b" / "song.FLAC", "audio");
  writeTextFile(root.path() / "b" / "song.lrc", "[00:01.00] lyric\n");
  writeTextFile(root.path() / "a" / "track.mp3", "audio");
  writeTextFile(root.path() / "a" / "movie.mp4", "video");
  writeTextFile(root.path() / "a" / "album.cue", "FILE \"album-audio.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / "a" / "sheet.CUE", "FILE \"sheet-audio.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  if (entries.size() != 7U) {
    MESSAGE("Expected 7 entries, got " << entries.size());
    for (const auto& entry : entries) {
      MESSAGE("  - " << entry.relativeUtf8 << " (kind=" << static_cast<int>(entry.kind) << ")");
    }
  }
  
  REQUIRE(entries.size() == 7U);
  CHECK(entries.front().kind == PathEntryKind::DirectoryRoot);
  CHECK(entries.front().relativeUtf8 == ".");
  CHECK(requireRelativePath(entries, "a/track.mp3").kind == PathEntryKind::AudioCandidate);
  CHECK(requireRelativePath(entries, "a/movie.mp4").kind == PathEntryKind::Unsupported);
  const auto& cue = requireRelativePath(entries, "a/album.cue");
  CHECK(cue.kind == PathEntryKind::CueSheet);
  const auto& cueUpper = requireRelativePath(entries, "a/sheet.CUE");
  CHECK(cueUpper.kind == PathEntryKind::CueSheet);
  const auto& flac = requireRelativePath(entries, "b/song.FLAC");
  CHECK(flac.kind == PathEntryKind::AudioCandidate);
  REQUIRE(flac.sidecarLyricsPath.has_value());
  CHECK(flac.sidecarLyricsPath->filename() == "song.lrc");
  CHECK(requireRelativePath(entries, "b/song.lrc").kind == PathEntryKind::LyricsSidecar);

  std::vector<std::string> relativePaths;
  std::ranges::transform(entries, std::back_inserter(relativePaths), &ClassifiedPath::relativeUtf8);
  CHECK(std::ranges::is_sorted(relativePaths));
}

TEST_CASE("scanner path classification handles single file roots and custom extension allowlist") {
  test::TempScannerRoot root("scanner-single-file");
  const auto audio = test::writeAudioFixture(root.path(), "single.weba");
  const auto unsupported = root.path() / "single.nfo";
  writeTextFile(unsupported, "text");

  const auto singleFile = discoverScannerPaths({.path = audio, .recursive = false});
  REQUIRE(singleFile.size() == 1U);
  CHECK(singleFile.front().kind == PathEntryKind::SingleFileRoot);
  CHECK(singleFile.front().displayName == "single.weba");

  CHECK(classifyScannerPath(root.path(), unsupported).kind == PathEntryKind::Unsupported);
  CHECK(classifyScannerPath(root.path(), unsupported, {.allowedExtensions = {".nfo"}}).kind ==
        PathEntryKind::AudioCandidate);
}

TEST_CASE("scanner path classification preserves native relative paths and utf8 serialization") {
  test::TempScannerRoot root("scanner-native-relative-path");
  const auto relativePath = std::filesystem::path{u8"音乐.flac"};
  const auto audio = root.path() / relativePath;
  writeTextFile(audio, "audio");

  ClassifiedPath classified;
  CHECK_NOTHROW(classified = classifyScannerPath(root.path(), audio));

  const auto expectedUtf8 = relativePath.generic_u8string();
  CHECK(classified.relativePath == relativePath);
  CHECK(classified.relativeUtf8 == std::string{expectedUtf8.begin(), expectedUtf8.end()});
}

TEST_CASE("scanner path classification does not follow symlinks by default") {
  test::TempScannerRoot root("scanner-symlink");
  const auto target = test::writeAudioFixture(root.path(), "target.flac");
  const auto link = root.path() / "link.flac";
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  if (error) {
    MESSAGE("symlink creation unavailable: " << error.message());
    return;
  }

  const auto skipped = classifyScannerPath(root.path(), link);
  CHECK(skipped.kind == PathEntryKind::Symlink);
  REQUIRE_FALSE(skipped.errors.empty());
  CHECK(skipped.errors.front().code == ScannerErrorCode::UnsupportedFile);

  const auto followed = classifyScannerPath(root.path(), link, {.followSymlinks = true});
  CHECK(followed.kind == PathEntryKind::AudioCandidate);
}

TEST_CASE("scanner path classification records vanished roots as recoverable errors") {
  test::TempScannerRoot root("scanner-missing");
  const auto missing = root.path() / "missing.flac";

  const auto entries = discoverScannerPaths({.path = missing, .recursive = false});

  REQUIRE(entries.size() == 1U);
  CHECK(entries.front().kind == PathEntryKind::Missing);
  REQUIRE_FALSE(entries.front().errors.empty());
  CHECK(entries.front().errors.front().code == ScannerErrorCode::RootUnavailable);
}

TEST_CASE("lrc parser normalizes line endings expands timestamps sorts and deduplicates") {
  const auto result = parseLrcText("[ar:Artist]\r\n[00:02.50][00:01.00]  Same line  \r\n[00:01.00]Same line\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(result.lines[0].text == "Same line");
  CHECK(result.lines[1].timestamp == std::chrono::milliseconds{2500});
  CHECK(result.lines[1].text == "Same line");
}

// 同时间戳、文本刻意乱序且含一对相同文本。稳定按时间戳排序必须保持输入出现顺序，
// 否则「组内第 1 行 = 原文」的配对约定（D22）不成立。
TEST_CASE("lrc parser keeps same-timestamp lines in stable input order") {
  const auto result = parseLrcText("[00:00.20]zeta\n[00:00.20]alpha\n[00:00.20]mid\n[00:00.20]zeta\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].text == "zeta");
  CHECK(result.lines[1].text == "alpha");
  CHECK(result.lines[2].text == "mid");
  CHECK(result.lines[3].text == "zeta");
}

// 去重谓词保持（时间戳, 文本）：相邻的相同行被去掉。
TEST_CASE("lrc parser collapses adjacent duplicate same-timestamp lines") {
  const auto result = parseLrcText("[00:00.30]dup\n[00:00.30]dup\n[00:00.30]other\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].text == "dup");
  CHECK(result.lines[1].text == "other");
}

// 排序键只剩时间戳后 std::ranges::unique 只去相邻重复：被不同文本隔开的重复保留。
// 这是显式决定的去重语义（最小改动，不是「首次出现去重」）。
TEST_CASE("lrc parser preserves non-adjacent duplicate same-timestamp lines") {
  const auto result = parseLrcText("[00:00.40]dup\n[00:00.40]mid\n[00:00.40]dup\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].text == "dup");
  CHECK(result.lines[1].text == "mid");
  CHECK(result.lines[2].text == "dup");
}

// G8 钉子：trimAscii 只去 ' '/'\t'/'\r'，全角空格（U+3000）不得被 trim（它由算法侧单独
// 处理）。trimAscii 位于匿名命名空间、不可从测试目标链接，故经公开入口 parseLrcText 做行为断言。
TEST_CASE("lrc parser trims ascii whitespace but preserves ideographic space") {
  const std::string ideographicSpace = "\xE3\x80\x80";
  std::string input;
  input += "[00:00.00] ";
  input += ideographicSpace;
  input += "a";
  input += ideographicSpace;
  input += " \n";
  input += "[00:00.01]  b  \n";

  const auto result = parseLrcText(std::move(input));

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].text == ideographicSpace + "a" + ideographicSpace);
  CHECK(result.lines[1].text == "b");
}

TEST_CASE("lrc parser accepts metadata-only files as empty lyrics") {
  const auto result = parseLrcText("[ti:Song]\n[ar:Artist]\n[al:Album]\n");

  CHECK(result.errors.empty());
  CHECK(result.lines.empty());
}

TEST_CASE("lrc parser reports malformed timestamps as recoverable structured errors") {
  const auto result = parseLrcText("[00:61.00] impossible\n[bad] value\n[00:02.000] ok\n");

  REQUIRE(result.errors.size() == 2U);
  CHECK(result.errors[0].code == LrcParseErrorCode::InvalidTimestamp);
  CHECK(result.errors[0].line == 1U);
  CHECK(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == std::chrono::milliseconds{2000});
}

TEST_CASE("lrc parser bounds file bytes and line count") {
  const auto oversized = parseLrcText("abcdef", {.maxBytes = 5U, .maxLines = 10U});
  REQUIRE(oversized.errors.size() == 1U);
  CHECK(oversized.errors.front().code == LrcParseErrorCode::FileTooLarge);

  const auto tooManyLines = parseLrcText("[00:01.00] one\n[00:02.00] two\n", {.maxBytes = 100U, .maxLines = 1U});
  REQUIRE(tooManyLines.errors.size() == 1U);
  CHECK(tooManyLines.errors.front().code == LrcParseErrorCode::TooManyLines);
  CHECK(tooManyLines.lines.size() == 1U);
}

TEST_CASE("lrc parser reads files without throwing through scanner callers") {
  test::TempScannerRoot root("scanner-lrc-file");
  const auto path = root.path() / "song.lrc";
  writeTextFile(path, "[00:00.50] hello\r\n");

  const auto result = parseLrcFile(path);

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == std::chrono::milliseconds{500});
}

// G1：`[offset:<±ms>]` 方向为 ts_new = ts_parsed − offset（正数 = 歌词提前显示 = 时间戳变小）。
// 首行原时间戳 100ms < 500ms ⇒ 相减为负 ⇒ 必须夹到 0（正 offset 独有分支）。无符号值缺省视为正。
TEST_CASE("lrc parser applies positive offset by shifting timestamps earlier and clamping at zero") {
  const auto shifted = parseLrcText("[offset:+500]\n[00:00.10]early\n[00:01.00]later\n");

  CHECK(shifted.errors.empty());
  REQUIRE(shifted.lines.size() == 2U);
  CHECK(shifted.lines[0].timestamp == std::chrono::milliseconds{0});
  CHECK(shifted.lines[0].text == "early");
  CHECK(shifted.lines[1].timestamp == std::chrono::milliseconds{500});
  CHECK(shifted.lines[1].text == "later");

  const auto bareSign = parseLrcText("[offset:500]\n[00:01.00]later\n");
  CHECK(bareSign.errors.empty());
  REQUIRE(bareSign.lines.size() == 1U);
  CHECK(bareSign.lines.front().timestamp == std::chrono::milliseconds{500});
}

// G1：负 offset（ts_new = ts + |offset|）使时间戳变大，不产生负值。
TEST_CASE("lrc parser applies negative offset by shifting timestamps later") {
  const auto shifted = parseLrcText("[offset:-500]\n[00:01.00]first\n[00:02.50]second\n");

  CHECK(shifted.errors.empty());
  REQUIRE(shifted.lines.size() == 2U);
  CHECK(shifted.lines[0].timestamp == std::chrono::milliseconds{1500});
  CHECK(shifted.lines[1].timestamp == std::chrono::milliseconds{3000});
}

// 无 offset 时行为逐字节不变：混合样例（元数据 + 多时间戳 + 前后空白 + 重复行）与既有解析一致。
// `[offset:abc]` 是无效值 ⇒ 沿用既有元数据跳过路径，不报错、不施加偏移。
TEST_CASE("lrc parser leaves timestamps unchanged without offset tag") {
  const auto result = parseLrcText("[ar:Artist]\r\n[00:02.50][00:01.00]  Same line  \r\n[00:01.00]Same line\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(result.lines[0].text == "Same line");
  CHECK(result.lines[1].timestamp == std::chrono::milliseconds{2500});
  CHECK(result.lines[1].text == "Same line");

  const auto invalid = parseLrcText("[offset:abc]\n[00:01.00]line\n");
  CHECK(invalid.errors.empty());
  REQUIRE(invalid.lines.size() == 1U);
  CHECK(invalid.lines.front().timestamp == std::chrono::milliseconds{1000});
}

// 多次出现后者覆盖前者：+500 先、-200 后 ⇒ 最终 -200 生效（1000+200=1200）。若误用首个 (+500)
// 结果为 500ms，故该断言可判负。
TEST_CASE("lrc parser lets the last offset tag win when it appears multiple times") {
  const auto result = parseLrcText("[offset:+500]\n[offset:-200]\n[00:01.00]line\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == std::chrono::milliseconds{1200});
}

// G4：整首无时间戳的纯文本 `.lrc` ⇒ 正文行全部保留（当前实现整首丢弃是本 todo 要修的缺陷）。
// 时间戳断言 `< 0`（哨兵）而非 `== 0`：哨兵独立于合法时间戳 0，不参与 D22 分组。
// 尾部重复行必须原样保留 ⇒ 同时钉住「提升晚于 unique、unsynced 不去重」这一放置决定。
TEST_CASE("lrc parser preserves whole-file unsynced plain text with negative sentinel timestamp") {
  const auto result = parseLrcText("First plain line\n\n  Second line  \nThird line\nThird line\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].text == "First plain line");
  CHECK(result.lines[0].timestamp < std::chrono::milliseconds{0});
  CHECK(result.lines[1].text == "Second line");
  CHECK(result.lines[1].timestamp < std::chrono::milliseconds{0});
  CHECK(result.lines[2].text == "Third line");
  CHECK(result.lines[2].timestamp < std::chrono::milliseconds{0});
  CHECK(result.lines[3].text == "Third line");
  CHECK(result.lines[3].timestamp < std::chrono::milliseconds{0});
}

// G4 混合形态：有任一时间戳行 ⇒ 无时间戳行按 §8.9.4 丢弃（这类行通常是文件尾制作者署名）。
// 若误把②也保留，size 会变成 3 ⇒ 可判负。既有元数据用例（纯 `[ti:]/[ar:]/[al:]` ⇒ 空）不受影响。
TEST_CASE("lrc parser drops unsynced lines in mixed files keeping only timestamped lines") {
  const auto result = parseLrcText("[00:01.00]timed one\nplain tail line\n[00:02.00]timed two\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(result.lines[0].text == "timed one");
  CHECK(result.lines[1].timestamp == std::chrono::milliseconds{2000});
  CHECK(result.lines[1].text == "timed two");
}

// 钉住「哨兵不复用 0」的另一半：`[00:00.000]` 是合法时间戳（语料 1,945 行 / 1,257 文件），
// 必须作为真实 0ms 行保留，不得被当作 unsynced 排除。
TEST_CASE("lrc parser keeps zero timestamps as real timestamps and not as unsynced marker") {
  const auto result = parseLrcText("[00:00.000]opening line\n[00:01.00]next line\n");

  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == std::chrono::milliseconds{0});
  CHECK(result.lines[0].text == "opening line");
  CHECK(result.lines[1].timestamp == std::chrono::milliseconds{1000});
  CHECK(result.lines[1].text == "next line");
}

// F2：minutes 越界（uint64→int64 回绕会产出负数，与 unsynced 哨兵 -1 碰撞）必须被拒绝为畸形时间戳；
// 任何被接受的合法时间戳恒 ≥ 0。`[323432912759040805:00.031]` 修复前实测产出 ts=-1（恰为哨兵）。
TEST_CASE("lrc parser rejects timestamps that cannot be represented as non-negative int64 milliseconds") {
  const auto sentinelCollision = parseLrcText("[323432912759040805:00.031]collides-with-sentinel\n");
  REQUIRE(sentinelCollision.errors.size() == 1U);
  CHECK(sentinelCollision.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
  CHECK(sentinelCollision.lines.empty());

  const auto negativeWrap = parseLrcText("[184467440737095516:00.00]negative-wrap\n");
  REQUIRE(negativeWrap.errors.size() == 1U);
  CHECK(negativeWrap.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
  CHECK(negativeWrap.lines.empty());

  const auto normal = parseLrcText("[00:01.00]ok\n");
  CHECK(normal.errors.empty());
  REQUIRE(normal.lines.size() == 1U);
  CHECK(normal.lines.front().timestamp == std::chrono::milliseconds{1000});
}

// F1：offset 幅值上界 —— 超大幅值（正/负各一）会让 `ts − offset` 有符号溢出（UB）。越界即按无效标签
// 跳过（同 `[offset:abc]` 路径），不报错、时间戳保持原值。
TEST_CASE("lrc parser ignores out-of-range offset magnitudes keeping timestamps unchanged") {
  const auto hugePositive = parseLrcText("[offset:9223372036854775807]\n[00:01.00]line\n");
  CHECK(hugePositive.errors.empty());
  REQUIRE(hugePositive.lines.size() == 1U);
  CHECK(hugePositive.lines.front().timestamp == std::chrono::milliseconds{1000});

  const auto hugeNegative = parseLrcText("[offset:-9223372036854775807]\n[00:01.00]line\n");
  CHECK(hugeNegative.errors.empty());
  REQUIRE(hugeNegative.lines.size() == 1U);
  CHECK(hugeNegative.lines.front().timestamp == std::chrono::milliseconds{1000});

  // 24h 边界：kMaxOffsetMagnitudeMs = 86'400'000 本身合法，+1 越界 ⇒ 无效标签跳过。
  // 取 `[1441:00.00]` = 86'460'000ms（> 24h）以免被夹 0 掩盖减法结果。
  const auto atBound = parseLrcText("[offset:+86400000]\n[1441:00.00]line\n");
  CHECK(atBound.errors.empty());
  REQUIRE(atBound.lines.size() == 1U);
  CHECK(atBound.lines.front().timestamp == std::chrono::milliseconds{60000});

  const auto overBound = parseLrcText("[offset:+86400001]\n[1441:00.00]line\n");
  CHECK(overBound.errors.empty());
  REQUIRE(overBound.lines.size() == 1U);
  CHECK(overBound.lines.front().timestamp == std::chrono::milliseconds{86460000});
}

// NEW-1：双符号 offset 不得绕过幅值上界（旧实现按**有符号** int64 解析，第二个符号被继续接受 ⇒
// magnitude 变负 ⇒ 负数通不过 `> kMaxOffsetMagnitudeMs` ⇒ 上界被静默绕过，`-magnitude` 与 `ts − offset`
// 均可有符号溢出 UB）。改为**无符号**解析后，剩余以符号开头的输入必然解析失败 ⇒ 按无效标签跳过。
TEST_CASE("lrc parser ignores double-signed offset tags that could bypass the magnitude bound") {
  const auto doubleSigns = std::array<std::string_view, 6>{
      "[offset:+-100000000]\n[00:01.00]line\n",   "[offset:--100000000]\n[00:01.00]line\n",
      "[offset:+-9223372036854775807]\n[00:01.00]line\n", "[offset:--9223372036854775808]\n[00:01.00]line\n",
      "[offset:++500]\n[00:01.00]line\n",          "[offset:-+500]\n[00:01.00]line\n"};

  for (const auto input : doubleSigns) {
    const auto result = parseLrcText(std::string{input});
    CHECK(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    CHECK(result.lines.front().timestamp == std::chrono::milliseconds{1000});
  }
}

// F6：offset × unsynced 最高风险交互 —— offset 标签带 `[...]` 故不被收进正文；提升晚于 offset 应用，
// 故哨兵行不经 `max(0,·)` 夹 0。断言 `== -1ms`（强于 `< 0`）钉住哨兵未被夹成 0；offset 在正文前后皆然。
TEST_CASE("lrc parser keeps unsynced sentinel negative when an offset tag is present") {
  const auto offsetBefore = parseLrcText("[offset:+500]\nFirst line\nSecond line\n");
  CHECK(offsetBefore.errors.empty());
  REQUIRE(offsetBefore.lines.size() == 2U);
  CHECK(offsetBefore.lines[0].timestamp == std::chrono::milliseconds{-1});
  CHECK(offsetBefore.lines[0].text == "First line");
  CHECK(offsetBefore.lines[1].timestamp == std::chrono::milliseconds{-1});
  CHECK(offsetBefore.lines[1].text == "Second line");

  const auto offsetAfter = parseLrcText("First line\n[offset:+500]\nSecond line\n");
  CHECK(offsetAfter.errors.empty());
  REQUIRE(offsetAfter.lines.size() == 2U);
  CHECK(offsetAfter.lines[0].timestamp == std::chrono::milliseconds{-1});
  CHECK(offsetAfter.lines[0].text == "First line");
  CHECK(offsetAfter.lines[1].timestamp == std::chrono::milliseconds{-1});
  CHECK(offsetAfter.lines[1].text == "Second line");
}

// G3（增强 LRC 逐字标签剥离）：`<mm:ss.xx>` 只剥离标签、丢弃逐字时间轴（不实现逐字高亮），
// 剥离后文本 = 「按标签切分的文字片段原样拼接」。多个标签 / 标签紧贴文字 / 连续标签均适用。
TEST_CASE("lrc parser strips inline per-character timestamp tags keeping merged text") {
  const auto basic = parseLrcText("[00:12.00]<00:12.00>Hello <00:12.50>world\n");
  CHECK(basic.errors.empty());
  REQUIRE(basic.lines.size() == 1U);
  CHECK(basic.lines.front().timestamp == std::chrono::milliseconds{12000});
  CHECK(basic.lines.front().text == "Hello world");

  // 3 位小数逐字标签（§8.9.3 允许 mm:ss.xxx）。
  const auto threeDigits = parseLrcText("[00:12.00]<00:12.340>Hello\n");
  CHECK(threeDigits.errors.empty());
  REQUIRE(threeDigits.lines.size() == 1U);
  CHECK(threeDigits.lines.front().text == "Hello");

  // 标签紧贴多字节文字、一行多个、无边界：剥离后必须是文字片段的原样拼接。
  const auto adjacent = parseLrcText("[00:00.00]你<00:01.00>好<00:02.00>\n");
  CHECK(adjacent.errors.empty());
  REQUIRE(adjacent.lines.size() == 1U);
  CHECK(adjacent.lines.front().text == "你好");
}

// 连续标签 / 一行 3+ 个标签 / 整行只有标签。整行只有标签时剥离结果为空串：既有解析对「带时间戳
// 但正文为空」的行同样保留为空文本 `LyricLine`（空正文不会触发 timestamps.empty() 跳过），
// 故此行为与既有语义一致，且不改变既有用例。
TEST_CASE("lrc parser strips consecutive and many inline timestamp tags") {
  const auto consecutive = parseLrcText("[00:01.00]<00:01.00><00:02.00>text\n");
  CHECK(consecutive.errors.empty());
  REQUIRE(consecutive.lines.size() == 1U);
  CHECK(consecutive.lines.front().text == "text");

  const auto many = parseLrcText("[00:01.00]<00:01.00>a<00:02.00>b<00:03.00>c<00:04.00>\n");
  CHECK(many.errors.empty());
  REQUIRE(many.lines.size() == 1U);
  CHECK(many.lines.front().text == "abc");

  const auto onlyTags = parseLrcText("[00:01.00]<00:01.00><00:02.00>\n");
  CHECK(onlyTags.errors.empty());
  REQUIRE(onlyTags.lines.size() == 1U);
  CHECK(onlyTags.lines.front().text.empty());
}

// 反例（§8.9.3「比较运算」歧义，宁缺勿滥）：只有严格 mm:ss.xx / mm:ss.xxx 形态才剥离；其余 `<`/`>`
// 是正文，必须逐字节保留。含无配对的 `>`、秒越界与超长小数等边界。
TEST_CASE("lrc parser preserves non-timestamp angle brackets verbatim") {
  const std::array<std::pair<std::string_view, std::string_view>, 10> preserved{{
      {"[00:01.00]a < b\n", "a < b"},
      {"[00:01.00]1 < 2 > 0\n", "1 < 2 > 0"},
      {"[00:01.00]a <b> c\n", "a <b> c"},
      {"[00:01.00]<3:4>\n", "<3:4>"},
      {"[00:01.00]<00:1.0>\n", "<00:1.0>"},
      {"[00:01.00]<abc>\n", "<abc>"},
      {"[00:01.00]<00:12>\n", "<00:12>"},
      {"[00:01.00]<00:12.3456>\n", "<00:12.3456>"},
      {"[00:01.00]< 00:12.34 >\n", "< 00:12.34 >"},
      {"[00:01.00]<00:61.00>\n", "<00:61.00>"},
  }};

  for (const auto& [input, expected] : preserved) {
    CAPTURE(input);
    const auto result = parseLrcText(std::string{input});
    CHECK(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    CHECK(std::string_view{result.lines.front().text} == expected);
  }
}

// 与既有行为正交：逐字标签剥离只改正文文本，不改行首时间戳、多时间戳展开与 offset 应用。
TEST_CASE("lrc parser strips inline tags without disturbing bracket timestamps or offset") {
  const auto multi = parseLrcText("[00:02.50][00:01.00]<00:01.00>Same line\n");
  CHECK(multi.errors.empty());
  REQUIRE(multi.lines.size() == 2U);
  CHECK(multi.lines[0].timestamp == std::chrono::milliseconds{1000});
  CHECK(multi.lines[0].text == "Same line");
  CHECK(multi.lines[1].timestamp == std::chrono::milliseconds{2500});
  CHECK(multi.lines[1].text == "Same line");

  const auto offset = parseLrcText("[offset:+500]\n[00:01.00]<00:01.00>line\n");
  CHECK(offset.errors.empty());
  REQUIRE(offset.lines.size() == 1U);
  CHECK(offset.lines.front().timestamp == std::chrono::milliseconds{500});
  CHECK(offset.lines.front().text == "line");
}

TEST_CASE("cue sheet path classification recognizes lowercase and uppercase extensions") {
  test::TempScannerRoot root("scanner-cue-extensions");
  writeTextFile(root.path() / "album.cue", "FILE \"album.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / "soundtrack.CUE", "FILE \"track.ape\" APE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / "mixed.Cue", "FILE \"audio.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  const auto& lowercase = requireRelativePath(entries, "album.cue");
  CHECK(lowercase.kind == PathEntryKind::CueSheet);

  const auto& uppercase = requireRelativePath(entries, "soundtrack.CUE");
  CHECK(uppercase.kind == PathEntryKind::CueSheet);

  const auto& mixedCase = requireRelativePath(entries, "mixed.Cue");
  CHECK(mixedCase.kind == PathEntryKind::CueSheet);
}

TEST_CASE("cue sheet classification does not depend on file content or parsing") {
  test::TempScannerRoot root("scanner-cue-content-independent");
  writeTextFile(root.path() / "empty.cue", "");
  writeTextFile(root.path() / "garbage.cue", "not valid cue sheet content\n");
  writeTextFile(root.path() / "binary.cue", "\x00\x01\x02\xFF\xFE");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  CHECK(requireRelativePath(entries, "empty.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "garbage.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "binary.cue").kind == PathEntryKind::CueSheet);
}

TEST_CASE("cue sheet classification works in subdirectories and with non-ascii names") {
  test::TempScannerRoot root("scanner-cue-paths");
  writeTextFile(root.path() / "nested" / "deep" / "album.cue", "FILE \"track.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / std::filesystem::path{u8"古典音乐.cue"}, "FILE \"track.ape\" APE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / "names with spaces.CUE", "FILE \"audio.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  CHECK(requireRelativePath(entries, "nested/deep/album.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "古典音乐.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "names with spaces.CUE").kind == PathEntryKind::CueSheet);
}

TEST_CASE("cue sheet classification distinguishes from similar extensions") {
  test::TempScannerRoot root("scanner-cue-similar");
  writeTextFile(root.path() / "valid.cue", "FILE \"album.flac\" FLAC\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
  writeTextFile(root.path() / "not-cue.log", "some text file\n");
  writeTextFile(root.path() / "also-not.cu", "cuda file maybe\n");
  writeTextFile(root.path() / "prefix.cue.bak", "backup of cue\n");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  CHECK(requireRelativePath(entries, "valid.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "not-cue.log").kind == PathEntryKind::Unsupported);
  CHECK(requireRelativePath(entries, "also-not.cu").kind == PathEntryKind::Unsupported);
  CHECK(requireRelativePath(entries, "prefix.cue.bak").kind == PathEntryKind::Unsupported);
}

TEST_CASE("lyrics sidecar predicate and source mapping cover the supported extension set") {
  for (const char* extension : {".lrc", ".srt", ".ass", ".ttml", ".txt"}) {
    CAPTURE(extension);
    CHECK(isLyricsSidecarPath(std::filesystem::path{std::string{"song"} + extension}));
  }
  for (const char* extension : {".krc", ".qrc", ".yrc"}) {
    CAPTURE(extension);
    CHECK_FALSE(isLyricsSidecarPath(std::filesystem::path{std::string{"song"} + extension}));
  }
  CHECK(isLyricsSidecarPath("SONG.SRT"));
  CHECK(static_cast<int>(LyricsSource::None) == 0);
  CHECK(static_cast<int>(LyricsSource::EmbeddedTag) == 1);
  CHECK(static_cast<int>(LyricsSource::ExternalLrc) == 2);
  CHECK(lyricsSourceForSidecarPath("song.lrc") == LyricsSource::ExternalLrc);
  CHECK(lyricsSourceForSidecarPath("song.srt") == LyricsSource::ExternalSrt);
  CHECK(lyricsSourceForSidecarPath("song.ass") == LyricsSource::ExternalAss);
  CHECK(lyricsSourceForSidecarPath("song.ttml") == LyricsSource::ExternalTtml);
  CHECK(lyricsSourceForSidecarPath("song.txt") == LyricsSource::ExternalText);
  CHECK(lyricsSourceForSidecarPath("song.krc") == LyricsSource::None);
  const auto candidates = candidateLyricsSidecarPaths("/music/song.flac");
  REQUIRE(candidates.size() == 5U);
  CHECK(candidates[0].filename() == "song.lrc");
  CHECK(candidates[1].filename() == "song.srt");
  CHECK(candidates[2].filename() == "song.ass");
  CHECK(candidates[3].filename() == "song.ttml");
  CHECK(candidates[4].filename() == "song.txt");
}

TEST_CASE("lyrics sidecar resolution picks the highest priority existing candidate") {
  test::TempScannerRoot root("scanner-sidecar-priority");
  const auto audio = root.path() / "song.flac";
  writeTextFile(audio, "audio");
  CHECK(resolveLyricsSidecarPath(audio).empty());

  writeTextFile(root.path() / "song.txt", "plain\n");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.txt");

  writeTextFile(root.path() / "song.ttml", "<tt><body><p begin=\"1s\">a</p></body></tt>\n");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.ttml");

  writeTextFile(root.path() / "song.ass", "[Events]\n");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.ass");

  writeTextFile(root.path() / "song.srt", "1\n00:00:01,000 --> 00:00:02,000\na\n");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.srt");

  writeTextFile(root.path() / "song.lrc", "[00:01.00]a\n");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.lrc");

  const auto classified = classifyScannerPath(root.path(), audio);
  REQUIRE(classified.sidecarLyricsPath.has_value());
  CHECK(classified.sidecarLyricsPath->filename() == "song.lrc");

  std::filesystem::remove(root.path() / "song.lrc");
  CHECK(resolveLyricsSidecarPath(audio).filename() == "song.srt");
}

TEST_CASE("plain text lyrics sidecar acceptance enforces the line ceiling") {
  CHECK_FALSE(acceptsPlainTextLyricsSidecar({}));
  std::vector<LyricLine> lines(kPlainTextLyricsMaxLines, LyricLine{std::chrono::milliseconds{-1}, "line"});
  CHECK(acceptsPlainTextLyricsSidecar(lines));
  lines.push_back(LyricLine{std::chrono::milliseconds{-1}, "line"});
  CHECK_FALSE(acceptsPlainTextLyricsSidecar(lines));
}

TEST_CASE("lyrics sidecar parse dispatch routes each extension to its matching parser") {
  test::TempScannerRoot root("scanner-sidecar-dispatch");
  const auto lrc = root.path() / "song.lrc";
  writeTextFile(lrc, "[00:01.00]lrc line\n");
  const auto srt = root.path() / "song.srt";
  writeTextFile(srt, "1\n00:00:02,000 --> 00:00:03,000\nsrt line\n");
  const auto ass = root.path() / "song.ass";
  writeTextFile(ass,
                "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,ass line\n");
  const auto ttml = root.path() / "song.ttml";
  writeTextFile(ttml, "<tt><body><p begin=\"4s\">ttml line</p></body></tt>\n");
  const auto txt = root.path() / "song.txt";
  writeTextFile(txt, "txt line\n");

  const auto lrcResult = parseLyricsSidecarFile(lrc);
  REQUIRE(lrcResult.lines.size() == 1U);
  CHECK(lrcResult.lines[0].text == "lrc line");
  CHECK(lrcResult.lines[0].timestamp == std::chrono::milliseconds{1000});

  const auto srtResult = parseLyricsSidecarFile(srt);
  REQUIRE(srtResult.lines.size() == 1U);
  CHECK(srtResult.lines[0].text == "srt line");
  CHECK(srtResult.lines[0].timestamp == std::chrono::milliseconds{2000});

  const auto assResult = parseLyricsSidecarFile(ass);
  REQUIRE(assResult.lines.size() == 1U);
  CHECK(assResult.lines[0].text == "ass line");
  CHECK(assResult.lines[0].timestamp == std::chrono::milliseconds{3000});

  const auto ttmlResult = parseLyricsSidecarFile(ttml);
  REQUIRE(ttmlResult.lines.size() == 1U);
  CHECK(ttmlResult.lines[0].text == "ttml line");
  CHECK(ttmlResult.lines[0].timestamp == std::chrono::milliseconds{4000});

  const auto txtResult = parseLyricsSidecarFile(txt);
  REQUIRE(txtResult.lines.size() == 1U);
  CHECK(txtResult.lines[0].text == "txt line");
  CHECK(txtResult.lines[0].timestamp < std::chrono::milliseconds{0});

  CHECK(parseLyricsSidecarFile(root.path() / "song.flac").lines.empty());
}

// 阶段 2 地基不变式：凡进入索引的路径（音频 / .cue），都必须落在目录树哈希的相关性集合内。
// 若两者错位，该路径的变化与消失既不改变哈希、也不置脏 → 周期探测永远发现不了（静默漏变化）；
// 且其「枚举不完整」兜底通道同时失效。当前该关系仅由 classifyScannerPath 与
// isLibraryRelevantPath 共用 isSupportedAudioExtension 维系，此处固化为回归。同 Bazel PR #22615。
TEST_CASE("scanner path discovery keeps every indexable path inside the tree hash relevance set") {
  test::TempScannerRoot root("scanner-paths-hash-relevance-invariant");
  static_cast<void>(test::writeAudioFixture(root.path(), "song.flac"));
  std::filesystem::create_directories(root.path() / "sub");
  static_cast<void>(test::writeAudioFixture(root.path() / "sub", "nested.flac"));
  writeTextFile(root.path() / "notes.txt", "unrelated text");
  writeTextFile(root.path() / "cover.jpg", "cover bytes");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  std::size_t indexable = 0;
  for (const auto& entry : entries) {
    if (entry.kind != PathEntryKind::AudioCandidate && entry.kind != PathEntryKind::SingleFileRoot &&
        entry.kind != PathEntryKind::CueSheet) {
      continue;
    }
    ++indexable;
    CAPTURE(entry.relativeUtf8);
    CHECK(isLibraryRelevantPath(entry.path));
  }
  CHECK(indexable == 2U);
}

#if !defined(_WIN32)
// 不可读目录根：stat 成功但 opendir 失败（skip_permission_denied 会静默吞掉 EACCES，必须用
// 无选项预探测）。discoverScannerPaths 须把根条目降级为 PermissionDenied（size==1 守卫
// → rootUnavailable，调用方保留索引），而不是返回"空枚举"。
TEST_CASE("scanner path discovery marks an unreadable directory root as unavailable") {
  test::TempScannerRoot root("scanner-paths-unreadable-root");
  if (::geteuid() == 0) {
    MESSAGE("running as root: permission bits are not enforced; skipping");
    return;
  }
  writeTextFile(root.path() / "song.flac", "audio");

  struct RootPermissionGuard {
    std::filesystem::path path;
    ~RootPermissionGuard() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } permissionGuard{root.path()};
  std::error_code permissionError;
  std::filesystem::permissions(root.path(), std::filesystem::perms::none, permissionError);
  REQUIRE_FALSE(permissionError);

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  REQUIRE(entries.size() == 1U);
  CHECK(entries.front().kind == PathEntryKind::PermissionDenied);
  REQUIRE_FALSE(entries.front().errors.empty());
}

// 不可读子目录（非根）：目录自身被枚举到但内容被 skip_permission_denied 静默跳过，必须显式
// 探测并 surface 为 PermissionDenied 条目（部分枚举信号），根条目仍是 DirectoryRoot（可读根
// 不得被误判为 rootUnavailable）。
TEST_CASE("scanner path discovery surfaces an unreadable subdirectory as a permission error") {
  test::TempScannerRoot root("scanner-paths-unreadable-subdir");
  if (::geteuid() == 0) {
    MESSAGE("running as root: permission bits are not enforced; skipping");
    return;
  }
  const auto audio = test::writeAudioFixture(root.path(), "song.flac");
  const auto denied = root.path() / "denied";
  writeTextFile(denied / "hidden.flac", "audio");

  struct DeniedPermissionGuard {
    std::filesystem::path path;
    ~DeniedPermissionGuard() {
      std::error_code error;
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    }
  } permissionGuard{denied};
  std::error_code permissionError;
  std::filesystem::permissions(denied, std::filesystem::perms::none, permissionError);
  REQUIRE_FALSE(permissionError);

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  CHECK(entries.front().kind == PathEntryKind::DirectoryRoot);
  const auto deniedEntry = std::ranges::find(entries, PathEntryKind::PermissionDenied, &ClassifiedPath::kind);
  REQUIRE(deniedEntry != entries.end());
  CHECK(deniedEntry->path == denied);
  REQUIRE_FALSE(deniedEntry->errors.empty());
  // 可读歌曲仍被完整枚举（部分枚举不阻断其余条目）。
  CHECK(requireRelativePath(entries, "song.flac").kind == PathEntryKind::AudioCandidate);
  CHECK(requireRelativePath(entries, "song.flac").path == audio);
}

// 子项分类错误（symlink 环 → Error 条目）不构成 rootUnavailable：根条目仍处理、可读文件
// 仍在枚举中，错误条目只作为"部分枚举"信号交给调用方置脏。
TEST_CASE("scanner path discovery keeps a readable root with child errors") {
  test::TempScannerRoot root("scanner-paths-child-error");
  const auto audio = test::writeAudioFixture(root.path(), "song.flac");
  std::error_code linkError;
  std::filesystem::create_symlink("b-link", root.path() / "a-link", linkError);
  if (linkError) {
    MESSAGE("symlink creation unavailable: " << linkError.message());
    return;
  }
  std::filesystem::create_symlink("a-link", root.path() / "b-link", linkError);
  REQUIRE_FALSE(linkError);

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true}, {.followSymlinks = true});

  CHECK(entries.size() > 1U);
  CHECK(entries.front().kind == PathEntryKind::DirectoryRoot);
  CHECK(std::ranges::any_of(entries, [](const ClassifiedPath& entry) { return entry.kind == PathEntryKind::Error; }));
  const auto& song = requireRelativePath(entries, "song.flac");
  CHECK(song.kind == PathEntryKind::AudioCandidate);
  CHECK(song.path == audio);
}
#endif

// ── G6（行内元数据 / 制作人员行丢弃）───────────────────────────────────────────
// 判据镜像算法侧 `clean_line`（设计文档 §6.2 阶段 A）：先剥时间戳 → 再剥行内 `[tag:value]`
// → 最后判元数据/制作人员（P1 顺序陷阱，§6.3）。语料实测形态：① `[ts]作词 : … [by:…]`
// 1039 文件 / 2300 行；② `[ts][by:…]`（纯元数据）90 文件 / 90 行；③ 正文 + 行内 tag 162 文件；
// 裸时间戳（无正文）837 文件 / 7496 行；G7 `[tr:…]`/`[lang:…]` 0 文件。

// ① 制作人员行 + ② 纯元数据行 + 裸时间戳行 ⇒ 一律丢弃；带时间戳的正文行一条不丢。
// ① 同时是 P1 顺序的可判负钉子：若先判制作人员/元数据、后剥时间戳，行首 `[00:00.000]`
// 会遮住 `作词`/`[by:` 两个行首锚定判据 ⇒ 本用例必须失败。
TEST_CASE("lrc parser drops credit-only metadata-only and bare timestamp lines") {
  const auto credit = parseLrcText("[00:00.000]作词 : みゅー [by:agexnit]\n[00:01.00]real lyric\n");
  CHECK(credit.errors.empty());
  REQUIRE(credit.lines.size() == 1U);
  CHECK(credit.lines.front().timestamp == std::chrono::milliseconds{1000});
  CHECK(credit.lines.front().text == "real lyric");

  const auto pureMetadata = parseLrcText("[00:00.000][by:夜羽小猫]\n[00:01.00]real lyric\n");
  CHECK(pureMetadata.errors.empty());
  REQUIRE(pureMetadata.lines.size() == 1U);
  CHECK(pureMetadata.lines.front().text == "real lyric");

  // 裸时间戳（无正文）：与 `clean_line` 的 `if not body: return None` 一致地丢弃。
  const auto bare = parseLrcText("[00:00.19]\n[00:01.00]real lyric\n");
  CHECK(bare.errors.empty());
  REQUIRE(bare.lines.size() == 1U);
  CHECK(bare.lines.front().text == "real lyric");

  // 多时间戳 + 纯元数据 ⇒ 整行丢弃（不得按时间戳展开成多个空文本行）。
  const auto multiTag = parseLrcText("[00:01.00][00:02.00][by:x]\n[00:03.00]real lyric\n");
  CHECK(multiTag.errors.empty());
  REQUIRE(multiTag.lines.size() == 1U);
  CHECK(multiTag.lines.front().text == "real lyric");

  // 正文位置只剩元数据标签与空白 ⇒ 同样丢弃。
  const auto onlyMeta = parseLrcText("[00:01.00] [ti:title] \n[00:02.00]ok\n");
  CHECK(onlyMeta.errors.empty());
  REQUIRE(onlyMeta.lines.size() == 1U);
  CHECK(onlyMeta.lines.front().text == "ok");

  // 半角/全角冒号 + 冒号前后空白均命中制作人员判据。
  const auto colons = parseLrcText("[00:01.00]作曲：someone\n[00:02.00]编曲 :  someone\n[00:03.00]ok\n");
  CHECK(colons.errors.empty());
  REQUIRE(colons.lines.size() == 1U);
  CHECK(colons.lines.front().text == "ok");
}

// ③ 正文 + 行内 `[tag:value]`：标签剥离、正文保留（镜像 `removeInlineTags` 的全局非重叠删除）。
TEST_CASE("lrc parser strips inline metadata tags from lyric bodies") {
  const auto trailing = parseLrcText("[00:01.00]再见 [by:匿]\n");
  CHECK(trailing.errors.empty());
  REQUIRE(trailing.lines.size() == 1U);
  CHECK(trailing.lines.front().text == "再见");

  const auto embedded = parseLrcText("[00:01.00]前[ar:Artist]后\n");
  CHECK(embedded.errors.empty());
  REQUIRE(embedded.lines.size() == 1U);
  CHECK(embedded.lines.front().text == "前后");

  const auto multiple = parseLrcText("[00:01.00]a[ti:T]b[al:A]c\n");
  CHECK(multiple.errors.empty());
  REQUIRE(multiple.lines.size() == 1U);
  CHECK(multiple.lines.front().text == "abc");

  // 空 value 与下划线仍属行内 tag；不匹配的 `[...]`（无 `word:` 形态）逐字节保留。
  const auto shapes = parseLrcText("[00:01.00]a[by:]b[_k:v]c\n");
  CHECK(shapes.errors.empty());
  REQUIRE(shapes.lines.size() == 1U);
  CHECK(shapes.lines.front().text == "abc");

  const auto bracketsKept = parseLrcText("[00:01.00]a [not a tag] b\n");
  CHECK(bracketsKept.errors.empty());
  REQUIRE(bracketsKept.lines.size() == 1U);
  CHECK(bracketsKept.lines.front().text == "a [not a tag] b");
}

// ④ 反例（不得误伤）：制作人员关键词出现在正文中（非行首，或行首但无冒号）⇒ 必须保留。
TEST_CASE("lrc parser preserves credit keywords appearing inside lyric bodies") {
  const std::array<std::pair<std::string_view, std::string_view>, 6> preserved{{
      {"[00:01.00]他说作词很难\n", "他说作词很难"},
      {"[00:01.00]这首歌的作曲是某人\n", "这首歌的作曲是某人"},
      {"[00:01.00]by the way\n", "by the way"},
      {"[00:01.00]Bob Dylan\n", "Bob Dylan"},
      {"[00:01.00]曲终人散\n", "曲终人散"},
      {"[00:01.00]歌词里没有冒号作词\n", "歌词里没有冒号作词"},
  }};

  for (const auto& [input, expected] : preserved) {
    CAPTURE(input);
    const auto result = parseLrcText(std::string{input});
    CHECK(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    CHECK(std::string_view{result.lines.front().text} == expected);
  }
}

// G6 空 body 判据的检查点必须**早于**逐字标签剥离：`[ts]<逐字标签>` 在 G6 阶段 body 非空
// ⇒ 保留空文本行（与既有断言一致）；`[ts][by:x]` 在 G6 阶段 body 即为空 ⇒ 丢弃。
// 若判据挪到逐字剥离之后，两者都成空串、行为不可区分（且会误丢前者的既有不变式）。
TEST_CASE("lrc parser evaluates G6 on the body before karaoke stripping") {
  const auto karaokeOnly = parseLrcText("[00:01.00]<00:01.00><00:02.00>\n");
  CHECK(karaokeOnly.errors.empty());
  REQUIRE(karaokeOnly.lines.size() == 1U);
  CHECK(karaokeOnly.lines.front().text.empty());

  const auto inlineTagOnly = parseLrcText("[00:01.00][by:x]\n");
  CHECK(inlineTagOnly.errors.empty());
  CHECK(inlineTagOnly.lines.empty());
}

// G7 钉子：`[tr:…]`/`[lang:…]` **不实现**语义 —— 与其它 `[tag:value]` 同样被丢弃，不产生
// 翻译/语言输出。`LyricLine` 契约未变（编译期钉住不得新增这类字段）。
template <typename T, typename = void>
struct HasTranslationField : std::false_type {};
template <typename T>
struct HasTranslationField<T, std::void_t<decltype(std::declval<T>().translation)>> : std::true_type {};

template <typename T, typename = void>
struct HasTranslatedTextField : std::false_type {};
template <typename T>
struct HasTranslatedTextField<T, std::void_t<decltype(std::declval<T>().translatedText)>> : std::true_type {};

template <typename T, typename = void>
struct HasLanguageField : std::false_type {};
template <typename T>
struct HasLanguageField<T, std::void_t<decltype(std::declval<T>().language)>> : std::true_type {};

template <typename T, typename = void>
struct HasLangField : std::false_type {};
template <typename T>
struct HasLangField<T, std::void_t<decltype(std::declval<T>().lang)>> : std::true_type {};

template <typename T, typename = void>
struct HasTargetLangField : std::false_type {};
template <typename T>
struct HasTargetLangField<T, std::void_t<decltype(std::declval<T>().targetLang)>> : std::true_type {};

TEST_CASE("lrc parser does not implement tr or lang semantics and drops those lines") {
  static_assert(!HasTranslationField<LyricLine>::value);
  static_assert(!HasTranslatedTextField<LyricLine>::value);
  static_assert(!HasLanguageField<LyricLine>::value);
  static_assert(!HasLangField<LyricLine>::value);
  static_assert(!HasTargetLangField<LyricLine>::value);

  const auto trOnly = parseLrcText("[00:00.00][tr:zh]\n[00:01.00]real lyric\n");
  CHECK(trOnly.errors.empty());
  REQUIRE(trOnly.lines.size() == 1U);
  CHECK(trOnly.lines.front().text == "real lyric");

  const auto langOnly = parseLrcText("[00:00.00][lang:zh]\n[00:01.00]real lyric\n");
  CHECK(langOnly.errors.empty());
  REQUIRE(langOnly.lines.size() == 1U);
  CHECK(langOnly.lines.front().text == "real lyric");

  // 行内出现时只剥离标签、保留正文（不得据此新增译文/语言输出）。
  const auto inlineTag = parseLrcText("[00:01.00]hello [tr:zh]\n");
  CHECK(inlineTag.errors.empty());
  REQUIRE(inlineTag.lines.size() == 1U);
  CHECK(inlineTag.lines.front().text == "hello");
}

// G6 同样作用于 G4 的无时间戳行：命中元数据/制作人员判据的丢弃；整首纯文本歌词不被误杀。
TEST_CASE("lrc parser applies G6 to unsynced lines without dropping plain lyrics") {
  const auto dropped = parseLrcText("作词 : someone\nFirst plain line\n");
  CHECK(dropped.errors.empty());
  REQUIRE(dropped.lines.size() == 1U);
  CHECK(dropped.lines.front().text == "First plain line");
  CHECK(dropped.lines.front().timestamp < std::chrono::milliseconds{0});

  const auto kept = parseLrcText("First plain line\nSecond line\n");
  CHECK(kept.errors.empty());
  REQUIRE(kept.lines.size() == 2U);
}

// ── G5a：纯文本歌词解析器（编码 6 步回退 + unsynced 哨兵）────────────────────────
#ifdef SERIONA_PLAIN_TEXT_FIXTURE_DIR

const std::filesystem::path& plainTextFixtureDir() {
  static const std::filesystem::path dir{SERIONA_PLAIN_TEXT_FIXTURE_DIR};
  return dir;
}

const std::string kPlainLine1 = "第一行 原曲";
const std::string kPlainLine2 = "第二行 歌词";
const std::string kPlainLine3 = "第三行";

// big5.txt / shift_jis.txt 的字节在 TEXT_ENCODINGS 顺序下被 gb18030 抢先解出的结果。
// 转义给出，避免源码里混入私有区/兼容区码点。
const std::string kBig5AsGb18030Line1 = "\xE6\x9D\x90\xEE\x97\xA6\xEF\xB8\xBD \xEE\x85\xA5\xCE\xA1";
const std::string kBig5AsGb18030Line2 = "\xE6\x9D\x90\xEE\x97\xAD\xEF\xB8\xBD \xE7\xB0\x88\xE8\xBF\xAD";
const std::string kBig5AsGb18030Line3 = "\xE6\x9D\x90\xEE\x97\xBA\xEF\xB8\xBD";
const std::string kSjisAsGb18030Line1 = "\xE5\x81\x99\xE5\x82\xAB\xE5\x81\xB5\xE5\x81\xAA\xE5\x81\xBC";
const std::string kSjisAsGb18030Line2 = "\xE5\x81\x9D\xE5\x82\x9B\xE5\x81\x86\xE5\x81\xB4\xE5\x82\x9C";
const std::string kSjisAsGb18030Line3 = "\xE5\x81\x81\xE5\x82\x9D\xE5\x81\x91\xE5\x81\xB2\xE5\x81\x86";

// G5a 的每一行都必须是 todo 13 定死的 unsynced 哨兵（< 0，具体 -1ms），不得是 0 或正数。
void requirePlainTextUnsyncedLines(const LrcParseResult& result, std::size_t expectedCount) {
  REQUIRE(result.lines.size() == expectedCount);
  for (const auto& line : result.lines) {
    CHECK(line.timestamp == std::chrono::milliseconds{-1});
    CHECK(line.timestamp < std::chrono::milliseconds{0});
  }
}

TEST_CASE("plain text lyrics decode utf8 with and without bom") {
  for (const char* name : {"utf8.txt", "utf8-bom.txt"}) {
    CAPTURE(name);
    const auto result = parsePlainTextLyricsFile(plainTextFixtureDir() / name);
    CHECK(result.errors.empty());
    requirePlainTextUnsyncedLines(result, 3U);
    CHECK(result.lines[0].text == kPlainLine1);
    CHECK(result.lines[1].text == "second line");
    CHECK(result.lines[2].text == kPlainLine3);
  }
}

TEST_CASE("plain text lyrics decode gb18030") {
  const auto result = parsePlainTextLyricsFile(plainTextFixtureDir() / "gb18030.txt");
  CHECK(result.errors.empty());
  requirePlainTextUnsyncedLines(result, 3U);
  CHECK(result.lines[0].text == kPlainLine1);
  CHECK(result.lines[1].text == kPlainLine2);
  CHECK(result.lines[2].text == kPlainLine3);
}

TEST_CASE("plain text lyrics decode utf16 with little and big endian bom") {
  for (const char* name : {"utf16le.txt", "utf16be.txt"}) {
    CAPTURE(name);
    const auto result = parsePlainTextLyricsFile(plainTextFixtureDir() / name);
    CHECK(result.errors.empty());
    requirePlainTextUnsyncedLines(result, 2U);
    CHECK(result.lines[0].text == kPlainLine1);
    CHECK(result.lines[1].text == "OK");
  }
}

// 6 步回退全失败 ⇒ 放弃该文件并记录（跳过并计数），不崩溃。
TEST_CASE("plain text lyrics skip and count files outside the encoding fallback set") {
  const auto result = parsePlainTextLyricsFile(plainTextFixtureDir() / "all-invalid.txt");
  CHECK(result.lines.empty());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == LrcParseErrorCode::UnsupportedEncoding);
  CHECK(result.errors.front().path == plainTextFixtureDir() / "all-invalid.txt");
  // 0x81 在 ICU 与 Python 的 6 步回退下均全失败（夹具字节自证见证据）。
  CHECK_FALSE(decodeLyricsBytes(std::string_view("\x81", 1)).has_value());
}

TEST_CASE("plain text lyrics mark every line with the shared unsynced sentinel") {
  const auto result = parsePlainTextLyrics("first\nsecond\n");
  CHECK(result.errors.empty());
  requirePlainTextUnsyncedLines(result, 2U);
  CHECK(result.lines[0].text == "first");
  CHECK(result.lines[1].text == "second");
  // 与 todo 13 的哨兵一致：所有行同一负值，既非 0（真实时间戳）也非按行递增的正数。
  CHECK(result.lines[0].timestamp == result.lines[1].timestamp);
}

// 纯文本侧车与 `.lrc` 的整首无时间戳（G4）路径同判据：丢元数据/制作人员行，正文里的
// 制作人员关键词（非行首或行首无冒号）不得误伤。
TEST_CASE("plain text lyrics drop metadata and credit lines but keep credit words inside lyrics") {
  const auto dropped = parsePlainTextLyrics("[ti:Song]\n作词 : someone\n[by:x]\nreal lyric\n");
  CHECK(dropped.errors.empty());
  requirePlainTextUnsyncedLines(dropped, 1U);
  CHECK(dropped.lines.front().text == "real lyric");

  const auto kept = parsePlainTextLyrics("他说作词很难\n这首歌的作曲是某人\n");
  CHECK(kept.errors.empty());
  REQUIRE(kept.lines.size() == 2U);
  CHECK(kept.lines[0].text == "他说作词很难");
  CHECK(kept.lines[1].text == "这首歌的作曲是某人");
}

// ★ 码表差异裁决的可失败钉子：ICU 与 Python 同名 gb18030 codec 对 A3 A0 给出不同码点
// （ICU U+3000，Python U+E5E5）。本实现采信 ICU 表；若未来 ICU 改为 Python 语义，本用例失败
// ⇒ 必须重审裁决（证据 README 的「码表差异裁决」）。
TEST_CASE("plain text fallback pins the registered icu versus python gb18030 divergence") {
  const auto decoded = decodeLyricsBytes(std::string_view("\xA3\xA0", 2));
  REQUIRE(decoded.has_value());
  CHECK(*decoded == "\xE3\x80\x80");                          // U+3000（ICU 表）
  CHECK(decoded->find("\xEE\x87\xA5") == std::string::npos);  // U+E5E5（Python 表）不得出现
}

// ★ 回退顺序的可失败钉子（S10 F1 订正）：**big5** 分支对良构 Big5 双字节文本不可达 —— 穷举
// ICU Big5 合法双字节 19,720 个（Python `big5` 子集 13,710 个亦然），其中「非法 UTF-8 且非法
// gb18030」= 0 ⇒ 恒被 gb18030 遮蔽。**但 shift_jis 分支对良构文本可达**（SJIS 有单字节非 ASCII
// 字符：半角片假名 0xA1–0xDF；该字节是 gb18030/big5 的前导字节，若后随 < 0x40 的字节则两者皆拒
// ⇒ shift_jis 命中）—— 该可达路径由 `shift_jis-halfwidth.txt` 与下方同名用例钉住。参考实现同序
// 同表，实测同一字节两端文本逐字节相同。若把 big5 提前或令 gb18030 拒收 Big5 字节，本用例失败。
TEST_CASE("plain text fallback decodes well formed big5 and shift_jis bytes as gb18030") {
  const auto big5 = parsePlainTextLyricsFile(plainTextFixtureDir() / "big5.txt");
  CHECK(big5.errors.empty());
  requirePlainTextUnsyncedLines(big5, 3U);
  CHECK(big5.lines[0].text == kBig5AsGb18030Line1);
  CHECK(big5.lines[1].text == kBig5AsGb18030Line2);
  CHECK(big5.lines[2].text == kBig5AsGb18030Line3);
  CHECK(big5.lines[0].text != kPlainLine1);

  const auto sjis = parsePlainTextLyricsFile(plainTextFixtureDir() / "shift_jis.txt");
  CHECK(sjis.errors.empty());
  requirePlainTextUnsyncedLines(sjis, 3U);
  CHECK(sjis.lines[0].text == kSjisAsGb18030Line1);
  CHECK(sjis.lines[1].text == kSjisAsGb18030Line2);
  CHECK(sjis.lines[2].text == kSjisAsGb18030Line3);
  CHECK(sjis.lines[0].text != "こんにちは");
}

// ★ shift_jis 分支**可达**的可失败钉子（S10 F1 实证要求；对照上一条的 big5 不可达）。
// `shift_jis-halfwidth.txt` = 半角片假名 + 后随 < 0x40 的字节（`A1 20 A2 20 A3 0A B1 20 B2 0A DF 0A`）：
// utf-8 拒（0xA1 是续接字节）、gb18030 拒（0xA1 为前导但 0x20/0x0A < 0x40）、big5 拒（同因）⇒
// shift_jis 命中。Python 参考 `read_text_file_with_codec` 对该夹具同样报 `shift_jis` 并解出同一文本。
// 断言解出的码点落在半角片假名区 U+FF61–U+FF9F（必须含 U+FF61/FF71/FF9F），以证明走的是 shift_jis
// 分支、而非末步 utf-16（12 字节偶数长度，去掉 shift_jis 后会被 utf-16LE 解成完全不同的文本）。
TEST_CASE("plain text fallback reaches the shift_jis branch for halfwidth katakana") {
  const auto result = parsePlainTextLyricsFile(plainTextFixtureDir() / "shift_jis-halfwidth.txt");
  CHECK(result.errors.empty());
  requirePlainTextUnsyncedLines(result, 3U);
  CHECK(result.lines[0].text == "\xEF\xBD\xA1 \xEF\xBD\xA2 \xEF\xBD\xA3");
  CHECK(result.lines[1].text == "\xEF\xBD\xB1 \xEF\xBD\xB2");
  CHECK(result.lines[2].text == "\xEF\xBE\x9F");
  // 逐字节确认每一行的非空格字符都以 U+FF61–U+FF9F 的 UTF-8 前缀（EF BD / EF BE）开头。
  for (const auto& line : result.lines) {
    std::size_t cursor = 0;
    while (cursor < line.text.size()) {
      if (line.text[cursor] == ' ') {
        ++cursor;
        continue;
      }
      const bool halfwidthKatakana = line.text.compare(cursor, 2, "\xEF\xBD") == 0 ||
                                     line.text.compare(cursor, 2, "\xEF\xBE") == 0;
      CHECK(halfwidthKatakana);
      REQUIRE(cursor + 3U <= line.text.size());
      cursor += 3U;
    }
  }
  // 与既有 `shift_jis.txt`（被 gb18030 解出）区分：本条不是 gb18030 遮蔽的结果。
  const auto shadowed = parsePlainTextLyricsFile(plainTextFixtureDir() / "shift_jis.txt");
  CHECK(result.lines[0].text != shadowed.lines[0].text);
}

#endif  // SERIONA_PLAIN_TEXT_FIXTURE_DIR

// ── G5b：SubRip（.srt）解析器 ────────────────────────────────────────────────
#ifdef SERIONA_SRT_FIXTURE_DIR

const std::filesystem::path& srtFixtureDir() {
  static const std::filesystem::path dir{SERIONA_SRT_FIXTURE_DIR};
  return dir;
}

const auto kSrt = [](std::int64_t millis) { return std::chrono::milliseconds{millis}; };

TEST_CASE("srt parser reads standard blocks and takes the start timestamp") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "standard-bom.srt");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kSrt(1000));
  CHECK(result.lines[0].text == "First line");
  CHECK(result.lines[1].timestamp == kSrt(5500));
  CHECK(result.lines[1].text == "Second line");
  // 01:02:03,004 = (1*3600 + 2*60 + 3)*1000 + 4
  CHECK(result.lines[2].timestamp == kSrt(3723004));
  CHECK(result.lines[2].text == "Third line");
}

TEST_CASE("srt parser keeps multi-line blocks as same-timestamp lines in file order") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "multiline.srt");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  // 块内多行共享块起始时间戳，且保持文件顺序（D22 依赖「组内第 1 行 = 原文」）。
  CHECK(result.lines[0].timestamp == kSrt(10000));
  CHECK(result.lines[1].timestamp == kSrt(10000));
  CHECK(result.lines[0].text == "Original text");
  CHECK(result.lines[1].text == "Translated text");
  CHECK(result.lines[2].timestamp == kSrt(15000));
  CHECK(result.lines[2].text == "Only one line");
}

TEST_CASE("srt parser strips html tags but preserves comparison and karaoke-like angles") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "html-tags.srt");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 6U);
  CHECK(result.lines[0].text == "italic text");
  CHECK(result.lines[1].text == "styled");
  CHECK(result.lines[2].text == "a < b and 1 < 2 > 0");
  // `<b>` 符合本实现的保守文法（HTML 粗体标签）⇒ 被剥离；该边界行为在此显式钉住。
  CHECK(result.lines[3].text == "bold");
  // SRT 路径不应用 `.lrc` 的 `<mm:ss.xx>` 逐字剥离；数字开头的 `<...>` 一律按正文保留。
  CHECK(result.lines[4].text == "<3:4> and <00:12.34>");
  CHECK(result.lines[5].text == "unmatched < here");
}

// 畸形块（时间轴格式错 / 只有序号 / 无时间轴）一律跳过；序号行与时间轴行绝不产出正文；
// 缺空行分隔时，时间轴行即块边界；纯数字行按序号行处理、不产出。
TEST_CASE("srt parser skips malformed blocks and never emits index or timeline lines") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "malformed.srt");
  REQUIRE(result.lines.size() == 6U);
  CHECK(result.lines[0].timestamp == kSrt(1000));
  CHECK(result.lines[0].text == "Valid one");
  CHECK(result.lines[1].timestamp == kSrt(5000));
  CHECK(result.lines[1].text == "Valid two");
  CHECK(result.lines[2].timestamp == kSrt(7000));
  CHECK(result.lines[2].text == "Valid three");
  CHECK(result.lines[3].timestamp == kSrt(9000));
  CHECK(result.lines[3].text == "tagged");
  CHECK(result.lines[4].timestamp == kSrt(13000));
  CHECK(result.lines[4].text == "before");
  CHECK(result.lines[5].timestamp == kSrt(15000));
  CHECK(result.lines[5].text == "after");
  REQUIRE_FALSE(result.errors.empty());
  for (const auto& error : result.errors) {
    CHECK(error.code == LrcParseErrorCode::InvalidTimestamp);
  }
  for (const auto& line : result.lines) {
    CHECK(line.text.find("-->") == std::string::npos);
    CHECK_FALSE(std::ranges::all_of(line.text, [](const char ch) { return ch >= '0' && ch <= '9'; }));
  }
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) { return line.text == "42"; }));
}

TEST_CASE("srt parser drops blocks without a valid timeline") {
  const auto malformedTimeline =
      parseSrtLyrics("1\n00:00:03.000 --> 00:00:04.000\n\n2\n00:00:05,000 --> 00:00:06,000\nok\n");
  CHECK(malformedTimeline.lines.size() == 1U);
  CHECK(malformedTimeline.lines.front().text == "ok");
  REQUIRE(malformedTimeline.errors.size() == 1U);
  CHECK(malformedTimeline.errors.front().code == LrcParseErrorCode::InvalidTimestamp);

  const auto noTimeline = parseSrtLyrics("1\nplain text without timeline\n");
  CHECK(noTimeline.lines.empty());
  REQUIRE(noTimeline.errors.size() == 1U);
  CHECK(noTimeline.errors.front().code == LrcParseErrorCode::InvalidTimestamp);

  const auto indexOnly = parseSrtLyrics("7\n\n");
  CHECK(indexOnly.lines.empty());
  CHECK(indexOnly.errors.empty());
}

TEST_CASE("srt parser accepts hour fields wider than two digits") {
  const auto wide = parseSrtLyrics("1\n100:00:00,000 --> 100:00:01,500\nWide\n");
  CHECK(wide.errors.empty());
  REQUIRE(wide.lines.size() == 1U);
  CHECK(wide.lines.front().timestamp == kSrt(360000000));

  const auto narrow = parseSrtLyrics("1\n01:00:00,000 --> 01:00:01,500\nNarrow\n");
  CHECK(narrow.errors.empty());
  REQUIRE(narrow.lines.size() == 1U);
  CHECK(narrow.lines.front().timestamp == kSrt(3600000));
}

TEST_CASE("srt parser normalizes crlf and bom line endings") {
  const auto crlf = parseSrtLyricsFile(srtFixtureDir() / "crlf.srt");
  CHECK(crlf.errors.empty());
  REQUIRE(crlf.lines.size() == 2U);
  CHECK(crlf.lines[0].text == "CRLF first");
  CHECK(crlf.lines[1].text == "CRLF second");

  const auto lf = parseSrtLyrics(
      "1\n00:00:20,000 --> 00:00:22,000\nCRLF first\n\n2\n00:00:23,000 --> 00:00:25,000\nCRLF second\n");
  CHECK(lf.errors.empty());
  REQUIRE(lf.lines.size() == 2U);
  CHECK(crlf.lines[0].timestamp == lf.lines[0].timestamp);
  CHECK(crlf.lines[0].text == lf.lines[0].text);
  CHECK(crlf.lines[1].timestamp == lf.lines[1].timestamp);
  CHECK(crlf.lines[1].text == lf.lines[1].text);
}

// 非 UTF-8 字节走与 `.txt` 同一条 6 步回退：GB18030 侧车必须解出正确文本（证明回退真的生效）。
TEST_CASE("srt lyrics decode gb18030") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "gb18030.srt");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kSrt(1000));
  CHECK(result.lines[0].text == "第一行歌词");
  CHECK(result.lines[1].text == "第二行歌词");
  CHECK(result.lines[2].text == "第三行歌词");
}

TEST_CASE("srt lyrics skip and count files outside the encoding fallback set") {
  const auto result = parseSrtLyricsFile(srtFixtureDir() / "all-invalid.srt");
  CHECK(result.lines.empty());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == LrcParseErrorCode::UnsupportedEncoding);
  CHECK(result.errors.front().path == srtFixtureDir() / "all-invalid.srt");
  CHECK_FALSE(decodeLyricsBytes(std::string_view("\x81", 1)).has_value());
}

TEST_CASE("srt parser bounds file bytes and line count") {
  const auto tooLarge = parseSrtLyrics(std::string(100U, 'x'), {.maxBytes = 10U});
  CHECK(tooLarge.lines.empty());
  REQUIRE(tooLarge.errors.size() == 1U);
  CHECK(tooLarge.errors.front().code == LrcParseErrorCode::FileTooLarge);

  const auto tooMany =
      parseSrtLyrics("1\n00:00:01,000 --> 00:00:02,000\ntext\nline\nline2\n", {.maxBytes = 1024U, .maxLines = 3U});
  REQUIRE(tooMany.errors.size() == 1U);
  CHECK(tooMany.errors.front().code == LrcParseErrorCode::TooManyLines);
  REQUIRE(tooMany.lines.size() == 1U);
  CHECK(tooMany.lines.front().text == "text");
}

#endif  // SERIONA_SRT_FIXTURE_DIR

// ── G5c：Advanced SubStation Alpha（.ass / .ssa）解析器 ────────────────────────
#ifdef SERIONA_ASS_FIXTURE_DIR

const std::filesystem::path& assFixtureDir() {
  static const std::filesystem::path dir{SERIONA_ASS_FIXTURE_DIR};
  return dir;
}

const auto kAss = [](std::int64_t millis) { return std::chrono::milliseconds{millis}; };
const std::string kAssFormat =
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

TEST_CASE("ass parser reads standard events and skips comments and foreign sections") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "standard.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "First line");
  CHECK(result.lines[1].timestamp == kAss(5500));
  CHECK(result.lines[1].text == "Second line");
  // 0:01:02.03 = ((0*60 + 1)*60 + 2)*1000 + 3*10
  CHECK(result.lines[2].timestamp == kAss(62030));
  CHECK(result.lines[2].text == "Third line");
}

// Format 列序被打乱（Start 由标准的下标 1 变为 2、Text 由 9 变为 5）仍取到正确的 Start/Text ⇒ 未硬编码列号。
TEST_CASE("ass parser resolves start and text by format column names not positions") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "format-shuffled.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kAss(2000));
  CHECK(result.lines[0].text == "Shuffled one");
  CHECK(result.lines[1].timestamp == kAss(6000));
  CHECK(result.lines[1].text == "Shuffled two");
}

TEST_CASE("ass parser keeps commas inside the trailing text field") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "comma-text.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "Hello, world, and more");
  CHECK(result.lines[1].timestamp == kAss(4000));
  CHECK(result.lines[1].text == "a,b,c");
}

// `\N` 硬换行 ⇒ 同一 timestamp 多行且保持顺序；`\n` 软换行 ⇒ 空格；`\h` 硬空格 ⇒ U+00A0。
TEST_CASE("ass parser splits hard breaks and maps soft breaks and hard spaces") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "hard-break.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].timestamp == kAss(7000));
  CHECK(result.lines[0].text == "Original line");
  CHECK(result.lines[1].timestamp == kAss(7000));
  CHECK(result.lines[1].text == "Translated line");
  CHECK(result.lines[2].timestamp == kAss(10000));
  CHECK(result.lines[2].text == "soft break here");
  CHECK(result.lines[3].timestamp == kAss(13000));
  CHECK(result.lines[3].text == "hard\xC2\xA0space");
  // `\N` 绝不能被当作字面量 N 丢弃：两行共享同一时间戳且都非空，即证明硬换行被识别。
  CHECK(result.lines[0].timestamp == result.lines[1].timestamp);
}

TEST_CASE("ass parser strips override blocks and html tags but preserves literal braces and comparisons") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "overrides.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 7U);
  CHECK(result.lines[0].text == "italic plain");
  CHECK(result.lines[1].text == "positioned");
  CHECK(result.lines[2].text == "visible");
  CHECK(result.lines[3].text == "abc");
  CHECK(result.lines[4].text == "open { brace with no close");
  CHECK(result.lines[5].text == "html and a < b");
  CHECK(result.lines[6].text == "empty block");
  // 纯覆盖块行（`{\i1}{\an8}`）剥离后为空 ⇒ 不产出，故不存在 6000ms 的行。
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.timestamp == std::chrono::milliseconds{6000};
  }));
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("\\i1") != std::string::npos || line.text.find('}') != std::string::npos;
  }));
}

// 段外的 Dialogue、Comment（即使 Start 非法）、字段不足的 Dialogue 都不产出正文；只有 [Events] 段内
// 有合法 Format 与合法 Start 的 Dialogue 才产出。
TEST_CASE("ass parser skips malformed dialogues and non events sections") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "malformed.ass");
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kAss(6000));
  CHECK(result.lines[0].text == "valid one");
  CHECK(result.lines[1].timestamp == kAss(10000));
  CHECK(result.lines[1].text == "valid two");
  // `Text` 非末列（Format: Start, Text, Layer）时其值只取到一个 token —— 该文件违反 ASS「文本在末列」
  // 的约定，行为在此显式钉住（不猜测边界、也不丢弃该行）。
  CHECK(result.lines[2].timestamp == kAss(12000));
  CHECK(result.lines[2].text == "hello");
  REQUIRE(result.errors.size() == 2U);
  for (const auto& error : result.errors) {
    CHECK(error.code == LrcParseErrorCode::InvalidTimestamp);
  }
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text == "must not be parsed" || line.text == "outside events" ||
           line.text == "before any format";
  }));
}

// 小时 1 位（`0:...`）与 2 位（`01:...`）都合法；SRT 的 `,mmm` 与越界字段一律拒绝。
TEST_CASE("ass parser accepts one and two digit hours and rejects srt style commas") {
  const auto oneDigit =
      parseAssLyrics("[Events]\n" + kAssFormat + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,one\n");
  CHECK(oneDigit.errors.empty());
  REQUIRE(oneDigit.lines.size() == 1U);
  CHECK(oneDigit.lines.front().timestamp == kAss(1000));

  const auto twoDigit =
      parseAssLyrics("[Events]\n" + kAssFormat + "Dialogue: 0,01:00:00.00,01:00:01.50,Default,,0,0,0,,two\n");
  CHECK(twoDigit.errors.empty());
  REQUIRE(twoDigit.lines.size() == 1U);
  CHECK(twoDigit.lines.front().timestamp == kAss(3600000));

  const auto srtForm = parseAssLyrics("[Events]\n" + kAssFormat +
                                      "Dialogue: 0,00:00:01,000,00:00:02,000,Default,,0,0,0,,no\n");
  CHECK(srtForm.lines.empty());
  CHECK_FALSE(srtForm.errors.empty());

  const auto threeDigitHour = parseAssLyrics("[Events]\n" + kAssFormat +
                                             "Dialogue: 0,000:00:01.00,000:00:02.00,Default,,0,0,0,,no\n");
  CHECK(threeDigitHour.lines.empty());

  const auto badSeconds = parseAssLyrics("[Events]\n" + kAssFormat +
                                         "Dialogue: 0,0:00:61.00,0:00:62.00,Default,,0,0,0,,no\n");
  CHECK(badSeconds.lines.empty());

  const auto badCentis = parseAssLyrics("[Events]\n" + kAssFormat +
                                        "Dialogue: 0,0:00:01.0000,0:00:02.00,Default,,0,0,0,,no\n");
  CHECK(badCentis.lines.empty());
}

TEST_CASE("ass parser matches sections keys and field names case insensitively") {
  const auto result = parseAssLyrics("[EVENTS]\nformat: LAYER, START, END, STYLE, TEXT\n"
                                     "DIALOGUE: 0,0:00:01.00,0:00:02.00,Default,Cased text\n");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == kAss(1000));
  CHECK(result.lines.front().text == "Cased text");
}

TEST_CASE("ass parser supports ssa v4 marked dialogue lines") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "ssa-marked.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "SSA line one");
  CHECK(result.lines[1].timestamp == kAss(4000));
  CHECK(result.lines[1].text == "SSA line two");
}

// 非 UTF-8 字节走与 `.txt`/`.srt` 同一条 6 步回退：GB18030 侧车必须解出正确文本。
TEST_CASE("ass lyrics decode gb18030") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "gb18030.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "第一行歌词");
  CHECK(result.lines[1].text == "第二行歌词");
  CHECK(result.lines[2].text == "第三行歌词");
}

TEST_CASE("ass lyrics skip and count files outside the encoding fallback set") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "all-invalid.ass");
  CHECK(result.lines.empty());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == LrcParseErrorCode::UnsupportedEncoding);
  CHECK(result.errors.front().path == assFixtureDir() / "all-invalid.ass");
  CHECK_FALSE(decodeLyricsBytes(std::string_view("\x81", 1)).has_value());
}

TEST_CASE("ass parser normalizes crlf and bom") {
  const auto crlf = parseAssLyricsFile(assFixtureDir() / "crlf-bom.ass");
  CHECK(crlf.errors.empty());
  REQUIRE(crlf.lines.size() == 2U);
  CHECK(crlf.lines[0].timestamp == kAss(20000));
  CHECK(crlf.lines[0].text == "CRLF first");
  CHECK(crlf.lines[1].timestamp == kAss(23000));
  CHECK(crlf.lines[1].text == "CRLF second");

  const auto lf = parseAssLyrics("[Events]\n" + kAssFormat +
                                 "Dialogue: 0,0:00:20.00,0:00:22.00,Default,,0,0,0,,CRLF first\n"
                                 "Dialogue: 0,0:00:23.00,0:00:25.00,Default,,0,0,0,,CRLF second\n");
  CHECK(lf.errors.empty());
  REQUIRE(lf.lines.size() == 2U);
  CHECK(lf.lines[0].timestamp == crlf.lines[0].timestamp);
  CHECK(lf.lines[0].text == crlf.lines[0].text);
  CHECK(lf.lines[1].timestamp == crlf.lines[1].timestamp);
  CHECK(lf.lines[1].text == crlf.lines[1].text);
}

TEST_CASE("ass parser bounds file bytes and line count") {
  const auto tooLarge = parseAssLyrics(std::string(100U, 'x'), {.maxBytes = 10U});
  CHECK(tooLarge.lines.empty());
  REQUIRE(tooLarge.errors.size() == 1U);
  CHECK(tooLarge.errors.front().code == LrcParseErrorCode::FileTooLarge);

  const auto tooMany =
      parseAssLyrics("[Events]\n" + kAssFormat + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,x\n",
                     {.maxBytes = 1024U, .maxLines = 2U});
  REQUIRE(tooMany.errors.size() == 1U);
  CHECK(tooMany.errors.front().code == LrcParseErrorCode::TooManyLines);
}

// MED-1：`\{` / `\}` 是字面花括号（libass ass_parse.c:1139-1146），不是覆盖块边界；不得静默丢弃。
TEST_CASE("ass parser unescapes brace escapes without treating them as override blocks") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "escaped-braces.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 7U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "x {a} y");
  CHECK(result.lines[1].text == "{open");
  CHECK(result.lines[2].text == "a}b");
  CHECK(result.lines[3].text == "{escaped brace}");
  // `\\{` = 字面 `\` 后接被转义的 `{`（libass 先消费 `\\` 为字面 `\`、再消费 `\{` 为 `{`）
  CHECK(result.lines[4].text == "\\{a}b");
  CHECK(result.lines[5].text == "{a}b{c");
  // 普通覆盖块仍剥离、转义花括号仍还原：`{\i1}styled{\i0} and \{literal\}` ⇒ `styled and {literal}`
  CHECK(result.lines[6].text == "styled and {literal}");
}

// MED-2：`\p<n>`（n≥1）内的矢量绘图负载不得当正文；`\p0` 后恢复。
TEST_CASE("ass parser drops drawing mode payload") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "drawing-mode.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 6U);
  // ts 1000（纯绘图 `{\p1}…{\p0}`）与 7000（`{\p1}` 行尾未复位）均不产出 ⇒ 首条为 3000
  CHECK(result.lines[0].timestamp == kAss(3000));
  CHECK(result.lines[0].text == "Real lyric");
  CHECK(result.lines[1].timestamp == kAss(5000));
  CHECK(result.lines[1].text == "only text");
  CHECK(result.lines[2].timestamp == kAss(9000));
  CHECK(result.lines[2].text == "bare p keeps text");
  CHECK(result.lines[3].timestamp == kAss(11000));
  CHECK(result.lines[3].text == "after positioned");
  CHECK(result.lines[4].timestamp == kAss(13000));
  CHECK(result.lines[4].text == "line one");
  CHECK(result.lines[5].timestamp == kAss(13000));
  CHECK(result.lines[5].text == "line two");
  // 可判负：任何一条都不含绘图指令串
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("m 0 0") != std::string::npos || line.text.find("l 100 0") != std::string::npos;
  }));
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.timestamp == std::chrono::milliseconds{1000} ||
           line.timestamp == std::chrono::milliseconds{7000};
  }));
}

// 绘图态**初始为关**时，`\p` / `\pX` 的结果与「复位为关」一致 ⇒ 本条只钉住「不因此丢正文」这一面
// （`\pos` / `\pbo` 则是真中性：libass 在 `\p` 之前匹配二者，:606 / :898 先于 :901）。
// 绘图态**已开**时 `\p` / `\pX` 的复位语义由下一条用例（`... resets drawing ...`）覆盖。
TEST_CASE("ass parser keeps text for non-numeric p tag when drawing is off") {
  const auto neutral = [](const std::string& block) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + block + "text\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "text";
  };
  CHECK(neutral("{\\p}"));
  CHECK(neutral("{\\pX}"));
  CHECK(neutral("{\\pos(1,2)}"));
  CHECK(neutral("{\\pbo1}"));
  // 对照（可判负）：`\p1` 开启绘图 ⇒ 其后正文被丢弃
  const auto drawingOn = parseAssLyrics("[Events]\n" + kAssFormat +
                                        "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,{\\p1}text\n");
  CHECK(drawingOn.lines.empty());
}

// NEW-MED-1：绘图态**已开**时，`\p` / `\pX` 必须按 libass `argtoi32`（空参/非数字 ⇒ 0）**复位为关**，
// 不得把其后正文整行丢弃；而 `\pos` / `\pbo` 在 libass 里于 `\p` 之前匹配 ⇒ 保持绘图态（中性）。
TEST_CASE("ass parser resets drawing on non-numeric p tag but keeps pos and pbo neutral") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  // 复位（修复前这两条产出 0 行，正文被静默丢弃）
  const auto bareP = linesFor("{\\p1}m 0 0{\\p}Real lyric");
  CHECK(bareP.errors.empty());
  REQUIRE(bareP.lines.size() == 1U);
  CHECK(bareP.lines.front().text == "Real lyric");
  const auto barePX = linesFor("{\\p1}m 0 0{\\pX}Real lyric");
  REQUIRE(barePX.lines.size() == 1U);
  CHECK(barePX.lines.front().text == "Real lyric");
  // 中性（可判负：若误复位为关，这两条会多出 1 行）
  CHECK(linesFor("{\\p1}m 0 0{\\pos(1,2)}still drawing").lines.empty());
  CHECK(linesFor("{\\p1}m 0 0{\\pbo1}still drawing").lines.empty());
  // 回归：显式数字仍唯一决定「开」；`\p0` 关、行尾未复位仍不产出
  const auto zero = linesFor("{\\p1}m 0 0{\\p0}zero turns off");
  REQUIRE(zero.lines.size() == 1U);
  CHECK(zero.lines.front().text == "zero turns off");
  CHECK(linesFor("{\\p1}m 0 0").lines.empty());
  // `strtoll` 跳过前导空白 ⇒ `\p 1` 与 libass 一致为「开」（其后正文被丢）
  CHECK(linesFor("{\\p1}m 0 0{\\p 1}space keeps drawing").lines.empty());
  CHECK(linesFor("{\\p 1}space alone turns on").lines.empty());
  // 负值：本实现镜像 `strtoll` 消费一个前导 `-`，经终值 `negative` 判为关
  //（与 libass `:903` 的 `val = (val < 0) ? 0 : val` 钳位一致）
  const auto negative = linesFor("{\\p1}m 0 0{\\p-1}negative turns off");
  REQUIRE(negative.lines.size() == 1U);
  CHECK(negative.lines.front().text == "negative turns off");
}

// `\N` 切片发生在剥块之后：复位后的正文应逐切片保留（不得因跨切片共享的绘图态而丢）
TEST_CASE("ass parser keeps every escape split line after drawing reset") {
  const auto result = parseAssLyrics(
      "[Events]\n" + kAssFormat +
      "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,{\\p1}m 0 0{\\p}first\\Nsecond\n");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].text == "first");
  CHECK(result.lines[1].text == "second");
}

// 夹具级：同一文件内混合「复位 / 中性 / 空白后为开 / 负值 / 行尾未复位」五种形态（真实字节）
TEST_CASE("ass parser drawing reset fixture") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "drawing-reset.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 6U);
  CHECK(result.lines[0].timestamp == kAss(1000));
  CHECK(result.lines[0].text == "Real lyric");
  CHECK(result.lines[1].timestamp == kAss(2000));
  CHECK(result.lines[1].text == "After pX");
  CHECK(result.lines[2].timestamp == kAss(6000));
  CHECK(result.lines[2].text == "negative turns off");
  CHECK(result.lines[3].timestamp == kAss(7000));
  CHECK(result.lines[3].text == "zero turns off");
  CHECK(result.lines[4].timestamp == kAss(8000));
  CHECK(result.lines[4].text == "first");
  CHECK(result.lines[5].timestamp == kAss(8000));
  CHECK(result.lines[5].text == "second");
  // 仍处于绘图态的三行（`\pos`/`\pbo` 中性保持 on、`\p 1` 空白后为 on）与行尾未复位行都不得出现
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("still drawing") != std::string::npos ||
           line.text.find("space keeps drawing") != std::string::npos ||
           line.timestamp == std::chrono::milliseconds{3000} ||
           line.timestamp == std::chrono::milliseconds{4000} ||
           line.timestamp == std::chrono::milliseconds{5000} ||
           line.timestamp == std::chrono::milliseconds{9000};
  }));
}

// 符号镜像（`strtoll` 接受一个可选前导 `+`/`-`）：`\p+1` 必须**开**绘图态（否则坐标串泄漏为歌词，
// 与 MED-2 同型）；`-` 为负 ⇒ libass 把 `val < 0` 钳 0 ⇒ **关**。既有行为（无符号）不得改变。
TEST_CASE("ass parser mirrors strtoll sign handling for p tag") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  // `\p+1` ⇒ 开：纯绘图不产出（修复前会泄漏 `m 0 0 l 100 0`）
  CHECK(linesFor("{\\p+1}m 0 0 l 100 0").lines.empty());
  // `\p+1` ⇒ 开：其后接 `\p0` + 正文 ⇒ 只保留正文（修复前粘成 `m 0 0 l 1 1Real lyric`）
  const auto glued = linesFor("{\\p+1}m 0 0 l 1 1{\\p0}Real lyric");
  CHECK(glued.errors.empty());
  REQUIRE(glued.lines.size() == 1U);
  CHECK(glued.lines.front().text == "Real lyric");
  // `+` 后多位数同样为开；超长数字串仍被钳位（不溢出）
  CHECK(linesFor("{\\p+5}m 0 0 l 1 1").lines.empty());
  CHECK(linesFor("{\\p+0000000000000000000001}m 0 0").lines.empty());
  // `-` ⇒ 负 ⇒ 关（`:903` 钳 0）；`+0` / 无数字的 `+`/`-` ⇒ 0 ⇒ 关
  const auto kept = [](const std::string& text) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "kept text";
  };
  CHECK(kept("{\\p-1}kept text"));
  CHECK(kept("{\\p-5}kept text"));
  CHECK(kept("{\\p+0}kept text"));
  CHECK(kept("{\\p+}kept text"));
  CHECK(kept("{\\p-}kept text"));
  // 回归：无符号行为不变（`\p1` 开、`\p`/`\pX` 关、`\pos`/`\pbo` 中性）
  CHECK(linesFor("{\\p1}m 0 0").lines.empty());
  const auto bare = linesFor("{\\p}text");
  REQUIRE(bare.lines.size() == 1U);
  CHECK(bare.lines.front().text == "text");
  const auto bareX = linesFor("{\\pX}text");
  REQUIRE(bareX.lines.size() == 1U);
  CHECK(bareX.lines.front().text == "text");
  const auto pos = linesFor("{\\pos(1,2)}talk");
  REQUIRE(pos.lines.size() == 1U);
  CHECK(pos.lines.front().text == "talk");
}

// 夹具级：同一文件内混合「`+` 开 / `-` 关 / `+0` 关 / 裸 `+`/`-` 关 / 空白后为开 / 双 `\p+1`」
TEST_CASE("ass parser drawing sign fixture") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "drawing-sign.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 5U);
  // ts 1000（`{\p+1}m 0 0 l 100 0` 纯绘图）与 6000（`{\p 1}` 空白后为开）均不产出
  CHECK(result.lines[0].timestamp == kAss(2000));
  CHECK(result.lines[0].text == "After plus");
  CHECK(result.lines[1].timestamp == kAss(3000));
  CHECK(result.lines[1].text == "negative turns off");
  CHECK(result.lines[2].timestamp == kAss(4000));
  CHECK(result.lines[2].text == "plus zero turns off");
  CHECK(result.lines[3].timestamp == kAss(5000));
  CHECK(result.lines[3].text == "bare minus turns off");
  CHECK(result.lines[4].timestamp == kAss(7000));
  CHECK(result.lines[4].text == "After double plus");
  // 可判负：任何一条都不得含绘图指令串（修复前 ts 1000 会泄漏 `m 0 0 l 100 0`）
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("m 0 0") != std::string::npos || line.text.find("l 100 0") != std::string::npos ||
           line.text.find("l 2 2") != std::string::npos || line.text.find("l 4 4") != std::string::npos;
  }));
}

// MED-1（S12 轮次 4）：libass 的**括号参数表**先于标签名匹配被解析 ⇒ `\p` 读的是**括号内第一个非空
// 参数**（`push_arg` 不 push 空字段），否则读标签名之后的文本。逐条以真实 libass 0.17.5 渲染探针取地面
// 真值（见证据 `qa/libass_probe.c` 与 2273 形态对照表）。
TEST_CASE("ass parser mirrors libass bracket argument lexer for p tag") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  const auto kept = [](const std::string& text) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "kept text";
  };
  // 括号内首个非空参数 ⇒ 开：负载必须被丢弃（修复前会把坐标串当正文产出）
  CHECK(linesFor("{\\p(1)}m 0 0 l 100 0").lines.empty());
  CHECK(linesFor("{\\p(2)}x 50 y 60").lines.empty());
  CHECK(linesFor("{\\p(12)}m 0 0").lines.empty());
  CHECK(linesFor("{\\p(1,2)}m 0 0").lines.empty());
  CHECK(linesFor("{\\p( 1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\p(+1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\p(1\\p0)}m 0 0").lines.empty());  // 反斜杠实参吞到闭括号；strtoll 读到 1
  // 括号内参数为 0/非数字 ⇒ 关：可见正文必须保留（修复前会丢正文）
  CHECK(kept("{\\p(0)}kept text"));
  CHECK(kept("{\\p()}kept text"));
  CHECK(kept("{\\p( )}kept text"));
  CHECK(kept("{\\p(foo)}kept text"));
  CHECK(kept("{\\p(-1)}kept text"));
  CHECK(kept("{\\p(\\p1)}kept text"));
  // 标签名后的文本成为 args[1] ⇒ 不参与；括号里的 0 决定为关（修复前读 1 ⇒ 丢正文）
  CHECK(kept("{\\p1(0)}kept text"));
  CHECK(linesFor("{\\p1(2)}m 0 0").lines.empty());
  CHECK(kept("{\\p1(0,5)}kept text"));
  // 括号表一个都没 push（全空字段）⇒ args[0] 回落到标签名之后的文本
  CHECK(linesFor("{\\p1()}m 0 0").lines.empty());
  CHECK(linesFor("{\\p1( )}m 0 0").lines.empty());
  CHECK(linesFor("{\\p1(,)}m 0 0").lines.empty());
  CHECK(linesFor("{\\p+1()}m 0 0").lines.empty());
  CHECK(linesFor("{\\p 1(  )}m 0 0").lines.empty());
  // `push_arg` 不 push 空字段 ⇒ 首个**非空**字段才是 args[0]（`,1` ⇒ 读 1）
  CHECK(linesFor("{\\p(,1)}m 0 0").lines.empty());
  CHECK(kept("{\\p(,)}kept text"));
  // 非 `t` 标签的括号**不递归** ⇒ 其中 `\p` 不生效（libass 唯一递归点是 `\t`）
  CHECK(kept("{\\fs(\\p1)}kept text"));
  CHECK(kept("{\\move(1,2,3,4)}kept text"));
  // `\t` 的实参内部按标签再解析 ⇒ 会打开绘图态
  CHECK(linesFor("{\\t(\\p1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\t(0,100,\\p1)}m 0 0").lines.empty());
  CHECK(kept("{\\t(\\p0)}kept text"));
}

// MED-2（S12 轮次 4）：libass 在 `\` 之后先 `skip_spaces`（只跳 `' '`/`'\t'`）⇒ `\ p1` 即 `\p1`。
TEST_CASE("ass parser mirrors libass backslash space lexer for p tag") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  const auto kept = [](const std::string& text) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "kept text";
  };
  // 开：负载必须被丢弃（修复前会把坐标串当正文产出）
  CHECK(linesFor("{\\ p1}m 0 0 l 100 0").lines.empty());
  CHECK(linesFor("{\\ p 1}m 0 0").lines.empty());
  CHECK(linesFor("{\\ p+1}m 0 0").lines.empty());
  CHECK(linesFor("{\\  p1}m 0 0").lines.empty());
  CHECK(linesFor("{\\ \tp1}m 0 0").lines.empty());
  // 反向：`{\ p0}` 必须能复位 ⇒ 可见正文保留（修复前丢正文）
  CHECK(kept("{\\ p0}kept text"));
  CHECK(kept("{\\p1}m 0 0{\\ p0}kept text"));
  CHECK(kept("{\\p1}m 0 0{\\  p0}kept text"));
  // 中性判定同样基于跳过空白后的标签名 ⇒ `\ pos` 仍中性、`\ pbo` 仍中性
  CHECK(kept("{\\ pos(1,2)}kept text"));
  CHECK(kept("{\\ pbo1}kept text"));
  CHECK(linesFor("{\\p1\\ pos(1,2)}m 0 0").lines.empty());  // 先开、再中性 ⇒ 保持开
  // 回归：无空白的既有行为不变
  CHECK(linesFor("{\\p1}m 0 0").lines.empty());
  CHECK(kept("{\\p0}kept text"));
  CHECK(kept("{\\pos(1,2)}kept text"));
  CHECK(kept("{\\pbc}kept text"));  // 未知名 ⇒ 非 os/bo ⇒ 关
}

// 夹具级：同一文件里把两条词法的「开/关」两个方向混排，任一方向错都会改变行数或正文。
TEST_CASE("ass parser drawing lexer fixture") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "drawing-lexer.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 5U);
  // 只有「判关」的 5 行产出；判开的 9 行（含 2 条纯绘图泄漏、2 条 `\ p1` 泄漏）一行都不许产出
  CHECK(result.lines[0].timestamp == kAss(4000));
  CHECK(result.lines[0].text == "bracket arg wins");
  CHECK(result.lines[1].timestamp == kAss(6000));
  CHECK(result.lines[1].text == "After bracket");
  CHECK(result.lines[2].timestamp == kAss(9000));
  CHECK(result.lines[2].text == "After space reset");
  CHECK(result.lines[3].timestamp == kAss(10000));
  CHECK(result.lines[3].text == "empty bracket off");
  CHECK(result.lines[4].timestamp == kAss(13000));
  CHECK(result.lines[4].text == "fs bracket keeps text");
  // 可判负：任何一条都不得含绘图指令串或标签残渣（修复前 ts 1000/2000/7000 会泄漏坐标串）
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("m 0 0") != std::string::npos || line.text.find("l 100 0") != std::string::npos ||
           line.text.find("x 50") != std::string::npos || line.text.find("\\p") != std::string::npos;
  }));
}
// MED-1（S12 第 5 次复评）：`\t` 的括号内部此前用**带深度上限的 C++ 递归**展开
// （`kMaxAssTagNesting = 16`），≥17 层时内层 `\p` 被漏看 ⇒ 判开者**泄漏**绘图负载为歌词、
// 判关者**丢失**可见正文。轮次 7 改为显式帧栈迭代展开（无深度上限、调用栈不随输入增长）。
// 真值：真实 libass 0.17.5 对任意深度嵌套 `\t` 都处理内层 `\p`（`assert(!nested)` 只在
// 「`\t(...)` 之后仍有内容」子分支，且 release 构建下被编掉；`\t(...)` 位于末尾走尾调用路径）。
TEST_CASE("ass parser expands nested t tags at any depth") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  const auto kept = [](const std::string& text) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "kept text";
  };
  const auto nested = [](int depth, const std::string& inner) {
    std::string text;
    for (int i = 0; i < depth; ++i) {
      text += "\\t(";
    }
    text += inner;
    for (int i = 0; i < depth; ++i) {
      text += ")";
    }
    return text;
  };
  // 开方向：任意深度都应判开 ⇒ 绘图负载必须丢弃（修复前 ≥17 层会泄漏 `m 0 0`）
  for (const int depth : {15, 16, 17, 18, 32, 50}) {
    CHECK(linesFor("{" + nested(depth, "\\p1") + "}m 0 0").lines.empty());
  }
  // 关方向：任意深度都应判关 ⇒ 可见正文必须保留（修复前 ≥17 层会丢这段正文）
  for (const int depth : {15, 16, 17, 18, 32, 50}) {
    const auto result = linesFor("{\\p1}drawcmd{" + nested(depth, "\\p0") + "}VISIBLE");
    REQUIRE(result.lines.size() == 1U);
    CHECK(result.lines.front().text == "VISIBLE");
  }
  // 16/17 边界必须**一致**（修复的判别点：修复前 16 判开、17 判关）
  CHECK(linesFor("{" + nested(16, "\\p1") + "}m 0 0").lines.empty());
  CHECK(linesFor("{" + nested(17, "\\p1") + "}m 0 0").lines.empty());
  // 顺序敏感：`drawing` 是跨帧共享状态，内层效果按出现顺序累积
  CHECK(kept("{\\t(\\p1\\p0)}kept text"));  // 开→关 ⇒ 保留
  CHECK(linesFor("{\\t(\\p0\\p1)}m 0 0").lines.empty());  // 关→开 ⇒ 丢弃
  // 非 `t` 标签的括号在深层内同样**不**递归（唯一递归点是 `\t`）
  CHECK(kept("{" + nested(17, "\\fs(\\p1)") + "}kept text"));
}

// 夹具级：把 16/17/20/50 层嵌套、两方向、顺序敏感、深层非 `t` 括号混排在同一文件；
// 任一深度错误都会改变行数或正文。真值由真实 libass 0.17.5 raw 模式逐行取得
// （见证据 qa/fixture-deep-truth-r7.txt：8 行 8/8 MATCH）。
TEST_CASE("ass parser deep nested t fixture matches libass") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "deep-nested-t.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 6U);
  // 判开的 2 行（ts 1000 的 17 层 `\p1`、ts 5000 的 `\t(\p0\p1)`）一行都不许产出
  CHECK(result.lines[0].timestamp == kAss(2000));
  CHECK(result.lines[0].text == "after fifty");
  CHECK(result.lines[1].timestamp == kAss(3000));
  CHECK(result.lines[1].text == "VISIBLE17");
  CHECK(result.lines[2].timestamp == kAss(4000));
  CHECK(result.lines[2].text == "order off");
  CHECK(result.lines[3].timestamp == kAss(6000));
  CHECK(result.lines[3].text == "BOUNDARY16");
  CHECK(result.lines[4].timestamp == kAss(7000));
  CHECK(result.lines[4].text == "BOUNDARY17");
  CHECK(result.lines[5].timestamp == kAss(8000));
  CHECK(result.lines[5].text == "non t bracket deep");
  // 可判负：任何产出正文都不得含绘图指令串或标签残渣
  CHECK(std::ranges::none_of(result.lines, [](const LyricLine& line) {
    return line.text.find("m 0 0") != std::string::npos || line.text.find("\\p") != std::string::npos;
  }));
}
// MED-A（S12 第 6 次复评）：libass 的 `\t` **仅当 `cnt = nargs - 1 ∈ [0,3]` 且括号实参内含 `\`**
// 时才递归解析内层标签（`ass_parse.c:672`/`:709`/`:713`）；实参 ≥5 个（`cnt ≥ 4`）
// 时整段 `continue` ⇒ 内层 `\p` 一律不生效。修复前无条件递归 ⇒ 判开者泄漏绘图负载、判关者丢正文。
TEST_CASE("ass parser applies t tags only for one to four arguments like libass") {
  const auto linesFor = [](const std::string& text) {
    return parseAssLyrics("[Events]\n" + kAssFormat +
                          "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
  };
  const auto kept = [](const std::string& text) {
    const auto result = parseAssLyrics("[Events]\n" + kAssFormat +
                                       "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + text + "\n");
    return result.errors.empty() && result.lines.size() == 1U && result.lines.front().text == "kept text";
  };
  // cnt 0..3 ⇒ 解析（内层 `\p1` 生效 ⇒ 绘图负载必须被丢弃）
  CHECK(linesFor("{\\t(\\p1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\t(0,\\p1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\t(0,1,\\p1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\t(0,1,2,\\p1)}m 0 0").lines.empty());
  // cnt ≥ 4（实参 ≥ 5 个）⇒ 整段跳过 ⇒ 可见正文必须保留（修复前会丢）
  CHECK(kept("{\\t(0,1,2,3,\\p1)}kept text"));
  CHECK(kept("{\\t(0,1,2,3,4,\\p1)}kept text"));
  CHECK(kept("{\\t(0,1,2,3,4,5,6,7,\\p1)}kept text"));
  CHECK(kept("{\\t(0,1,2,3,4,5,6,7,8,\\p1)}kept text"));
  // 泄漏方向：`\p0` 落在 cnt≥4 的 `\t` 内 ⇒ 不生效 ⇒ 绘图态保持开 ⇒ 坐标串不得产出
  CHECK(linesFor("{\\p1}m 0 0{\\t(0,1,2,3,\\p0)}m 1 1").lines.empty());
  // 边界对照：cnt=3 时 `\p0` **生效** ⇒ 绘图关 ⇒ 其后正文可见（libass raw 实测一致）
  CHECK(kept("{\\p1}m 0 0{\\t(0,1,2,\\p0)}kept text"));
  // nargs=0（`\t()` / `\t( )` / `\t(,,,)`）⇒ cnt=-1 ⇒ 跳过
  CHECK(kept("{\\t()}kept text"));
  CHECK(kept("{\\t( )}kept text"));
  CHECK(kept("{\\t(,,,)}kept text"));
  // 空字段不计入 nargs ⇒ `\t(,\p1)` 的 nargs=1 ⇒ 解析
  CHECK(linesFor("{\\t(,\\p1)}m 0 0").lines.empty());
  CHECK(linesFor("{\\t(,,,\\p1)}m 0 0").lines.empty());
  // 括号内不含 `\`（`:713`）⇒ 不解析，且无副作用
  CHECK(kept("{\\t(0,1,2,3)}kept text"));
  CHECK(kept("{\\t(0,1,2,3,4)}kept text"));
  // 嵌套：内层 cnt 独立判定（外层 cnt=0 也要看内层）
  CHECK(kept("{\\t(\\t(0,1,2,3,\\p1))}kept text"));
  CHECK(linesFor("{\\t(\\t(0,1,2,\\p1))}m 0 0").lines.empty());
  CHECK(kept("{\\t(0,1,2,3,\\t(\\p1))}kept text"));
  CHECK(linesFor("{\\t(0,1,2,\\t(\\p1))}m 0 0").lines.empty());
  // cnt≤3 内的多标签顺序仍按出现顺序累积
  CHECK(kept("{\\t(0,1,2,\\p1\\p0)}kept text"));
  CHECK(linesFor("{\\t(0,1,2,\\p0\\p1)}m 0 0").lines.empty());
}

// 夹具级：cnt 维度的 12 行混排（cnt 0..9、nargs=0/空字段、两方向、嵌套内层 cnt≥4、无 `\` 实参、
// 顺序敏感）。真值由真实 libass 0.17.5 raw 模式逐行取得（见证据 qa/fixture-argcount-truth-r8.txt：
// 12 行 12/12 MATCH）。
TEST_CASE("ass parser t argument-count fixture matches libass") {
  const auto result = parseAssLyricsFile(assFixtureDir() / "t-argcount.ass");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 7U);
  CHECK(result.lines[0].timestamp == kAss(3000));
  CHECK(result.lines[0].text == "argfour");
  CHECK(result.lines[1].timestamp == kAss(4000));
  CHECK(result.lines[1].text == "argfive");
  CHECK(result.lines[2].timestamp == kAss(6000));
  CHECK(result.lines[2].text == "m 1 1");
  CHECK(result.lines[3].timestamp == kAss(7000));
  CHECK(result.lines[3].text == "emptybracket");
  CHECK(result.lines[4].timestamp == kAss(9000));
  CHECK(result.lines[4].text == "nestedout");
  CHECK(result.lines[5].timestamp == kAss(11000));
  CHECK(result.lines[5].text == "plainnum");
  CHECK(result.lines[6].timestamp == kAss(12000));
  CHECK(result.lines[6].text == "orderok");
}

// MED-B（S12 第 6 次复评）：深嵌套 `\t` + 长尾随空白。压栈前必须按 libass `push_arg` 的
// `rskip_spaces` 对帧文本右去尾，否则每帧都回扫同一段尾随空白 ⇒ O(N·M)（N=M=60000 时量级约 1.5s，
// 去尾后约 2ms；实测数据见 `qa/deep-sweep-r8.txt`）。此处用「规模 ×4 的耗时比」判定
// （比值抵消机器快慢，取 3 次最小值抗噪声）：线性约 3–4×，二次约 14–16×。
TEST_CASE("ass parser trims nested t frames so deep trailing whitespace stays linear") {
  const auto nested = [](int depth, const std::string& inner) {
    std::string text;
    for (int i = 0; i < depth; ++i) {
      text += "\\t(";
    }
    text += inner;
    for (int i = 0; i < depth; ++i) {
      text += ")";
    }
    return text;
  };
  const auto measureOnce = [&nested](int depth, int spaces) {
    const std::string body =
        "{" + nested(depth, "\\p1" + std::string(static_cast<std::size_t>(spaces), ' ')) + "}m 0 0";
    const std::string document =
        "[Events]\n" + kAssFormat + "Dialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,," + body + "\n";
    const auto started = std::chrono::steady_clock::now();
    const auto result = parseAssLyrics(document);
    const auto finished = std::chrono::steady_clock::now();
    CHECK(result.lines.empty());
    return std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
  };
  // 两档规模交错测量（各取 3 次最小值）：抵消机器随时间的漂移，且最小值对调度噪声不敏感。
  long long small = -1;
  long long big = -1;
  for (int repetition = 0; repetition < 3; ++repetition) {
    const auto smallRun = measureOnce(15000, 15000);
    const auto bigRun = measureOnce(60000, 60000);
    small = (small < 0 || smallRun < small) ? smallRun : small;
    big = (big < 0 || bigRun < big) ? bigRun : big;
  }
  MESSAGE("trailing-ws deep t parse: N=15000 -> " << small << "us ; N=60000 -> " << big << "us");
  CHECK(big < 250000);
  CHECK(big < small * 8 + 3000);
}
#endif  // SERIONA_ASS_FIXTURE_DIR

// ── G5d：TTML / DFXP（.ttml / .dfxp）解析器 ──────────────────────────────────
#ifdef SERIONA_TTML_FIXTURE_DIR

const std::filesystem::path& ttmlFixtureDir() {
  static const std::filesystem::path dir{SERIONA_TTML_FIXTURE_DIR};
  return dir;
}

const auto kTtml = [](std::int64_t millis) { return std::chrono::milliseconds{millis}; };

// 单 `<p begin="...">` 探针：接受 ⇒ 返回时间戳毫秒；拒绝 ⇒ nullopt（并断言恰好一条 InvalidTimestamp）。
[[nodiscard]] std::optional<std::int64_t> ttmlAcceptedBegin(const std::string& begin) {
  const auto result = parseTtmlLyrics("<p begin=\"" + begin + "\">x</p>");
  if (result.errors.empty()) {
    REQUIRE(result.lines.size() == 1U);
    return result.lines.front().timestamp.count();
  }
  REQUIRE(result.lines.empty());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
  return std::nullopt;
}

TEST_CASE("ttml parser reads standard paragraphs and takes the begin timestamp") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "standard-bom.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "First line");
  CHECK(result.lines[1].timestamp == kTtml(5500));
  CHECK(result.lines[1].text == "Second line");
  // 00:01:02.003 = (0*3600 + 1*60 + 2)*1000 + 3
  CHECK(result.lines[2].timestamp == kTtml(62003));
  CHECK(result.lines[2].text == "Third line");
}

// 嵌套 `<span>` 与未知内联元素是透明容器：文本按**文档顺序**拼接。
// MRUT 反例：只取首个文本子节点会得到 "A"。
TEST_CASE("ttml parser concatenates nested spans and unknown inline elements in document order") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "nested-span.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].timestamp == kTtml(0));
  CHECK(result.lines[0].text == "ABCDE");
  CHECK(result.lines[1].text == "leading spaces");
  CHECK(result.lines[2].text == "onetwo");
  CHECK(result.lines[3].text == "xy");
}

TEST_CASE("ttml parser decodes predefined and numeric entities") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "entities.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].text == "A & B < C > D \" E ' F");
  CHECK(result.lines[1].text == "numeric & and & end");
  CHECK(result.lines[2].text == "apos'and'hex");
}

// CDATA 与普通文本混排时按文档顺序拼接。
TEST_CASE("ttml parser treats cdata as literal text without entity decoding") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "cdata.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].text == "a & b < c > d");
  CHECK(result.lines[1].text == "before x &amp; y after");
}

// `<br/>` 与 `<br>` 都是硬换行：切成多行、共享同一 timestamp、保持文档顺序。
TEST_CASE("ttml parser splits br hard breaks into same-timestamp lines") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "br-breaks.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].timestamp == kTtml(10000));
  CHECK(result.lines[0].text == "one");
  CHECK(result.lines[1].timestamp == kTtml(10000));
  CHECK(result.lines[1].text == "two");
  CHECK(result.lines[2].timestamp == kTtml(10000));
  CHECK(result.lines[2].text == "three");
  CHECK(result.lines[3].timestamp == kTtml(20000));
  CHECK(result.lines[3].text == "solo");

  // 内联等价：自闭合 `<br/>` 与裸 `<br>` 必须产生**相同**结果（两者都不得退化成文本拼接）。
  // 该断言是「selfClosing 提前 continue 会吞掉 `<br/>` 切分」回归的守卫。
  const auto selfClosed = parseTtmlLyrics("<p begin=\"0s\">a<br/>b</p>");
  const auto bare = parseTtmlLyrics("<p begin=\"0s\">a<br>b</p>");
  REQUIRE(selfClosed.lines.size() == 2U);
  REQUIRE(bare.lines.size() == 2U);
  CHECK(selfClosed.lines[0].text == "a");
  CHECK(selfClosed.lines[1].text == "b");
  CHECK(bare.lines[0].text == selfClosed.lines[0].text);
  CHECK(bare.lines[1].text == selfClosed.lines[1].text);
}

TEST_CASE("ttml lyrics decode gb18030") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "gb18030.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(0));
  CHECK(result.lines[0].text == "第一行歌词");
  CHECK(result.lines[1].text == "第二行歌词");
}

TEST_CASE("ttml parser normalizes crlf") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "crlf.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(0));
  CHECK(result.lines[0].text == "crlf line");
  CHECK(result.lines[1].text == "second crlf");
}

// `<translation><text for=...>` 是**平行译文轨道**，不是 `<p begin=...>` ⇒ 本子集天然不产出它。
// 该夹具证明译文不泄漏成歌词、且不产生任何多余行（若误当歌词 size 会是 4）。
TEST_CASE("ttml parser does not emit translation track text as lyric lines") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "translation.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(0));
  CHECK(result.lines[0].text == "Original one");
  CHECK(result.lines[1].timestamp == kTtml(1000));
  CHECK(result.lines[1].text == "Original two");
}

// 残余差异（已登记）：`ttm:role="translation"` 标注的 `<p>` 仍按普通 `<p>` 产出。
// 钉住该限制，避免「不读 `ttm:role`」被误读成「会过滤译文」。
TEST_CASE("ttml parser keeps role tagged paragraphs because metadata attributes are not read") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "ttm-role.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].text == "Original");
  CHECK(result.lines[1].text == "role tagged translation");
}

// `<div begin>` 不被继承：只读 `<p>` 自身的 `begin`；缺 `begin` ⇒ unsynced 哨兵（不是 div 的 5s）。
TEST_CASE("ttml parser does not inherit begin from ancestor div") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "inheritance.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].timestamp == kTtml(-1));
  CHECK(result.lines[0].text == "no own begin");
  CHECK(result.lines[1].timestamp == kTtml(1000));
  CHECK(result.lines[1].text == "own begin wins");
  CHECK(result.lines[2].timestamp == kTtml(2000));
  CHECK(result.lines[2].text == "child span");
}

TEST_CASE("ttml parser skips malformed begins and keeps no-begin text with the unsynced sentinel") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "malformed.ttml");
  REQUIRE(result.lines.size() == 3U);
  // unsynced 哨兵（<0）排在最前，其后按时间戳稳定排序。
  CHECK(result.lines[0].timestamp == kTtml(-1));
  CHECK(result.lines[0].text == "no begin at all");
  CHECK(result.lines[1].timestamp == kTtml(1000));
  CHECK(result.lines[1].text == "valid one");
  // 未闭合 `<p>` 在文档尾按已收文本 flush，不丢文本。
  CHECK(result.lines[2].timestamp == kTtml(2000));
  CHECK(result.lines[2].text == "before truncated");
  // 两条畸形 begin：`not-a-time` 与帧形式 `00:00:12:05`。
  REQUIRE(result.errors.size() == 2U);
  for (const auto& error : result.errors) {
    CHECK(error.code == LrcParseErrorCode::InvalidTimestamp);
  }
}

// 登记差异：未定义实体与非法数字引用**原样保留**（不报错、不丢文本）；XML 解析器会拒绝这些文档。
TEST_CASE("ttml parser preserves undefined entities and illegal references literally") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "lenient-undefined-entity.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 3U);
  CHECK(result.lines[0].text == "A &nbsp; B");
  CHECK(result.lines[1].text == "bare & here");
  CHECK(result.lines[2].text == "illegal &#0; and &#xD800; refs");
}

// 登记差异：裸 `<br>` 按硬换行处理（该文档非良构 XML，ElementTree 会拒绝）。
TEST_CASE("ttml parser treats bare br as a hard break despite non-well-formed xml") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "lenient-bare-br.ttml");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(0));
  CHECK(result.lines[0].text == "one");
  CHECK(result.lines[1].timestamp == kTtml(0));
  CHECK(result.lines[1].text == "two");
}

TEST_CASE("ttml lyrics skip and count files outside the encoding fallback set") {
  const auto result = parseTtmlLyricsFile(ttmlFixtureDir() / "all-invalid.ttml");
  CHECK(result.lines.empty());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == LrcParseErrorCode::UnsupportedEncoding);
  CHECK(result.errors.front().path == ttmlFixtureDir() / "all-invalid.ttml");
}

TEST_CASE("ttml parser bounds file bytes and line count") {
  const auto tooLarge = parseTtmlLyrics(std::string(100U, 'x'), {.maxBytes = 10U});
  CHECK(tooLarge.lines.empty());
  REQUIRE(tooLarge.errors.size() == 1U);
  CHECK(tooLarge.errors.front().code == LrcParseErrorCode::FileTooLarge);

  const auto tooMany = parseTtmlLyrics("<p begin=\"1s\">a</p>\n<p begin=\"2s\">b</p>",
                                       {.maxBytes = 1024U, .maxLines = 1U});
  REQUIRE(tooMany.errors.size() == 1U);
  CHECK(tooMany.errors.front().code == LrcParseErrorCode::TooManyLines);
  REQUIRE(tooMany.lines.size() == 1U);
  CHECK(tooMany.lines.front().text == "a");
}

TEST_CASE("ttml parser accepts clock and offset begin forms and rejects frames and signed values") {
  // clock-time：hours ≥2 位，minutes/seconds 恰 2 位且 ≤59，可带十进制秒小数（取前 3 位）。
  CHECK(ttmlAcceptedBegin("00:00:12.500") == 12500);
  CHECK(ttmlAcceptedBegin("00:00:12") == 12000);
  CHECK(ttmlAcceptedBegin("100:00:00.000") == 360000000);
  CHECK(ttmlAcceptedBegin("00:00:00.001") == 1);
  CHECK(ttmlAcceptedBegin("00:00:00.0009") == 0);
  // offset-time：`ms` 必须先于单字符 `s`/`m` 匹配（否则 `12500ms` 会被读成 `m` 度量）。
  CHECK(ttmlAcceptedBegin("12.5s") == 12500);
  CHECK(ttmlAcceptedBegin("12500ms") == 12500);
  CHECK(ttmlAcceptedBegin("2m") == 120000);
  CHECK(ttmlAcceptedBegin("0.5h") == 1800000);
  // 拒绝：帧形式（需 ttp:frameRate）、f/t 度量、无度量、两段式、带符号、wallclock、空、越界、位数不足。
  CHECK_FALSE(ttmlAcceptedBegin("00:00:12:05").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("12f").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("12t").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("12.5").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("00:12.5").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("+1s").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("-1s").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("wallclock(2024-01-01T00:00:00Z)").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("00:60:00").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("00:00:60").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("0:00:12").has_value());
  CHECK_FALSE(ttmlAcceptedBegin("00:0:12").has_value());
}

// 命名空间前缀被忽略（按 local name 匹配）；注释 / PI / DOCTYPE 被跳过，其中的伪 `<p>` 不产出。
TEST_CASE("ttml parser matches local names ignoring prefixes and skips comments and declarations") {
  const std::string document =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE tt SYSTEM \"ttml.dtd\">\n"
      "<!-- a comment with <p begin=\"9s\">fake</p> inside -->\n"
      "<tt:tt xmlns:tt=\"http://www.w3.org/ns/ttml\">\n"
      "  <tt:body>\n"
      "    <tt:div>\n"
      "      <tt:p begin=\"1s\">prefixed</tt:p>\n"
      "      <p begin=\"2s\">plain</p>\n"
      "    </tt:div>\n"
      "  </tt:body>\n"
      "</tt:tt>\n";
  const auto result = parseTtmlLyrics(document);
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "prefixed");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "plain");
}

// 重复段落/重复文本不去重（与 srt/ass 一致）；同一 `<p>` 的多段共享 timestamp 且保持文档顺序。
TEST_CASE("ttml parser keeps duplicate lines and stable same-timestamp order") {
  const auto result =
      parseTtmlLyrics("<p begin=\"5s\">same</p>\n<p begin=\"5s\">same</p>\n<p begin=\"1s\">x<br/>y</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 4U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "x");
  CHECK(result.lines[1].timestamp == kTtml(1000));
  CHECK(result.lines[1].text == "y");
  CHECK(result.lines[2].timestamp == kTtml(5000));
  CHECK(result.lines[2].text == "same");
  CHECK(result.lines[3].timestamp == kTtml(5000));
  CHECK(result.lines[3].text == "same");
}

// 空段落 / 自闭合 `<p/>` / 纯空白段落不产出（与 srt/ass「空文本不产出」一致）。
TEST_CASE("ttml parser drops empty and self-closing paragraphs") {
  const auto result = parseTtmlLyrics(
      "<p begin=\"0s\"/><p begin=\"1s\"></p><p begin=\"2s\"> \t </p><p begin=\"3s\">ok</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines[0].timestamp == kTtml(3000));
  CHECK(result.lines[0].text == "ok");
}

// 正文里的孤立 `<`（不构成标签）按字面保留；`<p>` 之外的文本不产出。
TEST_CASE("ttml parser keeps literal less-than and ignores text outside paragraphs") {
  const auto literal = parseTtmlLyrics("<p begin=\"0s\">a < b and 1 <2</p>");
  CHECK(literal.errors.empty());
  REQUIRE(literal.lines.size() == 1U);
  CHECK(literal.lines[0].text == "a < b and 1 <2");

  const auto outside = parseTtmlLyrics("<body>stray<div>also stray<p begin=\"0s\">kept</p></div></body>");
  CHECK(outside.errors.empty());
  REQUIRE(outside.lines.size() == 1U);
  CHECK(outside.lines[0].text == "kept");
}

// ── S13 打回修复（F1–F5）的新增可判负用例 ────────────────────────────────────

// F1：XML 1.0 §3.3.3 属性值归一化把**字面** `#x9`/`#xA`/`#xD` 折成空格 ⇒ 这些良构写法都等同 `1s`。
// 修复前 `begin="1s\n"`（与 `\r\n`）会被判畸形并**丢弃整段歌词**（lines=0）。
TEST_CASE("ttml parser normalizes literal whitespace inside begin attribute values") {
  const auto expect = [](const std::string& begin) {
    const auto result = parseTtmlLyrics("<p begin=\"" + begin + "\">x</p>");
    REQUIRE(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    CHECK(result.lines.front().timestamp == kTtml(1000));
    CHECK(result.lines.front().text == "x");
  };
  expect("1s\n");
  expect("1s\r");
  expect("1s\r\n");
  expect("1s\t");
  expect(" 1s ");
  expect("\n 1s \t\n");
  expect("00:00:01.000\n");
  expect("1000ms\n");
}

// F1 的两条边界：字面空白按 XML 折成空格（错误 detail 里已无 `\n`）；字符引用产生的 `\n` 不归一
// （XML 豁免）但本子集仍允许其居于首尾（有意放宽，README §10）；值**内部**的空白仍有意义 ⇒ 仍拒绝。
TEST_CASE("ttml parser folds literal attribute whitespace but exempts character references") {
  const auto literal = parseTtmlLyrics("<p begin=\"a\nb\">x</p>");
  REQUIRE(literal.lines.empty());
  REQUIRE(literal.errors.size() == 1U);
  CHECK(literal.errors.front().detail == "a b");

  const auto reference = parseTtmlLyrics("<p begin=\"1s&#10;\">x</p>");
  REQUIRE(reference.errors.empty());
  REQUIRE(reference.lines.size() == 1U);
  CHECK(reference.lines.front().timestamp == kTtml(1000));
  CHECK(reference.lines.front().text == "x");

  const auto interior = parseTtmlLyrics("<p begin=\"1&#10;s\">x</p>");
  REQUIRE(interior.lines.empty());
  REQUIRE(interior.errors.size() == 1U);
  CHECK(interior.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
}

// F2：有界前瞻不得改变任何可观测输出。既有 20 个用例已覆盖常规实体；此处专钉「病态/边界」输入。
TEST_CASE("ttml parser keeps entity scan output identical for ampersand heavy input") {
  const auto textOf = [](const std::string& body) {
    const auto result = parseTtmlLyrics("<p begin=\"1s\">" + body + "</p>");
    REQUIRE(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    return result.lines.front().text;
  };
  // 未定义实体 / 不构成实体的 `&`：一律原样保留（Must-NOT「不丢弃文本」）。
  CHECK(textOf("a & b") == "a & b");
  CHECK(textOf("&;") == "&;");
  CHECK(textOf("&") == "&");
  CHECK(textOf("&nbsp;") == "&nbsp;");
  CHECK(textOf("&#0;") == "&#0;");
  CHECK(textOf("&#xD800;") == "&#xD800;");
  CHECK(textOf("&#1114112;") == "&#1114112;");  // U+110000：超 XML 码点上界 ⇒ 原样保留
  CHECK(textOf("&#99999999999999999999;") == "&#99999999999999999999;");
  CHECK(textOf("&a;mp;") == "&a;mp;");
  CHECK(textOf("&&&;;;") == "&&&;;;");
  // 「N 个 `&` + 远处一个 `;`」（修复前 Θ(N²) 的输入）：必须逐字节与修复前相同。
  const auto run = textOf(std::string(4096U, '&') + ";");
  CHECK(run.size() == 4097U);
  CHECK(run == std::string(4096U, '&') + ";");
  // 合法实体仍照常还原（含 astral 码点，走四字节 UTF-8 分支）。
  CHECK(textOf("&amp;&lt;&gt;&quot;&apos;") == "&<>\"'");
  CHECK(textOf("&#38;&#x26;") == "&&");
  CHECK(textOf("&#x1F600;") == "\U0001F600");
}

// N1（第 2 轮回归）：第 1 轮 F2 的「有界前瞻」把**合法前导零**的长名数字引用（`&#000000038;`）误判为
// 字面量 —— XML 允许引用名前导零，长度没有上界，任何名字长度上界都会破坏合法输入。此处钉死：
// 任意长度的前导零都必须还原（含 1000 个零这类极端输入）。
TEST_CASE("ttml parser decodes numeric references with arbitrary leading zeros") {
  const auto textOf = [](const std::string& body) {
    const auto result = parseTtmlLyrics("<p begin=\"1s\">" + body + "</p>");
    REQUIRE(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    return result.lines.front().text;
  };

  CHECK(textOf("&#000000038;") == "&");
  CHECK(textOf("&#0000000038;") == "&");
  CHECK(textOf("&#x00000026;") == "&");
  CHECK(textOf("&#000000065;") == "A");
  CHECK(textOf("&#00065;") == "A");
  CHECK(textOf("&#x0000005A;") == "Z");
  CHECK(textOf("&#00000090;") == "Z");
  CHECK(textOf("pre&#00000069;post") == "preEpost");
  CHECK(textOf("&#0000000000000000000000000000000000000000000000000000000000000000000000090;") == "Z");
  CHECK(textOf("&#" + std::string(1000U, '0') + "38;") == "&");
  CHECK(textOf("&#x" + std::string(1000U, '0') + "26;") == "&");
  CHECK(textOf("&#" + std::string(4096U, '0') + "65;") == "A");
  CHECK(textOf("&#x" + std::string(4096U, '0') + "10FFFF;") == "\U0010FFFF");
  CHECK(textOf("&#" + std::string(1000U, '0') + "128512;") == "\U0001F600");
  // 溢出与非法码点仍须原样保留（前导零不得让溢出检查失效）。
  CHECK(textOf("&#" + std::string(40U, '0') + "99999999999999999999999999;") ==
        "&#" + std::string(40U, '0') + "99999999999999999999999999;");
  CHECK(textOf("&#x" + std::string(40U, '0') + "FFFFFFFFFFFFFFFFFF;") ==
        "&#x" + std::string(40U, '0') + "FFFFFFFFFFFFFFFFFF;");
  // 属性值路径同口径：`begin="&#000000049;s"` ≡ `begin="1s"`。
  CHECK(ttmlAcceptedBegin("&#000000049;s") == 1000);
  CHECK(ttmlAcceptedBegin("&#x00000031;s") == 1000);
  CHECK(ttmlAcceptedBegin("&#" + std::string(1000U, '0') + "49;s") == 1000);
}

// N1 守卫：与**本测试内独立编写的无界参考解码器**差分。F2 的旧证据只覆盖 `&` 串与 `&;` 对，对
// 「长名前导零」完全盲 —— 参考实现按 README §2.4/§2.5 语义独立重写（不调用被测代码），随机语料
// 逐字节对拍 ⇒ 同类回归不可能再溜过。
TEST_CASE("ttml parser entity decoding matches an unbounded reference decoder") {
  // 折叠后为空 ⇒ 不产出节点（与参考侧返回空串一致），故此处不能断言恰好一行。
  const auto textOf = [](const std::string& body) {
    const auto result = parseTtmlLyrics("<p begin=\"1s\">" + body + "</p>");
    REQUIRE(result.errors.empty());
    if (result.lines.empty()) {
      return std::string{};
    }
    REQUIRE(result.lines.size() == 1U);
    return result.lines.front().text;
  };

  // 参考答案：无界实体还原 ⇒ ASCII 空白折叠（连续折成一个 ' '、去首尾）。
  const auto reference = [](std::string_view raw) {
    const auto parseDigits = [](std::string_view digits, bool hexadecimal, std::uint64_t& value) {
      if (digits.empty()) {
        return false;
      }
      std::uint64_t accumulator = 0;
      for (const char ch : digits) {
        std::uint64_t digit = 0;
        if (ch >= '0' && ch <= '9') {
          digit = static_cast<std::uint64_t>(ch - '0');
        } else if (hexadecimal && ch >= 'a' && ch <= 'f') {
          digit = static_cast<std::uint64_t>(ch - 'a' + 10);
        } else if (hexadecimal && ch >= 'A' && ch <= 'F') {
          digit = static_cast<std::uint64_t>(ch - 'A' + 10);
        } else {
          return false;
        }
        if (hexadecimal ? accumulator > (UINT64_MAX >> 4U) : accumulator > (UINT64_MAX - digit) / 10ULL) {
          return false;
        }
        accumulator = hexadecimal ? (accumulator << 4U) | digit : accumulator * 10ULL + digit;
      }
      value = accumulator;
      return true;
    };
    const auto appendCodePoint = [](std::uint32_t codePoint, std::string& out) {
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
    };
    std::string decoded;
    std::size_t index = 0;
    while (index < raw.size()) {
      if (raw[index] != '&') {
        decoded.push_back(raw[index]);
        ++index;
        continue;
      }
      const auto semicolon = raw.find(';', index + 1U);
      if (semicolon == std::string_view::npos) {
        decoded.push_back('&');
        ++index;
        continue;
      }
      const auto name = raw.substr(index + 1U, semicolon - (index + 1U));
      const auto named = [&name, &decoded]() {
        if (name == "amp") {
          decoded.push_back('&');
        } else if (name == "lt") {
          decoded.push_back('<');
        } else if (name == "gt") {
          decoded.push_back('>');
        } else if (name == "quot") {
          decoded.push_back('"');
        } else if (name == "apos") {
          decoded.push_back('\'');
        } else {
          return false;
        }
        return true;
      };
      if (named()) {
        index = semicolon + 1U;
        continue;
      }
      if (name.size() > 1U && name.front() == '#') {
        const bool hexadecimal = name[1] == 'x' || name[1] == 'X';
        const auto digits = name.substr(hexadecimal ? 2U : 1U);
        std::uint64_t codePoint = 0;
        if (parseDigits(digits, hexadecimal, codePoint) && codePoint != 0U && codePoint <= 0x10FFFFU &&
            !(codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
          appendCodePoint(static_cast<std::uint32_t>(codePoint), decoded);
          index = semicolon + 1U;
          continue;
        }
      }
      decoded.push_back('&');
      ++index;
    }
    std::string folded;
    bool pendingSpace = false;
    for (const char ch : decoded) {
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        pendingSpace = true;
        continue;
      }
      if (pendingSpace && !folded.empty()) {
        folded.push_back(' ');
      }
      pendingSpace = false;
      folded.push_back(ch);
    }
    return folded;
  };

  const std::array<std::string, 12> curated{
      "&#000000038;",       "&#0000000038;",        "&#x00000026;",       "&#000000065;",
      "a&nbsp;b",           "a & b",                "&amp;&lt;&gt;&quot;&apos;", "&#0;",
      "&#xD800;",           "&#1114112;",           "&&&;;;",             "x&amp;y&#65;z",
  };
  for (const auto& body : curated) {
    CHECK(textOf(body) == reference(body));
  }
  // 受控随机：专打「引用名长度」维度（0..300 个前导零），另加全字母表随机串。
  std::mt19937 generator(20260925U);
  std::uniform_int_distribution<std::size_t> zerosDist(0U, 300U);
  std::uniform_int_distribution<int> valueDist(0, 0x120000);
  for (std::size_t iteration = 0; iteration < 400U; ++iteration) {
    const bool hexadecimal = (iteration % 2U) == 0U;
    std::string body = hexadecimal ? "&#x" : "&#";
    body.append(zerosDist(generator), '0');
    body += std::to_string(valueDist(generator));
    body += ";";
    CHECK(textOf(body) == reference(body));
  }
  const std::string alphabet = "&;#xX019afAFzqmltguos pP";
  std::uniform_int_distribution<std::size_t> lengthDist(1U, 60U);
  std::uniform_int_distribution<std::size_t> charDist(0U, alphabet.size() - 1U);
  for (std::size_t iteration = 0; iteration < 4000U; ++iteration) {
    const auto length = lengthDist(generator);
    std::string body;
    body.reserve(length);
    for (std::size_t count = 0; count < length; ++count) {
      body.push_back(alphabet[charDist(generator)]);
    }
    CHECK(textOf(body) == reference(body));
  }
}

// F3：XML 1.0 §2.11 行尾归一 CR/CRLF → LF，再按本实现的 xml:space="default" 折叠为单个空格。
// 修复前孤立 CR 被 `normalizeNewlines` 直接删除（`a\rb` → `ab`）。
TEST_CASE("ttml parser normalizes isolated carriage returns per xml line endings") {
  const auto textOf = [](const std::string& body) {
    const auto result = parseTtmlLyrics(body);
    REQUIRE(result.errors.empty());
    REQUIRE(result.lines.size() == 1U);
    return result.lines.front().text;
  };
  CHECK(textOf("<p begin=\"1s\">a\rb</p>") == "a b");
  CHECK(textOf("<p begin=\"1s\">a\r\nb</p>") == "a b");
  CHECK(textOf("<p begin=\"1s\">a\nb</p>") == "a b");
  CHECK(textOf("<p begin=\"1s\"><![CDATA[a\rb]]></p>") == "a b");
  CHECK(textOf("<p begin=\"1s\"><![CDATA[a\r\nb]]></p>") == "a b");
  // CRLF 与 LF 文档归一后完全等价。
  const auto crlf = parseTtmlLyrics("<p begin=\"1s\">x</p>\r\n<p begin=\"2s\">y</p>");
  const auto lf = parseTtmlLyrics("<p begin=\"1s\">x</p>\n<p begin=\"2s\">y</p>");
  REQUIRE(crlf.lines.size() == 2U);
  REQUIRE(lf.lines.size() == 2U);
  CHECK(crlf.lines[0].timestamp == lf.lines[0].timestamp);
  CHECK(crlf.lines[0].text == lf.lines[0].text);
  CHECK(crlf.lines[1].timestamp == lf.lines[1].timestamp);
  CHECK(crlf.lines[1].text == lf.lines[1].text);
}

// F4 登记（不修代码）：`maxLines` 与本 TU 其余 4 个解析器一致，是**源文件行数**上限、**不是**产出行数
// 上限；产出内存由 `maxBytes` 兜住：**N 个产出行至少需 `5N-1` 字节**输入（首行 4 字节 = `<p>` +
// ≥1 字节文本；其后每行 +5 字节 = **裸 `<br>`** + ≥1 字节文本，`<br>` 与 `<br/>` 同样切分；`<p>` 外不产出）
// ⇒ `N ≤ floor((maxBytes+1)/5)`。改为「约束产出」会使 TTML 与同 TU 其余解析器不一致，故判为有意行为
// 并在此钉住 —— 若将来真的改为约束产出，本用例会判负。
TEST_CASE("ttml parser counts maxLines in source lines and bounds output by maxBytes") {
  std::string singleLine;
  for (int index = 0; index < 200; ++index) {
    singleLine += "<p begin=\"0s\">x</p>";
  }
  const auto many = parseTtmlLyrics(singleLine, {.maxBytes = 1024U * 1024U, .maxLines = 10U});
  CHECK(many.errors.empty());        // 单行文档：源行数 = 1，未触发 maxLines
  CHECK(many.lines.size() == 200U);  // 产出行数不受 maxLines 约束（已登记）

  const auto tooManyLines = parseTtmlLyrics("<p begin=\"1s\">a</p>\n<p begin=\"2s\">b</p>",
                                            {.maxBytes = 1024U, .maxLines = 1U});
  REQUIRE(tooManyLines.errors.size() == 1U);
  CHECK(tooManyLines.errors.front().code == LrcParseErrorCode::TooManyLines);

  const auto tooLarge = parseTtmlLyrics(singleLine, {.maxBytes = 64U, .maxLines = 1'000'000U});
  CHECK(tooLarge.lines.empty());
  REQUIRE(tooLarge.errors.size() == 1U);
  CHECK(tooLarge.errors.front().code == LrcParseErrorCode::FileTooLarge);
}

// F-1（第 3 轮，N2 精修）：`maxBytes` 下产出上界的分母是「首行 4 字节、其后每行 +5 字节」的等差结构，
// 即 **N 行至少需 `5N-1` 字节** ⇒ `N ≤ floor((maxBytes+1)/5)`。旧文档的「≥8 字节/行 ⇒ ≤131,072 行」
// 用的是 `<p>x</p>` 密排（8 字节/行，且 `<p>` 内无分隔）这一**特定形状的点值**；而代码自己把 `br`
// 定义为产出行，最小单元是 `a<br>`（5 字节）。本条钉住 `5N-1` 关系（含 N=1 的 4 字节反例）、
// 1 MiB 实测最大 209,715，并显式否定旧的 131,072 上界 ⇒ 旧假数字若被写回即判负。
TEST_CASE("ttml parser output bound is maxBytes over the bare br minimal unit") {
  constexpr std::size_t kMiB = 1024U * 1024U;

  // N 行最小文档 = `<p>a` + (`<br>a` × (N-1))；实测字节数须恰为 `5N-1`。N=1 即 4 字节仍产 1 行，
  // 是「每行至少 ≥5 字节」为假的直接反例（`maxBytes/5` 会把它低估为 0 行）。
  for (const std::size_t n : {1U, 2U, 3U, 10U, 100U}) {
    std::string minimal = "<p>a";
    for (std::size_t i = 1U; i < n; ++i) {
      minimal += "<br>a";
    }
    CHECK(minimal.size() == 5U * n - 1U);
    // 旧式「总字节 / 5」（分母不减 1）**恒低估 1 行**（(5N-1)/5 == N-1 < N）；精确解须用 (bytes+1)/5。
    // 这两条使「每行 ≥5 字节」的旧表述可判负，并在 N=1..100 全程成立（非单点巧合）。
    CHECK(minimal.size() / 5U == n - 1U);
    CHECK((minimal.size() + 1U) / 5U == n);
    const auto result = parseTtmlLyrics(minimal, {.maxBytes = minimal.size(), .maxLines = 1'000'000'000U});
    REQUIRE(result.errors.empty());
    CHECK(result.lines.size() == n);
  }

  // 首行边界：`<p>a`（4 字节）在 `maxBytes=4` 下仍产 1 行 ⇒ 上界须用 `floor((maxBytes+1)/5)`（本例 = 1），
  // 而非低估首行的 `maxBytes/5`（= 0）。
  const auto firstLine = parseTtmlLyrics("<p>a", {.maxBytes = 4U, .maxLines = 1'000'000'000U});
  REQUIRE(firstLine.errors.empty());
  CHECK(firstLine.lines.size() == 1U);
  CHECK((4U + 1U) / 5U == 1U);
  CHECK(firstLine.lines.size() != 4U / 5U);  // 旧除数 maxBytes/5 = 0 ⇒ 对首行低估，此断言将其证伪

  // 最小单元密排：`<p>` + (`a<br>` × k) + `a`。末段有文本 ⇒ 尾部 `<br>` 不产生被丢弃的空段。
  std::string dense = "<p>";
  while (dense.size() + 5U <= kMiB) {
    dense += "a<br>";
  }
  dense += "a";
  const auto denseResult = parseTtmlLyrics(dense, {.maxBytes = kMiB, .maxLines = 1'000'000'000U});
  REQUIRE(denseResult.errors.empty());
  // `(kMiB+1)/5` 是 `5N-1 ≤ kMiB` 的精确解；在 1 MiB 上它与 `kMiB/5` 恰好同值 209715
  // （1048576/5 = 209715.2，两者 floor 后相等），但语义不同 —— 换个小 `maxBytes`（如上面的 4）即暴露差异。
  CHECK(denseResult.lines.size() == (kMiB + 1U) / 5U);          // 209,715：1 MiB 内实测可达的最大产出行数
  CHECK(denseResult.lines.size() > 131'072U);                   // 推翻旧的「≤ 131,072 行」上界
  CHECK((dense.size() + 1U) / 5U >= denseResult.lines.size());  // 构造性上界：N 行 ≥ 5N-1 字节

  // 对照形状：`<p>x</p>` 密排 = 8 字节/行 ⇒ 仅 131,072 行（是点值，不是上界）。且 `<p>` 外不产出。
  std::string paragraphs;
  while (paragraphs.size() + 8U <= kMiB) {
    paragraphs += "<p>x</p>";
  }
  const auto paragraphResult = parseTtmlLyrics(paragraphs, {.maxBytes = kMiB, .maxLines = 1'000'000'000U});
  REQUIRE(paragraphResult.errors.empty());
  CHECK(paragraphResult.lines.size() == 131'072U);
  CHECK(denseResult.lines.size() > paragraphResult.lines.size());

  const auto outsideParagraph = parseTtmlLyrics("a<br>a<br>a", {.maxBytes = kMiB, .maxLines = 1'000'000'000U});
  CHECK(outsideParagraph.errors.empty());
  CHECK(outsideParagraph.lines.empty());  // `<p>` 之外不产出 ⇒ 最小单元必须位于 `<p>` 内

  // 超 `maxBytes` 仍直接 FileTooLarge、空 lines ⇒ 产出由 maxBytes 封顶，不存在无界产出。
  const auto oversized = parseTtmlLyrics(dense, {.maxBytes = dense.size() - 1U, .maxLines = 1'000'000'000U});
  CHECK(oversized.lines.empty());
  REQUIRE(oversized.errors.size() == 1U);
  CHECK(oversized.errors.front().code == LrcParseErrorCode::FileTooLarge);
}

// F5：属性名与元素名同口径（按 local name 比较，忽略前缀）⇒ `tt:begin` 等同 `begin`。
// 修复前属性名是精确比较 ⇒ `tt:begin` 不被识别、产出 unsynced 哨兵。
TEST_CASE("ttml parser matches begin attribute by local name ignoring prefix") {
  const auto prefixed = parseTtmlLyrics("<tt:p tt:begin=\"1s\">x</tt:p>");
  REQUIRE(prefixed.errors.empty());
  REQUIRE(prefixed.lines.size() == 1U);
  CHECK(prefixed.lines.front().timestamp == kTtml(1000));
  CHECK(prefixed.lines.front().text == "x");

  const auto otherPrefix = parseTtmlLyrics("<x:p y:begin=\"2s\">y</x:p>");
  REQUIRE(otherPrefix.lines.size() == 1U);
  CHECK(otherPrefix.lines.front().timestamp == kTtml(2000));

  // 大小写**不**忽略（XML 大小写敏感）⇒ `Begin` 不是 `begin` ⇒ 无 begin ⇒ unsynced 且文本保留。
  const auto caseSensitive = parseTtmlLyrics("<p Begin=\"1s\">x</p>");
  REQUIRE(caseSensitive.errors.empty());
  REQUIRE(caseSensitive.lines.size() == 1U);
  CHECK(caseSensitive.lines.front().timestamp == kTtml(-1));
  CHECK(caseSensitive.lines.front().text == "x");
}

// R3-F1：畸形但**终止于 `>`** 的标签不再被当作「文档截断」⇒ 其后内容继续解析。
// 修复前这些分支返回未闭合，调用方据此丢弃整个文档、且 0 错误（正文静默消失）。
TEST_CASE("ttml parser keeps scanning after a malformed but >-terminated tag") {
  // 缺引号值：`<p begin=1s>` 被跳过并记一条错误，其后 `<p>` 照常产出。
  const auto noQuote = parseTtmlLyrics("<p begin=1s>x</p><p begin=\"2s\">y</p>");
  REQUIRE(noQuote.errors.size() == 1U);
  CHECK(noQuote.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
  REQUIRE(noQuote.lines.size() == 1U);
  CHECK(noQuote.lines.front().text == "y");
  CHECK(noQuote.lines.front().timestamp == kTtml(2000));

  // 空值且无引号：`<p begin=>`。
  const auto emptyValue = parseTtmlLyrics("<p begin=>x</p><p begin=\"3s\">z</p>");
  REQUIRE(emptyValue.errors.size() == 1U);
  REQUIRE(emptyValue.lines.size() == 1U);
  CHECK(emptyValue.lines.front().text == "z");
  CHECK(emptyValue.lines.front().timestamp == kTtml(3000));

  // 与既有的「begin 值畸形」同处置（记 InvalidTimestamp、跳过该段、后续照常）。
  const auto malformedValue = parseTtmlLyrics("<p begin=\"zzz\">x</p><p begin=\"4s\">w</p>");
  REQUIRE(malformedValue.errors.size() == 1U);
  REQUIRE(malformedValue.lines.size() == 1U);
  CHECK(malformedValue.lines.front().text == "w");
  CHECK(malformedValue.lines.front().timestamp == kTtml(4000));

  // 真截断（标签其后无 `>`）：维持文档截断语义 —— 已开 `<p>` 的已收文本仍产出、不记错。
  const auto truncated = parseTtmlLyrics("<p begin=\"1s\">x");
  CHECK(truncated.errors.empty());
  REQUIRE(truncated.lines.size() == 1U);
  CHECK(truncated.lines.front().text == "x");

  // 畸形且其后无 `>`（亦属截断）⇒ 不产出、不记错。
  const auto malformedYetTruncated = parseTtmlLyrics("<p begin=1s");
  CHECK(malformedYetTruncated.errors.empty());
  CHECK(malformedYetTruncated.lines.empty());
}

// R3-F2：`xmlns` / `xmlns:*` 是命名空间声明，不参与 `begin` 匹配。
// 修复前 `xmlns:begin` 剥前缀后误命中 `begin`，把 URI 当时间值 ⇒ 整段被跳过（lines=0）。
TEST_CASE("ttml parser ignores xmlns declarations when matching begin") {
  const auto nsPrefixed = parseTtmlLyrics("<p xmlns:begin=\"http://x\" begin=\"2s\">x</p>");
  REQUIRE(nsPrefixed.errors.empty());
  REQUIRE(nsPrefixed.lines.size() == 1U);
  CHECK(nsPrefixed.lines.front().timestamp == kTtml(2000));  // 取真 `begin`，而非 `xmlns:begin` 的 URI
  CHECK(nsPrefixed.lines.front().text == "x");

  // 只有 `xmlns:begin` 声明、无真 `begin` ⇒ 无 begin ⇒ unsynced 且文本保留。
  const auto nsOnly = parseTtmlLyrics("<p xmlns:begin=\"http://x\">x</p>");
  REQUIRE(nsOnly.errors.empty());
  REQUIRE(nsOnly.lines.size() == 1U);
  CHECK(nsOnly.lines.front().timestamp == kTtml(-1));
  CHECK(nsOnly.lines.front().text == "x");

  // 裸 `xmlns` 声明同样不参与匹配（无 `begin` ⇒ unsynced）。
  const auto plainXmlns = parseTtmlLyrics("<p xmlns=\"http://www.w3.org/ns/ttml\">t</p>");
  REQUIRE(plainXmlns.errors.empty());
  REQUIRE(plainXmlns.lines.size() == 1U);
  CHECK(plainXmlns.lines.front().timestamp == kTtml(-1));

  // 既有行为保留：非 `xmlns` 前缀仍按 local name 等价（`tt:begin` ≡ `begin`）。
  const auto ttBegin = parseTtmlLyrics("<p tt:begin=\"1s\">x</p>");
  REQUIRE(ttBegin.errors.empty());
  REQUIRE(ttBegin.lines.size() == 1U);
  CHECK(ttBegin.lines.front().timestamp == kTtml(1000));
}

// R4-F1 O1：良构 XML（非 ASCII 属性名 + 属性值内含 `>`）曾被畸形恢复把标签尾部泄漏进歌词。
// 期望文本 `xyz`（`<span>` 是透明容器），由独立 ElementTree oracle 复核（`itertext()` 拼接）。
TEST_CASE("ttml parser handles non-ascii attribute names in well-formed xml") {
  const auto nonAscii = parseTtmlLyrics("<p begin=\"1s\">x<span \xc3\xbc=\"u\" q=\"a>b\">y</span>z</p>");
  REQUIRE(nonAscii.errors.empty());
  REQUIRE(nonAscii.lines.size() == 1U);
  CHECK(nonAscii.lines.front().text == "xyz");
  CHECK(nonAscii.lines.front().timestamp == kTtml(1000));
}

// R4-F1 O2：`<p>` 上的非 ASCII **未知**属性值曾被误当 begin 值 ⇒ 整段被跳过。
// 未知属性应被忽略，`t` 以真实 `begin` 时间产出。
TEST_CASE("ttml parser ignores unknown non-ascii attributes on p") {
  const auto nonAsciiAttr = parseTtmlLyrics("<p begin=\"1s\" \xc3\xbc=\"x\">t</p>");
  REQUIRE(nonAsciiAttr.errors.empty());
  REQUIRE(nonAsciiAttr.lines.size() == 1U);
  CHECK(nonAsciiAttr.lines.front().timestamp == kTtml(1000));
  CHECK(nonAsciiAttr.lines.front().text == "t");

  // 非 ASCII 元素名按未知元素（透明容器）处置 ⇒ 其文本仍落入当前片段。
  const auto nonAsciiElement = parseTtmlLyrics("<p begin=\"1s\">a<\xc3\xbc>m</\xc3\xbc>b</p>");
  REQUIRE(nonAsciiElement.errors.empty());
  REQUIRE(nonAsciiElement.lines.size() == 1U);
  CHECK(nonAsciiElement.lines.front().text == "amb");
}

// R4-F1（同根因，畸形输入）：`malformedAt` 现跳过成对引号内的 `>` ⇒ 畸形标签的真终止符被正确
// 定位，标签尾部（`">b` 之类）不再泄漏进歌词。对照：属性值内含 `>` 的**良构**属性不受影响。
TEST_CASE("ttml parser does not leak tag tail from malformed tags with quoted gt") {
  // 畸形内层 `<p>`（`begin=1s` 缺引号）且其属性含 `q="a>b"`：应在真 `>` 处结束。
  const auto malformedWithQuotedGt = parseTtmlLyrics("<p begin=\"1s\">A<p begin=1s q=\"a>b\">B</p>C</p>");
  REQUIRE(malformedWithQuotedGt.errors.size() == 1U);
  CHECK(malformedWithQuotedGt.errors.front().code == LrcParseErrorCode::InvalidTimestamp);
  REQUIRE(malformedWithQuotedGt.lines.size() == 1U);
  CHECK(malformedWithQuotedGt.lines.front().text == "AB");  // 无 `b">` 泄漏（畸形内层段被跳过）

  // 良构：属性值内的 `>` 属引号内内容，不终止标签，也不进入歌词。
  const auto quotedGt = parseTtmlLyrics("<p q=\"a>b\" begin=\"2s\">t</p>");
  REQUIRE(quotedGt.errors.empty());
  REQUIRE(quotedGt.lines.size() == 1U);
  CHECK(quotedGt.lines.front().timestamp == kTtml(2000));
  CHECK(quotedGt.lines.front().text == "t");

  // 自闭合元素 + 属性值含 `>`：`/>` 仍被正确识别。
  const auto selfClosingQuotedGt = parseTtmlLyrics("<p begin=\"1s\">a<span q=\"x>y\"/></p>");
  REQUIRE(selfClosingQuotedGt.errors.empty());
  REQUIRE(selfClosingQuotedGt.lines.size() == 1U);
  CHECK(selfClosingQuotedGt.lines.front().text == "a");
}

// R4-F1 边界：真畸形仍按契约处置 —— 畸形但终止于 `>` ⇒ 记 InvalidTimestamp 并跳过该段；
// 引号未闭合到末尾 ⇒ 未终止（截断）。并验证密集畸形下 `cursor` 严格前进（不挂）。
TEST_CASE("ttml parser treats unclosed quote as truncation and advances past malformed tags") {
  // 引号未闭合到末尾 ⇒ 未终止（截断）：不产出、不记错误（该 `<p>` 尚未登记）。
  const auto unclosed = parseTtmlLyrics("<p begin=\"1s>x");
  CHECK(unclosed.errors.empty());
  CHECK(unclosed.lines.empty());

  // 未闭合到 EOF、无任何 `>`：截断，已开 `<p>` 的已收文本仍产出（不丢）。
  const auto truncated = parseTtmlLyrics("<p begin=\"1s\">x");
  CHECK(truncated.errors.empty());
  REQUIRE(truncated.lines.size() == 1U);
  CHECK(truncated.lines.front().text == "x");

  // 密集畸形：每条都须前进并判定，末尾合法段仍产出。
  std::string dense;
  for (int index = 0; index < 500; ++index) {
    dense += "<p begin=1s q=\"a>b\">drop</p>";
  }
  dense += "<p begin=\"7s\">tail</p>";
  const auto manyMalformed = parseTtmlLyrics(dense);
  CHECK(manyMalformed.errors.size() == 500U);
  REQUIRE(manyMalformed.lines.size() == 1U);
  CHECK(manyMalformed.lines.front().text == "tail");
  CHECK(manyMalformed.lines.front().timestamp == kTtml(7000));
}

// R5-F1：`malformedAt` 遇**引号外的裸 `<`** 即停（该 `<` 属于下一个标签，典型是所在块的 `</p>`）。
// 修复前它会越过那个 `</p>` 去找 `>` ⇒ `paragraphDepth` 不归零 ⇒ 后续 `<p>` 不接管 `begin`
// ⇒ 段落被并入首段并继承首段时间戳（静默、0 错误）。
TEST_CASE("ttml parser keeps paragraph boundaries when a malformed tag precedes the closing tag") {
  // 对照（良构）：两段各自的 begin 生效。
  const auto control = parseTtmlLyrics("<p begin=\"1s\">A</p><p begin=\"2s\">B</p>");
  REQUIRE(control.errors.empty());
  REQUIRE(control.lines.size() == 2U);
  CHECK(control.lines[0].timestamp == kTtml(1000));
  CHECK(control.lines[0].text == "A");
  CHECK(control.lines[1].timestamp == kTtml(2000));
  CHECK(control.lines[1].text == "B");

  // 漏写 `>` 的开启标签：`</p>` 仍被看到 ⇒ 两段独立。
  const auto strayOpen = parseTtmlLyrics("<p begin=\"1s\">A<span</p><p begin=\"2s\">B</p>");
  REQUIRE(strayOpen.errors.empty());
  REQUIRE(strayOpen.lines.size() == 2U);
  CHECK(strayOpen.lines[0].timestamp == kTtml(1000));
  CHECK(strayOpen.lines[0].text == "A");
  CHECK(strayOpen.lines[1].timestamp == kTtml(2000));
  CHECK(strayOpen.lines[1].text == "B");

  // 三段：三个时间戳都要保留（修复前只剩首个）。
  const auto three = parseTtmlLyrics("<p begin=\"1s\">A<span</p><p begin=\"2s\">B</p><p begin=\"3s\">C</p>");
  REQUIRE(three.errors.empty());
  REQUIRE(three.lines.size() == 3U);
  CHECK(three.lines[0].timestamp == kTtml(1000));
  CHECK(three.lines[1].timestamp == kTtml(2000));
  CHECK(three.lines[2].timestamp == kTtml(3000));

  // 缺 `begin` 的段取 unsynced 哨兵（不继承首段时间戳）。unsynced 哨兵排在有时间戳者之前。
  const auto unsyncedSecond = parseTtmlLyrics("<p begin=\"1s\">A<span</p><p>B</p>");
  REQUIRE(unsyncedSecond.errors.empty());
  REQUIRE(unsyncedSecond.lines.size() == 2U);
  CHECK(unsyncedSecond.lines[0].text == "B");
  CHECK(unsyncedSecond.lines[0].timestamp == kTtml(-1));
  CHECK(unsyncedSecond.lines[1].text == "A");
  CHECK(unsyncedSecond.lines[1].timestamp == kTtml(1000));

  // 正文里的裸 `<b`：按不构成标签/畸形开启标签忽略，段边界与时间戳不被破坏。
  const auto bodyLessThan = parseTtmlLyrics("<p begin=\"1s\">a <b and c</p><p begin=\"2s\">D</p>");
  REQUIRE(bodyLessThan.errors.empty());
  REQUIRE(bodyLessThan.lines.size() == 2U);
  CHECK(bodyLessThan.lines[0].timestamp == kTtml(1000));
  CHECK(bodyLessThan.lines[0].text == "a");
  CHECK(bodyLessThan.lines[1].timestamp == kTtml(2000));
  CHECK(bodyLessThan.lines[1].text == "D");

  // 真截断仍按截断处置（该 `<` 之后到 EOF 无 `>` 与更早的 `<`）。
  const auto truncated = parseTtmlLyrics("<p begin=\"1s\">x<span");
  CHECK(truncated.errors.empty());
  REQUIRE(truncated.lines.size() == 1U);
  CHECK(truncated.lines.front().text == "x");

  // 密集畸形（含引号内 `>`）后接合法段：须收敛并产出尾部段。
  std::string dense;
  for (int index = 0; index < 400; ++index) {
    dense += "<p begin=1s q=\"a>b\">drop<span</p>";
  }
  dense += "<p begin=\"9s\">tail</p>";
  const auto many = parseTtmlLyrics(dense);
  REQUIRE(many.lines.size() == 1U);
  CHECK(many.lines.front().text == "tail");
  CHECK(many.lines.front().timestamp == kTtml(9000));
}

// R7-F1 共用断言：`A` 与 `B` 须各自成段，时间戳分别取各自的 `begin`（对照的良构形式见首条用例）。
void requireTwoOwnParagraphs(const std::string& doc) {
  const auto result = parseTtmlLyrics(doc);
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "A");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "B");
}

TEST_CASE("ttml parser keeps paragraph boundaries with a well-formed control") {
  // 对照：无畸形片段时两段本就各自成立，是下列各条的前提。
  requireTwoOwnParagraphs("<p begin=\"1s\">A</p><p begin=\"2s\">B</p>");
}

TEST_CASE("ttml parser keeps paragraph boundaries when an end tag lacks gt") {
  // `</span` 缺 `>`：恢复须停在 `</p>` 前的裸 `<`；否则 `</p>` 被吞 ⇒ `B` 并入 `A` 并继承 1000ms。
  requireTwoOwnParagraphs("<p begin=\"1s\">A</span</p><p begin=\"2s\">B</p>");
}

TEST_CASE("ttml parser keeps paragraph boundaries when a declaration lacks gt") {
  requireTwoOwnParagraphs("<p begin=\"1s\">A<!x</p><p begin=\"2s\">B</p>");
}

TEST_CASE("ttml parser keeps paragraph boundaries when a comment is unterminated") {
  requireTwoOwnParagraphs("<p begin=\"1s\">A<!--x</p><p begin=\"2s\">B</p>");
}

TEST_CASE("ttml parser keeps paragraph boundaries when a processing instruction is unterminated") {
  requireTwoOwnParagraphs("<p begin=\"1s\">A<?x</p><p begin=\"2s\">B</p>");
}

TEST_CASE("ttml parser keeps paragraph boundaries when cdata is unterminated and drops its marker") {
  // 未闭合 CDATA：内容只取到裸 `<` 之前，`<![CDATA[` 起始标记丢弃；其后标签文本**不得**进入歌词。
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<![CDATA[x</p><p begin=\"2s\">B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "Ax");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "B");
}

TEST_CASE("ttml parser keeps the unsynced timestamp when an end tag lacks gt") {
  // 首段缺 `begin` ⇒ 哨兵 -1ms（排在有时间戳者之前）；`B` 的 2000ms 不得被吞。
  const auto result = parseTtmlLyrics("<p>A</span</p><p begin=\"2s\">B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(-1));
  CHECK(result.lines[0].text == "A");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "B");
}

TEST_CASE("ttml parser skips well-formed comments cdata pi and declarations") {
  // 共享判定的前提：正常终止符在场时整体跳过，即使正文含 `<`/`>`/引号（这些是良构 XML）。
  const auto comment =
      parseTtmlLyrics("<tt><!-- a <p begin=\"9s\">fake</p> --><p begin=\"1s\">A</p></tt>");
  REQUIRE(comment.lines.size() == 1U);
  CHECK(comment.lines.front().timestamp == kTtml(1000));
  CHECK(comment.lines.front().text == "A");

  const auto cdata = parseTtmlLyrics("<p begin=\"1s\"><![CDATA[a & b < c > d]]></p>");
  REQUIRE(cdata.lines.size() == 1U);
  CHECK(cdata.lines.front().text == "a & b < c > d");

  const auto pi = parseTtmlLyrics("<?xml version=\"1.0\" encoding=\"UTF-8\"?><p begin=\"1s\">A</p>");
  REQUIRE(pi.lines.size() == 1U);
  CHECK(pi.lines.front().text == "A");

  const auto doctype = parseTtmlLyrics("<!DOCTYPE tt SYSTEM \"a>b\"><p begin=\"1s\">A</p>");
  REQUIRE(doctype.lines.size() == 1U);
  CHECK(doctype.lines.front().text == "A");

  // R4-F1 代表输入：引号内的 `>` 不得提前结束标签；非 ASCII 名字不得误判畸形。
  const auto r4 = parseTtmlLyrics("<p begin=\"1s\">x<span ü=\"u\" q=\"a>b\">y</span>z</p>");
  REQUIRE(r4.errors.empty());
  REQUIRE(r4.lines.size() == 1U);
  CHECK(r4.lines.front().timestamp == kTtml(1000));
  CHECK(r4.lines.front().text == "xyz");

  // R5-F1 代表输入：畸形起始标签须停在裸 `<`，否则后段并入前段。
  const auto r5 = parseTtmlLyrics("<p begin=\"1s\">A<span</p><p begin=\"2s\">B</p>");
  REQUIRE(r5.errors.empty());
  REQUIRE(r5.lines.size() == 2U);
  CHECK(r5.lines[0].timestamp == kTtml(1000));
  CHECK(r5.lines[0].text == "A");
  CHECK(r5.lines[1].timestamp == kTtml(2000));
  CHECK(r5.lines[1].text == "B");
}

// R8-F1：多字符构造（注释 `-->` / CDATA `]]>` / PI `?>`）的**真终止符优先** —— 终止符在场时，
// 构造体内的一切（`</p>`、引号、`<span>`）都只是载荷字符。修复前终止符被裸 `</` 抢先截断 ⇒
// 载荷泄漏进歌词或后续文本整段丢失（各条症状见下）。

TEST_CASE("ttml parser gives a comment its real terminator even when the payload holds an end tag") {
  // 注释载荷里的 `</p>` 不关闭段落；终止符 `-->` 之后的 `B` 必须保留（修复前 B 丢失）。
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<!-- </p> -->B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == kTtml(1000));
  CHECK(result.lines.front().text == "AB");
}

TEST_CASE("ttml parser gives a comment its real terminator even when the payload holds markup") {
  // 载荷含 `<span>x</span>` 时整体跳过（修复前 ` -->` 泄漏进歌词，产出 `A -->B`）。
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<!-- <span>x</span> -->B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == kTtml(1000));
  CHECK(result.lines.front().text == "AB");
}

TEST_CASE("ttml parser gives cdata its real terminator and takes the payload literally") {
  // CDATA 载荷按字面取用 ⇒ `</p>` 成为文本（修复前该内容丢失，只余 `A`）。
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<![CDATA[</p>]]>B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == kTtml(1000));
  CHECK(result.lines.front().text == "A</p>B");
}

TEST_CASE("ttml parser gives a processing instruction its real terminator even with an end tag inside") {
  // PI 载荷里的 `</p>` 只是字符；`?>` 之后的 `B` 必须保留（修复前 B 丢失）。
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<?x </p> ?>B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 1U);
  CHECK(result.lines.front().timestamp == kTtml(1000));
  CHECK(result.lines.front().text == "AB");
}

// R8-F3：`<!` 声明内的引号与 `[ ... ]` 内部子集**不得越过裸 `</`**。`</` 是唯一能关闭 `<p>` 的形式，
// 被跳过就把后段并入前段、吞掉其后时间戳（本接缝的已知阻塞类）。内部子集本身仍要支持（见既有
// `ttml parser matches local names ignoring prefixes and skips comments and declarations` 用例）。
TEST_CASE("ttml parser stops a declaration at the bare end tag inside an internal subset") {
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<! [ </p> ] ><p begin=\"2s\">B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "A");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "B");
}

TEST_CASE("ttml parser stops a declaration at the bare end tag inside quotes") {
  const auto result = parseTtmlLyrics("<p begin=\"1s\">A<! \"x </p> y\" ><p begin=\"2s\">B</p>");
  CHECK(result.errors.empty());
  REQUIRE(result.lines.size() == 2U);
  CHECK(result.lines[0].timestamp == kTtml(1000));
  CHECK(result.lines[0].text == "A");
  CHECK(result.lines[1].timestamp == kTtml(2000));
  CHECK(result.lines[1].text == "B");
}

// R8-F1 规模守卫（判负方式）：终止符「存在性」若靠每次构造扫到文档尾来判定，重复的未闭合构造
// 就是 Θ(N²)。本用例的规模在总额 O(N) 下耗时在毫秒量级；退化为超线性时同规模会放大到分钟量级
// ⇒ 下面的宽松上限（比线性实测高约三个数量级）足以判负，且不会因机器负载而误报。
TEST_CASE("ttml parser terminates repeated unterminated constructs without superlinear blowup") {
  constexpr std::size_t kCount = 100000U;
  std::string cdata;
  cdata.reserve(kCount * 10U);
  for (std::size_t i = 0; i < kCount; ++i) {
    cdata += "<![CDATA[>";
  }
  const auto cdataStart = std::chrono::steady_clock::now();
  const auto cdataResult = parseTtmlLyrics(cdata, {.maxBytes = 8U << 20, .maxLines = 8U << 20});
  const auto cdataElapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cdataStart)
          .count();
  CHECK(cdataResult.errors.empty());
  CHECK(cdataResult.lines.empty());
  CHECK(cdataElapsed < 5000);

  std::string comments;
  comments.reserve(kCount * 4U);
  for (std::size_t i = 0; i < kCount; ++i) {
    comments += "<!--";
  }
  const auto commentStart = std::chrono::steady_clock::now();
  const auto commentResult = parseTtmlLyrics(comments, {.maxBytes = 8U << 20, .maxLines = 8U << 20});
  const auto commentElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - commentStart)
                                  .count();
  CHECK(commentResult.errors.empty());
  CHECK(commentResult.lines.empty());
  CHECK(commentElapsed < 5000);
}

#endif  // SERIONA_TTML_FIXTURE_DIR

}
}
