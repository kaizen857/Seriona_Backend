#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "control_test_harness.h"
#include "seriona/control/media_controller.h"
#include "../../src/control/control_state_reducer.h"

#include <algorithm>
#include <filesystem>
#include <set>

using namespace seriona::control;
using namespace seriona::scanner;
namespace control_test = seriona::control::test;

namespace {

SongMetadata makeSong(std::string id, std::string path) {
  SongMetadata song{};
  song.trackId = std::move(id);
  song.filePath = std::filesystem::path{std::move(path)};
  song.title = "Title " + song.trackId;
  song.artist = "Artist";
  song.contentHash = song.trackId;
  song.duration = std::chrono::milliseconds{3000};
  return song;
}

PlaylistNode trackNode(std::string nodeId, SongMetadata metadata) {
  return PlaylistNode{
      .nodeId = std::move(nodeId),
      .parentNodeId = std::string{"root"},
      .kind = PlaylistNodeKind::Track,
      .displayName = metadata.trackId,
      .song = std::move(metadata),
      .childNodeIds = {}
  };
}

PlaylistNode rootNode(std::vector<std::string> children) {
  return PlaylistNode{
      .nodeId = "root",
      .parentNodeId = std::nullopt,
      .kind = PlaylistNodeKind::Root,
      .displayName = "Library",
      .song = std::nullopt,
      .childNodeIds = std::move(children)
  };
}

PlaylistTreeSnapshot makeLibraryWithFolders() {
  PlaylistTreeSnapshot tree{};
  tree.version = 1;
  tree.rootNodeId = "root";
  
  std::vector<std::string> children = {
      "folder1-song1", "folder1-song2", "folder1-song3",
      "folder2-song1", "folder2-song2"
  };
  
  tree.nodes.push_back(rootNode(children));
  tree.nodes.push_back(trackNode("folder1-song1", makeSong("folder1-song1", "/music/folder1/song1.flac")));
  tree.nodes.push_back(trackNode("folder1-song2", makeSong("folder1-song2", "/music/folder1/song2.flac")));
  tree.nodes.push_back(trackNode("folder1-song3", makeSong("folder1-song3", "/music/folder1/song3.flac")));
  tree.nodes.push_back(trackNode("folder2-song1", makeSong("folder2-song1", "/music/folder2/song1.flac")));
  tree.nodes.push_back(trackNode("folder2-song2", makeSong("folder2-song2", "/music/folder2/song2.flac")));
  
  return tree;
}

ScannerEvent scannerSnapshotEvent(PlaylistTreeSnapshot snapshot, std::uint64_t eventVersion) {
  return ScannerEvent{
      .type = ScannerEventType::PlaylistSnapshotUpdated,
      .monotonicVersion = eventVersion,
      .timestamp = {},
      .payload = std::move(snapshot)
  };
}

struct ControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio;
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner;
  std::unique_ptr<MediaController> controller;
  
  ControllerFixture() {
    fakeAudio = std::make_shared<control_test::FakeAudioPlaybackService>();
    fakeScanner = std::make_shared<control_test::FakeFileScannerService>();
    
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    
    controller = makeMediaController(
        MediaControllerDependencies{
            .audio = fakeAudio,
            .scanner = fakeScanner,
            .metadata = std::move(metadataService)
        },
        MediaControllerOptions{.runInlineForTests = true}
    );
  }
};

}

TEST_CASE("ShuffleHistory basic operations") {
  ShuffleHistory history(3);
  
  TrackIdentity track1{.trackId = "track1", .filePath = "/music/song1.flac"};
  TrackIdentity track2{.trackId = "track2", .filePath = "/music/song2.flac"};
  TrackIdentity track3{.trackId = "track3", .filePath = "/music/song3.flac"};
  
  SUBCASE("push and pop") {
    history.push(track1);
    history.push(track2);
    history.push(track3);
    
    CHECK(history.size() == 3);
    CHECK(!history.empty());
    
    auto popped1 = history.pop();
    REQUIRE(popped1.has_value());
    CHECK(popped1->trackId == track3.trackId);
    
    auto popped2 = history.pop();
    REQUIRE(popped2.has_value());
    CHECK(popped2->trackId == track2.trackId);
    
    auto popped3 = history.pop();
    REQUIRE(popped3.has_value());
    CHECK(popped3->trackId == track1.trackId);
    
    CHECK(history.empty());
    CHECK(!history.pop().has_value());
  }
  
  SUBCASE("respects max size") {
    history.push(track1);
    history.push(track2);
    history.push(track3);
    
    TrackIdentity track4{.trackId = "track4", .filePath = "/music/song4.flac"};
    history.push(track4);
    
    CHECK(history.size() == 3);
    CHECK(!history.contains(track1));
    CHECK(history.contains(track2));
    CHECK(history.contains(track3));
    CHECK(history.contains(track4));
  }
  
  SUBCASE("contains check") {
    history.push(track1);
    history.push(track2);
    
    CHECK(history.contains(track1));
    CHECK(history.contains(track2));
    CHECK(!history.contains(track3));
  }
  
  SUBCASE("clear") {
    history.push(track1);
    history.push(track2);
    
    history.clear();
    
    CHECK(history.empty());
    CHECK(history.size() == 0);
    CHECK(!history.contains(track1));
  }
}

