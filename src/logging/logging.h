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

std::filesystem::path prepareLogFile(
    const std::filesystem::path& logDir,
    std::uintmax_t maxTotalBytes = 50ULL * 1024ULL * 1024ULL);

}  // namespace logging
}  // namespace seriona
