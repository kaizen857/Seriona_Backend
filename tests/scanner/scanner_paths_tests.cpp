#include "scanner_test_harness.h"

#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"

#include <doctest.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
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
  const auto unsupported = root.path() / "single.txt";
  writeTextFile(unsupported, "text");

  const auto singleFile = discoverScannerPaths({.path = audio, .recursive = false});
  REQUIRE(singleFile.size() == 1U);
  CHECK(singleFile.front().kind == PathEntryKind::SingleFileRoot);
  CHECK(singleFile.front().displayName == "single.weba");

  CHECK(classifyScannerPath(root.path(), unsupported).kind == PathEntryKind::Unsupported);
  CHECK(classifyScannerPath(root.path(), unsupported, {.allowedExtensions = {".txt"}}).kind ==
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
  writeTextFile(root.path() / "not-cue.txt", "some text file\n");
  writeTextFile(root.path() / "also-not.cu", "cuda file maybe\n");
  writeTextFile(root.path() / "prefix.cue.bak", "backup of cue\n");

  const auto entries = discoverScannerPaths({.path = root.path(), .recursive = true});

  CHECK(requireRelativePath(entries, "valid.cue").kind == PathEntryKind::CueSheet);
  CHECK(requireRelativePath(entries, "not-cue.txt").kind == PathEntryKind::Unsupported);
  CHECK(requireRelativePath(entries, "also-not.cu").kind == PathEntryKind::Unsupported);
  CHECK(requireRelativePath(entries, "prefix.cue.bak").kind == PathEntryKind::Unsupported);
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

}
}