TEST_CASE("shuffle playback stays in same folder") {
  ControllerFixture fixture{};
  fixture.controller->start();
  
  auto library = makeLibraryWithFolders();
  fixture.fakeScanner->emit(scannerSnapshotEvent(library, 1));
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetShuffle,
      .shuffle = true
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder1-song1", .filePath = "/music/folder1/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  std::set<std::string> playedTracks;
  playedTracks.insert("folder1-song1");
  
  for (int i = 0; i < 10; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
    fixture.controller->drainForTests();
    
    auto state = fixture.controller->playerStateSnapshot();
    if (state.currentTrack.has_value()) {
      const auto& track = *state.currentTrack;
      playedTracks.insert(track.trackId);
      
      CHECK(track.filePath.parent_path() == std::filesystem::path{"/music/folder1"});
    }
  }
  
  CHECK(playedTracks.size() >= 2);
}

TEST_CASE("shuffle playback avoids repeats within history") {
  ControllerFixture fixture{};
  fixture.controller->start();
  
  auto library = makeLibraryWithFolders();
  fixture.fakeScanner->emit(scannerSnapshotEvent(library, 1));
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetShuffle,
      .shuffle = true
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder1-song1", .filePath = "/music/folder1/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  std::vector<std::string> playedSequence;
  playedSequence.push_back("folder1-song1");
  
  for (int i = 0; i < 2; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
    fixture.controller->drainForTests();
    
    auto state = fixture.controller->playerStateSnapshot();
    if (state.currentTrack.has_value()) {
      playedSequence.push_back(state.currentTrack->trackId);
    }
  }
  
  std::set<std::string> uniqueTracks(playedSequence.begin(), playedSequence.end());
  
  CAPTURE(playedSequence);
  CAPTURE(uniqueTracks.size());
  
  CHECK(uniqueTracks.size() == playedSequence.size());
  CHECK(playedSequence.size() == 3);
}

TEST_CASE("shuffle previous track returns history") {
  ControllerFixture fixture{};
  fixture.controller->start();
  
  auto library = makeLibraryWithFolders();
  fixture.fakeScanner->emit(scannerSnapshotEvent(library, 1));
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetShuffle,
      .shuffle = true
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder1-song1", .filePath = "/music/folder1/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  std::vector<std::string> forwardSequence;
  forwardSequence.push_back("folder1-song1");
  
  for (int i = 0; i < 3; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
    fixture.controller->drainForTests();
    
    auto state = fixture.controller->playerStateSnapshot();
    if (state.currentTrack.has_value()) {
      forwardSequence.push_back(state.currentTrack->trackId);
    }
  }
  
  REQUIRE(forwardSequence.size() == 4);
  
  for (int i = 0; i < 2; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipPrevious});
    fixture.controller->drainForTests();
  }
  
  auto state = fixture.controller->playerStateSnapshot();
  REQUIRE(state.currentTrack.has_value());
  CHECK(state.currentTrack->trackId == forwardSequence[1]);
}

TEST_CASE("manual track selection clears history") {
  ControllerFixture fixture{};
  fixture.controller->start();
  
  auto library = makeLibraryWithFolders();
  fixture.fakeScanner->emit(scannerSnapshotEvent(library, 1));
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetShuffle,
      .shuffle = true
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder1-song1", .filePath = "/music/folder1/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  for (int i = 0; i < 3; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
    fixture.controller->drainForTests();
  }
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder2-song1", .filePath = "/music/folder2/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipPrevious});
  fixture.controller->drainForTests();
  
  auto state = fixture.controller->playerStateSnapshot();
  CHECK(state.currentTrack->trackId == "folder2-song1");
}

TEST_CASE("shuffle with RepeatAll cycles through folder") {
  ControllerFixture fixture{};
  fixture.controller->start();
  
  auto library = makeLibraryWithFolders();
  fixture.fakeScanner->emit(scannerSnapshotEvent(library, 1));
  fixture.controller->drainForTests();
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetShuffle,
      .shuffle = true
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SetRepeatMode,
      .repeatMode = RepeatMode::All
  });
  
  fixture.controller->submitCommand(MediaControlCommand{
      .kind = MediaControlCommandKind::SelectTrack,
      .track = TrackIdentity{.trackId = "folder1-song1", .filePath = "/music/folder1/song1.flac"}
  });
  fixture.controller->drainForTests();
  
  std::set<std::string> playedTracks;
  playedTracks.insert("folder1-song1");
  
  for (int i = 0; i < 10; ++i) {
    fixture.controller->submitCommand(MediaControlCommand{.kind = MediaControlCommandKind::SkipNext});
    fixture.controller->drainForTests();
    
    auto state = fixture.controller->playerStateSnapshot();
    if (state.currentTrack.has_value()) {
      playedTracks.insert(state.currentTrack->trackId);
    }
  }
  
  CHECK(playedTracks.size() == 3);
  CHECK(playedTracks.count("folder1-song1") > 0);
  CHECK(playedTracks.count("folder1-song2") > 0);
  CHECK(playedTracks.count("folder1-song3") > 0);
}
