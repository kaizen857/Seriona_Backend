#include "scanner_test_harness.h"

#include "../../inc/seriona/scanner/path_utils.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace seriona::scanner {
namespace {

TEST_CASE("discoverScannerPaths skips audio files referenced by CUE sheet") {
  test::TempScannerRoot root{std::string{"two_pass_cue_reference"}};
  const auto audioPath = root.path() / std::filesystem::path{"album.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto otherAudioPath = root.path() / std::filesystem::path{"standalone.mp3"};

  // Create audio files
  std::ofstream audioFile{audioPath};
  audioFile.close();
  std::ofstream otherAudioFile{otherAudioPath};
  otherAudioFile.close();

  // Create valid CUE referencing album.flac
  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Test Album\"\n";
  cueFile << "PERFORMER \"Test Artist\"\n";
  cueFile << "FILE \"album.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    TITLE \"Track One\"\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  ScannerRoot scanRoot{.path = root.path(), .recursive = false};
  PathClassificationConfig config{.allowedExtensions = {".flac", ".mp3"}};

  const auto results = discoverScannerPaths(scanRoot, config);

  // Verify: should have DirectoryRoot, CueSheet (album.cue), and standalone.mp3
  // Should NOT have album.flac (referenced by CUE)
  bool hasCueSheet = false;
  bool hasStandalone = false;
  bool hasReferencedAudio = false;

  for (const auto& entry : results) {
    if (entry.kind == PathEntryKind::CueSheet) {
      hasCueSheet = true;
    }
    if (entry.path.filename() == "standalone.mp3" && entry.kind == PathEntryKind::AudioCandidate) {
      hasStandalone = true;
    }
    if (entry.path.filename() == "album.flac" && entry.kind == PathEntryKind::AudioCandidate) {
      hasReferencedAudio = true;
    }
  }

  CHECK(hasCueSheet);
  CHECK(hasStandalone);
  CHECK_FALSE(hasReferencedAudio);
}

TEST_CASE("discoverScannerPaths handles multiple CUE files referencing same audio") {
  test::TempScannerRoot root{std::string{"two_pass_multi_cue"}};
  const auto audioPath = root.path() / std::filesystem::path{"shared.flac"};
  const auto cue1Path = root.path() / std::filesystem::path{"disc1.cue"};
  const auto cue2Path = root.path() / std::filesystem::path{"disc2.cue"};

  // Create audio file
  std::ofstream audioFile{audioPath};
  audioFile.close();

  // Create two CUE files both referencing shared.flac
  std::ofstream cue1File{cue1Path};
  cue1File << "TITLE \"Disc 1\"\n";
  cue1File << "FILE \"shared.flac\" FLAC\n";
  cue1File << "  TRACK 01 AUDIO\n";
  cue1File << "    INDEX 01 00:00:00\n";
  cue1File.close();

  std::ofstream cue2File{cue2Path};
  cue2File << "TITLE \"Disc 2\"\n";
  cue2File << "FILE \"shared.flac\" FLAC\n";
  cue2File << "  TRACK 01 AUDIO\n";
  cue2File << "    INDEX 01 00:00:00\n";
  cue2File.close();

  ScannerRoot scanRoot{.path = root.path(), .recursive = false};
  PathClassificationConfig config{.allowedExtensions = {".flac"}};

  const auto results = discoverScannerPaths(scanRoot, config);

  // Verify: should have 2 CUE sheets, but NOT the shared.flac
  int cueCount = 0;
  bool hasSharedAudio = false;

  for (const auto& entry : results) {
    if (entry.kind == PathEntryKind::CueSheet) {
      cueCount++;
    }
    if (entry.path.filename() == "shared.flac" && entry.kind == PathEntryKind::AudioCandidate) {
      hasSharedAudio = true;
    }
  }

  CHECK(cueCount == 2);
  CHECK_FALSE(hasSharedAudio);
}

TEST_CASE("discoverScannerPaths includes audio files when no CUE references them") {
  test::TempScannerRoot root{std::string{"two_pass_no_cue"}};
  const auto audio1Path = root.path() / std::filesystem::path{"track1.flac"};
  const auto audio2Path = root.path() / std::filesystem::path{"track2.flac"};

  // Create audio files
  std::ofstream audio1File{audio1Path};
  audio1File.close();
  std::ofstream audio2File{audio2Path};
  audio2File.close();

  ScannerRoot scanRoot{.path = root.path(), .recursive = false};
  PathClassificationConfig config{.allowedExtensions = {".flac"}};

  const auto results = discoverScannerPaths(scanRoot, config);

  // Verify: should have both audio files
  int audioCount = 0;
  for (const auto& entry : results) {
    if (entry.kind == PathEntryKind::AudioCandidate) {
      audioCount++;
    }
  }

  CHECK(audioCount == 2);
}

TEST_CASE("discoverScannerPaths handles malformed CUE gracefully") {
  test::TempScannerRoot root{std::string{"two_pass_bad_cue"}};
  const auto audioPath = root.path() / std::filesystem::path{"album.flac"};
  const auto cuePath = root.path() / std::filesystem::path{"broken.cue"};

  // Create audio file
  std::ofstream audioFile{audioPath};
  audioFile.close();

  // Create malformed CUE
  std::ofstream cueFile{cuePath};
  cueFile << "GARBAGE DATA\n";
  cueFile.close();

  ScannerRoot scanRoot{.path = root.path(), .recursive = false};
  PathClassificationConfig config{.allowedExtensions = {".flac"}};

  const auto results = discoverScannerPaths(scanRoot, config);

  // Verify: malformed CUE doesn't crash, audio file is still included
  bool hasCueSheet = false;
  bool hasAudio = false;

  for (const auto& entry : results) {
    if (entry.kind == PathEntryKind::CueSheet) {
      hasCueSheet = true;
    }
    if (entry.path.filename() == "album.flac" && entry.kind == PathEntryKind::AudioCandidate) {
      hasAudio = true;
    }
  }

  CHECK(hasCueSheet);
  CHECK(hasAudio); // Audio not excluded since CUE parse failed
}

TEST_CASE("discoverScannerPaths handles CUE with missing audio file") {
  test::TempScannerRoot root{std::string{"two_pass_missing_audio"}};
  const auto cuePath = root.path() / std::filesystem::path{"album.cue"};
  const auto otherAudioPath = root.path() / std::filesystem::path{"other.flac"};

  // Create only the "other" audio file, not the one referenced by CUE
  std::ofstream otherAudioFile{otherAudioPath};
  otherAudioFile.close();

  // Create CUE referencing non-existent file
  std::ofstream cueFile{cuePath};
  cueFile << "TITLE \"Test Album\"\n";
  cueFile << "FILE \"nonexistent.flac\" FLAC\n";
  cueFile << "  TRACK 01 AUDIO\n";
  cueFile << "    INDEX 01 00:00:00\n";
  cueFile.close();

  ScannerRoot scanRoot{.path = root.path(), .recursive = false};
  PathClassificationConfig config{.allowedExtensions = {".flac"}};

  const auto results = discoverScannerPaths(scanRoot, config);

  // Verify: CUE is present, other.flac is present (not referenced)
  bool hasCueSheet = false;
  bool hasOtherAudio = false;

  for (const auto& entry : results) {
    if (entry.kind == PathEntryKind::CueSheet) {
      hasCueSheet = true;
    }
    if (entry.path.filename() == "other.flac" && entry.kind == PathEntryKind::AudioCandidate) {
      hasOtherAudio = true;
    }
  }

  CHECK(hasCueSheet);
  CHECK(hasOtherAudio);
}

}
}
