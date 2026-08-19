#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

class CapturingScannerService final : public seriona::scanner::FileScannerService {
public:
  void setEventSink(seriona::scanner::ScannerEventSink sink) override { sink_ = std::move(sink); }

  void configure(const seriona::scanner::ScannerConfig& config) override { config_ = config; }

  void scan(const std::vector<seriona::scanner::ScannerRoot>& roots,
            seriona::scanner::ScanMode mode) override {
    roots_ = roots;
    mode_ = mode;
  }

  void startWatching(const std::vector<seriona::scanner::ScannerRoot>& roots) override { watchedRoots_ = roots; }

  void stopWatching() override { watchingStopped_ = true; }

  void stop() override { stopped_ = true; }

  [[nodiscard]] seriona::scanner::PlaylistTreeSnapshot snapshot() const override { return snapshot_; }

  bool removeLocation(const std::filesystem::path& path) override {
    removedPaths_.push_back(path);
    return removeLocationResult_;
  }

  seriona::scanner::ScannerEventSink sink_{};
  seriona::scanner::ScannerConfig config_{};
  std::vector<seriona::scanner::ScannerRoot> roots_{};
  std::vector<seriona::scanner::ScannerRoot> watchedRoots_{};
  seriona::scanner::ScanMode mode_{seriona::scanner::ScanMode::Incremental};
  seriona::scanner::PlaylistTreeSnapshot snapshot_{};
  std::vector<std::filesystem::path> removedPaths_{};
  bool removeLocationResult_{true};
  bool watchingStopped_{false};
  bool stopped_{false};
};

template <typename T>
concept HasCachedSongMember = requires(T mapped) { mapped.cachedSong; };

}

