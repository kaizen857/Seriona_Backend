#include "../../src/scanner/scanner_internal_types.h"

#include <doctest.h>

#include <chrono>
#include <thread>

namespace seriona::scanner {
namespace {

TEST_CASE("scanner internal node type and cue info expose stable fields") {
  const auto directory = NodeType::Directory;
  const auto song = NodeType::Song;
  const auto cueContainer = NodeType::CueContainer;
  const auto cueTrack = NodeType::CueTrack;

  CHECK(directory == NodeType::Directory);
  CHECK(song == NodeType::Song);
  CHECK(cueContainer == NodeType::CueContainer);
  CHECK(cueTrack == NodeType::CueTrack);

  CueInfo cueInfo{};
  cueInfo.cueFilePath = "/music/album.cue";
  cueInfo.audioFilePath = "/music/album.flac";
  cueInfo.offset = std::chrono::microseconds{1234567};
  cueInfo.duration = std::chrono::microseconds{2345678};
  cueInfo.trackIndex = 2U;

  CHECK(cueInfo.cueFilePath == "/music/album.cue");
  CHECK(cueInfo.audioFilePath == "/music/album.flac");
  CHECK(cueInfo.offset == std::chrono::microseconds{1234567});
  CHECK(cueInfo.duration == std::chrono::microseconds{2345678});
  CHECK(cueInfo.trackIndex == 2U);
}

TEST_CASE("scanner internal scan item origin exposes stable variants") {
  CHECK(ScanItemOrigin::CacheHit == ScanItemOrigin::CacheHit);
  CHECK(ScanItemOrigin::CueTrackCacheHit == ScanItemOrigin::CueTrackCacheHit);
  CHECK(ScanItemOrigin::RescannedChanged == ScanItemOrigin::RescannedChanged);
  CHECK(ScanItemOrigin::ScannedNew == ScanItemOrigin::ScannedNew);
  CHECK(ScanItemOrigin::ScannedFull == ScanItemOrigin::ScannedFull);
  CHECK(ScanItemOrigin::CueTrackRescannedChanged == ScanItemOrigin::CueTrackRescannedChanged);
  CHECK(ScanItemOrigin::CueTrackScannedNew == ScanItemOrigin::CueTrackScannedNew);
  CHECK(ScanItemOrigin::VirtualContainer == ScanItemOrigin::VirtualContainer);
}

TEST_CASE("NodeType distinguishes Song, CueContainer, and CueTrack") {
  const auto song = NodeType::Song;
  const auto cueContainer = NodeType::CueContainer;
  const auto cueTrack = NodeType::CueTrack;

  CHECK(song != cueContainer);
  CHECK(song != cueTrack);
  CHECK(cueContainer != cueTrack);
}

TEST_CASE("CueInfo holds microsecond-precision offset and duration") {
  CueInfo cueInfo{};
  cueInfo.offset = std::chrono::microseconds{500000};
  cueInfo.duration = std::chrono::microseconds{180000000};

  CHECK(cueInfo.offset.count() == 500000);
  CHECK(cueInfo.duration.count() == 180000000);
}

TEST_CASE("CueInfo supports zero offset for first track") {
  CueInfo cueInfo{};
  cueInfo.offset = std::chrono::microseconds{0};
  cueInfo.trackIndex = 1U;

  CHECK(cueInfo.offset == std::chrono::microseconds{0});
  CHECK(cueInfo.trackIndex == 1U);
}

TEST_CASE("IndexedPublishedSong default initialization sets Song nodeType") {
  IndexedPublishedSong indexed{};

  CHECK(indexed.nodeType == NodeType::Song);
  CHECK_FALSE(indexed.cueInfo.has_value());
  CHECK(indexed.origin == ScanItemOrigin::ScannedFull);
  CHECK_FALSE(indexed.locationId.has_value());
  CHECK_FALSE(indexed.filled.load());
  CHECK(indexed.needsScan.load());
  CHECK_FALSE(indexed.isVirtualFolder);
}

TEST_CASE("IndexedPublishedSong allows CueTrack nodeType with CueInfo") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::CueTrack;
  indexed.cueInfo = CueInfo{
      .cueFilePath = "/music/album.cue",
      .audioFilePath = "/music/album.flac",
      .offset = std::chrono::microseconds{1000000},
      .duration = std::chrono::microseconds{180000000},
      .trackIndex = 3U,
  };

