#include "seriona/scanner/file_scanner_service.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
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

  seriona::scanner::ScannerEventSink sink_{};
  seriona::scanner::ScannerConfig config_{};
  std::vector<seriona::scanner::ScannerRoot> roots_{};
  std::vector<seriona::scanner::ScannerRoot> watchedRoots_{};
  seriona::scanner::ScanMode mode_{seriona::scanner::ScanMode::Incremental};
  seriona::scanner::PlaylistTreeSnapshot snapshot_{};
  bool watchingStopped_{false};
  bool stopped_{false};
};

}

TEST_CASE("scanner public contracts use standard-library value types") {
  using namespace seriona::scanner;

  static_assert(std::is_same_v<decltype(ScannerConfig::progressInterval), std::chrono::milliseconds>);
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

  CHECK(metadata.effectiveLyricsSource == LyricsSource::ExternalLrc);
  CHECK(metadata.effectiveLyrics.size() == 1);
  CHECK(metadata.externalLyricsPath == std::filesystem::path{"song.lrc"});
  CHECK(metadata.externalLyricsHash == "hash");
  CHECK(metadata.externalLyricsMtime.has_value());
  CHECK(metadata.sourceFilePath == std::filesystem::path{"disc.flac"});
  CHECK(metadata.offset == std::chrono::milliseconds{30000});
  CHECK(metadata.duration == std::chrono::milliseconds{180000});
  CHECK(metadata.logicalTrackId == "disc.flac#track-01");
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
