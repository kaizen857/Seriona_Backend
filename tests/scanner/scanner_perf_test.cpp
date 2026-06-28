#include "scanner_perf_support.h"

#include <exception>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
  try {
    const auto logSink = seriona::scanner::perf::installBenchmarkLogger();
    const auto outputPath = seriona::scanner::perf::outputPathFromArgs(argc, argv);
    const auto measurements = std::vector<seriona::scanner::perf::LibraryMeasurement>{
        seriona::scanner::perf::measureLibrary(1000, logSink),
        seriona::scanner::perf::measureLibrary(5000, logSink),
        seriona::scanner::perf::measureLibrary(10000, logSink)};
    const auto report = seriona::scanner::perf::renderReport(measurements);
    seriona::scanner::perf::writeReport(outputPath, report);
    std::cout << report;
  } catch (const std::exception& error) {
    std::cerr << "scanner perf benchmark failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
