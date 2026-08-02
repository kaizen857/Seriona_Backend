#include "scanner_perf_support.h"
#include "scanner_test_harness.h"

#include "file_scanner_service_internal.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/hash_utils.h"
#include "seriona/scanner/path_utils.h"

#include <algorithm>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace seriona::scanner::perf {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct DetailedTimings {
  std::uint64_t directoryTraversalMs{0};
  std::uint64_t pathClassificationMs{0};
  std::uint64_t cacheLoadMs{0};
  std::uint64_t incrementalPlanMs{0};
  std::uint64_t workerSetupMs{0};
  std::uint64_t workerExecutionMs{0};
  std::uint64_t workerWaitMs{0};
  std::uint64_t resultAggregationMs{0};
  std::uint64_t lyricsReconcileMs{0};
  std::uint64_t cacheSaveMs{0};
  std::uint64_t treeHashComputeMs{0};
  std::uint64_t totalWallMs{0};
};

struct DetailedScanMeasurement {
  std::string name;
  std::uint64_t discovered{0};
  std::uint64_t scanned{0};
  std::uint64_t skipped{0};
  std::uint64_t errors{0};
  DetailedTimings timings;
  ProductionPerfStats productionStats;
  std::size_t snapshotSongs{0};
};

struct DetailedLibraryMeasurement {
  std::size_t requestedSongs{0};
  std::filesystem::path root;
  std::filesystem::path database;
  std::vector<DetailedScanMeasurement> scans;
};

class InstrumentedMetadataReader final : public TagMetadataReader {
public:
  void put(std::filesystem::path path, RawTagMetadata metadata) {
    metadataByPath_[std::move(path)] = std::move(metadata);
  }

  [[nodiscard]] RawTagMetadata read(const TagReadRequest& request) override {
    static_cast<void>(request.coverExportDir);
    const auto start = Clock::now();
    auto metadata = RawTagMetadata{};
    {
      std::lock_guard lock{mutex_};
      const auto iterator = metadataByPath_.find(request.path);
      if (iterator == metadataByPath_.end()) {
        throw std::runtime_error("missing metadata for " + request.path.string());
      }
      metadata = iterator->second;
    }
    metadata.filePath = request.path;
    const auto elapsed = Clock::now() - start;
    {
      std::lock_guard lock{mutex_};
      totalReadTime_ += elapsed;
      ++totalReads_;
    }
    return metadata;
  

  }

  [[nodiscard]] std::vector<RawTagMetadata> readCueSheet(const TagReadRequest&) override { return {}; }

  [[nodiscard]] std::chrono::nanoseconds totalReadTime() const {
    std::lock_guard lock{mutex_};
    return totalReadTime_;
  }

  [[nodiscard]] std::uint64_t totalReads() const {
    std::lock_guard lock{mutex_};
    return totalReads_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::filesystem::path, RawTagMetadata> metadataByPath_;
  std::chrono::nanoseconds totalReadTime_{0};
  std::uint64_t totalReads_{0};
};

class DetailedScanObserver {
public:
  void push(ScannerEvent event) {
    std::lock_guard lock{mutex_};
    if (event.type == ScannerEventType::ProgressUpdated && 
        std::holds_alternative<ScanProgress>(event.payload)) {
      progress_ = std::get<ScanProgress>(event.payload);
    } else if (event.type == ScannerEventType::ScanCompleted) {
      ++completions_;
    } else if (event.type == ScannerEventType::ScanError) {
      ++errors_;
    }
    changed_.notify_all();
  }

  bool waitForCompletion(std::chrono::seconds timeout) {
    std::unique_lock lock{mutex_};
    const auto expected = completions_ + 1U;
    return changed_.wait_for(lock, timeout, [this, expected] { 
      return completions_ >= expected; 
    });
  }

  [[nodiscard]] ScanProgress progress() const {
    std::lock_guard lock{mutex_};
    return progress_;
  }

