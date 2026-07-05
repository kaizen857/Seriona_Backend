// 独立的 Scanner 冷扫描性能测试程序
// 只链接 seriona_scanner，不依赖其他模块

#include "seriona/scanner/scanner_contracts.h"
#include "seriona/scanner/file_scanner_service.h"
#include "seriona/scanner/tag_reader_metadata_adapter.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <fstream>
#include <variant>

namespace fs = std::filesystem;

struct PerfMetrics {
    std::chrono::milliseconds wallTime{0};
    uint64_t filesDiscovered{0};
    uint64_t filesScanned{0};
    uint64_t filesSkipped{0};
    uint64_t errors{0};
    int peakThreads{0};
};

void printMetrics(const PerfMetrics& metrics) {
    std::cout << "\n===== Performance Results =====\n";
    std::cout << "Wall time: " << metrics.wallTime.count() << " ms";
    std::cout << " (" << std::fixed << std::setprecision(2) 
              << metrics.wallTime.count() / 1000.0 << " seconds)\n";
    std::cout << "Files discovered: " << metrics.filesDiscovered << "\n";
    std::cout << "Files scanned: " << metrics.filesScanned << "\n";
    std::cout << "Files skipped: " << metrics.filesSkipped << "\n";
    std::cout << "Errors: " << metrics.errors << "\n";
    std::cout << "Peak threads: " << metrics.peakThreads << "\n";
    
    if (metrics.filesScanned > 0) {
        double avgMs = static_cast<double>(metrics.wallTime.count()) / metrics.filesScanned;
        std::cout << "Avg time per file: " << std::fixed << std::setprecision(2) 
                  << avgMs << " ms\n";
    }
    std::cout << "==============================\n";
}

int countThreads() {
    // 读取 /proc/self/status 获取线程数
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.find("Threads:") == 0) {
            return std::stoi(line.substr(9));
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " /path/to/music/library\n";
        std::cerr << "\nThis program performs a COLD scan (full rescan with no cache).\n";
        std::cerr << "Cache and covers will be created in /tmp/scanner_perf_test/\n";
        return 1;
    }

    const fs::path musicRoot{argv[1]};
    if (!fs::exists(musicRoot)) {
        std::cerr << "Error: Path does not exist: " << musicRoot << "\n";
        return 1;
    }

    // 每次运行都删除旧的测试数据，确保冷扫描
    const fs::path testBase = "/tmp/scanner_perf_test";
    const fs::path cacheDb = testBase / "scanner_cache.sqlite";
    const fs::path coverDir = testBase / "covers";
    
    std::cout << "===== Scanner Cold Scan Performance Test =====\n";
    std::cout << "Music root: " << musicRoot << "\n";
    std::cout << "Cache DB: " << cacheDb << "\n";
    std::cout << "Cover dir: " << coverDir << "\n";
    std::cout << "\n";

    // 删除旧数据，确保冷扫描
    std::cout << "Cleaning old test data...\n";
    std::error_code ec;
    fs::remove_all(testBase, ec);
    fs::create_directories(testBase, ec);
    fs::create_directories(coverDir, ec);

    std::cout << "Creating scanner service...\n";
    
    // 使用公开 API 创建 scanner
    using namespace seriona::scanner;
    
    auto service = makeFileScannerService();
    FileScanner scanner{service};
    
    // 配置 scanner
    ScannerConfig config;
    config.enableIncrementalScan = false;  // 强制全量扫描
    config.forceFull = true;
    scanner.configure(config);
    
    // 设置事件监听器来获取进度
    std::atomic<uint64_t> discovered{0};
    std::atomic<uint64_t> scanned{0};
    std::atomic<uint64_t> skipped{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<bool> completed{false};
    
    scanner.setEventSink([&](const ScannerEvent& event) {
        if (event.type == ScannerEventType::ProgressUpdated) {
            if (auto* prog = std::get_if<ScanProgress>(&event.payload)) {
                discovered.store(prog->filesDiscovered);
                scanned.store(prog->filesScanned);
                skipped.store(prog->filesSkipped);
                errors.store(prog->errors);
            }
        }
        else if (event.type == ScannerEventType::ScanCompleted) {
            completed.store(true);
        }
    });
    
    std::cout << "Starting COLD SCAN (full rescan, no cache)...\n";
    std::cout << "Press Ctrl+C to cancel\n";
    std::cout << "\n";

    PerfMetrics metrics;
    int peakThreads = 0;
    
    // 监控线程数的后台任务
    std::atomic<bool> monitoring{true};
    std::thread monitorThread([&]() {
        while (monitoring.load()) {
            int threads = countThreads();
            if (threads > peakThreads) {
                peakThreads = threads;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    const auto startTime = std::chrono::steady_clock::now();
    
    try {
        ScannerRoot root{musicRoot, true};
        scanner.scan({root}, ScanMode::Full);
        
        // 等待扫描完成
        while (!completed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        std::cerr << "\nError during scan: " << e.what() << "\n";
        monitoring.store(false);
        monitorThread.join();
        return 1;
    }
    
    const auto endTime = std::chrono::steady_clock::now();
    monitoring.store(false);
    monitorThread.join();
    
    metrics.wallTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    metrics.filesDiscovered = discovered.load();
    metrics.filesScanned = scanned.load();
    metrics.filesSkipped = skipped.load();
    metrics.errors = errors.load();
    metrics.peakThreads = peakThreads;
    
    printMetrics(metrics);
    
    return 0;
}
