#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "seriona/scanner/path_utils.h"
#include "scanner_test_harness.h"

#include <filesystem>
#include <fstream>

using namespace seriona::scanner;
using namespace seriona::scanner::test;

TEST_CASE("CUE error isolation: single bad CUE with good CUE and audio") {
  TempScannerRoot temp{"cue_error_isolation_bad_good"};
  
  std::ofstream(temp.path() / "bad.cue") << "INVALID CUE CONTENT WITHOUT FILE LINES\n";
  std::ofstream(temp.path() / "good.cue") << R"(FILE "audio.flac" WAVE
  TRACK 01 AUDIO
    TITLE "Track 1"
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "audio.flac");
  std::ofstream(temp.path() / "standalone.mp3");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::size_t badCueCount = 0;
  std::size_t goodCueCount = 0;
  std::size_t standaloneAudioCount = 0;
  std::size_t referencedAudioCount = 0;
  std::size_t cueWithErrorsCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      if (entry.path.filename() == "bad.cue") {
        ++badCueCount;
        if (!entry.errors.empty()) {
          ++cueWithErrorsCount;
          CHECK(entry.errors[0].code == ScannerErrorCode::MetadataReadFailed);
          CHECK(entry.errors[0].message == "CUE sheet contains no valid FILE lines");
        }
      } else if (entry.path.filename() == "good.cue") {
        ++goodCueCount;
        CHECK(entry.errors.empty());
      }
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      if (entry.path.filename() == "standalone.mp3") {
        ++standaloneAudioCount;
      } else if (entry.path.filename() == "audio.flac") {
        ++referencedAudioCount;
      }
    }
  }
  
  CHECK(badCueCount == 1);
  CHECK(goodCueCount == 1);
  CHECK(standaloneAudioCount == 1);
  CHECK(referencedAudioCount == 0);
  REQUIRE(cueWithErrorsCount == 1);
}

TEST_CASE("CUE error isolation: unreadable CUE file records error") {
  TempScannerRoot temp{"test"};
  
  const auto cuePath = temp.path() / "unreadable.cue";
  std::ofstream(cuePath) << "FILE \"audio.flac\" WAVE\n";
  std::filesystem::permissions(cuePath, std::filesystem::perms::none);
  
  std::ofstream(temp.path() / "audio.flac");
  std::ofstream(temp.path() / "normal.mp3");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::filesystem::permissions(cuePath, std::filesystem::perms::owner_all);
  
  std::size_t cueCount = 0;
  std::size_t cueWithErrorCount = 0;
  std::size_t audioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
      if (!entry.errors.empty()) {
        ++cueWithErrorCount;
        CHECK(entry.errors[0].code == ScannerErrorCode::MetadataReadFailed);
        CHECK(!entry.errors[0].message.empty());
      }
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      ++audioCount;
    }
  }
  
  CHECK(cueCount == 1);
  CHECK(cueWithErrorCount == 1);
  CHECK(audioCount == 2);
}

TEST_CASE("CUE error isolation: all CUEs bad still returns audio files") {
  TempScannerRoot temp{"test"};
  
  std::ofstream(temp.path() / "bad1.cue") << "GARBAGE\n";
  std::ofstream(temp.path() / "bad2.cue") << "MORE GARBAGE\n";
  std::ofstream(temp.path() / "song1.mp3");
  std::ofstream(temp.path() / "song2.flac");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::size_t cueCount = 0;
  std::size_t cueWithErrorsCount = 0;
  std::size_t audioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
      if (!entry.errors.empty()) {
        ++cueWithErrorsCount;
      }
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      ++audioCount;
    }
  }
  
  CHECK(cueCount == 2);
  REQUIRE(cueWithErrorsCount == 2);
  CHECK(audioCount == 2);
}

TEST_CASE("CUE error isolation: mixed bad and good CUEs process correctly") {
  TempScannerRoot temp{"test"};
  
  std::ofstream(temp.path() / "disc1.cue") << R"(FILE "audio1.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "disc2_empty.cue") << "REM No FILE lines\n";
  std::ofstream(temp.path() / "disc3.cue") << R"(FILE "audio3.wav" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  
  std::ofstream(temp.path() / "audio1.flac");
  std::ofstream(temp.path() / "audio3.wav");
  std::ofstream(temp.path() / "standalone.mp3");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::size_t goodCueCount = 0;
  std::size_t badCueCount = 0;
  std::size_t referencedAudioCount = 0;
  std::size_t standaloneAudioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      if (entry.errors.empty()) {
        ++goodCueCount;
      } else {
        ++badCueCount;
      }
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      if (entry.path.filename() == "standalone.mp3") {
        ++standaloneAudioCount;
      } else {
        ++referencedAudioCount;
      }
    }
  }
  
  CHECK(goodCueCount == 2);
  REQUIRE(badCueCount == 1);
  CHECK(standaloneAudioCount == 1);
  CHECK(referencedAudioCount == 0);
}

TEST_CASE("CUE error isolation: CUE with I/O error during read") {
  TempScannerRoot temp{"test"};
  
  const auto cuePath = temp.path() / "ioerror.cue";
  {
    std::ofstream cue(cuePath);
    cue << "FILE \"audio.flac\" WAVE\n";
  }
  
  std::ofstream(temp.path() / "audio.flac");
  std::ofstream(temp.path() / "normal.mp3");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::size_t cueCount = 0;
  std::size_t audioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      ++audioCount;
    }
  }
  
  CHECK(cueCount == 1);
  CHECK(audioCount == 1);
}

TEST_CASE("CUE error isolation: exception during parsing does not crash") {
  TempScannerRoot temp{"test"};
  
  std::ofstream(temp.path() / "unicode.cue") << "FILE \"测试.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream(temp.path() / "normal.cue") << R"(FILE "audio.mp3" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "audio.mp3");
  std::ofstream(temp.path() / "standalone.flac");
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::size_t cueCount = 0;
  std::size_t audioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      ++audioCount;
    }
  }
  
  CHECK(cueCount == 2);
  CHECK(audioCount >= 1);
}

TEST_CASE("CUE error isolation: error details are structured") {
  TempScannerRoot temp{"test"};
  
  const auto cuePath = temp.path() / "fail.cue";
  std::ofstream(cuePath) << "BROKEN\n";
  std::filesystem::permissions(cuePath, std::filesystem::perms::none);
  
  const auto paths = discoverScannerPaths({temp.path()});
  
  std::filesystem::permissions(cuePath, std::filesystem::perms::owner_all);
  
  bool foundErrorWithDetails = false;
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet && !entry.errors.empty()) {
      const auto& error = entry.errors[0];
      CHECK(error.code == ScannerErrorCode::MetadataReadFailed);
      CHECK(!error.message.empty());
      CHECK(!error.detail.empty());
      CHECK(error.path == entry.path);
      foundErrorWithDetails = true;
    }
  }
  
  CHECK(foundErrorWithDetails);
}
