#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"
#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {
namespace fs = std::filesystem;

// Records every read/readCueSheet request (including the cover options) so a
// single fake proves the scanner-wide ThumbnailOnly + Ignore policy.
class RecordingMetadataReader final : public TagMetadataReader {
public:
  void put(fs::path path, RawTagMetadata metadata) {
    std::lock_guard lock{mutex_};
    metadataByPath_[std::move(path)] = std::move(metadata);
  }

  void putCueTracks(fs::path cuePath, std::vector<RawTagMetadata> tracks) {
    std::lock_guard lock{mutex_};
    cueTracksByPath_[std::move(cuePath)] = std::move(tracks);
  }

  void failWithCoverError(fs::path path, std::vector<RawTagLyricLine> lyrics = {}) {
    std::lock_guard lock{mutex_};
    coverFailureLyrics_[std::move(path)] = std::move(lyrics);
  }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    {
      std::lock_guard lock{mutex_};
      requests.push_back(request);
    }
    std::vector<RawTagLyricLine> failureLyrics;
    {
      std::lock_guard lock{mutex_};
      const auto failure = coverFailureLyrics_.find(request.path);
      if (failure != coverFailureLyrics_.end()) {
        failureLyrics = failure->second;
      }
    }
    if (!failureLyrics.empty() || wasMarkedForCoverFailure(request.path)) {
      if (request.options.failurePolicy == CoverProcessingOptions::CoverFailurePolicy::Propagate) {
        throw CoverProcessingError{CoverErrorCode::ExportDirectoryUnavailable, "cover export directory unavailable",
                                    request.path};
      }
      RawTagMetadata noArt{};
      noArt.filePath = request.path;
      noArt.title = "Coverless Track";
      noArt.artist = "Artist";
      noArt.album = "Album";
      noArt.duration = std::chrono::milliseconds{90000};
      noArt.sampleRate = 44100;
      noArt.bitDepth = 16;
      noArt.channels = 2;
      noArt.embeddedLyrics = failureLyrics;
      return noArt;
    }
    std::lock_guard lock{mutex_};
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("missing fake metadata for: " + request.path.string());
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;
  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest& request) override {
    {
      std::lock_guard lock{mutex_};
      cueRequests.push_back(request);
    }
    std::lock_guard lock{mutex_};
    const auto iterator = cueTracksByPath_.find(request.path);
    if (iterator == cueTracksByPath_.end()) {
      return {};
    }
    return iterator->second;
  }

  [[nodiscard]] bool allRequestsThumbnailOnlyIgnore() const {
    std::lock_guard lock{mutex_};
    for (const auto& request : requests) {
      if (request.options.mode != CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly ||
          request.options.failurePolicy != CoverProcessingOptions::CoverFailurePolicy::Ignore) {
        return false;
      }
    }
    for (const auto& request : cueRequests) {
      if (request.options.mode != CoverProcessingOptions::CoverProcessingMode::ThumbnailOnly ||
          request.options.failurePolicy != CoverProcessingOptions::CoverFailurePolicy::Ignore) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::size_t readCount() const {
    std::lock_guard lock{mutex_};
    return requests.size();
  }

  [[nodiscard]] std::size_t cueReadCount() const {
    std::lock_guard lock{mutex_};
    return cueRequests.size();
  }

private:
  [[nodiscard]] bool wasMarkedForCoverFailure(const fs::path& path) const {
    std::lock_guard lock{mutex_};
    return coverFailureLyrics_.contains(path);
  }

  mutable std::mutex mutex_;
  std::map<fs::path, RawTagMetadata> metadataByPath_;
  std::map<fs::path, std::vector<RawTagMetadata>> cueTracksByPath_;
  std::map<fs::path, std::vector<RawTagLyricLine>> coverFailureLyrics_;
  std::vector<TagReadRequest> requests;
  std::vector<TagReadRequest> cueRequests;
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
  raw.format = "flac";
  return raw;
}

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeService(test::TempScannerRoot& temp,
                                                              std::shared_ptr<RecordingMetadataReader> reader) {
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

[[nodiscard]] PlaylistTreeSnapshot waitForSongs(const FileScannerService& service, std::size_t expectedCount) {
  for (auto attempts = 0; attempts < 2000; ++attempts) {
    auto snapshot = service.snapshot();
    if (songsIn(snapshot).size() == expectedCount) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return service.snapshot();
}

[[nodiscard]] fs::path canonicalRootPath(const fs::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    canonical = path.lexically_normal();
  }
  return canonical;
}

void forceNextScanIncrementalForCurrentTree(const test::TempScannerRoot& temp) {
  const auto rootPath = canonicalRootPath(temp.path());
  const auto treeHash = computeDirectoryTreeHash(rootPath);
  REQUIRE(treeHash.hash.has_value());
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{
      .databasePath = fs::path{temp.dbPath().generic_string() + ".scan-roots.sqlite"}}};
  auto scanRoot = sidecar.loadScanRoot(rootPath);
  REQUIRE(scanRoot.has_value());
  scanRoot->directoryTreeHash = *treeHash.hash;
  sidecar.updateScanRoot(*scanRoot);
}

TEST_CASE("lazy cover gateway requests thumbnail-only + ignore for full incremental and cue reads") {
  test::TempScannerRoot temp{std::string{"lazy-cover-gateway-policy"}};
  auto reader = std::make_shared<RecordingMetadataReader>();
  reader->put(temp.path() / "song-a.flac", rawMetadata("Song A"));
  auto cueOne = rawMetadata("Cue One", {RawTagLyricLine{std::chrono::microseconds{1500000}, "cue lyric one"}});
  cueOne.filePath = temp.path() / "song-b.flac";
  cueOne.offset = std::chrono::microseconds{0};
  auto cueTwo = rawMetadata("Cue Two");
  cueTwo.filePath = temp.path() / "song-b.flac";
  cueTwo.offset = std::chrono::microseconds{1500000};
  reader->putCueTracks(temp.path() / "album.cue", {cueOne, cueTwo});
  writeText(temp.path() / "song-a.flac", "audio bytes one");
  writeText(temp.path() / "album.cue",
            "TITLE \"Full\"\nPERFORMER \"Artist\"\nFILE \"song-b.flac\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Cue One\"\n"
            "  TRACK 02 AUDIO\n    TITLE \"Cue Two\"\n");

  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Full);
  const auto fullSnapshot = waitForSongs(*service, 3U);
  REQUIRE(songsIn(fullSnapshot).size() == 3U);

  REQUIRE(reader->readCount() >= 1U);
  REQUIRE(reader->cueReadCount() >= 1U);
  CHECK(reader->allRequestsThumbnailOnlyIgnore());

  writeText(temp.path() / "song-a.flac", "audio bytes changed");
  writeText(temp.path() / "album.cue",
            "TITLE \"Changed\"\nPERFORMER \"Artist\"\nFILE \"song-b.flac\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Cue One\"\n"
            "  TRACK 02 AUDIO\n    TITLE \"Cue Two\"\n");
  forceNextScanIncrementalForCurrentTree(temp);
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Incremental);
  const auto incrementalSnapshot = waitForSongs(*service, 3U);
  REQUIRE(songsIn(incrementalSnapshot).size() == 3U);

  CHECK(reader->readCount() >= 1U);
  CHECK(reader->cueReadCount() >= 2U);
  CHECK(reader->allRequestsThumbnailOnlyIgnore());
}

