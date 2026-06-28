#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace seriona::scanner {

[[nodiscard]] std::string normalizeForId(std::string_view text);
[[nodiscard]] std::string computeContentId(std::chrono::milliseconds durationMs,
                                           std::string_view title,
                                           std::string_view artist);
[[nodiscard]] std::string computeLocationId(const std::filesystem::path& path,
                                            std::uint64_t fileSize,
                                            std::optional<std::filesystem::file_time_type> mtime);

}
