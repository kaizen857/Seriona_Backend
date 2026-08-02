#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "scanner_internal_types.h"
#include "scanner_test_harness.h"
#include "file_scanner_service_internal.h"
#include "file_scanner_orchestrator_test_access.h"

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

using namespace seriona::scanner;
namespace fs = std::filesystem;

namespace {

class FakeMetadataReader final : public TagMetadataReader {
public:
  void put(fs::path path, RawTagMetadata metadata) { 
    metadataByPath_[std::move(path)] = std::move(metadata); 
  }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + request.path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;

  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

private:
  std::map<fs::path, RawTagMetadata> metadataByPath_;
};

struct ScanEvents {
  std::atomic<bool> completed{false};
  std::mutex mutex;
  std::condition_variable cv;
  
  void onEvent(const ScannerEvent& event) {
    if (event.type == ScannerEventType::ScanCompleted) {
      {
        std::lock_guard lock{mutex};
        completed.store(true);
      }
      cv.notify_all();
    }
  }
  
  bool waitForScanCompletion(std::chrono::seconds timeout) {
    std::unique_lock lock{mutex};
    return cv.wait_for(lock, timeout, [this] { return completed.load(); });
  }
};

RawTagMetadata makeMetadata(const std::string& title) {
  RawTagMetadata meta;
  meta.title = title;
  meta.artist = "Test Artist";
  meta.album = "Test Album";
  return meta;
}

[[nodiscard]] std::vector<PlaylistNode> songNodesWithFilePath(const PlaylistTreeSnapshot& snapshot,
                                                              const fs::path& filePath) {
  std::vector<PlaylistNode> matches;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value() && node.song->filePath == filePath) {
      matches.push_back(node);
    }
  }
  return matches;
}

} // namespace

