#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "seriona/scanner/path_utils.h"
#include "scanner_test_harness.h"

#include <filesystem>
#include <fstream>

using namespace seriona::scanner;
using namespace seriona::scanner::test;

TEST_CASE("Two-pass integration: CUE + referenced audio + standalone audio") {
  TempScannerRoot temp{"two_pass_integration_mixed"};
  
  // Create a directory with:
  // - 1 CUE file referencing album.flac
  // - 1 referenced audio file (album.flac) - should be hidden
  // - 2 standalone audio files - should be visible
  std::ofstream(temp.path() / "album.cue") << R"(TITLE "Album Title"
PERFORMER "Artist Name"
FILE "album.flac" WAVE
  TRACK 01 AUDIO
    TITLE "Track 1"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Track 2"
    INDEX 01 03:45:00
)";
  std::ofstream(temp.path() / "album.flac");
  std::ofstream(temp.path() / "standalone1.mp3");
  std::ofstream(temp.path() / "standalone2.wav");
  
  const auto paths = discoverScannerPaths({temp.path(), false});
  
  std::size_t cueCount = 0;
  std::size_t standaloneAudioCount = 0;
  std::size_t referencedAudioCount = 0;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
      CHECK(entry.path.filename() == "album.cue");
      CHECK(entry.errors.empty());
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      const auto filename = entry.path.filename();
      if (filename == "standalone1.mp3" || filename == "standalone2.wav") {
        ++standaloneAudioCount;
      } else if (filename == "album.flac") {
        ++referencedAudioCount;
      }
    }
  }
  
  CHECK(cueCount == 1);
  CHECK(standaloneAudioCount == 2);
  CHECK(referencedAudioCount == 0); // album.flac should be hidden
}

TEST_CASE("Two-pass integration: nested directories with recursive discovery") {
  TempScannerRoot temp{"two_pass_integration_nested"};
  
  // Create nested directory structure:
  // root/
  //   subdir1/
  //     disc1.cue -> audio1.flac (referenced, hidden)
  //     audio1.flac (hidden)
  //     standalone.mp3 (visible)
  //   subdir2/
  //     disc2.cue -> audio2.wav (referenced, hidden)
  //     audio2.wav (hidden)
  //     other.flac (visible)
  
  std::filesystem::create_directories(temp.path() / "subdir1");
  std::filesystem::create_directories(temp.path() / "subdir2");
  
  std::ofstream(temp.path() / "subdir1" / "disc1.cue") << R"(FILE "audio1.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "subdir1" / "audio1.flac");
  std::ofstream(temp.path() / "subdir1" / "standalone.mp3");
  
  std::ofstream(temp.path() / "subdir2" / "disc2.cue") << R"(FILE "audio2.wav" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "subdir2" / "audio2.wav");
  std::ofstream(temp.path() / "subdir2" / "other.flac");
  
  const auto paths = discoverScannerPaths({temp.path(), true}); // recursive = true
  
  std::size_t cueCount = 0;
  std::size_t visibleAudioCount = 0;
  bool hasReferencedAudio1 = false;
  bool hasReferencedAudio2 = false;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      const auto filename = entry.path.filename();
      if (filename == "standalone.mp3" || filename == "other.flac") {
        ++visibleAudioCount;
      } else if (filename == "audio1.flac") {
        hasReferencedAudio1 = true;
      } else if (filename == "audio2.wav") {
        hasReferencedAudio2 = true;
      }
    }
  }
  
  CHECK(cueCount == 2);
  CHECK(visibleAudioCount == 2);
  CHECK_FALSE(hasReferencedAudio1);
  CHECK_FALSE(hasReferencedAudio2);
}