TEST_CASE("scanner public contracts use standard-library value types") {
  using namespace seriona::scanner;

  static_assert(std::is_same_v<decltype(ScannerConfig::progressInterval), std::chrono::milliseconds>);
  static_assert(std::is_same_v<decltype(ScannerConfig::workerCount), std::size_t>);
  static_assert(std::is_same_v<decltype(ScannerConfig::tagReaderConcurrency), std::ptrdiff_t>);
  static_assert(std::is_same_v<decltype(ScanProgress::elapsed), std::chrono::milliseconds>);
  static_assert(std::is_same_v<decltype(ScannerEvent::timestamp), std::chrono::steady_clock::time_point>);
  static_assert(std::is_same_v<decltype(SongMetadata::offset), std::optional<std::chrono::milliseconds>>);
  static_assert(std::is_same_v<decltype(SongMetadata::duration), std::optional<std::chrono::milliseconds>>);
  static_assert(std::is_same_v<decltype(SongMetadata::externalLyricsMtime),
                               std::optional<std::filesystem::file_time_type>>);

  SongMetadata metadata{};
  metadata.effectiveLyricsSource = LyricsSource::ExternalLrc;
  metadata.effectiveLyrics = {LyricLine{std::chrono::milliseconds{1200}, "line"}};
  metadata.externalLyricsPath = std::filesystem::path{"song.lrc"};
  metadata.externalLyricsHash = "hash";
  metadata.externalLyricsMtime = std::filesystem::file_time_type{};
  metadata.sourceFilePath = std::filesystem::path{"disc.flac"};
  metadata.offset = std::chrono::milliseconds{30000};
  metadata.duration = std::chrono::milliseconds{180000};
  metadata.logicalTrackId = "disc.flac#track-01";
  metadata.artworkPath = std::filesystem::path{"covers/disc.png"};

  CHECK(metadata.effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(metadata.effectiveLyrics.size() == 1);
  CHECK(metadata.externalLyricsPath == std::filesystem::path{"song.lrc"});
  CHECK(metadata.externalLyricsHash == "hash");
  CHECK(metadata.externalLyricsMtime.has_value());
  CHECK(metadata.sourceFilePath == std::filesystem::path{"disc.flac"});
  CHECK(metadata.offset == std::chrono::milliseconds{30000});
  CHECK(metadata.duration == std::chrono::milliseconds{180000});
  CHECK(metadata.logicalTrackId == "disc.flac#track-01");
  CHECK(metadata.artworkPath == std::filesystem::path{"covers/disc.png"});
}

TEST_CASE("scanner contract defaults are explicit and dependency-free") {
  using namespace seriona::scanner;

  const ScannerConfig config{};
  CHECK(config.progressInterval == std::chrono::milliseconds{250});
  CHECK(config.allowedExtensions.empty());
  CHECK_FALSE(config.followSymlinks);
  CHECK(config.readEmbeddedLyrics);
  CHECK(config.readExternalLyrics);
  CHECK(config.workerCount == 0U);
  CHECK(config.tagReaderConcurrency == 0);
  CHECK(config.enableIncrementalScan);
  CHECK_FALSE(config.forceFull);

  const SongMetadata metadata{};
  CHECK(metadata.effectiveLyricsSource == LyricsSource::None);
  CHECK(metadata.effectiveLyrics.empty());
  CHECK_FALSE(metadata.externalLyricsPath.has_value());
  CHECK_FALSE(metadata.externalLyricsHash.has_value());
  CHECK_FALSE(metadata.externalLyricsMtime.has_value());
  CHECK(metadata.sourceFilePath.empty());
  CHECK_FALSE(metadata.offset.has_value());
  CHECK_FALSE(metadata.duration.has_value());
  CHECK(metadata.logicalTrackId.empty());
  CHECK_FALSE(metadata.artworkPath.has_value());
}

TEST_CASE("playlist nodes distinguish structural and track-like kinds") {
  using namespace seriona::scanner;

  PlaylistNode root{};
  root.nodeId = "root";
  root.kind = PlaylistNodeKind::Root;
  root.displayName = "Library";

  PlaylistNode directory{};
  directory.nodeId = "dir";
  directory.parentNodeId = root.nodeId;
  directory.kind = PlaylistNodeKind::Directory;
  directory.displayName = "Music";

  PlaylistNode track{};
  track.nodeId = "track";
  track.parentNodeId = directory.nodeId;
  track.kind = PlaylistNodeKind::Track;
  track.displayName = "Song";

  CHECK(root.kind == PlaylistNodeKind::Root);
  CHECK(directory.kind == PlaylistNodeKind::Directory);
  CHECK(track.kind == PlaylistNodeKind::Track);
  CHECK(root.kind != directory.kind);
  CHECK(directory.kind != track.kind);
  CHECK_FALSE(root.parentNodeId.has_value());
  REQUIRE(directory.parentNodeId.has_value());
  CHECK(*directory.parentNodeId == root.nodeId);
  REQUIRE(track.parentNodeId.has_value());
  CHECK(*track.parentNodeId == directory.nodeId);

  // node-level thumbnailPath：默认空；仅 Directory 节点使用，填充语义由 resolver（任务 6/7）落实
  CHECK_FALSE(root.thumbnailPath.has_value());
  CHECK_FALSE(directory.thumbnailPath.has_value());
  CHECK_FALSE(track.thumbnailPath.has_value());
}

TEST_CASE("playlist node thumbnail path is settable without touching other fields") {
  using namespace seriona::scanner;

  PlaylistNode directory{};
  directory.kind = PlaylistNodeKind::Directory;
  directory.thumbnailPath = "folder.png";
  CHECK(directory.thumbnailPath.has_value());
  CHECK(*directory.thumbnailPath == "folder.png");
  CHECK_FALSE(directory.song.has_value());
  CHECK(directory.childNodeIds.empty());
}

TEST_CASE("scanner events keep declared type and payload alternative consistent") {
  using namespace seriona::scanner;

  ScanProgress progress{};
  progress.filesDiscovered = 3;
  progress.filesScanned = 2;
  ScannerEvent progressEvent{};
  progressEvent.type = ScannerEventType::ProgressUpdated;
  progressEvent.payload = progress;
  CHECK(progressEvent.type == ScannerEventType::ProgressUpdated);
  REQUIRE(std::holds_alternative<ScanProgress>(progressEvent.payload));
  CHECK(std::get<ScanProgress>(progressEvent.payload).filesDiscovered == 3);
  CHECK(std::get<ScanProgress>(progressEvent.payload).filesScanned == 2);

  PlaylistTreeSnapshot snapshot{};
  snapshot.version = 7;
  ScannerEvent snapshotEvent{};
  snapshotEvent.type = ScannerEventType::PlaylistSnapshotUpdated;
  snapshotEvent.payload = snapshot;
  CHECK(snapshotEvent.type == ScannerEventType::PlaylistSnapshotUpdated);
  REQUIRE(std::holds_alternative<PlaylistTreeSnapshot>(snapshotEvent.payload));
  CHECK(std::get<PlaylistTreeSnapshot>(snapshotEvent.payload).version == 7);

  SongMetadata song{};
  song.trackId = "song-1";
  song.title = "Song";
  ScannerEvent songEvent{};
  songEvent.type = ScannerEventType::FileScanned;
  songEvent.payload = song;
  CHECK(songEvent.type == ScannerEventType::FileScanned);
  REQUIRE(std::holds_alternative<SongMetadata>(songEvent.payload));
  CHECK(std::get<SongMetadata>(songEvent.payload).trackId == "song-1");
  CHECK(std::get<SongMetadata>(songEvent.payload).title == "Song");

  ScannerError error{};
  error.code = ScannerErrorCode::PermissionDenied;
  error.message = "denied";
  ScannerEvent errorEvent{};
  errorEvent.type = ScannerEventType::ScanError;
  errorEvent.payload = error;
  CHECK(errorEvent.type == ScannerEventType::ScanError);
  REQUIRE(std::holds_alternative<ScannerError>(errorEvent.payload));
  CHECK(std::get<ScannerError>(errorEvent.payload).code == ScannerErrorCode::PermissionDenied);
  CHECK(std::get<ScannerError>(errorEvent.payload).message == "denied");
}

TEST_CASE("scanner service facade forwards to an injected service") {
  using namespace seriona::scanner;

  auto service = std::make_shared<CapturingScannerService>();
  FileScanner facade{service};

  auto eventCount = 0;
  facade.setEventSink([&eventCount](ScannerEvent) { ++eventCount; });
  facade.configure(ScannerConfig{.progressInterval = std::chrono::milliseconds{250}});
  facade.scan({ScannerRoot{.path = std::filesystem::path{"music"}, .recursive = false}}, ScanMode::Full);
  facade.startWatching({ScannerRoot{.path = std::filesystem::path{"watched"}, .recursive = true}});
  facade.stopWatching();
  facade.stop();

  REQUIRE(service->sink_);
  service->sink_(ScannerEvent{.type = ScannerEventType::ScanStarted});

  CHECK(eventCount == 1);
  CHECK(service->config_.progressInterval == std::chrono::milliseconds{250});
  REQUIRE(service->roots_.size() == 1);
  CHECK(service->roots_[0].path == std::filesystem::path{"music"});
  CHECK_FALSE(service->roots_[0].recursive);
  CHECK(service->mode_ == ScanMode::Full);
  REQUIRE(service->watchedRoots_.size() == 1);
  CHECK(service->watchedRoots_[0].path == std::filesystem::path{"watched"});
  CHECK(service->watchingStopped_);
  CHECK(service->stopped_);
}

TEST_CASE("scanner factory is declared with service ownership") {
  using namespace seriona::scanner;

  static_assert(std::is_same_v<decltype(makeFileScannerService()), std::shared_ptr<FileScannerService>>);
}

TEST_CASE("tag reader adapter public boundary does not expose sqlite cache dto types") {
  using namespace seriona::scanner;

  static_assert(!HasCachedSongMember<MappedTagMetadata>);
  static_assert(std::is_same_v<decltype(MappedTagMetadata::metadata), SongMetadata>);
  static_assert(std::is_same_v<decltype(MappedTagMetadata::embeddedLyrics), std::vector<LyricLine>>);
}