TEST_CASE("CUE nodes: multi-track CUE creates container + tracks with complete structure") {
  test::TempScannerRoot temp{"cue-nodes-multitrack"};
  
  const auto cueFile = temp.path() / "album.cue";
  const auto audioFile = temp.path() / "album.flac";
  
  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{audioFile} << "fake audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  // Test seam: inject 3 tracks with distinct metadata
  setTestCueSheetProvider([&audioFile](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {
        {
          .audioFilePath = audioFile,
          .offset = 0,
          .duration = 180000000,  // 3 minutes in microseconds
          .title = "Opening Track",
          .artist = "Artist One",
          .album = "Test Album",
          .trackNumber = 1
        },
        {
          .audioFilePath = audioFile,
          .offset = 180000000,
          .duration = 240000000,  // 4 minutes
          .title = "Middle Track",
          .artist = "Artist Two",
          .album = "Test Album",
          .trackNumber = 2
        },
        {
          .audioFilePath = audioFile,
          .offset = 420000000,
          .duration = 210000000,  // 3.5 minutes
          .title = "Closing Track",
          .artist = "Artist Three",
          .album = "Test Album",
          .trackNumber = 3
        }
      };
    }
    return {};
  });
  
  std::size_t observedContainerCount = 0;
  std::size_t observedTrackCount = 0;
  std::vector<NodeType> nodeSequence;
  std::vector<ScanItemOrigin> originSequence;
  std::vector<std::optional<std::string>> locationIdSequence;
  std::vector<cache::CachedSong> trackSongs;
  std::vector<CueInfo> trackCueInfos;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    for (const auto& node : nodes) {
      nodeSequence.push_back(node.nodeType);
      originSequence.push_back(node.origin);
      locationIdSequence.push_back(node.locationId);
      
      if (node.nodeType == NodeType::CueContainer) {
        ++observedContainerCount;
        CHECK(node.isVirtualFolder == true);
        CHECK(node.origin == ScanItemOrigin::VirtualContainer);
        CHECK_FALSE(node.locationId.has_value());
      } else if (node.nodeType == NodeType::CueTrack) {
        ++observedTrackCount;
        REQUIRE(node.cueInfo.has_value());
        trackCueInfos.push_back(*node.cueInfo);
        trackSongs.push_back(node.song);
        CHECK(node.filled.load() == true);
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  clearPreallocationObserver();
  
  // Verify structure: 1 container + 3 tracks = 4 nodes
  CHECK(observedContainerCount == 1);
  CHECK(observedTrackCount == 3);
  REQUIRE(nodeSequence.size() == 4);
  CHECK(nodeSequence[0] == NodeType::CueContainer);
  CHECK(nodeSequence[1] == NodeType::CueTrack);
  CHECK(nodeSequence[2] == NodeType::CueTrack);
  CHECK(nodeSequence[3] == NodeType::CueTrack);
  REQUIRE(originSequence.size() == 4);
  CHECK(originSequence[0] == ScanItemOrigin::VirtualContainer);
  CHECK(originSequence[1] == ScanItemOrigin::ScannedFull);
  CHECK(originSequence[2] == ScanItemOrigin::ScannedFull);
  CHECK(originSequence[3] == ScanItemOrigin::ScannedFull);
  REQUIRE(locationIdSequence.size() == 4);
  CHECK_FALSE(locationIdSequence[0].has_value());
  
  // Verify CueInfo population
  REQUIRE(trackCueInfos.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(trackCueInfos[i].cueFilePath == cueFile);
    CHECK(trackCueInfos[i].audioFilePath == audioFile);
    CHECK(trackCueInfos[i].trackIndex == i);
    CHECK(trackCueInfos[i].offset >= std::chrono::microseconds{0});
    CHECK(trackCueInfos[i].duration > std::chrono::microseconds{0});
  }
  
  // Verify metadata population
  REQUIRE(trackSongs.size() == 3);
  CHECK(trackSongs[0].metadata.title == "Opening Track");
  CHECK(trackSongs[0].metadata.artist == "Artist One");
  CHECK(trackSongs[0].metadata.sourceFilePath == audioFile);
  CHECK(trackSongs[0].metadata.filePath == cueFile);
  CHECK(trackSongs[0].metadata.offset == std::chrono::milliseconds{0});
  CHECK(trackSongs[0].metadata.duration == std::chrono::milliseconds{180000});
  
  CHECK(trackSongs[1].metadata.title == "Middle Track");
  CHECK(trackSongs[1].metadata.artist == "Artist Two");
  CHECK(trackSongs[1].metadata.offset == std::chrono::milliseconds{180000});
  CHECK(trackSongs[1].metadata.duration == std::chrono::milliseconds{240000});
  
  CHECK(trackSongs[2].metadata.title == "Closing Track");
  CHECK(trackSongs[2].metadata.artist == "Artist Three");
  CHECK(trackSongs[2].metadata.offset == std::chrono::milliseconds{420000});
  CHECK(trackSongs[2].metadata.duration == std::chrono::milliseconds{210000});
}

TEST_CASE("CUE nodes: offset and duration are correctly applied from CUE semantics") {
  test::TempScannerRoot temp{"cue-nodes-timing"};
  
  const auto cueFile = temp.path() / "precise.cue";
  const auto audioFile = temp.path() / "precise.flac";
  
  std::ofstream{cueFile} << "FILE \"precise.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{audioFile} << "fake audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  // Test precise offset/duration handling
  setTestCueSheetProvider([&audioFile](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "precise.cue") {
      return {
        {
          .audioFilePath = audioFile,
          .offset = 1234567,      // Odd microseconds
          .duration = 98765432,
          .title = "Track with precise timing",
          .artist = "Precision Artist",
          .album = "Timing Test",
          .trackNumber = 1
        }
      };
    }
    return {};
  });
  
  std::vector<cache::CachedSong> observedTracks;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    for (const auto& node : nodes) {
      if (node.nodeType == NodeType::CueTrack) {
        observedTracks.push_back(node.song);
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  clearPreallocationObserver();
  
  REQUIRE(observedTracks.size() == 1);
  
  // Verify microseconds → milliseconds conversion
  CHECK(observedTracks[0].metadata.offset == std::chrono::milliseconds{1234});  // 1234567 µs → 1234 ms
  CHECK(observedTracks[0].metadata.duration == std::chrono::milliseconds{98765});  // 98765432 µs → 98765 ms
}

TEST_CASE("CUE nodes: 0-track CUE creates container only, no tracks") {
  test::TempScannerRoot temp{"cue-nodes-empty"};
  
  const auto cueFile = temp.path() / "empty.cue";
  const auto audioFile = temp.path() / "empty.flac";
  
  std::ofstream{cueFile} << "FILE \"empty.flac\" WAVE\n";
  std::ofstream{audioFile} << "fake audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  // Return empty track list
  setTestCueSheetProvider([](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "empty.cue") {
      return {};  // 0 tracks
    }
    return {};
  });
  
  std::size_t observedContainerCount = 0;
  std::size_t observedTrackCount = 0;
  
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    for (const auto& node : nodes) {
      if (node.nodeType == NodeType::CueContainer) {
        ++observedContainerCount;
      } else if (node.nodeType == NodeType::CueTrack) {
        ++observedTrackCount;
      }
    }
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) {
    events.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  clearPreallocationObserver();
  
  CHECK(observedContainerCount == 1);
  CHECK(observedTrackCount == 0);
}

TEST_CASE("CUE nodes: referenced audio file not found triggers error recording without crash") {
  test::TempScannerRoot temp{"cue-nodes-missing-audio"};
  
  const auto cueFile = temp.path() / "broken.cue";
  
  // CUE file exists but references non-existent audio
  std::ofstream{cueFile} << "FILE \"nonexistent.flac\" WAVE\n  TRACK 01 AUDIO\n";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  // Test seam throws to simulate TagReader failure when audio file doesn't exist
  setTestCueSheetProvider([](const fs::path&) -> std::vector<TestCueTrackData> {
    throw std::runtime_error("Referenced audio file not found");
  });
  
  std::size_t errorCount = 0;
  std::vector<std::string> errorMessages;
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&](const ScannerEvent& event) {
    events.onEvent(event);
    if (event.type == ScannerEventType::ScanError) {
      if (const auto* error = std::get_if<ScannerError>(&event.payload)) {
        ++errorCount;
        errorMessages.push_back(error->message);
      }
    }
  });
  
  // Should not crash
  CHECK_NOTHROW(service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full));
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  
  // Verify error was recorded
  CHECK(errorCount >= 1);
  bool foundCueError = false;
  for (const auto& msg : errorMessages) {
    if (msg.find("CUE") != std::string::npos || msg.find("cue") != std::string::npos) {
      foundCueError = true;
      break;
    }
  }
  CHECK(foundCueError);
}