TEST_CASE("lazy cover gateway songs survive cover failures with lyrics under ignore") {
  test::TempScannerRoot temp{std::string{"lazy-cover-gateway-ignore"}};
  auto reader = std::make_shared<RecordingMetadataReader>();
  reader->put(temp.path() / "song-ok.flac", rawMetadata("Healthy Song"));
  reader->failWithCoverError(temp.path() / "song-broken.flac",
                             {RawTagLyricLine{std::chrono::microseconds{500000}, "kept lyric"}});
  writeText(temp.path() / "song-ok.flac", "audio ok");
  writeText(temp.path() / "song-broken.flac", "audio broken");

  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Full);
  const auto snapshot = waitForSongs(*service, 2U);
  REQUIRE(songsIn(snapshot).size() == 2U);

  const auto songs = songsIn(snapshot);
  const auto& broken = *std::ranges::find_if(songs, [](const SongMetadata& song) {
    return song.filePath.filename() == "song-broken.flac";
  });
  CHECK(broken.title == "Coverless Track");
  CHECK(broken.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(broken.effectiveLyrics.size() == 1U);
  CHECK(broken.effectiveLyrics[0].text == "kept lyric");
  CHECK(broken.artworkPath.value_or(fs::path{}) == fs::path{});
  CHECK(broken.thumbnailPath.value_or(fs::path{}) == fs::path{});
  CHECK(reader->allRequestsThumbnailOnlyIgnore());
}

