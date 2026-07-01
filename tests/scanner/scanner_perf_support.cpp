#include "scanner_perf_support.h"

#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"
#include "seriona/scanner/directory_tree_hash.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace seriona::scanner::perf {
namespace {

using Clock = std::chrono::steady_clock;

struct ReaderStats {
  std::uint64_t calls{0};
  std::chrono::nanoseconds totalTime{0};
};

struct EventStats {
  ScanProgress progress;
  std::size_t completions{0};
  std::size_t errors{0};
};

class PerfMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) { metadataByPath_[std::move(path)] = std::move(metadata); }

  [[nodiscard]] RawTagMetadata read(const std::filesystem::path& path,
                                    const std::filesystem::path& coverExportDir) override {
    static_cast<void>(coverExportDir);
    const auto start = Clock::now();
    auto metadata = RawTagMetadata{};
    {
      std::lock_guard lock{mutex_};
      ++stats_.calls;
      const auto iterator = metadataByPath_.find(path);
      if (iterator == metadataByPath_.end()) {
        throw std::runtime_error("missing perf metadata for " + path.string());
      }
      metadata = iterator->second;
    }
    metadata.filePath = path;
    const auto elapsed = Clock::now() - start;
    {
      std::lock_guard lock{mutex_};
      stats_.totalTime += elapsed;
    }
    return metadata;
  }

  [[nodiscard]] ReaderStats stats() const {
    std::lock_guard lock{mutex_};
    return stats_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
  ReaderStats stats_{};
};

class ScanObserver {
public:
  void push(ScannerEvent event) {
    {
      std::lock_guard lock{mutex_};
      if (event.type == ScannerEventType::ProgressUpdated && std::holds_alternative<ScanProgress>(event.payload)) {
        stats_.progress = std::get<ScanProgress>(event.payload);
      } else if (event.type == ScannerEventType::ScanCompleted) {
        ++stats_.completions;
      } else if (event.type == ScannerEventType::ScanError) {
        ++stats_.errors;
      }
    }
    changed_.notify_all();
  }

  [[nodiscard]] EventStats waitForNextCompletion(std::chrono::seconds timeout) {
    std::unique_lock lock{mutex_};
    const auto expected = stats_.completions + 1U;
    if (!changed_.wait_for(lock, timeout, [this, expected] { return stats_.completions >= expected; })) {
      throw std::runtime_error("scanner perf run timed out waiting for ScanCompleted");
    }
    return stats_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  EventStats stats_{};
};

[[nodiscard]] RawTagMetadata rawMetadataFor(std::size_t index) {
  RawTagMetadata raw{};
  raw.title = "Perf Track " + std::to_string(index);
  raw.artist = "Perf Artist " + std::to_string(index % 37U);
  raw.album = "Perf Album " + std::to_string(index % 113U);
  raw.albumArtist = "Perf Album Artist";
  raw.genre = "Generated";
  raw.trackNumber = static_cast<std::uint16_t>((index % 99U) + 1U);
  raw.discNumber = 1;
  raw.duration = std::chrono::milliseconds{180000 + static_cast<int>(index % 1000U)};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.channels = 2;
  raw.format = "flac";
  return raw;
}

[[nodiscard]] std::filesystem::path trackPathFor(const std::filesystem::path& root, std::size_t index) {
  std::ostringstream filename;
  filename << "track-" << std::setw(5) << std::setfill('0') << index << ".flac";
  return root / ("artist-" + std::to_string(index % 25U)) / ("album-" + std::to_string(index % 40U)) / filename.str();
}

void writeTextFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output.good()) {
    throw std::runtime_error("failed to open " + path.string());
  }
  output << content;
}

void createLibrary(const std::filesystem::path& root, std::size_t count, PerfMetadataReader& reader) {
  for (std::size_t index = 0; index < count; ++index) {
    const auto path = trackPathFor(root, index);
    writeTextFile(path, "SERIONA_PERF_AUDIO\nindex=" + std::to_string(index) + "\n");
    reader.put(path, rawMetadataFor(index));
  }
}

[[nodiscard]] std::filesystem::path sidecarPath(const std::filesystem::path& databasePath) {
  return std::filesystem::path{databasePath.generic_string() + ".scan-roots-v3.sqlite"};
}

[[nodiscard]] std::filesystem::path canonicalPath(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : canonical;
}