  CHECK(indexed.nodeType == NodeType::CueTrack);
  REQUIRE(indexed.cueInfo.has_value());
  CHECK(indexed.cueInfo->cueFilePath == "/music/album.cue");
  CHECK(indexed.cueInfo->trackIndex == 3U);
}

TEST_CASE("IndexedPublishedSong atomic filled flag supports load and store") {
  IndexedPublishedSong indexed{};

  CHECK_FALSE(indexed.filled.load());
  indexed.filled.store(true);
  CHECK(indexed.filled.load());
  indexed.filled.store(false);
  CHECK_FALSE(indexed.filled.load());
}

TEST_CASE("IndexedPublishedSong atomic needsScan flag defaults to true") {
  IndexedPublishedSong indexed{};

  CHECK(indexed.needsScan.load());
  indexed.needsScan.store(false);
  CHECK_FALSE(indexed.needsScan.load());
}

TEST_CASE("IndexedPublishedSong move constructor transfers atomic state") {
  IndexedPublishedSong source{};
  source.nodeType = NodeType::CueContainer;
  source.origin = ScanItemOrigin::VirtualContainer;
  source.locationId = "virtual-container-location";
  source.filled.store(true);
  source.needsScan.store(false);
  source.isVirtualFolder = true;

  IndexedPublishedSong moved{std::move(source)};

  CHECK(moved.nodeType == NodeType::CueContainer);
  CHECK(moved.origin == ScanItemOrigin::VirtualContainer);
  REQUIRE(moved.locationId.has_value());
  CHECK(*moved.locationId == "virtual-container-location");
  CHECK(moved.filled.load());
  CHECK_FALSE(moved.needsScan.load());
  CHECK(moved.isVirtualFolder);
}

TEST_CASE("IndexedPublishedSong move assignment transfers atomic state") {
  IndexedPublishedSong source{};
  source.nodeType = NodeType::Directory;
  source.origin = ScanItemOrigin::CacheHit;
  source.locationId = "cached-location-id";
  source.filled.store(true);
  source.needsScan.store(false);
  source.isVirtualFolder = true;

  IndexedPublishedSong target{};
  target = std::move(source);

  CHECK(target.nodeType == NodeType::Directory);
  CHECK(target.origin == ScanItemOrigin::CacheHit);
  REQUIRE(target.locationId.has_value());
  CHECK(*target.locationId == "cached-location-id");
  CHECK(target.filled.load());
  CHECK_FALSE(target.needsScan.load());
  CHECK(target.isVirtualFolder);
}

TEST_CASE("IndexedPublishedSong atomic flags are thread-safe") {
  IndexedPublishedSong indexed{};

  std::thread writer{[&indexed] {
    for (int i = 0; i < 100; ++i) {
      indexed.filled.store(true);
      indexed.needsScan.store(false);
    }
  }};

  std::thread reader{[&indexed] {
    for (int i = 0; i < 100; ++i) {
      static_cast<void>(indexed.filled.load());
      static_cast<void>(indexed.needsScan.load());
    }
  }};

  writer.join();
  reader.join();

  CHECK(indexed.filled.load());
  CHECK_FALSE(indexed.needsScan.load());
}

// ============================================================================
// Task 5: Boundary and edge case coverage
// ============================================================================

TEST_CASE("CueInfo handles empty paths gracefully") {
  CueInfo cueInfo{};
  cueInfo.cueFilePath = "";
  cueInfo.audioFilePath = "";
  cueInfo.offset = std::chrono::microseconds{60000000};
  cueInfo.duration = std::chrono::microseconds{180000000};
  cueInfo.trackIndex = 1U;

  CHECK(cueInfo.cueFilePath.empty());
  CHECK(cueInfo.audioFilePath.empty());
  CHECK(cueInfo.offset.count() == 60000000);
  CHECK(cueInfo.duration.count() == 180000000);
}

TEST_CASE("CueInfo supports zero duration for unknown-length tracks") {
  CueInfo cueInfo{};
  cueInfo.cueFilePath = "/music/album.cue";
  cueInfo.audioFilePath = "/music/album.flac";
  cueInfo.offset = std::chrono::microseconds{120000000};
  cueInfo.duration = std::chrono::microseconds{0};
  cueInfo.trackIndex = 5U;

  CHECK(cueInfo.duration == std::chrono::microseconds{0});
  CHECK(cueInfo.offset.count() == 120000000);
  CHECK(cueInfo.trackIndex == 5U);
}

TEST_CASE("CueInfo handles maximum microsecond values") {
  CueInfo cueInfo{};
  constexpr auto maxDuration = std::chrono::microseconds::max();
  cueInfo.offset = maxDuration;
  cueInfo.duration = maxDuration;
  cueInfo.trackIndex = 999U;

  CHECK(cueInfo.offset == maxDuration);
  CHECK(cueInfo.duration == maxDuration);
  CHECK(cueInfo.trackIndex == 999U);
}

TEST_CASE("CueInfo default construction initializes to zero") {
  CueInfo cueInfo{};

  CHECK(cueInfo.cueFilePath.empty());
  CHECK(cueInfo.audioFilePath.empty());
  CHECK(cueInfo.offset == std::chrono::microseconds{0});
  CHECK(cueInfo.duration == std::chrono::microseconds{0});
  CHECK(cueInfo.trackIndex == 0U);
}

TEST_CASE("CueInfo handles trackIndex zero boundary") {
  CueInfo cueInfo{};
  cueInfo.trackIndex = 0U;

  CHECK(cueInfo.trackIndex == 0U);
}

TEST_CASE("IndexedPublishedSong supports all four NodeType variants") {
  IndexedPublishedSong directory{};
  directory.nodeType = NodeType::Directory;
  CHECK(directory.nodeType == NodeType::Directory);

  IndexedPublishedSong song{};
  song.nodeType = NodeType::Song;
  CHECK(song.nodeType == NodeType::Song);

  IndexedPublishedSong cueContainer{};
  cueContainer.nodeType = NodeType::CueContainer;
  CHECK(cueContainer.nodeType == NodeType::CueContainer);

  IndexedPublishedSong cueTrack{};
  cueTrack.nodeType = NodeType::CueTrack;
  CHECK(cueTrack.nodeType == NodeType::CueTrack);
}

TEST_CASE("IndexedPublishedSong CueContainer with isVirtualFolder flag") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::CueContainer;
  indexed.isVirtualFolder = true;
  indexed.filled.store(true);
  indexed.needsScan.store(false);