TEST_CASE("lazy cover gateway cue tracks retain lyrics and timing through the gateway") {
  test::TempScannerRoot temp{std::string{"lazy-cover-gateway-cue"}};
  auto reader = std::make_shared<RecordingMetadataReader>();
  auto trackOne = rawMetadata("Cue One");
  trackOne.filePath = temp.path() / "song-a.flac";
  trackOne.offset = std::chrono::microseconds{0};
  trackOne.duration = std::chrono::microseconds{10000000};
  trackOne.embeddedLyrics = {RawTagLyricLine{std::chrono::microseconds{1000000}, "first line"},
                             RawTagLyricLine{std::chrono::microseconds{2000000}, "second line"}};
  auto trackTwo = rawMetadata("Cue Two");
  trackTwo.filePath = temp.path() / "song-a.flac";
  trackTwo.offset = std::chrono::microseconds{10000000};
  trackTwo.duration = std::chrono::microseconds{5000000};
  reader->putCueTracks(temp.path() / "album.cue", {trackOne, trackTwo});
  writeText(temp.path() / "song-a.flac", "audio bytes one");
  writeText(temp.path() / "album.cue",
            "TITLE \"Test Album\"\nPERFORMER \"Test Artist\"\nFILE \"song-a.flac\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Cue One\"\n  TRACK 02 AUDIO\n    TITLE \"Cue Two\"\n");

  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Full);
  const auto snapshot = waitForSongs(*service, 2U);
  REQUIRE(songsIn(snapshot).size() == 2U);

  REQUIRE(reader->cueReadCount() >= 1U);
  CHECK(reader->allRequestsThumbnailOnlyIgnore());

  const auto songs = songsIn(snapshot);
  const auto& first = *std::ranges::find_if(songs, [](const SongMetadata& song) {
    return song.title == "Cue One";
  });
  CHECK(first.offset == std::chrono::milliseconds{0});
  CHECK(first.duration == std::chrono::milliseconds{10000});
  CHECK(first.effectiveLyricsSource == LyricsSource::EmbeddedTag);
  REQUIRE(first.effectiveLyrics.size() == 2U);
  CHECK(first.effectiveLyrics[0].text == "first line");
  CHECK(first.effectiveLyrics[1].text == "second line");
  const auto& second = *std::ranges::find_if(songs, [](const SongMetadata& song) {
    return song.title == "Cue Two";
  });
  CHECK(second.offset == std::chrono::milliseconds{10000});
  CHECK(second.duration == std::chrono::milliseconds{5000});
}

// ---- Real TagReader end-to-end probes ------------------------------------

