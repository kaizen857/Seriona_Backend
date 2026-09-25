// 轨 C 收尾（todo 22）端到端集成测试：把 G5 的 4 个新歌词格式与 D22 的同时间戳参照行
// 放进**一条真实扫描链**里验证 —— 发现 → 侧车决议 → 解析 → 生效歌词 → 缓存往返，
// 而不是逐个调用解析函数（那是单元测试的活：scanner_paths_tests / scanner_service_tests）。
//
// 组合要素（同一曲库目录内同时出现）：
//   · `.lrc` 与 `.srt` 并存 ⇒ `.lrc` 胜出（确定性优先级）
//   · `[offset:+ms]` 生效（时间戳被平移）
//   · 逐字标签 `<mm:ss.xx>` 被剥离
//   · 行内元数据 `[ar:]`/`[ti:]` 与制作人员行被丢弃
//   · 同时间戳双行都存活，且 scanner::groupLyricReferenceLines 给出「第 1 行 = 原文」
//   · `.srt` 内容替换后重扫被判为歌词变更（分别走 watcher 的 lyricsTouched 与增量对账）
#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/lyric_reference_pairing.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <doctest.h>

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
#include <thread>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

constexpr auto kEventWaitBudget = std::chrono::milliseconds{5000};

class FakeE2eMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    const auto iterator = metadataByPath_.find(request.path);
    if (iterator == metadataByPath_.end()) {
      throw std::runtime_error("unexpected metadata read for e2e fixture");
    }
    auto metadata = iterator->second;
    metadata.filePath = request.path;
    return metadata;
  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

private:
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
};

class CapturedE2eWatcher final : public FolderWatcher {
public:
  struct State {
    std::filesystem::path root;
    WatchEventCallback callback;
    bool closed{false};
  };

  explicit CapturedE2eWatcher(std::shared_ptr<State> state) : state_(std::move(state)) {}

  void close() noexcept override { state_->closed = true; }

  void emit(const WatchEvent& event) {
    if (!state_->closed) {
      state_->callback(event);
    }
  }

  [[nodiscard]] std::shared_ptr<State> state() const noexcept { return state_; }

private:
  std::shared_ptr<State> state_;
};

class CapturingE2eWatcherFactory final : public FolderWatcherFactory {
public:
  [[nodiscard]] std::unique_ptr<FolderWatcher> watch(const std::filesystem::path& root,
                                                     WatchEventCallback callback) override {
    auto state = std::make_shared<CapturedE2eWatcher::State>();
    state->root = root;
    state->callback = std::move(callback);
    states.push_back(state);
    return std::make_unique<CapturedE2eWatcher>(std::move(state));
  }

  std::vector<std::shared_ptr<CapturedE2eWatcher::State>> states;
};

[[nodiscard]] RawTagMetadata rawMetadata(std::string title,
                                         std::vector<RawTagLyricLine> lyrics = {},
                                         std::optional<std::chrono::milliseconds> offset = std::nullopt) {
  RawTagMetadata raw{};
  raw.title = std::move(title);
  raw.artist = "Artist";
  raw.album = "Album";
  raw.embeddedLyrics = std::move(lyrics);
  raw.duration = std::chrono::milliseconds{120000};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.channels = 2;
  if (offset.has_value()) {
    raw.offset = *offset;
  }
  return raw;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] std::shared_ptr<FileScannerService> makeE2eService(
    test::TempScannerRoot& temp,
    std::shared_ptr<FakeE2eMetadataReader> reader,
    std::shared_ptr<CapturingE2eWatcherFactory> watchers = nullptr) {
  return makeFileScannerService(FileScannerServiceDependencies{.metadataReader = std::move(reader),
                                                               .watcherFactory = std::move(watchers),
                                                               .databasePath = temp.dbPath(),
                                                               .coverExportDir = temp.path() / "covers"});
}

class ScanEventLog {
public:
  void push(ScannerEvent event) {
    std::lock_guard lock{mutex_};
    events_.push_back(std::move(event));
    changed_.notify_all();
  }

  [[nodiscard]] bool waitForCount(ScannerEventType type, std::size_t expectedCount,
                                  std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, type, expectedCount] {
      return static_cast<std::size_t>(std::ranges::count(events_, type, &ScannerEvent::type)) >= expectedCount;
    });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<ScannerEvent> events_;
};

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

// 按值返回：songsIn 的向量是本函数的局部量，返回其元素的引用会悬垂（-Wdangling-reference）。
[[nodiscard]] SongMetadata songForPath(const PlaylistTreeSnapshot& snapshot,
                                      const std::filesystem::path& filePath) {
  const auto songs = songsIn(snapshot);
  const auto iterator = std::ranges::find(songs, filePath, &SongMetadata::filePath);
  REQUIRE(iterator != songs.end());
  return *iterator;
}

