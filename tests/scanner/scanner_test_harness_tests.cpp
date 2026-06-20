#include "scanner_test_harness.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace seriona::scanner::test {
namespace {

TEST_CASE("scanner test harness removes temp root and database marker") {
  std::filesystem::path rootPath;
  std::filesystem::path dbPath;
  {
    TempScannerRoot root("cleanup");
    rootPath = root.path();
    dbPath = root.dbPath();
    writeDbMarker(dbPath);

    REQUIRE(std::filesystem::exists(rootPath));
    REQUIRE(std::filesystem::exists(dbPath));
    CHECK_FALSE(root.removed());
  }

  CHECK_FALSE(std::filesystem::exists(rootPath));
  CHECK_FALSE(std::filesystem::exists(dbPath));
}

TEST_CASE("scanner fixture writers create deterministic audio and lyric paths") {
  TempScannerRoot root("fixtures");

  const auto audioPath = writeAudioFixture(root.path(), "artist/track.flac");
  const auto validLrcPath = writeValidLrcFixture(root.path(), "artist/track.lrc");
  const auto invalidLrcPath = writeInvalidLrcFixture(root.path(), "artist/broken.lrc");

  CHECK(audioPath.extension() == ".flac");
  CHECK(validLrcPath.extension() == ".lrc");
  CHECK(invalidLrcPath.extension() == ".lrc");
  CHECK(std::filesystem::file_size(audioPath) == 49U);
  CHECK(std::filesystem::file_size(validLrcPath) == 68U);
  CHECK(std::filesystem::file_size(invalidLrcPath) == 52U);
}

TEST_CASE("fake clock and event sink expose deterministic scanner assertions") {
  FakeClock clock;
  FakeEventSink sink;

  clock.advance(std::chrono::milliseconds{250});
  sink.publish({ScannerEvent::Type::ScanStarted, "library", {}});
  sink.publish({ScannerEvent::Type::WatcherWarning, "library", "overflow"});

  CHECK(clock.now().time_since_epoch() == std::chrono::milliseconds{250});
  CHECK(sink.events().size() == 2U);
  CHECK_NOTHROW(requireEventCount(sink, ScannerEvent::Type::ScanStarted, 1U));
  CHECK_THROWS_AS(requireEventCount(sink, ScannerEvent::Type::TrackAccepted, 1U), std::runtime_error);
}

TEST_CASE("fake tag reader can succeed throw and block deterministically") {
  FakeTagReader reader;
  reader.setSuccess({{}, "Title", "Artist", std::chrono::milliseconds{3000}});
  const auto first = reader.read("song.wav");

  CHECK(first.filePath == "song.wav");
  CHECK(first.title == "Title");
  CHECK(reader.readCount() == 1U);

  reader.setThrow("metadata boom");
  CHECK_THROWS_WITH_AS(static_cast<void>(reader.read("broken.wav")), "metadata boom", std::runtime_error);
  CHECK(reader.readCount() == 2U);

  reader.setBlocked({{}, "Slow", "Artist", std::chrono::milliseconds{4000}});
  std::atomic_bool completed{false};
  std::thread worker([&reader, &completed] {
    const auto tag = reader.read("slow.wav");
    completed = tag.title == "Slow";
  });

  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  CHECK_FALSE(completed.load());
  reader.releaseBlockedReads();
  worker.join();
  CHECK(completed.load());
  CHECK(reader.readCount() == 3U);
}

TEST_CASE("fake watcher represents audio lrc rename and warning events") {
  FakeWatcher watcher;

  watcher.audioCreated("music/new.flac");
  watcher.audioModified("music/new.flac");
  watcher.audioRenamed("music/new.flac", "music/renamed.flac");
  watcher.lrcCreated("music/renamed.lrc");
  watcher.lrcRenamed("music/renamed.lrc", "music/final.lrc");
  watcher.lrcDestroyed("music/final.lrc");
  watcher.warning("music", "recursive watcher overflow");

  CHECK(watcher.events().size() == 7U);
  CHECK_NOTHROW(requireWatcherEvent(watcher.popNext(), FakeWatcherEvent::Type::Created,
                                    FakeWatcherEvent::PathKind::Audio, "music/new.flac"));
  CHECK_NOTHROW(requireWatcherEvent(watcher.popNext(), FakeWatcherEvent::Type::Modified,
                                    FakeWatcherEvent::PathKind::Audio, "music/new.flac"));

  const auto audioRename = watcher.popNext();
  CHECK(audioRename.type == FakeWatcherEvent::Type::Renamed);
  CHECK(audioRename.pathKind == FakeWatcherEvent::PathKind::Audio);
  REQUIRE(audioRename.oldPath.has_value());
  CHECK(*audioRename.oldPath == "music/new.flac");
  CHECK(audioRename.path == "music/renamed.flac");

  CHECK_NOTHROW(requireWatcherEvent(watcher.popNext(), FakeWatcherEvent::Type::Created,
                                    FakeWatcherEvent::PathKind::Lrc, "music/renamed.lrc"));
  const auto lrcRename = watcher.popNext();
  CHECK(lrcRename.type == FakeWatcherEvent::Type::Renamed);
  CHECK(lrcRename.pathKind == FakeWatcherEvent::PathKind::Lrc);
  REQUIRE(lrcRename.oldPath.has_value());
  CHECK(*lrcRename.oldPath == "music/renamed.lrc");
  CHECK(lrcRename.path == "music/final.lrc");
  CHECK_NOTHROW(requireWatcherEvent(watcher.popNext(), FakeWatcherEvent::Type::Destroyed,
                                    FakeWatcherEvent::PathKind::Lrc, "music/final.lrc"));

  const auto warning = watcher.popNext();
  CHECK(warning.type == FakeWatcherEvent::Type::Warning);
  CHECK(warning.path == "music");
  CHECK(warning.warning == "recursive watcher overflow");
}

}

}