void forceIncrementalDecisionForCurrentTree(const std::filesystem::path& root, const std::filesystem::path& databasePath) {
  const auto treeHash = computeDirectoryTreeHash(canonicalPath(root));
  if (!treeHash.hash.has_value()) {
    throw std::runtime_error("missing directory tree hash for incremental perf fixture");
  }
  auto config = cache::ScannerCacheConfig{};
  config.databasePath = sidecarPath(databasePath);
  cache::SQLiteCacheV3 sidecar{config};
  auto scanRoot = sidecar.loadScanRoot(canonicalPath(root));
  if (!scanRoot.has_value()) {
    throw std::runtime_error("missing scan root sidecar state for incremental perf fixture");
  }
  scanRoot->directoryTreeHash = *treeHash.hash;
  sidecar.updateScanRoot(*scanRoot);
}

[[nodiscard]] std::size_t snapshotSongCount(const PlaylistTreeSnapshot& snapshot) {
  return static_cast<std::size_t>(std::ranges::count_if(snapshot.nodes, [](const PlaylistNode& node) {
    return node.song.has_value();
  }));
}

[[nodiscard]] ScanMeasurement runScan(FileScannerService& service, ScanObserver& observer, PerfMetadataReader& reader,
                                      const std::shared_ptr<CapturingSink>& logSink,
                                      std::string name, const std::filesystem::path& root, ScanMode mode) {
  const auto beforeReader = reader.stats();
  const auto logOffset = captureLogOffset(logSink);
  const auto start = Clock::now();
  service.scan({ScannerRoot{.path = root}}, mode);
  const auto events = observer.waitForNextCompletion(std::chrono::seconds{600});
  const auto productionStats = capturedProductionStats(logSink, logOffset);
  const auto afterReader = reader.stats();
  const auto readerDelta = afterReader.totalTime - beforeReader.totalTime;
  return ScanMeasurement{.name = std::move(name),
                         .discovered = events.progress.filesDiscovered,
                         .scanned = events.progress.filesScanned,
                         .skipped = events.progress.filesSkipped,
                         .errors = events.progress.errors + events.errors,
                         .readerCallsDelta = afterReader.calls - beforeReader.calls,
                         .readerTimeDeltaMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(readerDelta).count()),
                         .wallMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()),
                         .productionStats = productionStats,
                         .snapshotSongs = snapshotSongCount(service.snapshot())};
}

void mutateForIncremental(const std::filesystem::path& root, std::size_t baseCount, PerfMetadataReader& reader) {
  const auto changed = trackPathFor(root, baseCount / 2U);
  writeTextFile(changed, "SERIONA_PERF_AUDIO\nchanged=true\n");
  auto changedMetadata = rawMetadataFor(baseCount / 2U);
  changedMetadata.title += " Changed";
  reader.put(changed, std::move(changedMetadata));
  std::filesystem::remove(trackPathFor(root, baseCount - 1U));
  const auto added = trackPathFor(root, baseCount);
  writeTextFile(added, "SERIONA_PERF_AUDIO\nadded=true\n");
  reader.put(added, rawMetadataFor(baseCount));
  std::this_thread::sleep_for(std::chrono::milliseconds{5}); // mtime granularity guard for changed/added files
}

}

LibraryMeasurement measureLibrary(std::size_t count, const std::shared_ptr<CapturingSink>& logSink) {
  test::TempScannerRoot temp{"scanner-perf-" + std::to_string(count)};
  const auto libraryRoot = temp.path() / "library";
  auto reader = std::make_shared<PerfMetadataReader>();
  createLibrary(libraryRoot, count, *reader);
  auto service = makeFileScannerService(FileScannerServiceDependencies{.metadataReader = reader,
                                                                       .watcherFactory = nullptr,
                                                                       .databasePath = temp.dbPath(),
                                                                       .coverExportDir = temp.path() / "covers"});
  ScanObserver observer;
  service->setEventSink([&observer](ScannerEvent event) { observer.push(std::move(event)); });
  auto result = LibraryMeasurement{};
  result.requestedSongs = count;
  result.root = libraryRoot;
  result.database = temp.dbPath();
  result.scans.push_back(runScan(*service, observer, *reader, logSink, "cold_full", libraryRoot, ScanMode::Full));
  result.scans.push_back(runScan(*service, observer, *reader, logSink, "hot_full", libraryRoot, ScanMode::Full));
  mutateForIncremental(libraryRoot, count, *reader);
  forceIncrementalDecisionForCurrentTree(libraryRoot, temp.dbPath());
  result.scans.push_back(runScan(*service, observer, *reader, logSink, "incremental_changed_added_deleted", libraryRoot, ScanMode::Incremental));
  service->stop();
  return result;
}

}