[[nodiscard]] std::filesystem::path scannerSidecarPath(const test::TempScannerRoot& temp) {
  return std::filesystem::path{temp.dbPath().generic_string() + ".scan-roots.sqlite"};
}

[[nodiscard]] std::filesystem::path canonicalRootPath(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    canonical = path.lexically_normal();
  }
  return canonical;
}

[[nodiscard]] cache::CachedLocation cachedLocationForPath(cache::SQLiteCache& cache,
                                                          const std::filesystem::path& rootPath,
                                                          const std::filesystem::path& filePath) {
  const auto locations = cache.loadLocationsByRoot(rootPath);
  const auto iterator = std::ranges::find(locations, filePath, &cache::CachedLocation::filePath);
  if (iterator == locations.end()) {
    throw std::runtime_error("missing cached location for lyrics formats e2e test");
  }
  return *iterator;
}

[[nodiscard]] WatchEvent fileEvent(std::filesystem::path path, WatchEffectKind effect) {
  return WatchEvent{.path = std::move(path), .pathKind = WatchPathKind::File, .effectKind = effect, .associated = {}};
}

// 组合侧车：一条 `.lrc` 里同时有 offset、逐字标签、元数据行、制作人员行、同时间戳双行。
constexpr std::string_view kCombinedLrc =
    "[ar:Someone]\n"
    "[ti:Priority Title]\n"
    "[offset:+1000]\n"
    "[00:01.00]\xE4\xBD\x9C\xE8\xAF\x8D : \xE3\x81\xBF\xE3\x82\x85\xE3\x83\xBC\n"  // 作词 : みゅー
    "[00:05.00]<00:05.00>\xE5\x8E\x9F\xE6\x96\x87\x41\n"                              // <00:05.00>原文A
    "[00:05.00]\xE8\xAF\x91\xE6\x96\x87\x42\n"                                        // 译文B
    "[00:09.00]\xE6\x9C\xAB\xE8\xA1\x8C\n";                                            // 末行

}  // namespace

TEST_CASE("scanner lyrics formats e2e combines priority offset inline timestamps metadata lines and pairing") {
  test::TempScannerRoot temp{"scanner-lyrics-formats-combined"};
  const auto priorityAudio = test::writeAudioFixture(temp.path(), "priority.flac");

  writeText(temp.path() / "priority.lrc", std::string{kCombinedLrc});
  // 同 basename 的 `.srt` 内容不同：若优先级失效，生效行文本会变成下面的字符串。
  writeText(temp.path() / "priority.srt", "1\n00:00:01,000 --> 00:00:02,000\nsrt must lose\n");

  auto reader = std::make_shared<FakeE2eMetadataReader>();
  reader->put(priorityAudio, rawMetadata("Priority"));
  auto service = makeE2eService(temp, reader);
  ScanEventLog events;
  service->setEventSink([&events](ScannerEvent event) { events.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 1U, kEventWaitBudget));

  const auto song = songForPath(service->snapshot(), priorityAudio);
  CHECK(song.effectiveLyricsSource == LyricsSource::ExternalLrc);

  // offset +1000 ⇒ 解析出的 5000/5000/9000 变为 4000/4000/8000；元数据行与制作人员行被丢弃。
  REQUIRE(song.effectiveLyrics.size() == 3U);
  CHECK(song.effectiveLyrics[0].timestamp == std::chrono::milliseconds{4000});
  CHECK(song.effectiveLyrics[0].text == "\xE5\x8E\x9F\xE6\x96\x87\x41");
  CHECK(song.effectiveLyrics[1].timestamp == std::chrono::milliseconds{4000});
  CHECK(song.effectiveLyrics[1].text == "\xE8\xAF\x91\xE6\x96\x87\x42");
  CHECK(song.effectiveLyrics[2].timestamp == std::chrono::milliseconds{8000});
  CHECK(song.effectiveLyrics[2].text == "\xE6\x9C\xAB\xE8\xA1\x8C");

  // 逐字标签被剥离：文本里不残留字面量。
  for (const auto& line : song.effectiveLyrics) {
    CHECK(line.text.find('<') == std::string::npos);
    CHECK(line.text.find('>') == std::string::npos);
    CHECK(line.text.find("[ar:") == std::string::npos);
    CHECK(line.text.find("[ti:") == std::string::npos);
  }

  // 同时间戳双行：都存活，且配对给出「第 1 行（原文A）= 原文」、其余为候选译文。
  const auto groups = groupLyricReferenceLines(song.effectiveLyrics);
  REQUIRE(groups.size() == 1U);
  CHECK(groups[0].timestamp == std::chrono::milliseconds{4000});
  CHECK(groups[0].originalIndex == 0U);
  CHECK(song.effectiveLyrics[groups[0].originalIndex].text == "\xE5\x8E\x9F\xE6\x96\x87\x41");
  REQUIRE(groups[0].translationIndexes.size() == 1U);
  CHECK(groups[0].translationIndexes[0] == 1U);
  CHECK(song.effectiveLyrics[groups[0].translationIndexes[0]].text == "\xE8\xAF\x91\xE6\x96\x87\x42");

  // 缓存往返：来源与行数按 external 槽落库并读回。
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto location = cachedLocationForPath(sidecar, canonicalRootPath(temp.path()), priorityAudio);
  CHECK(location.lyricsSource == LyricsSource::ExternalLrc);
  CHECK(sidecar.loadLyrics(location.locationId, "external").size() == 3U);
}

