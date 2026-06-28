#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace seriona::scanner::perf {

class CapturingSink;

struct ProductionPerfStats {
  std::uint64_t wallMs{0};
  std::uint64_t phaseFileProcessingMs{0};
  std::uint64_t phaseAggregationMs{0};
  std::uint64_t workerHashMs{0};
  std::uint64_t workerTagReaderMs{0};
};

struct ScanMeasurement {
  std::string name;
  std::uint64_t discovered{0};
  std::uint64_t scanned{0};
  std::uint64_t skipped{0};
  std::uint64_t errors{0};
  std::uint64_t readerCallsDelta{0};
  std::uint64_t readerTimeDeltaMs{0};
  std::uint64_t wallMs{0};
  ProductionPerfStats productionStats{};
  std::size_t snapshotSongs{0};
};

struct LibraryMeasurement {
  std::size_t requestedSongs{0};
  std::filesystem::path root;
  std::filesystem::path database;
  std::vector<ScanMeasurement> scans;
};

[[nodiscard]] std::shared_ptr<CapturingSink> installBenchmarkLogger();
[[nodiscard]] std::size_t captureLogOffset(const std::shared_ptr<CapturingSink>& sink);
[[nodiscard]] ProductionPerfStats capturedProductionStats(const std::shared_ptr<CapturingSink>& sink, std::size_t offset);
[[nodiscard]] LibraryMeasurement measureLibrary(std::size_t count, const std::shared_ptr<CapturingSink>& logSink);
[[nodiscard]] std::string renderReport(const std::vector<LibraryMeasurement>& measurements);
[[nodiscard]] std::filesystem::path outputPathFromArgs(int argc, char** argv);
void writeReport(const std::filesystem::path& path, const std::string& report);

}