TEST_CASE("Two-pass integration: error isolation with mixed good and bad CUEs") {
  TempScannerRoot temp{"two_pass_integration_error"};
  
  // Create mixed scenario:
  // - 1 good CUE referencing audio1.flac
  // - 1 bad CUE (empty, no FILE lines)
  // - 1 bad CUE (malformed content)
  // - audio1.flac (referenced by good CUE, should be hidden)
  // - audio2.mp3 (standalone, should be visible)
  
  std::ofstream(temp.path() / "good.cue") << R"(FILE "audio1.flac" WAVE
  TRACK 01 AUDIO
    TITLE "Track 1"
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "empty.cue") << "";
  std::ofstream(temp.path() / "malformed.cue") << "INVALID CONTENT WITHOUT FILE DIRECTIVE\n";
  std::ofstream(temp.path() / "audio1.flac");
  std::ofstream(temp.path() / "audio2.mp3");
  
  const auto paths = discoverScannerPaths({temp.path(), false});
  
  std::size_t totalCueCount = 0;
  std::size_t goodCueCount = 0;
  std::size_t badCueCount = 0;
  std::size_t visibleAudioCount = 0;
  bool hasReferencedAudio = false;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++totalCueCount;
      const auto filename = entry.path.filename();
      
      if (filename == "good.cue") {
        ++goodCueCount;
        CHECK(entry.errors.empty());
      } else if (filename == "empty.cue" || filename == "malformed.cue") {
        ++badCueCount;
        REQUIRE_FALSE(entry.errors.empty());
        CHECK(entry.errors[0].code == ScannerErrorCode::MetadataReadFailed);
        CHECK(entry.errors[0].message == "CUE sheet contains no valid FILE lines");
      }
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      const auto filename = entry.path.filename();
      if (filename == "audio2.mp3") {
        ++visibleAudioCount;
      } else if (filename == "audio1.flac") {
        hasReferencedAudio = true;
      }
    }
  }
  
  CHECK(totalCueCount == 3);
  CHECK(goodCueCount == 1);
  CHECK(badCueCount == 2);
  CHECK(visibleAudioCount == 1);
  CHECK_FALSE(hasReferencedAudio); // audio1.flac should be hidden by good.cue
}

TEST_CASE("Two-pass integration: multiple CUEs referencing same audio file") {
  TempScannerRoot temp{"two_pass_integration_shared"};
  
  // Create scenario where multiple CUE files reference the same audio:
  // - disc1.cue -> shared.flac
  // - disc2.cue -> shared.flac
  // - disc3.cue -> shared.flac
  // - shared.flac (referenced by all 3, should be hidden)
  // - other.mp3 (standalone, should be visible)
  
  std::ofstream(temp.path() / "disc1.cue") << R"(TITLE "Disc 1"
FILE "shared.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "disc2.cue") << R"(TITLE "Disc 2"
FILE "shared.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "disc3.cue") << R"(TITLE "Disc 3"
FILE "shared.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "shared.flac");
  std::ofstream(temp.path() / "other.mp3");
  
  const auto paths = discoverScannerPaths({temp.path(), false});
  
  std::size_t cueCount = 0;
  bool hasSharedAudio = false;
  bool hasOtherAudio = false;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      if (entry.path.filename() == "shared.flac") {
        hasSharedAudio = true;
      } else if (entry.path.filename() == "other.mp3") {
        hasOtherAudio = true;
      }
    }
  }
  
  CHECK(cueCount == 3);
  CHECK_FALSE(hasSharedAudio); // Should be hidden due to CUE references
  CHECK(hasOtherAudio);
}

TEST_CASE("Two-pass integration: CUE referencing non-existent audio preserves other files") {
  TempScannerRoot temp{"two_pass_integration_missing_ref"};
  
  // CUE references a non-existent file, but other files should still be discovered correctly
  std::ofstream(temp.path() / "album.cue") << R"(FILE "nonexistent.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "track1.mp3");
  std::ofstream(temp.path() / "track2.wav");
  
  const auto paths = discoverScannerPaths({temp.path(), false});
  
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
  CHECK(audioCount == 2); // Both audio files should be visible
}

TEST_CASE("Two-pass integration: relative path resolution in nested CUE") {
  TempScannerRoot temp{"two_pass_integration_relative"};
  
  // Test that CUE files correctly resolve relative audio paths
  std::filesystem::create_directories(temp.path() / "album");
  
  std::ofstream(temp.path() / "album" / "disc.cue") << R"(FILE "audio.flac" WAVE
  TRACK 01 AUDIO
    INDEX 01 00:00:00
)";
  std::ofstream(temp.path() / "album" / "audio.flac");
  std::ofstream(temp.path() / "album" / "bonus.mp3");
  
  const auto paths = discoverScannerPaths({temp.path(), true}); // recursive
  
  std::size_t cueCount = 0;
  bool hasReferencedAudio = false;
  bool hasBonusAudio = false;
  
  for (const auto& entry : paths) {
    if (entry.kind == PathEntryKind::CueSheet) {
      ++cueCount;
    } else if (entry.kind == PathEntryKind::AudioCandidate) {
      const auto filename = entry.path.filename();
      if (filename == "audio.flac") {
        hasReferencedAudio = true;
      } else if (filename == "bonus.mp3") {
        hasBonusAudio = true;
      }
    }
  }
  
  CHECK(cueCount == 1);
  CHECK_FALSE(hasReferencedAudio); // Should be hidden
  CHECK(hasBonusAudio);
}
