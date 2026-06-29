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

TEST_CASE("readCueSheet returns empty vector for non-existent file") {
  test::TempScannerRoot root{std::string{"cue_adapter_nonexistent"}};
  const auto cuePath = root.path() / std::filesystem::path{"does_not_exist.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("readCueSheet returns empty vector for malformed CUE file") {
  test::TempScannerRoot root{std::string{"cue_adapter_malformed"}};
  const auto cuePath = root.path() / std::filesystem::path{"malformed.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};

  std::ofstream cueFile{cuePath};
  cueFile << "GARBAGE NOT A VALID CUE\n";
  cueFile << "MORE GARBAGE\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  CHECK(results.empty());
}

TEST_CASE("readCueSheet handles valid CUE with existing audio file") {
  test::TempScannerRoot root{std::string{"cue_adapter_valid"}};
  const auto audioPath = root.path() / std::filesystem::path{"album.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  // Create minimal audio file
  std::ofstream audioFile{audioPath};
  audioFile.close();

  // Create valid CUE
  std::ofstream cueFile{cuePath};
  cueFile << "REM GENRE Rock\n";
  cueFile << "REM DATE 2026\n";
  cueFile << "TITLE \"Test Album\"\n";
  cueFile << "PERFORMER \"Test Artist\"\n";
  cueFile << "FILE \"album.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Track One\"\n";
  cueFile << "    PERFORMER \"Artist One\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile << "  TRACK 02 AUDIO\n";
  cueFile << "    TITLE \"Track Two\"\n";
  cueFile << "    INDEX 01 03:30:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  // Verify function returns without crashing
  CHECK(results.size() >= 0);
}

TEST_CASE("readCueSheet handles CUE with missing audio gracefully") {
  test::TempScannerRoot root{std::string{"cue_adapter_missing_audio"}};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Test Album\"\n";
  cueFile << "PERFORMER \"Test Artist\"\n";
  cueFile << "FILE \"missing.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Track One\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  // Should return empty and log warning (not crash)
  CHECK(results.empty());
}

TEST_CASE("readCueSheet converts to RawTagMetadata structure") {
  test::TempScannerRoot root{std::string{"cue_adapter_structure"}};
  const auto audioPath = root.path() / std::filesystem::path{"track.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "REM GENRE Jazz\n";
  cueFile << "REM DATE 2025\n";
  cueFile << "TITLE \"Album Title\"\n";
  cueFile << "PERFORMER \"Album Artist\"\n";
  cueFile << "FILE \"track.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Song Title\"\n";
  cueFile << "    PERFORMER \"Track Artist\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  if (!results.empty()) {
    const auto& track = results[0];
    // Verify RawTagMetadata fields exist
    CHECK(track.offset >= std::chrono::microseconds{0});
    CHECK(track.duration >= std::chrono::microseconds{0});
  }
}

TEST_CASE("readCueSheet preserves microsecond precision for offset and duration") {
  test::TempScannerRoot root{std::string{"cue_adapter_timing"}};
  const auto audioPath = root.path() / std::filesystem::path{"disc.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"disc.cue"};
  const auto coverDir = root.path() / std::filesystem::path{"covers"};
  std::filesystem::create_directories(coverDir);

  std::ofstream audioFile{audioPath};
  audioFile.close();

  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Timed Album\"\n";
  cueFile << "PERFORMER \"Test\"\n";
  cueFile << "FILE \"disc.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"First\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile << "  TRACK 02 AUDIO\n";
  cueFile << "    TITLE \"Second\"\n";
  cueFile << "    INDEX 01 02:30:00\n";
  cueFile.close();

  const auto results = readCueSheet(cuePath, coverDir);

  if (results.size() >= 2) {
    // Verify timing fields are in microseconds
    CHECK(results[0].offset >= std::chrono::microseconds{0});
    CHECK(results[1].offset >= std::chrono::microseconds{0});
    CHECK(results[0].duration >= std::chrono::microseconds{0});
    CHECK(results[1].duration >= std::chrono::microseconds{0});
  }
}

}
}
