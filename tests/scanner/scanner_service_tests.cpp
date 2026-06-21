#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

class FakeServiceMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }
  void fail(std::filesystem::path path, std::string message) { failures_[std::move(path)] = std::move(message); }

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override {
    requestedPaths.push_back(path);
    requestedCoverDirs.push_back(coverExportDir);
    const auto failure = failures_.find(path);
    if (failure != failures_.end()) {
      throw std::runtime_error(failure->second);
    }
    auto iterator = metadataByPath_.find(path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata");
    }
    auto metadata = iterator->second;
    metadata.filePath = path;
    return metadata;
  }

  [[nodiscard]] std::size_t readCount() const noexcept { return requestedPaths.size(); }

  std::vector<std::filesystem::path> requestedPaths;
  std::vector<std::filesystem::path> requestedCoverDirs;

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
  std::map<std::filesystem::path, std::string> failures_;
};

[[nodiscard]] RawTagMetadata rawMetadata(std::string title, std::vector<RawTagLyricLine> lyrics = {}) {
  RawTagMetadata raw{};
  raw.title = std::move(title);
  raw.artist = "Artist";
  raw.album = "Album";
  raw.embeddedLyrics = std::move(lyrics);
  raw.duration = std::chrono::milliseconds{120000};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.channels = 2;
  return raw;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeService(test::TempScannerRoot& temp,
                                                              std::shared_ptr<FakeServiceMetadataReader> reader) {
  return makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                               .watcherFactory = nullptr,
                                                               .databasePath = temp.dbPath(),
                                                               .coverExportDir = temp.path() / "covers"});
}

[[nodiscard]] std::vector<SongMetadata> songsIn(const PlaylistTreeSnapshot& snapshot) {
  std::vector<SongMetadata> songs;
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value()) {
      songs.push_back(*node.song);
    }
  }
  std::ranges::sort(songs, {}, &SongMetadata::filePath);
  return songs;
}

[[nodiscard]] std::vector<ScannerError> errorsFrom(const std::vector<ScannerEvent>& events) {
  std::vector<ScannerError> errors;
  for (const auto& event : events) {
    if (event.type == ScannerEventType::ScanError && std::holds_alternative<ScannerError>(event.payload)) {
      errors.push_back(std::get<ScannerError>(event.payload));
    }
  }
  return errors;
}

[[nodiscard]] const SongMetadata& songByPath(const std::vector<SongMetadata>& songs, const std::filesystem::path& path) {
  const auto iterator = std::ranges::find(songs, path, &SongMetadata::filePath);
  if (iterator == songs.end()) {
    throw std::runtime_error("missing song in scanner service test");
  }
  return *iterator;
}

[[nodiscard]] const PlaylistNode& nodeById(const PlaylistTreeSnapshot& snapshot, std::string_view nodeId) {
  const auto iterator = std::ranges::find_if(snapshot.nodes, [nodeId](const PlaylistNode& node) {
    return node.nodeId == nodeId;
  });
  if (iterator == snapshot.nodes.end()) {
    throw std::runtime_error("missing playlist node in scanner service test");
  }
  return *iterator;
}

