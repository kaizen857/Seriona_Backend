#include "scanner_perf_support.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <string>
#include <vector>

namespace seriona::scanner::perf {

class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock{messagesMutex_};
    return messages_.size();
  }

  [[nodiscard]] std::vector<std::string> messagesSince(std::size_t offset) const {
    std::lock_guard lock{messagesMutex_};
    if (offset >= messages_.size()) {
      return {};
    }
    return {messages_.begin() + static_cast<std::ptrdiff_t>(offset), messages_.end()};
  }

protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::lock_guard lock{messagesMutex_};
    messages_.emplace_back(msg.payload.data(), msg.payload.size());
  }

  void flush_() override {}

private:
  mutable std::mutex messagesMutex_;
  std::vector<std::string> messages_;
};

namespace {

[[nodiscard]] std::uint64_t parseMetricMs(const std::string& message) {
  const auto colon = message.find(':');
  const auto firstDigit = message.find_first_of("0123456789", colon == std::string::npos ? 0U : colon + 1U);
  if (firstDigit == std::string::npos) {
    return 0;
  }
  const auto endDigit = message.find_first_not_of("0123456789", firstDigit);
  return static_cast<std::uint64_t>(std::stoull(message.substr(firstDigit, endDigit - firstDigit)));
}

}

std::shared_ptr<CapturingSink> installBenchmarkLogger() {
  auto capture = std::make_shared<CapturingSink>();
  auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto logger = std::make_shared<spdlog::logger>("seriona-scanner-perf", spdlog::sinks_init_list{console, capture});
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(std::move(logger));
  return capture;
}

std::size_t captureLogOffset(const std::shared_ptr<CapturingSink>& sink) {
  return sink->size();
}

ProductionPerfStats capturedProductionStats(const std::shared_ptr<CapturingSink>& sink, std::size_t offset) {
  auto stats = ProductionPerfStats{};
  for (const auto& message : sink->messagesSince(offset)) {
    if (message.find("Total Wall Time") != std::string::npos) {
      stats.wallMs = parseMetricMs(message);
    } else if (message.find("[Phase 1] Dir Scan + File Processing") != std::string::npos) {
      stats.phaseFileProcessingMs = parseMetricMs(message);
    } else if (message.find("[Phase 2] Aggregation") != std::string::npos) {
      stats.phaseAggregationMs = parseMetricMs(message);
    } else if (message.find("- File Hash") != std::string::npos) {
      stats.workerHashMs = parseMetricMs(message);
    } else if (message.find("- TagReader Parse") != std::string::npos) {
      stats.workerTagReaderMs = parseMetricMs(message);
    }
  }
  return stats;
}

}
