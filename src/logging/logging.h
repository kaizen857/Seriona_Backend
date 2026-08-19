#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace seriona {
namespace logging {

void initialize(spdlog::level::level_enum console_level,
                const std::string& log_file_path,
                spdlog::level::level_enum logger_level = spdlog::level::trace);

// 运行时设置全局日志等级（默认 logger 与全部已注册 named logger 及其 sink 同步；
// 线程安全，spdlog 保证）。level 仅接受 [trace, off]，越界值忽略。
void setLogLevel(spdlog::level::level_enum level);

std::filesystem::path prepareLogFile(
    const std::filesystem::path& logDir,
    std::uintmax_t maxTotalBytes = 50ULL * 1024ULL * 1024ULL);

std::shared_ptr<spdlog::logger> createDedicatedLogger(
    const std::string& logger_name,
    const std::filesystem::path& log_file_path,
    spdlog::level::level_enum logger_level = spdlog::level::trace);

}  // namespace logging
}  // namespace seriona