void writePcmWavFixture(const fs::path& path, const std::string& title) {
  constexpr std::uint32_t sampleRate = 44100;
  constexpr std::uint16_t channels = 1;
  constexpr std::uint16_t bits = 16;
  constexpr std::uint32_t dataBytes = sampleRate * channels * (bits / 8) / 5;
  std::vector<std::uint8_t> listData = {'I', 'N', 'F', 'O'};
  auto pushListChunk = [&listData](const char* id, const std::string& text) {
    const std::uint32_t size = static_cast<std::uint32_t>(text.size()) + 1;
    for (int i = 0; i < 4; ++i) {
      listData.push_back(static_cast<std::uint8_t>(id[i]));
    }
    listData.push_back(static_cast<std::uint8_t>(size & 0xFF));
    listData.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
    listData.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFF));
    listData.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFF));
    for (char c : text) {
      listData.push_back(static_cast<std::uint8_t>(c));
    }
    listData.push_back(0);
  };
  pushListChunk("INAM", title);
  if (listData.size() % 2U != 0U) {
    listData.push_back(0);
  }
  const std::uint32_t listSize = static_cast<std::uint32_t>(listData.size());
  std::vector<std::uint8_t> bytes;
  auto pushU32 = [&bytes](std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
  };
  bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
  pushU32(44 + listSize + dataBytes);
  bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
  bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
  pushU32(16);
  bytes.push_back(1);
  bytes.push_back(0);
  bytes.push_back(static_cast<std::uint8_t>(channels));
  bytes.push_back(0);
  pushU32(sampleRate);
  pushU32(sampleRate * channels * (bits / 8));
  bytes.push_back(static_cast<std::uint8_t>(channels * (bits / 8)));
  bytes.push_back(0);
  bytes.push_back(static_cast<std::uint8_t>(bits));
  bytes.push_back(0);
  bytes.insert(bytes.end(), {'L', 'I', 'S', 'T'});
  pushU32(listSize);
  bytes.insert(bytes.end(), listData.begin(), listData.end());
  bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
  pushU32(dataBytes);
  bytes.insert(bytes.end(), dataBytes, 0);
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeValidPng(const fs::path& path) {
  // 64x64 red PNG produced by ffmpeg: `ffmpeg -f lavfi -i color=c=red:s=64x64 -frames:v 1 good.png`.
  const unsigned char png[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x08, 0x02, 0x00, 0x00, 0x00, 0x25, 0x0B, 0xE6,
      0x89, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x4F, 0x25, 0xC4, 0xD6, 0x00, 0x00, 0x00, 0x60, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C,
      0xED, 0xCF, 0xC1, 0x09, 0x00, 0x20, 0x10, 0xC0, 0x30, 0x05, 0xF7, 0xDF, 0xF8, 0xC0, 0x21, 0x7C,
      0x04, 0xA1, 0x99, 0xA0, 0xDD, 0xB3, 0xFE, 0x76, 0x74, 0xC0, 0xAB, 0x06, 0xB4, 0x06, 0xB4, 0x06,
      0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06,
      0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06,
      0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06,
      0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x06, 0xB4, 0x0B, 0xD2, 0x6C, 0x01, 0xFB, 0x54, 0xA0,
      0x0F, 0xD3, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(sizeof(png)));
}

TEST_CASE("production tagreader gateway maps sidecar thumbnail to thumbnailPath and keeps artworkPath empty") {
  test::TempScannerRoot temp{std::string{"lazy-cover-production-sidecar"}};
  writePcmWavFixture(temp.path() / "base.wav", "Sidecar Wav");
  writeValidPng(temp.path() / "cover.png");

  ProductionTagMetadataReader reader;
  const auto raw = reader.read(thumbnailOnlyRequest(temp.path() / "base.wav", temp.path() / "covers"));

  CHECK(raw.title == "Sidecar Wav");
  CHECK(raw.coverPath.empty());
  CHECK_FALSE(raw.thumbnailPath.empty());
  CHECK(std::filesystem::exists(raw.thumbnailPath));

  const auto mapped = mapRawTagMetadata(raw, "hash", std::nullopt, false);
  CHECK(mapped.metadata.thumbnailPath.has_value());
  CHECK_FALSE(mapped.metadata.thumbnailPath->empty());
  CHECK(mapped.metadata.artworkPath.has_value());
  CHECK(mapped.metadata.artworkPath->empty());
}

