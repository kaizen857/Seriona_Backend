#include <doctest/doctest.h>

#include "file_scanner_orchestrator_test_access.h"
#include "file_scanner_service_internal.h"
#include "scanner_test_harness.h"

#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace seriona::scanner;
namespace fs = std::filesystem;

namespace {

class FakeMetadataReader final : public TagMetadataReader {
public:
  void put(fs::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const fs::path& path, const fs::path&) override {
    const auto iterator = metadataByPath_.find(path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = path;
    return metadata;
  }

private:
  std::map<fs::path, RawTagMetadata> metadataByPath_;
};

class TestCueProviderGuard {
public:
  explicit TestCueProviderGuard(TestCueSheetProvider provider) { setTestCueSheetProvider(std::move(provider)); }
  ~TestCueProviderGuard() { clearTestCueSheetProvider(); }

  TestCueProviderGuard(const TestCueProviderGuard&) = delete;
  TestCueProviderGuard& operator=(const TestCueProviderGuard&) = delete;
};

struct ScanEvents {
  std::atomic<bool> completed{false};
  std::mutex mutex;
  std::condition_variable cv;
  PlaylistTreeSnapshot snapshot{};

  void onEvent(const ScannerEvent& event) {
    if (event.type != ScannerEventType::ScanCompleted) {
      return;
    }
    if (const auto* completedSnapshot = std::get_if<PlaylistTreeSnapshot>(&event.payload)) {
      snapshot = *completedSnapshot;
    }
    {
      std::lock_guard lock{mutex};
      completed.store(true);
    }
    cv.notify_all();
  }

  [[nodiscard]] bool waitForScanCompletion(std::chrono::seconds timeout) {
    std::unique_lock lock{mutex};
    return cv.wait_for(lock, timeout, [this] { return completed.load(); });
  }
};

[[nodiscard]] RawTagMetadata makeMetadata(std::string title) {
  RawTagMetadata metadata{};
  metadata.title = std::move(title);
  metadata.artist = "Test Artist";
  metadata.album = "Test Album";
  metadata.duration = std::chrono::milliseconds{180000};
  metadata.sampleRate = 48000;
  metadata.bitDepth = 24;
  metadata.channels = 2;
  return metadata;
}

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] PlaylistTreeSnapshot scanRoot(const fs::path& root, std::shared_ptr<TagMetadataReader> reader) {
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                                        .watcherFactory = nullptr,
                                                                        .databasePath = root / "cache.db",
                                                                        .coverExportDir = root / "covers"});
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) { events.onEvent(event); });

  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  return events.snapshot;
}

[[nodiscard]] const PlaylistNode& requireNode(const PlaylistTreeSnapshot& snapshot, const std::string_view nodeId) {
  const auto iterator = std::ranges::find(snapshot.nodes, nodeId, &PlaylistNode::nodeId);
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] const PlaylistNode& requireSongWithTitle(const PlaylistTreeSnapshot& snapshot, const std::string_view title) {
  const auto iterator = std::ranges::find_if(snapshot.nodes, [&title](const PlaylistNode& node) {
    return node.song.has_value() && node.song->title == title;
  });
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] std::vector<const PlaylistNode*> songNodesWithFilePath(const PlaylistTreeSnapshot& snapshot,
                                                                     const fs::path& filePath) {
  std::vector<const PlaylistNode*> matches;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value() && node.song->filePath == filePath) {
      matches.push_back(&node);
    }
  }
  return matches;
}

}