  [[nodiscard]] std::size_t errors() const {
    std::lock_guard lock{mutex_};
    return errors_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  ScanProgress progress_{};
  std::size_t completions_{0};
  std::size_t errors_{0};
};

[[nodiscard]] RawTagMetadata generateMetadata(std::size_t index) {
  RawTagMetadata raw{};
  raw.title = "Track " + std::to_string(index);
  raw.artist = "Artist " + std::to_string(index % 50);
  raw.album = "Album " + std::to_string(index % 150);
  raw.albumArtist = "Album Artist";
  raw.genre = "Generated";
  raw.trackNumber = static_cast<std::uint16_t>((index % 99) + 1);
  raw.discNumber = 1;
  raw.duration = std::chrono::milliseconds{180000 + static_cast<int>(index % 2000)};
  raw.sampleRate = 48000;
  raw.bitDepth = 24;
  raw.channels = 2;
  raw.format = "flac";
  return raw;
}

[[nodiscard]] std::filesystem::path trackPath(const std::filesystem::path& root, std::size_t index) {
  std::ostringstream filename;
  filename << "track-" << std::setw(5) << std::setfill('0') << index << ".flac";
  return root / ("artist-" + std::to_string(index % 30)) / 
               ("album-" + std::to_string(index % 50)) / filename.str();
}

void writeFile(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output.good()) {
    throw std::runtime_error("failed to create " + path.string());
  }
  output << content;
}

void createLibrary(const std::filesystem::path& root, std::size_t count, 
                  InstrumentedMetadataReader& reader) {
  std::cout << "Creating test library with " << count << " files...\n";
  const auto start = Clock::now();
  for (std::size_t i = 0; i < count; ++i) {
    const auto path = trackPath(root, i);
    writeFile(path, "SERIONA_DETAILED_PERF\nindex=" + std::to_string(i) + "\n");
    reader.put(path, generateMetadata(i));
    if ((i + 1) % 1000 == 0) {
      std::cout << "  Created " << (i + 1) << " files...\n";
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  std::cout << "Library creation completed in " << elapsed.count() << " ms\n\n";
}

[[nodiscard]] std::size_t countSongs(const PlaylistTreeSnapshot& snapshot) {
  return static_cast<std::size_t>(std::count_if(snapshot.nodes.begin(), snapshot.nodes.end(),
    [](const PlaylistNode& node) { return node.song.has_value(); }));
}

[[nodiscard]] DetailedScanMeasurement runDetailedScan(
    FileScannerService& service,
    DetailedScanObserver& observer,
    InstrumentedMetadataReader& reader,
    const std::shared_ptr<CapturingSink>& logSink,
    std::string name,
    const std::filesystem::path& root,
    ScanMode mode) {
  static_cast<void>(reader);
  
  DetailedScanMeasurement measurement;
  measurement.name = std::move(name);
  
  const auto logOffset = captureLogOffset(logSink);
  const auto overallStart = Clock::now();
  
  std::cout << "Running scan: " << measurement.name << " (" 
            << (mode == ScanMode::Full ? "Full" : "Incremental") << ")\n";
  
  service.scan({ScannerRoot{.path = root}}, mode);
  
  if (!observer.waitForCompletion(std::chrono::seconds{600})) {
    throw std::runtime_error("scan timed out");
  }
  
  const auto overallElapsed = Clock::now() - overallStart;
  measurement.timings.totalWallMs = std::chrono::duration_cast<std::chrono::milliseconds>(overallElapsed).count();
  
  const auto progress = observer.progress();
  measurement.discovered = progress.filesDiscovered;
  measurement.scanned = progress.filesScanned;
  measurement.skipped = progress.filesSkipped;
  measurement.errors = progress.errors + observer.errors();
  measurement.snapshotSongs = countSongs(service.snapshot());
  measurement.productionStats = capturedProductionStats(logSink, logOffset);
  
  std::cout << "  Discovered: " << measurement.discovered 
            << ", Scanned: " << measurement.scanned
            << ", Skipped: " << measurement.skipped << "\n";
  std::cout << "  Wall time: " << measurement.timings.totalWallMs << " ms\n";
  std::cout << "  Phase 1: " << measurement.productionStats.phaseFileProcessingMs << " ms\n";
  std::cout << "  Phase 2: " << measurement.productionStats.phaseAggregationMs << " ms\n";
  std::cout << "  Worker Hash: " << measurement.productionStats.workerHashMs << " ms\n";
  std::cout << "  Worker TagReader: " << measurement.productionStats.workerTagReaderMs << " ms\n\n";
  
  return measurement;
}

void mutateLibrary(const std::filesystem::path& root, std::size_t baseCount,
                  InstrumentedMetadataReader& reader) {
  std::cout << "Mutating library for incremental scan...\n";
  
  // Modify one file
  const auto changedPath = trackPath(root, baseCount / 2);
  writeFile(changedPath, "SERIONA_DETAILED_PERF\nchanged=true\n");
  auto changedMeta = generateMetadata(baseCount / 2);
  changedMeta.title += " [Modified]";
  reader.put(changedPath, std::move(changedMeta));
  
  // Delete one file
  std::filesystem::remove(trackPath(root, baseCount - 1));
  
  // Add one file
  const auto addedPath = trackPath(root, baseCount);
  writeFile(addedPath, "SERIONA_DETAILED_PERF\nadded=true\n");
  reader.put(addedPath, generateMetadata(baseCount));
  
  std::cout << "  Changed: 1, Deleted: 1, Added: 1\n\n";
}

[[nodiscard]] std::filesystem::path sidecarDbPath(const std::filesystem::path& dbPath) {
  return std::filesystem::path{dbPath.generic_string() + ".scan-roots.sqlite"};
}

[[nodiscard]] std::filesystem::path canonicalize(const std::filesystem::path& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : canonical;
}

void forceIncrementalMode(const std::filesystem::path& root, 
                         const std::filesystem::path& dbPath) {
  const auto treeHash = computeDirectoryTreeHash(canonicalize(root));
  if (!treeHash.hash.has_value()) {
    throw std::runtime_error("failed to compute tree hash for incremental setup");
  }
  
  cache::SQLiteCache sidecar{cache::ScannerCacheConfig{.databasePath = sidecarDbPath(dbPath)}};
  auto scanRoot = sidecar.loadScanRoot(canonicalize(root));
  if (!scanRoot.has_value()) {
    throw std::runtime_error("missing scan root for incremental setup");
  }
  
  scanRoot->directoryTreeHash = *treeHash.hash;
  sidecar.updateScanRoot(*scanRoot);
  std::cout << "Forced incremental mode by updating tree hash\n\n";
}

DetailedLibraryMeasurement measureDetailedLibrary(
    std::size_t count,
    const std::shared_ptr<CapturingSink>& logSink) {
  
  test::TempScannerRoot temp{"scanner-detailed-perf-" + std::to_string(count)};
  const auto libraryRoot = temp.path() / "library";
  
  auto reader = std::make_shared<InstrumentedMetadataReader>();
  createLibrary(libraryRoot, count, *reader);
  
  auto deps = FileScannerServiceDependencies{};
  deps.metadataReader = reader;
  deps.watcherFactory = nullptr;
  deps.databasePath = temp.dbPath();
  deps.coverExportDir = temp.path() / "covers";
  
  auto service = makeFileScannerService(std::move(deps));
  
  DetailedScanObserver observer;
  service->setEventSink([&observer](ScannerEvent event) { 
    observer.push(std::move(event)); 
  });
  
  DetailedLibraryMeasurement result;
  result.requestedSongs = count;
  result.root = libraryRoot;
  result.database = temp.dbPath();
  
  // Cold full scan
  result.scans.push_back(runDetailedScan(*service, observer, *reader, logSink,
                                        "cold_full", libraryRoot, ScanMode::Full));
  
  // Hot full scan
  result.scans.push_back(runDetailedScan(*service, observer, *reader, logSink,
                                        "hot_full", libraryRoot, ScanMode::Full));
  
  // Incremental scan
  mutateLibrary(libraryRoot, count, *reader);
  forceIncrementalMode(libraryRoot, temp.dbPath());
  result.scans.push_back(runDetailedScan(*service, observer, *reader, logSink,
                                        "incremental", libraryRoot, ScanMode::Incremental));
  
  service->stop();
  return result;
}

[[nodiscard]] std::string renderDetailedReport(
    const std::vector<DetailedLibraryMeasurement>& measurements) {
  
  std::ostringstream report;
  report << "========================================\n";
  report << "Detailed Scanner Performance Report\n";
  report << "========================================\n\n";
  
  for (const auto& lib : measurements) {
    report << "Library Size: " << lib.requestedSongs << " songs\n";
    report << "----------------------------------------\n\n";
    
    for (const auto& scan : lib.scans) {
      report << "Scan Type: " << scan.name << "\n";
      report << "  Files discovered: " << scan.discovered << "\n";
      report << "  Files scanned   : " << scan.scanned << "\n";
      report << "  Files skipped   : " << scan.skipped << "\n";
      report << "  Errors          : " << scan.errors << "\n";
      report << "  Snapshot songs  : " << scan.snapshotSongs << "\n";
      report << "\n";
      report << "  Timing Breakdown:\n";
      report << "    Total wall time       : " << scan.timings.totalWallMs << " ms\n";
      report << "    Phase 1 (processing)  : " << scan.productionStats.phaseFileProcessingMs << " ms";
      if (scan.timings.totalWallMs > 0) {
        report << " (" << (scan.productionStats.phaseFileProcessingMs * 100 / scan.timings.totalWallMs) << "%)";
      }
      report << "\n";
      report << "    Phase 2 (aggregation) : " << scan.productionStats.phaseAggregationMs << " ms";
      if (scan.timings.totalWallMs > 0) {
        report << " (" << (scan.productionStats.phaseAggregationMs * 100 / scan.timings.totalWallMs) << "%)";
      }
      report << "\n";
      report << "    Worker hash time      : " << scan.productionStats.workerHashMs << " ms\n";
      report << "    Worker TagReader time : " << scan.productionStats.workerTagReaderMs << " ms\n";
      
      if (scan.scanned > 0) {
        report << "\n  Per-file averages:\n";
        report << "    Avg hash time      : " 
               << std::fixed << std::setprecision(2)
               << (static_cast<double>(scan.productionStats.workerHashMs) / scan.scanned) 
               << " ms/file\n";
        report << "    Avg TagReader time : " 
               << std::fixed << std::setprecision(2)
               << (static_cast<double>(scan.productionStats.workerTagReaderMs) / scan.scanned)
               << " ms/file\n";
      }
      
      report << "\n";
    }
    
    report << "\n";
  }
  
  return report.str();
}

} // anonymous namespace
} // namespace seriona::scanner::perf

int main(int argc, char** argv) {
  using namespace seriona::scanner::perf;
  
  try {
    const auto logSink = installBenchmarkLogger();
    
    std::vector<std::size_t> sizes{1000, 5000, 10000};
    if (argc > 1) {
      sizes.clear();
      for (int i = 1; i < argc; ++i) {
        sizes.push_back(std::stoull(argv[i]));
      }
    }
    
    std::vector<DetailedLibraryMeasurement> measurements;
    for (const auto size : sizes) {
      std::cout << "\n========================================\n";
      std::cout << "Measuring library with " << size << " songs\n";
      std::cout << "========================================\n\n";
      measurements.push_back(measureDetailedLibrary(size, logSink));
    }
    
    const auto report = renderDetailedReport(measurements);
    
    const auto outputPath = std::filesystem::current_path() / "detailed-scanner-perf-report.txt";
    std::ofstream output{outputPath};
    if (output.good()) {
      output << report;
      std::cout << "\nDetailed report written to: " << outputPath << "\n";
    }
    
    std::cout << "\n" << report;
    
  } catch (const std::exception& error) {
    std::cerr << "Detailed performance test failed: " << error.what() << '\n';
    return 1;
  }
  
  return 0;
}
