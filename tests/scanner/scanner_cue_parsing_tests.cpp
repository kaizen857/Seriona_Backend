#include "scanner_test_harness.h"

#include "../../inc/seriona/scanner/tag_reader_metadata_adapter.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace seriona::scanner {
namespace {

TEST_CASE("CUE parsing: empty CUE file returns empty track list") {
  test::TempScannerRoot root{std::string{"cue_parse_empty"}};
  const auto cuePath = root.path() / std::filesystem::path{"empty.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Create empty CUE file
  std::ofstream cueFile{cuePath};
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: adapter handles empty audio file without crash") {
  test::TempScannerRoot root{std::string{"cue_parse_single_track"}};
  const auto audioPath = root.path() / std::filesystem::path{"single.wav"};
  const auto cuePath = root.path() / std::filesystem::path{"single.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Single Track Album\"\n";
  cueFile << "PERFORMER \"Solo Artist\"\n";
  cueFile << "FILE \"single.wav\" WAVE\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Only Track\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: adapter handles multi-track CUE with empty audio") {
  test::TempScannerRoot root{std::string{"cue_parse_multi_track"}};
  const auto audioPath = root.path() / std::filesystem::path{"album.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "REM GENRE Progressive Rock\n";
  cueFile << "REM DATE 2026\n";
  cueFile << "TITLE \"Three Track Album\"\n";
  cueFile << "PERFORMER \"Test Band\"\n";
  cueFile << "FILE \"album.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"First Song\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile << "  TRACK 02 AUDIO\n";
  cueFile << "    TITLE \"Second Song\"\n";
  cueFile << "    INDEX 01 04:15:30\n";
  cueFile << "  TRACK 03 AUDIO\n";
  cueFile << "    TITLE \"Third Song\"\n";
  cueFile << "    INDEX 01 08:45:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: adapter handles two-track CUE with empty audio") {
  test::TempScannerRoot root{std::string{"cue_parse_duration"}};
  const auto audioPath = root.path() / std::filesystem::path{"timed.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"timed.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Duration Test\"\n";
  cueFile << "FILE \"timed.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Track A\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile << "  TRACK 02 AUDIO\n";
  cueFile << "    TITLE \"Track B\"\n";
  cueFile << "    INDEX 01 03:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: invalid CUE syntax returns empty without crash") {
  test::TempScannerRoot root{std::string{"cue_parse_invalid_syntax"}};
  const auto cuePath = root.path() / std::filesystem::path{"broken.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Create CUE with invalid syntax
  std::ofstream cueFile{cuePath};
  cueFile << "THIS IS NOT VALID CUE SYNTAX\n";
  cueFile << "NO FILE DIRECTIVE\n";
  cueFile << "NO TRACK DIRECTIVES\n";
  cueFile << "JUST GARBAGE DATA\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  // Should handle gracefully and return empty
  CHECK(results.empty());
}

TEST_CASE("CUE parsing: binary file treated as invalid CUE") {
  test::TempScannerRoot root{std::string{"cue_parse_binary"}};
  const auto cuePath = root.path() / std::filesystem::path{"binary.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Create binary file with .cue extension
  std::ofstream cueFile{cuePath, std::ios::binary};
  for (int i = 0; i < 256; ++i) {
    cueFile.put(static_cast<char>(i));
  }
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  // Should not crash on binary data
  CHECK(results.empty());
}

TEST_CASE("CUE parsing: UTF-8 CUE with empty audio returns empty") {
  test::TempScannerRoot root{std::string{"cue_parse_utf8"}};
  const auto audioPath = root.path() / std::filesystem::path{"utf8.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"utf8.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"测试专辑\"\n";
  cueFile << "PERFORMER \"テストアーティスト\"\n";
  cueFile << "FILE \"utf8.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Café Music\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: large 20-track CUE with empty audio returns empty") {
  test::TempScannerRoot root{std::string{"cue_parse_large"}};
  const auto audioPath = root.path() / std::filesystem::path{"compilation.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"compilation.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Large Compilation\"\n";
  cueFile << "FILE \"compilation.flac\" FLAC\n";
  for (int i = 1; i <= 20; ++i) {
    cueFile << "  TRACK " << (i < 10 ? "0" : "") << i << " AUDIO\n";
    cueFile << "    TITLE \"Track " << i << "\"\n";
    cueFile << "    INDEX 01 " << (i < 10 ? "0" : "") << (i - 1) * 3 << ":00:00\n";
  }
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("CUE parsing: CUE referencing non-existent audio file returns empty") {
  test::TempScannerRoot root{std::string{"cue_parse_missing_audio"}};
  const auto cuePath = root.path() / std::filesystem::path{"orphan.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Create CUE referencing file that doesn't exist
  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Orphan CUE\"\n";
  cueFile << "FILE \"does_not_exist.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Ghost Track\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  // Should return empty due to missing audio (TagReader behavior)
  CHECK(results.empty());
}

TEST_CASE("CUE parsing: adapter returns correct type for empty audio") {
  test::TempScannerRoot root{std::string{"cue_parse_type_check"}};
  const auto audioPath = root.path() / std::filesystem::path{"type.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"type.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Type Test\"\n";
  cueFile << "FILE \"type.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Test\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  REQUIRE(std::is_same_v<decltype(results), const std::vector<RawTagMetadata>>);
  CHECK(results.empty());
}

TEST_CASE("CUE parsing: readCueSheet does not throw exceptions on error") {
  test::TempScannerRoot root{std::string{"cue_parse_no_throw"}};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Test various error conditions - none should throw
  
  // Non-existent file
  const auto nonExistent = root.path() / std::filesystem::path{"missing.cue"};
  CHECK_NOTHROW({
    auto result = readCueSheet(nonExistent, coverDir);
    CHECK(result.empty());
  });

  // Invalid CUE content
  const auto invalidPath = root.path() / std::filesystem::path{"invalid.cue"};
  std::ofstream invalidFile{invalidPath};
  invalidFile << "NOT A CUE FILE\n";
  invalidFile.close();
  CHECK_NOTHROW({
    auto result = readCueSheet(invalidPath, coverDir);
    CHECK(result.empty());
  });

  // Binary content
  const auto binaryPath = root.path() / std::filesystem::path{"binary.cue"};
  std::ofstream binaryFile{binaryPath, std::ios::binary};
  binaryFile.put('\0');
  binaryFile.put('\xFF');
  binaryFile.close();
  CHECK_NOTHROW({
    auto result = readCueSheet(binaryPath, coverDir);
    CHECK(result.empty());
  });
}

}
}