TEST_CASE("scanner service scans hashes caches lyrics and skips unchanged rereads") {
  test::TempScannerRoot temp{"scanner-service-cache-hit"};
  const auto first = test::writeAudioFixture(temp.path(), "song.flac");
  const auto second = test::writeAudioFixture(temp.path(), "second.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external one\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(first, rawMetadata("First", {RawTagLyricLine{std::chrono::milliseconds{500}, "embedded one"}}));
  reader->put(second, rawMetadata("Second", {RawTagLyricLine{std::chrono::milliseconds{700}, "embedded two"}}));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto firstSnapshot = service->snapshot();
  const auto firstSongs = songsIn(firstSnapshot);

  REQUIRE(firstSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(firstSongs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(firstSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(firstSongs[1].effectiveLyrics.size() == 1U);
  CHECK(firstSongs[1].effectiveLyrics[0].text == "external one");

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto cachedSongs = songsIn(service->snapshot());

  REQUIRE(cachedSongs.size() == 2U);
  CHECK(reader->readCount() == 2U);
  CHECK(cachedSongs[1].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(cachedSongs[1].effectiveLyrics[0].text == "external one");
}

TEST_CASE("scanner service rereads changed audio and reparses only changed lrc") {
  test::TempScannerRoot temp{"scanner-service-reconcile"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  writeText(temp.path() / "song.lrc", "[00:01.00]external one\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Before", {RawTagLyricLine{std::chrono::milliseconds{300}, "embedded before"}}));
  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  CHECK(reader->readCount() == 1U);

  writeText(temp.path() / "song.lrc", "[00:02.00]external two\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  REQUIRE(songs[0].effectiveLyrics.size() == 1U);
  CHECK(songs[0].effectiveLyrics[0].text == "external two");

  writeText(audio, "changed audio bytes");
  reader->put(audio, rawMetadata("After", {RawTagLyricLine{std::chrono::milliseconds{400}, "embedded after"}}));
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 2U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "After");
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songs[0].effectiveLyrics[0].text == "external two");
}

TEST_CASE("scanner service handles new and deleted lrc without tagreader and prunes deleted audio") {
  test::TempScannerRoot temp{"scanner-service-delete"};
  const auto embeddedAudio = test::writeAudioFixture(temp.path(), "embedded.flac");
  const auto plainAudio = test::writeAudioFixture(temp.path(), "plain.flac");
  const auto deletedAudio = test::writeAudioFixture(temp.path(), "deleted.flac");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(embeddedAudio, rawMetadata("Embedded", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded lyric"}}));
  reader->put(plainAudio, rawMetadata("Plain"));
  reader->put(deletedAudio, rawMetadata("Deleted"));
  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  CHECK(reader->readCount() == 3U);

  writeText(temp.path() / "embedded.lrc", "[00:01.00]external embedded\n");
  writeText(temp.path() / "plain.lrc", "[00:01.00]external plain\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 3U);
  REQUIRE(songs.size() == 3U);
  CHECK(songByPath(songs, embeddedAudio).effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songByPath(songs, plainAudio).effectiveLyricsSource == LyricsSource::ExternalLrc);

  std::filesystem::remove(temp.path() / "embedded.lrc");
  std::filesystem::remove(temp.path() / "plain.lrc");
  std::filesystem::remove(deletedAudio);
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 3U);
  REQUIRE(songs.size() == 2U);
  const auto& embeddedSong = songByPath(songs, embeddedAudio);
  CHECK(embeddedSong.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(embeddedSong.effectiveLyrics.size() == 1U);
  CHECK(embeddedSong.effectiveLyrics[0].text == "embedded lyric");
  const auto& plainSong = songByPath(songs, plainAudio);
  CHECK(plainSong.effectiveLyricsSource == LyricsSource::None);
  CHECK(plainSong.effectiveLyrics.empty());
}

TEST_CASE("scanner service supports single file roots with same basename lrc") {
  test::TempScannerRoot temp{"scanner-service-single-file"};
  const auto audio = test::writeAudioFixture(temp.path(), "single.flac");
  writeText(temp.path() / "single.lrc", "[00:01.00]single external\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Single", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = audio}}, ScanMode::Full);
  const auto songs = songsIn(service->snapshot());

  CHECK(reader->readCount() == 1U);
  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == audio);
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(songs[0].effectiveLyrics[0].text == "single external");

  const auto snapshot = service->snapshot();
  CHECK(snapshot.nodes.size() == 2U);
  CHECK(nodeById(snapshot, "track:single.flac").parentNodeId == snapshot.rootNodeId);
}

TEST_CASE("scanner service publishes nested directory hierarchy for directory roots") {
  test::TempScannerRoot temp{"scanner-service-hierarchy"};
  const auto audio = test::writeAudioFixture(temp.path(), "artists/album/nested.flac");
  writeText(temp.path() / "artists" / "album" / "nested.lrc", "[00:01.00]nested external\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(audio, rawMetadata("Nested"));
  auto service = makeService(temp, reader);

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  const auto snapshot = service->snapshot();
  const auto songs = songsIn(snapshot);

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].filePath == audio);
  const auto& artists = nodeById(snapshot, "dir:artists");
  const auto& album = nodeById(snapshot, "dir:artists/album");
  const auto& track = nodeById(snapshot, "track:artists/album/nested.flac");
  CHECK(artists.kind == PlaylistNodeKind::Directory);
  CHECK(album.kind == PlaylistNodeKind::Directory);
  CHECK(track.kind == PlaylistNodeKind::Track);
  CHECK(artists.parentNodeId == snapshot.rootNodeId);
  REQUIRE(album.parentNodeId.has_value());
  CHECK(*album.parentNodeId == artists.nodeId);
  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == album.nodeId);
  CHECK(std::ranges::none_of(snapshot.nodes, [](const PlaylistNode& node) { return node.nodeId.ends_with(".lrc"); }));
}

TEST_CASE("scanner service records failures malformed lrc cancellation and preserves cache") {
  test::TempScannerRoot temp{"scanner-service-errors"};
  const auto good = test::writeAudioFixture(temp.path(), "good.flac");
  const auto broken = test::writeAudioFixture(temp.path(), "broken.flac");
  writeText(temp.path() / "good.lrc", "[00:99.00]bad timestamp\n");
  auto reader = std::make_shared<FakeServiceMetadataReader>();
  reader->put(good, rawMetadata("Good", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded good"}}));
  reader->fail(broken, "broken metadata");
  auto service = makeService(temp, reader);
  std::vector<ScannerEvent> events;
  service->setEventSink([&events](ScannerEvent event) { events.push_back(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  auto songs = songsIn(service->snapshot());
  auto errors = errorsFrom(events);

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "Good");
  CHECK(songs[0].effectiveLyricsSource == LyricsSource::EmbeddedTag);
  CHECK(reader->readCount() == 2U);
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.code == ScannerErrorCode::MetadataReadFailed; }));
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.message == "failed to parse external lyrics"; }));

  service->stop();
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  songs = songsIn(service->snapshot());
  errors = errorsFrom(events);

  REQUIRE(songs.size() == 1U);
  CHECK(songs[0].title == "Good");
  CHECK(std::ranges::any_of(errors, [](const ScannerError& error) { return error.code == ScannerErrorCode::Cancelled; }));
}

}
}