TEST_CASE("production tagreader gateway songs survive no cover corrupt oversize and permission failures") {
  test::TempScannerRoot temp{std::string{"lazy-cover-production-survival"}};
  const auto audioPath = temp.path() / "base.wav";
  writePcmWavFixture(audioPath, "Survival Wav");
  ProductionTagMetadataReader reader;
  const auto request = [&](const fs::path& coverDir) {
    return thumbnailOnlyRequest(audioPath, coverDir);
  };

  SUBCASE("no cover") {
    const auto raw = reader.read(request(temp.path() / "covers-none"));
    CHECK(raw.title == "Survival Wav");
    CHECK(raw.coverPath.empty());
    CHECK(raw.thumbnailPath.empty());
  }

  SUBCASE("corrupt sidecar") {
    writeText(temp.path() / "cover.png", "not a real png");
    const auto raw = reader.read(request(temp.path() / "covers-corrupt"));
    CHECK(raw.title == "Survival Wav");
    CHECK(raw.coverPath.empty());
    CHECK(raw.thumbnailPath.empty());
  }

  SUBCASE("oversized sidecar") {
    std::filesystem::create_directory(temp.path() / "oversize");
    std::ofstream big{temp.path() / "oversize" / "cover.png", std::ios::binary};
    big.seekp(64 * 1024 * 1024 + 1);
    big.write("x", 1);
    big.close();
    std::filesystem::copy_file(audioPath, temp.path() / "oversize" / "base.wav",
                               std::filesystem::copy_options::overwrite_existing);
    const auto raw = reader.read(thumbnailOnlyRequest(temp.path() / "oversize" / "base.wav",
                                                      temp.path() / "covers-oversize"));
    CHECK(raw.title == "Survival Wav");
    CHECK(raw.coverPath.empty());
    CHECK(raw.thumbnailPath.empty());
  }

  SUBCASE("unavailable cover export directory") {
    std::filesystem::create_directory(temp.path() / "real-covers");
    std::filesystem::create_symlink(temp.path() / "real-covers", temp.path() / "link-covers");
    const auto raw = reader.read(request(temp.path() / "link-covers"));
    CHECK(raw.title == "Survival Wav");
    CHECK(raw.coverPath.empty());
    CHECK(raw.thumbnailPath.empty());
  }
}

TEST_CASE("production tagreader gateway reads cue sheet with timing under thumbnail-only ignore") {
  test::TempScannerRoot temp{std::string{"lazy-cover-production-cue"}};
  writePcmWavFixture(temp.path() / "base.wav", "Cue Wav");
  writeText(temp.path() / "album.cue",
            "TITLE \"Test Album\"\nPERFORMER \"Test Artist\"\nFILE \"base.wav\" WAVE\n"
            "  TRACK 01 AUDIO\n    TITLE \"Track One\"\n    INDEX 01 00:00:00\n"
            "  TRACK 02 AUDIO\n    TITLE \"Track Two\"\n    INDEX 01 00:00:10\n");

  ProductionTagMetadataReader reader;
  const auto tracks = reader.readCueSheet(thumbnailOnlyRequest(temp.path() / "album.cue", temp.path() / "covers"));

  REQUIRE(tracks.size() == 2U);
  CHECK(tracks[0].title == "Track One");
  CHECK(tracks[0].offset == std::chrono::microseconds{0});
  CHECK(tracks[1].title == "Track Two");
  CHECK(tracks[1].offset > std::chrono::microseconds{0});
  CHECK(tracks[1].offset == tracks[0].duration);
  CHECK(tracks[1].duration > std::chrono::microseconds{0});
}

[[nodiscard]] std::size_t countPngFilesUnder(const fs::path& root) {
  std::size_t count = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it{root, ec}, end; it != end; it.increment(ec)) {
    if (!ec && it->path().extension() == ".png") {
      ++count;
    }
  }
  return count;
}

