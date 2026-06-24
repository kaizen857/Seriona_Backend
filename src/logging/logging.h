#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace seriona {
namespace logging {

void initialize(spdlog::level::level_enum console_level,
                const std::string& log_file_path);

}  // namespace logging
}  // namespace seriona