TEST_CASE("scanner lyrics formats e2e routes each sidecar extension to its own parser end to end") {
  test::TempScannerRoot temp{"scanner-lyrics-formats-routing"};
  const auto srtAudio = test::writeAudioFixture(temp.path(), "srt.flac");
  const auto assAudio = test::writeAudioFixture(temp.path(), "ass.flac");
  const auto ttmlAudio = test::writeAudioFixture(temp.path(), "ttml.flac");
  const auto txtAudio = test::writeAudioFixture(temp.path(), "txt.flac");

  writeText(temp.path() / "srt.srt", "1\n00:00:01,000 --> 00:00:02,000\nsrt body\n");
  writeText(temp.path() / "ass.ass",
            "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,ass body\n");
  writeText(temp.path() / "ttml.ttml", "<tt><body><p begin=\"3s\">ttml body</p></body></tt>\n");
  writeText(temp.path() / "txt.txt", "txt body one\ntxt body two\n");

  auto reader = std::make_shared<FakeE2eMetadataReader>();
  reader->put(srtAudio, rawMetadata("Srt", {RawTagLyricLine{std::chrono::milliseconds{100}, "srt embedded"}}));
  reader->put(assAudio, rawMetadata("Ass", {RawTagLyricLine{std::chrono::milliseconds{100}, "ass embedded"}}));
  reader->put(ttmlAudio, rawMetadata("Ttml", {RawTagLyricLine{std::chrono::milliseconds{100}, "ttml embedded"}}));
  reader->put(txtAudio, rawMetadata("Txt", {RawTagLyricLine{std::chrono::milliseconds{100}, "txt embedded"}}));
  auto service = makeE2eService(temp, reader);
  ScanEventLog events;
  service->setEventSink([&events](ScannerEvent event) { events.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 1U, kEventWaitBudget));

  const auto snapshot = service->snapshot();
  const auto srtSong = songForPath(snapshot, srtAudio);
  CHECK(srtSong.effectiveLyricsSource == LyricsSource::ExternalSrt);
  REQUIRE(srtSong.effectiveLyrics.size() == 1U);
  CHECK(srtSong.effectiveLyrics[0].text == "srt body");
  CHECK(srtSong.effectiveLyrics[0].timestamp == std::chrono::milliseconds{1000});

  const auto assSong = songForPath(snapshot, assAudio);
  CHECK(assSong.effectiveLyricsSource == LyricsSource::ExternalAss);
  REQUIRE(assSong.effectiveLyrics.size() == 1U);
  CHECK(assSong.effectiveLyrics[0].text == "ass body");
  CHECK(assSong.effectiveLyrics[0].timestamp == std::chrono::milliseconds{2000});

  const auto ttmlSong = songForPath(snapshot, ttmlAudio);
  CHECK(ttmlSong.effectiveLyricsSource == LyricsSource::ExternalTtml);
  REQUIRE(ttmlSong.effectiveLyrics.size() == 1U);
  CHECK(ttmlSong.effectiveLyrics[0].text == "ttml body");
  CHECK(ttmlSong.effectiveLyrics[0].timestamp == std::chrono::milliseconds{3000});

  const auto txtSong = songForPath(snapshot, txtAudio);
  CHECK(txtSong.effectiveLyricsSource == LyricsSource::ExternalText);
  REQUIRE(txtSong.effectiveLyrics.size() == 2U);
  CHECK(txtSong.effectiveLyrics[0].text == "txt body one");
  CHECK(txtSong.effectiveLyrics[1].text == "txt body two");

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto rootPath = canonicalRootPath(temp.path());
  for (const auto& [audio, expectedSource] : std::vector<std::pair<std::filesystem::path, LyricsSource>>{
           {srtAudio, LyricsSource::ExternalSrt},
           {assAudio, LyricsSource::ExternalAss},
           {ttmlAudio, LyricsSource::ExternalTtml},
           {txtAudio, LyricsSource::ExternalText}}) {
    CAPTURE(audio.generic_string());
    const auto location = cachedLocationForPath(sidecar, rootPath, audio);
    CHECK(location.lyricsSource == expectedSource);
    CHECK_FALSE(sidecar.loadLyrics(location.locationId, "external").empty());
  }
}