  CHECK(indexed.nodeType == NodeType::CueContainer);
  CHECK(indexed.isVirtualFolder);
  CHECK(indexed.filled.load());
  CHECK_FALSE(indexed.needsScan.load());
  CHECK_FALSE(indexed.cueInfo.has_value());
}

TEST_CASE("IndexedPublishedSong Song type without cueInfo remains nullopt") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::Song;
  indexed.filled.store(true);

  CHECK(indexed.nodeType == NodeType::Song);
  CHECK_FALSE(indexed.cueInfo.has_value());
  CHECK(indexed.filled.load());
}

TEST_CASE("IndexedPublishedSong both atomic flags can be false") {
  IndexedPublishedSong indexed{};
  indexed.filled.store(false);
  indexed.needsScan.store(false);

  CHECK_FALSE(indexed.filled.load());
  CHECK_FALSE(indexed.needsScan.load());
}

TEST_CASE("IndexedPublishedSong both atomic flags can be true") {
  IndexedPublishedSong indexed{};
  indexed.filled.store(true);
  indexed.needsScan.store(true);

  CHECK(indexed.filled.load());
  CHECK(indexed.needsScan.load());
}

TEST_CASE("IndexedPublishedSong CueTrack with zero offset and duration") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::CueTrack;
  indexed.cueInfo = CueInfo{
      .cueFilePath = "/music/single.cue",
      .audioFilePath = "/music/single.wav",
      .offset = std::chrono::microseconds{0},
      .duration = std::chrono::microseconds{0},
      .trackIndex = 0U,
  };

  REQUIRE(indexed.cueInfo.has_value());
  CHECK(indexed.cueInfo->offset == std::chrono::microseconds{0});
  CHECK(indexed.cueInfo->duration == std::chrono::microseconds{0});
  CHECK(indexed.cueInfo->trackIndex == 0U);
}

TEST_CASE("IndexedPublishedSong CueTrack with empty cueInfo paths") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::CueTrack;
  indexed.cueInfo = CueInfo{
      .cueFilePath = "",
      .audioFilePath = "",
      .offset = std::chrono::microseconds{5000000},
      .duration = std::chrono::microseconds{120000000},
      .trackIndex = 1U,
  };

  REQUIRE(indexed.cueInfo.has_value());
  CHECK(indexed.cueInfo->cueFilePath.empty());
  CHECK(indexed.cueInfo->audioFilePath.empty());
  CHECK(indexed.cueInfo->offset.count() == 5000000);
}

TEST_CASE("IndexedPublishedSong move constructor with default-initialized source") {
  IndexedPublishedSong source{};
  IndexedPublishedSong moved{std::move(source)};

  CHECK(moved.nodeType == NodeType::Song);
  CHECK(moved.origin == ScanItemOrigin::ScannedFull);
  CHECK_FALSE(moved.locationId.has_value());
  CHECK_FALSE(moved.filled.load());
  CHECK(moved.needsScan.load());
  CHECK_FALSE(moved.isVirtualFolder);
  CHECK_FALSE(moved.cueInfo.has_value());
}

TEST_CASE("IndexedPublishedSong move assignment with default-initialized source") {
  IndexedPublishedSong source{};
  IndexedPublishedSong target{};
  target.nodeType = NodeType::CueContainer;
  target.filled.store(true);

  target = std::move(source);

  CHECK(target.nodeType == NodeType::Song);
  CHECK_FALSE(target.filled.load());
  CHECK(target.needsScan.load());
}

TEST_CASE("IndexedPublishedSong Directory type without virtual folder flag") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::Directory;
  indexed.isVirtualFolder = false;

  CHECK(indexed.nodeType == NodeType::Directory);
  CHECK_FALSE(indexed.isVirtualFolder);
}

TEST_CASE("IndexedPublishedSong CueTrack with maximum offset and duration") {
  IndexedPublishedSong indexed{};
  indexed.nodeType = NodeType::CueTrack;
  constexpr auto maxTime = std::chrono::microseconds::max();
  indexed.cueInfo = CueInfo{
      .cueFilePath = "/music/huge.cue",
      .audioFilePath = "/music/huge.flac",
      .offset = maxTime,
      .duration = maxTime,
      .trackIndex = 9999U,
  };

  REQUIRE(indexed.cueInfo.has_value());
  CHECK(indexed.cueInfo->offset == maxTime);
  CHECK(indexed.cueInfo->duration == maxTime);
  CHECK(indexed.cueInfo->trackIndex == 9999U);
}

}
}