TEST_CASE("scanner complete e2e: full scan publishes nested cue tree and hides referenced audio") {
  test::TempScannerRoot temp{"scanner-complete-e2e"};
  const auto rootCue = temp.path() / "album.cue";
  const auto referencedAudio = temp.path() / "album.flac";
  const auto standaloneAudio = temp.path() / "bonus.flac";
  const auto nestedDir = temp.path() / "live" / "disc1";
  fs::create_directories(nestedDir);
  const auto nestedAudio = nestedDir / "encore.flac";

  writeText(rootCue, "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n  TRACK 02 AUDIO\n");
  writeText(referencedAudio, "fake referenced audio");
  writeText(standaloneAudio, "fake standalone audio");
  writeText(nestedAudio, "fake nested audio");

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, makeMetadata("Bare Album File"));
  reader->put(standaloneAudio, makeMetadata("Bonus Track"));
  reader->put(nestedAudio, makeMetadata("Nested Encore"));

  const TestCueProviderGuard cueProvider{[&referencedAudio](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 180000000,
             .title = "Cue Opening",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 1},
            {.audioFilePath = referencedAudio,
             .offset = 180000000,
             .duration = 210000000,
             .title = "Cue Finale",
             .artist = "Cue Artist",
             .album = "Cue Album",
             .trackNumber = 2}};
  }};

  const auto snapshot = scanRoot(temp.path(), reader);

  REQUIRE(snapshot.rootNodeId.has_value());
  const auto& root = requireNode(snapshot, *snapshot.rootNodeId);
  const auto& cueContainer = requireNode(snapshot, "dir:album.cue");
  const auto& firstTrack = requireSongWithTitle(snapshot, "Cue Opening");
  const auto& secondTrack = requireSongWithTitle(snapshot, "Cue Finale");
  const auto& bonusTrack = requireSongWithTitle(snapshot, "Bonus Track");
  const auto& liveDir = requireNode(snapshot, "dir:live");
  const auto& discDir = requireNode(snapshot, "dir:live/disc1");
  const auto& nestedTrack = requireSongWithTitle(snapshot, "Nested Encore");

  CHECK(cueContainer.kind == PlaylistNodeKind::Directory);
  CHECK(cueContainer.displayName == "album.cue");
  CHECK_FALSE(cueContainer.song.has_value());
  CHECK(cueContainer.parentNodeId == root.nodeId);
  CHECK(std::ranges::find(root.childNodeIds, cueContainer.nodeId) != root.childNodeIds.end());

  REQUIRE(firstTrack.parentNodeId.has_value());
  REQUIRE(secondTrack.parentNodeId.has_value());
  CHECK(*firstTrack.parentNodeId == cueContainer.nodeId);
  CHECK(*secondTrack.parentNodeId == cueContainer.nodeId);
  CHECK(std::ranges::find(cueContainer.childNodeIds, firstTrack.nodeId) != cueContainer.childNodeIds.end());
  CHECK(std::ranges::find(cueContainer.childNodeIds, secondTrack.nodeId) != cueContainer.childNodeIds.end());
  CHECK(firstTrack.song->sourceFilePath == referencedAudio);
  CHECK(secondTrack.song->sourceFilePath == referencedAudio);
  CHECK(firstTrack.song->filePath == rootCue);
  CHECK(secondTrack.song->filePath == rootCue);
  CHECK(firstTrack.song->offset == std::chrono::milliseconds{0});
  CHECK(secondTrack.song->offset == std::chrono::milliseconds{180000});

  CHECK(songNodesWithFilePath(snapshot, referencedAudio).empty());
  REQUIRE(songNodesWithFilePath(snapshot, rootCue).size() == 2U);
  REQUIRE(songNodesWithFilePath(snapshot, standaloneAudio).size() == 1U);
  CHECK(bonusTrack.parentNodeId == root.nodeId);

  CHECK(discDir.parentNodeId == liveDir.nodeId);
  REQUIRE(nestedTrack.parentNodeId.has_value());
  CHECK(*nestedTrack.parentNodeId == discDir.nodeId);
  REQUIRE(songNodesWithFilePath(snapshot, nestedAudio).size() == 1U);
}

TEST_CASE("scanner complete e2e: empty cue remains interpretable without tracks") {
  test::TempScannerRoot temp{"scanner-complete-empty-cue-e2e"};
  const auto cueFile = temp.path() / "empty.cue";
  const auto standaloneAudio = temp.path() / "single.flac";

  writeText(cueFile, "FILE \"missing.flac\" WAVE\n");
  writeText(standaloneAudio, "fake standalone audio");

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(standaloneAudio, makeMetadata("Single Track"));

  const TestCueProviderGuard cueProvider{[](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() == "empty.cue") {
      return {};
    }
    return {};
  }};

  const auto snapshot = scanRoot(temp.path(), reader);

  REQUIRE(snapshot.rootNodeId.has_value());
  const auto& root = requireNode(snapshot, *snapshot.rootNodeId);
  const auto& cueContainer = requireNode(snapshot, "dir:empty.cue");
  const auto& standaloneTrack = requireSongWithTitle(snapshot, "Single Track");

  CHECK(cueContainer.kind == PlaylistNodeKind::Directory);
  CHECK(cueContainer.displayName == "empty.cue");
  CHECK_FALSE(cueContainer.song.has_value());
  CHECK(cueContainer.parentNodeId == root.nodeId);
  CHECK(cueContainer.childNodeIds.empty());
  CHECK(songNodesWithFilePath(snapshot, cueFile).empty());
  REQUIRE(songNodesWithFilePath(snapshot, standaloneAudio).size() == 1U);
  CHECK(standaloneTrack.parentNodeId == root.nodeId);
}
