#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace seriona {
namespace logging {

// 路径文本统一为 UTF-8：Windows 上 path::string() 按 ANSI 代码页转换，
// 不可表示字符会抛异常或乱码，路径文本通道禁止使用。
[[nodiscard]] inline std::string pathText(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

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