TEST_CASE("CUE nodes: metadata read failure via seam records error without crash") {
  test::TempScannerRoot temp{"cue-nodes-metadata-fail"};
  
  const auto cueFile = temp.path() / "corrupt.cue";
  const auto audioFile = temp.path() / "corrupt.flac";
  
  std::ofstream{cueFile} << "FILE \"corrupt.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{audioFile} << "fake corrupted audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  // Simulate metadata extraction failure
  setTestCueSheetProvider([](const fs::path&) -> std::vector<TestCueTrackData> {
    throw std::runtime_error("Failed to parse CUE metadata: corrupted file structure");
  });
  
  std::size_t errorCount = 0;
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events;
  service->setEventSink([&](const ScannerEvent& event) {
    events.onEvent(event);
    if (event.type == ScannerEventType::ScanError) {
      ++errorCount;
    }
  });
  
  CHECK_NOTHROW(service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full));
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  
  CHECK(errorCount >= 1);
}

TEST_CASE("CUE nodes: logicalTrackId stability across scans") {
  test::TempScannerRoot temp{"cue-nodes-trackid"};
  
  const auto cueFile = temp.path() / "stable.cue";
  const auto audioFile = temp.path() / "stable.flac";
  
  std::ofstream{cueFile} << "FILE \"stable.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{audioFile} << "fake audio";
  
  auto reader = std::make_shared<FakeMetadataReader>();
  
  setTestCueSheetProvider([&audioFile](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "stable.cue") {
      return {
        {
          .audioFilePath = audioFile,
          .offset = 0,
          .duration = 180000000,
          .title = "Track 1",
          .artist = "Artist",
          .album = "Album",
          .trackNumber = 1
        }
      };
    }
    return {};
  });
  
  std::vector<std::string> firstScanTrackIds;
  std::vector<std::string> secondScanTrackIds;
  
  auto captureTrackIds = [](const std::vector<IndexedPublishedSong>& nodes, std::vector<std::string>& output) {
    for (const auto& node : nodes) {
      if (node.nodeType == NodeType::CueTrack) {
        output.push_back(node.song.metadata.logicalTrackId);
        output.push_back(node.song.metadata.trackId);
      }
    }
  };
  
  // First scan
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    captureTrackIds(nodes, firstScanTrackIds);
  });
  
  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });
  
  ScanEvents events1;
  service->setEventSink([&events1](const ScannerEvent& event) {
    events1.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events1.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearPreallocationObserver();
  
  // Second scan
  setPreallocationObserver([&](const std::vector<IndexedPublishedSong>& nodes) {
    captureTrackIds(nodes, secondScanTrackIds);
  });
  
  ScanEvents events2;
  service->setEventSink([&events2](const ScannerEvent& event) {
    events2.onEvent(event);
  });
  
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events2.waitForScanCompletion(std::chrono::seconds{5}));
  
  clearTestCueSheetProvider();
  clearPreallocationObserver();
  
  // Verify trackIds are stable across scans
  REQUIRE(firstScanTrackIds.size() == 2);  // logicalTrackId + trackId
  REQUIRE(secondScanTrackIds.size() == 2);
  
  CHECK(firstScanTrackIds[0] == secondScanTrackIds[0]);  // logicalTrackId matches
  CHECK(firstScanTrackIds[1] == secondScanTrackIds[1]);  // trackId matches
  CHECK(firstScanTrackIds[0] == firstScanTrackIds[1]);   // logicalTrackId == trackId
  
  // Verify format: <cue-path>#track<N>
  CHECK(firstScanTrackIds[0].find(cueFile.generic_string()) != std::string::npos);
  CHECK(firstScanTrackIds[0].find("#track") != std::string::npos);
}

