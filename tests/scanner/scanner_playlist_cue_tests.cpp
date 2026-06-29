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
  return metadata;
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

[[nodiscard]] const PlaylistNode& requireNode(const PlaylistTreeSnapshot& snapshot, const std::string_view nodeId) {
  const auto iterator = std::ranges::find(snapshot.nodes, nodeId, &PlaylistNode::nodeId);
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] const PlaylistNode& requireRoot(const PlaylistTreeSnapshot& snapshot) {
  REQUIRE(snapshot.rootNodeId.has_value());
  return requireNode(snapshot, *snapshot.rootNodeId);
}

[[nodiscard]] const PlaylistNode& requireSongWithTitle(const PlaylistTreeSnapshot& snapshot,
                                                       const std::string_view title) {
  const auto iterator = std::ranges::find_if(snapshot.nodes, [&title](const PlaylistNode& node) {
    return node.song.has_value() && node.song->title == title;
  });
  REQUIRE(iterator != snapshot.nodes.end());
  return *iterator;
}

[[nodiscard]] PlaylistTreeSnapshot scanRoot(const fs::path& root, std::shared_ptr<TagMetadataReader> reader) {
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                                        .watcherFactory = nullptr,
                                                                        .databasePath = root / "cache.db",
                                                                        .coverExportDir = {}});
  ScanEvents events;
  service->setEventSink([&events](const ScannerEvent& event) { events.onEvent(event); });
  service->scan({ScannerRoot{.path = root}}, ScanMode::Full);
  REQUIRE(events.waitForScanCompletion(std::chrono::seconds{5}));
  return events.snapshot;
}

}

TEST_CASE("PlaylistTree CUE integration: container owns tracks and hides referenced audio") {
  test::TempScannerRoot temp{"playlist-cue-flat"};
  const auto cueFile = temp.path() / fs::path{"album.cue"};
  const auto referencedAudio = temp.path() / fs::path{"album.flac"};
  const auto standaloneAudio = temp.path() / fs::path{"bonus.flac"};

  std::ofstream{cueFile} << "FILE \"album.flac\" WAVE\n  TRACK 01 AUDIO\n  TRACK 02 AUDIO\n";
  std::ofstream{referencedAudio} << "fake referenced audio";
  std::ofstream{standaloneAudio} << "fake standalone audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, makeMetadata("Bare Album File"));
  reader->put(standaloneAudio, makeMetadata("Bonus Track"));

  const TestCueProviderGuard cueProvider{[&referencedAudio](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "album.cue") {
      return {};
    }
    return {
      {.audioFilePath = referencedAudio,
       .offset = 0,
       .duration = 180000000,
       .title = "Cue Track 1",
       .artist = "Cue Artist",
       .album = "Cue Album",
       .trackNumber = 1},
      {.audioFilePath = referencedAudio,
       .offset = 180000000,
       .duration = 200000000,
       .title = "Cue Track 2",
       .artist = "Cue Artist",
       .album = "Cue Album",
       .trackNumber = 2},
    };
  }};

  const auto snapshot = scanRoot(temp.path(), reader);

  const auto& root = requireRoot(snapshot);
  const auto& cueContainer = requireNode(snapshot, "dir:album.cue");
  const auto& firstTrack = requireSongWithTitle(snapshot, "Cue Track 1");
  const auto& secondTrack = requireSongWithTitle(snapshot, "Cue Track 2");
  const auto& bonusTrack = requireSongWithTitle(snapshot, "Bonus Track");

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

  CHECK(songNodesWithFilePath(snapshot, referencedAudio).empty());
  REQUIRE(songNodesWithFilePath(snapshot, cueFile).size() == 2U);
  REQUIRE(songNodesWithFilePath(snapshot, standaloneAudio).size() == 1U);
  CHECK(bonusTrack.nodeId == "track:bonus.flac");
  CHECK(bonusTrack.parentNodeId == root.nodeId);
}

TEST_CASE("PlaylistTree CUE integration: nested cue tracks stay under nested cue container") {
  test::TempScannerRoot temp{"playlist-cue-nested"};
  const auto discDir = temp.path() / fs::path{"box"} / fs::path{"disc"};
  fs::create_directories(discDir);
  const auto cueFile = discDir / "live.cue";
  const auto referencedAudio = discDir / "live.flac";

  std::ofstream{cueFile} << "FILE \"live.flac\" WAVE\n  TRACK 01 AUDIO\n";
  std::ofstream{referencedAudio} << "fake live audio";

  auto reader = std::make_shared<FakeMetadataReader>();
  reader->put(referencedAudio, makeMetadata("Bare Live File"));

  const TestCueProviderGuard cueProvider{[&referencedAudio](const fs::path& cuePath) -> std::vector<TestCueTrackData> {
    if (cuePath.filename() != "live.cue") {
      return {};
    }
    return {{.audioFilePath = referencedAudio,
             .offset = 0,
             .duration = 240000000,
             .title = "Nested Cue Track",
             .artist = "Live Artist",
             .album = "Live Album",
             .trackNumber = 1}};
  }};

  const auto snapshot = scanRoot(temp.path(), reader);

  const auto& box = requireNode(snapshot, "dir:box");
  const auto& disc = requireNode(snapshot, "dir:box/disc");
  const auto& cueContainer = requireNode(snapshot, "dir:box/disc/live.cue");
  const auto& cueTrack = requireSongWithTitle(snapshot, "Nested Cue Track");

  CHECK(disc.parentNodeId == box.nodeId);
  CHECK(cueContainer.kind == PlaylistNodeKind::Directory);
  CHECK(cueContainer.parentNodeId == disc.nodeId);
  CHECK(std::ranges::find(disc.childNodeIds, cueContainer.nodeId) != disc.childNodeIds.end());

  REQUIRE(cueTrack.parentNodeId.has_value());
  CHECK(*cueTrack.parentNodeId == cueContainer.nodeId);
  CHECK(std::ranges::find(cueContainer.childNodeIds, cueTrack.nodeId) != cueContainer.childNodeIds.end());

  CHECK(songNodesWithFilePath(snapshot, referencedAudio).empty());
  REQUIRE(songNodesWithFilePath(snapshot, cueFile).size() == 1U);
}
