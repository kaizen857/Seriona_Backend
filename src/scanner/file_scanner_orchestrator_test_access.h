#pragma once

#include "scanner_internal_types.h"

#include <functional>
#include <vector>
#include <filesystem>

namespace seriona::scanner {

// Test-only access point for observing internal pre-allocated node sequence.
// This header should ONLY be included by tests, never by production code.
//
// Usage: Set the callback via setPreallocationObserver() before scan(),
// and it will be invoked with the indexedSongs vector after pre-allocation
// but before Worker filling.

struct PreallocationSnapshot {
  std::size_t totalNodes{0};
  std::vector<NodeType> nodeTypes;
  std::vector<std::optional<CueInfo>> cueInfos;
};

using PreallocationObserver = std::function<void(const std::vector<IndexedPublishedSong>&)>;

void setPreallocationObserver(PreallocationObserver observer);
void clearPreallocationObserver();

// Test-only CUE sheet provider: allows tests to inject controlled track data
// without requiring parseable audio files.
//
// Usage: Call setTestCueSheetProvider() with a function that returns track metadata
// for specific .cue paths. Production code (readCueSheet) checks this provider first;
// if set and returns non-empty, uses that; otherwise falls back to real TagReader.
//
// Default: nullptr (production behavior - always calls TagReader).

struct TestCueTrackData {
  std::filesystem::path audioFilePath;
  std::int64_t offset{0};
  std::int64_t duration{0};
  std::string title;
  std::string artist;
  std::string album;
  std::uint32_t trackNumber{0};
};

using TestCueSheetProvider = std::function<std::vector<TestCueTrackData>(const std::filesystem::path& cuePath)>;

void setTestCueSheetProvider(TestCueSheetProvider provider);
void clearTestCueSheetProvider();

} // namespace seriona::scanner