TEST_CASE("CUE nodes: referenced source audio is hidden from final playlist tree") {
  test::TempScannerRoot temp{"cue-nodes-hide-source-audio"};

  const auto cueFile = temp.path() / "album.cue";
  const auto referencedAudio = temp.path() / "album.flac";
  const auto standaloneAudio = temp.path() / "bonus.flac";

  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{referencedAudio} << "fake referenced audio";
  std::ofstream{standaloneAudio} << "fake standalone audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, makeMetadata("Bare album file"));
  reader->put(standaloneAudio, makeMetadata("Bonus Track"));

  setTestCueSheetProvider([&referencedAudio](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "album.cue") {
      return {
        {
          .audioFilePath = referencedAudio,
          .offset = 0,
          .duration = 180000000,
          .title = "Cue Track 1",
          .artist = "Cue Artist",
          .album = "Cue Album",
          .trackNumber = 1
        }
      };
    }
    return {};
  });

  auto service = makeFileScannerService(FileScannerServiceDependencies{
    .metadataReader = reader,
    .databasePath = temp.path() / "cache.db"
  });

  PlaylistTreeSnapshot completedSnapshot;
  ScanEvents events;
  service->setEventSink([&](const ScannerEvent& event) {
    events.onEvent(event);
    if (event.type == ScannerEventType::ScanCompleted) {
      if (const auto* snapshot = std::get_if<PlaylistTreeSnapshot>(&event.payload)) {
        completedSnapshot = *snapshot;
      }
    }
  });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));

  clearTestCueSheetProvider();

  CHECK(songNodesWithFilePath(completedSnapshot, referencedAudio).empty());
  REQUIRE(songNodesWithFilePath(completedSnapshot, standaloneAudio).size() == 1);
  REQUIRE(songNodesWithFilePath(completedSnapshot, cueFile).size() == 1);
  CHECK(songNodesWithFilePath(completedSnapshot, cueFile)[0].song->sourceFilePath == referencedAudio);
  CHECK(songNodesWithFilePath(completedSnapshot, standaloneAudio)[0].song->title == "Bonus Track");
}
