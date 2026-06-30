#include <doctest.h>

#include "seriona/scanner/file_scanner_service.h"
#include "scanner_test_harness.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using seriona::scanner::test::TempScannerRoot;

TEST_CASE("scanner logs detailed error information when errors occur") {
  TempScannerRoot temp{"error_logging_test"};
  
  // 创建一些会导致错误的文件
  
  // 空的 FLAC 文件（TagReader 会失败）
  for (int i = 0; i < 3; ++i) {
    std::ofstream(temp.path() / ("empty_" + std::to_string(i) + ".flac"));
  }
  
  // 损坏的 CUE 文件
  for (int i = 0; i < 2; ++i) {
    std::ofstream cue(temp.path() / ("broken_" + std::to_string(i) + ".cue"));
    cue << "INVALID CUE CONTENT\n";
  }
  
  // 捕获日志输出
  std::ostringstream logCapture;
  auto originalSink = spdlog::default_logger()->sinks()[0];
  auto captureSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logCapture);
  spdlog::default_logger()->sinks().clear();
  spdlog::default_logger()->sinks().push_back(captureSink);
  
  seriona::scanner::FileScanner scanner;
  std::atomic<bool> scanCompleted{false};
  std::atomic<size_t> errorEventCount{0};
  
  scanner.setEventSink([&](const seriona::scanner::ScannerEvent& event) {
    if (event.type == seriona::scanner::ScannerEventType::ScanError) {
      ++errorEventCount;
    } else if (event.type == seriona::scanner::ScannerEventType::ScanCompleted) {
      scanCompleted = true;
    }
  });
  
  scanner.scan({seriona::scanner::ScannerRoot{.path = temp.path()}});
  
  // 等待扫描完成
  auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (!scanCompleted.load() && std::chrono::steady_clock::now() < timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  
  // 恢复原始日志 sink
  spdlog::default_logger()->sinks().clear();
  spdlog::default_logger()->sinks().push_back(originalSink);
  
  REQUIRE(scanCompleted.load());
  REQUIRE(errorEventCount.load() > 0);
  
  const auto logOutput = logCapture.str();
  
  // 验证错误汇总日志存在
  CHECK(logOutput.find("scan complete:") != std::string::npos);
  CHECK(logOutput.find("errors") != std::string::npos);
  
  // 验证错误详细信息标题
  CHECK(logOutput.find("========== Scan Errors") != std::string::npos);
  
  // 验证错误类型统计
  CHECK(logOutput.find("Error breakdown by type:") != std::string::npos);
  CHECK(logOutput.find("MetadataReadFailed") != std::string::npos);
  
  // 验证详细错误列表
  CHECK(logOutput.find("error(s) with details:") != std::string::npos);
  
  // 验证至少包含文件路径信息
  bool foundPathInError = logOutput.find(".flac") != std::string::npos || 
                          logOutput.find(".cue") != std::string::npos;
  CHECK(foundPathInError);
}

TEST_CASE("scanner does not log error section when no errors occur") {
  TempScannerRoot temp{"no_error_test"};
  
  // 不创建任何文件，空扫描不会产生错误
  
  std::ostringstream logCapture;
  auto originalSink = spdlog::default_logger()->sinks()[0];
  auto captureSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(logCapture);
  spdlog::default_logger()->sinks().clear();
  spdlog::default_logger()->sinks().push_back(captureSink);
  
  seriona::scanner::FileScanner scanner;
  std::atomic<bool> scanCompleted{false};
  
  scanner.setEventSink([&](const seriona::scanner::ScannerEvent& event) {
    if (event.type == seriona::scanner::ScannerEventType::ScanCompleted) {
      scanCompleted = true;
    }
  });
  
  scanner.scan({seriona::scanner::ScannerRoot{.path = temp.path()}});
  
  auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (!scanCompleted.load() && std::chrono::steady_clock::now() < timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  
  spdlog::default_logger()->sinks().clear();
  spdlog::default_logger()->sinks().push_back(originalSink);
  
  REQUIRE(scanCompleted.load());
  
  const auto logOutput = logCapture.str();
  
  // 验证包含成功信息
  CHECK(logOutput.find("scan complete:") != std::string::npos);
  CHECK(logOutput.find("0 errors") != std::string::npos);
  
  // 验证不包含错误详情部分
  CHECK(logOutput.find("========== Scan Errors") == std::string::npos);
}