TEST_CASE("scanner cold scan stores thumbnail and cache hit publishes identical metadata lyrics thumbnail") {
  test::TempScannerRoot temp{std::string{"lazy-cover-thumbnail-cache-hit"}};
  auto reader = std::make_shared<RecordingMetadataReader>();
  const auto audio = temp.path() / "song-a.flac";
  const auto thumbnail = temp.path() / "covers" / "thumbnails" / "ab" / "thumb.png";
  auto raw = rawMetadata("Thumbnail Song", {RawTagLyricLine{std::chrono::microseconds{1000000}, "cache hit lyric"}});
  raw.thumbnailPath = thumbnail;
  reader->put(audio, raw);
  writeText(audio, "audio bytes one");

  auto service = makeService(temp, reader);
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Full);
  const auto fullSnapshot = waitForSongs(*service, 1U);
  const auto fullSongs = songsIn(fullSnapshot);
  REQUIRE(fullSongs.size() == 1U);
  CHECK(fullSongs[0].title == "Thumbnail Song");
  CHECK(fullSongs[0].thumbnailPath.value_or(fs::path{}) == thumbnail);
  CHECK(fullSongs[0].artworkPath.value_or(fs::path{}) == fs::path{});
  REQUIRE(fullSongs[0].effectiveLyrics.size() == 1U);
  CHECK(fullSongs[0].effectiveLyrics[0].text == "cache hit lyric");
  const auto readCountAfterFull = reader->readCount();
  REQUIRE(readCountAfterFull == 1U);

  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Incremental);
  const auto hitSnapshot = waitForSongs(*service, 1U);
  const auto hitSongs = songsIn(hitSnapshot);
  REQUIRE(hitSongs.size() == 1U);
  CHECK(hitSongs[0].title == "Thumbnail Song");
  CHECK(hitSongs[0].thumbnailPath.value_or(fs::path{}) == thumbnail);
  CHECK(hitSongs[0].artworkPath.value_or(fs::path{}) == fs::path{});
  REQUIRE(hitSongs[0].effectiveLyrics.size() == 1U);
  CHECK(hitSongs[0].effectiveLyrics[0].text == "cache hit lyric");
  CHECK(reader->readCount() == readCountAfterFull);

  CHECK(countPngFilesUnder(temp.path() / "covers") == 0U);
}

TEST_CASE("scanner with production reader exports only thumbnail and cache hit preserves it") {
  test::TempScannerRoot temp{std::string{"lazy-cover-thumbnail-only-export"}};
  writePcmWavFixture(temp.path() / "base.wav", "Thumbnail Only Wav");
  writeValidPng(temp.path() / "cover.png");

  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::make_shared<ProductionTagMetadataReader>(),
                                                                       .watcherFactory = nullptr,
                                                                       .databasePath = temp.dbPath(),
                                                                       .coverExportDir = temp.path() / "covers"});
  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Full);
  const auto fullSnapshot = waitForSongs(*service, 1U);
  const auto fullSongs = songsIn(fullSnapshot);
  REQUIRE(fullSongs.size() == 1U);
  REQUIRE(fullSongs[0].thumbnailPath.has_value());
  const auto thumbnail = *fullSongs[0].thumbnailPath;
  CHECK_FALSE(thumbnail.empty());
  CHECK(std::filesystem::exists(thumbnail));
  CHECK(fullSongs[0].artworkPath.value_or(fs::path{}) == fs::path{});

  const auto covers = temp.path() / "covers";
  CHECK(countPngFilesUnder(covers) == 1U);
  std::error_code ec;
  std::vector<fs::path> pngs;
  for (std::filesystem::recursive_directory_iterator it{covers, ec}, end; it != end; it.increment(ec)) {
    if (!ec && it->path().extension() == ".png") {
      pngs.push_back(it->path());
    }
  }
  REQUIRE(pngs.size() == 1U);
  CHECK(fs::relative(pngs[0], covers).generic_string().find("thumbnails") == 0U);

  service->scan({ScannerRoot{.path = temp.path(), .recursive = true}}, ScanMode::Incremental);
  const auto hitSnapshot = waitForSongs(*service, 1U);
  const auto hitSongs = songsIn(hitSnapshot);
  REQUIRE(hitSongs.size() == 1U);
  CHECK(hitSongs[0].thumbnailPath.value_or(fs::path{}) == thumbnail);
  CHECK(hitSongs[0].artworkPath.value_or(fs::path{}) == fs::path{});
  CHECK(countPngFilesUnder(covers) == 1U);
}

}  // namespace
}  // namespace seriona::scanner
