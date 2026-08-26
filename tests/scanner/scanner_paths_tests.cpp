#include "scanner_test_harness.h"

#include "seriona/scanner/lrc_parser.h"
#include "seriona/scanner/path_utils.h"

#include <doctest.h>

#include <algorithm>
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

}
}
