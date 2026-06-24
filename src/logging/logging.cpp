#include "logging/logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>
#include <vector>

namespace seriona {
namespace logging {

void initialize(spdlog::level::level_enum console_level,
                const std::string& log_file_path) {
    constexpr const char* pattern =
        "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v";

    auto console_sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(console_level);
    console_sink->set_pattern(pattern);

    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_sink;
    try {
        file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_path, 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern(pattern);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "spdlog: unable to create rotating file sink at "
                  << log_file_path << ": " << e.what() << "\n"
                  << "spdlog: falling back to console-only logging"
                  << std::endl;
    }

    std::vector<spdlog::sink_ptr> sinks{console_sink};
    if (file_sink) {
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("seriona", sinks.begin(),
                                                   sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::err);
    spdlog::set_default_logger(std::move(logger));
}

}  // namespace logging
}  // namespace seriona