TEST_CASE("scanner lyrics formats e2e reclassifies a replaced srt sidecar through the watcher") {
  test::TempScannerRoot temp{"scanner-lyrics-formats-srt-watch"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  const auto srt = temp.path() / "song.srt";

  auto reader = std::make_shared<FakeE2eMetadataReader>();
  reader->put(audio, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto watchers = std::make_shared<CapturingE2eWatcherFactory>();
  auto service = makeE2eService(temp, reader, watchers);
  ScanEventLog events;
  service->setEventSink([&events](ScannerEvent event) { events.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 1U, kEventWaitBudget));
  service->startWatching({ScannerRoot{.path = temp.path()}});
  REQUIRE(watchers->states.size() == 1U);

  // 新建 `.srt`：watcher 判为歌词侧车变更（lyricsTouched），生效歌词切到 external。
  writeText(srt, "1\n00:00:02,000 --> 00:00:03,000\nfirst srt\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  watchers->states[0]->callback(fileEvent(srt, WatchEffectKind::Modified));
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 2U, kEventWaitBudget));
  {
    const auto song = songForPath(service->snapshot(), audio);
    CHECK(song.effectiveLyricsSource == LyricsSource::ExternalSrt);
    REQUIRE(song.effectiveLyrics.size() == 1U);
    CHECK(song.effectiveLyrics[0].text == "first srt");
  }

  // 替换内容：再次判为歌词变更，生效歌词更新为第二版。
  writeText(srt, "1\n00:00:04,000 --> 00:00:05,000\nsecond srt\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{3});
  watchers->states[0]->callback(fileEvent(srt, WatchEffectKind::Modified));
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 3U, kEventWaitBudget));
  const auto replaced = songForPath(service->snapshot(), audio);
  CHECK(replaced.effectiveLyricsSource == LyricsSource::ExternalSrt);
  REQUIRE(replaced.effectiveLyrics.size() == 1U);
  CHECK(replaced.effectiveLyrics[0].text == "second srt");
  CHECK(replaced.effectiveLyrics[0].timestamp == std::chrono::milliseconds{4000});
}

TEST_CASE("scanner lyrics formats e2e reconciles a replaced srt sidecar on an incremental rescan") {
  test::TempScannerRoot temp{"scanner-lyrics-formats-srt-rescan"};
  const auto audio = test::writeAudioFixture(temp.path(), "song.flac");
  const auto srt = temp.path() / "song.srt";
  writeText(srt, "1\n00:00:01,000 --> 00:00:02,000\nbefore rescan\n");

  auto reader = std::make_shared<FakeE2eMetadataReader>();
  reader->put(audio, rawMetadata("Song", {RawTagLyricLine{std::chrono::milliseconds{100}, "embedded"}}));
  auto service = makeE2eService(temp, reader);
  ScanEventLog events;
  service->setEventSink([&events](ScannerEvent event) { events.push(std::move(event)); });

  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Full);
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 1U, kEventWaitBudget));
  {
    const auto song = songForPath(service->snapshot(), audio);
    CHECK(song.effectiveLyricsSource == LyricsSource::ExternalSrt);
    REQUIRE(song.effectiveLyrics.size() == 1U);
    CHECK(song.effectiveLyrics[0].text == "before rescan");
  }

  writeText(srt, "1\n00:00:07,000 --> 00:00:08,000\nafter rescan\n");
  service->scan({ScannerRoot{.path = temp.path()}}, ScanMode::Incremental);
  REQUIRE(events.waitForCount(ScannerEventType::ScanCompleted, 2U, kEventWaitBudget));

  const auto song = songForPath(service->snapshot(), audio);
  CHECK(song.effectiveLyricsSource == LyricsSource::ExternalSrt);
  REQUIRE(song.effectiveLyrics.size() == 1U);
  CHECK(song.effectiveLyrics[0].text == "after rescan");
  CHECK(song.effectiveLyrics[0].timestamp == std::chrono::milliseconds{7000});

  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = scannerSidecarPath(temp)}};
  const auto location = cachedLocationForPath(sidecar, canonicalRootPath(temp.path()), audio);
  CHECK(location.lyricsSource == LyricsSource::ExternalSrt);
  REQUIRE(location.externalLrcPath.has_value());
  CHECK(location.externalLrcPath->filename() == "song.srt");
}

}  // namespace seriona::scanner
