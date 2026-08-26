#include "logging/logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <vector>

namespace seriona {
namespace logging {

void initialize(spdlog::level::level_enum console_level,
                const std::string& log_file_path,
                spdlog::level::level_enum logger_level) {
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
    logger->set_level(logger_level);
    logger->flush_on(spdlog::level::err);
    spdlog::set_default_logger(std::move(logger));
}

void setLogLevel(spdlog::level::level_enum level) {
    // 防御无效枚举：level_enum 是 int 底层枚举，越界值会破坏 should_log 比较；
    // 只接受 [trace, off] 有效区间，其余直接忽略（等级保持现状）。
    if (level < spdlog::level::trace || level > spdlog::level::off) {
        return;
    }

    // 默认 logger（initialize 注册的 "seriona"）与全部已注册 named logger
    // （createDedicatedLogger 等）同步：spdlog 1.17 的 registry::set_level
    // 遍历所有已注册 logger，线程安全。
    spdlog::set_level(level);

    // sink 级过滤同步：控制台/文件 sink 各持有独立级别（控制台初始为 console_level、
    // 文件初始为 trace），logger 级别放开后 sink 级别仍可能挡掉新级别消息，
    // 因此将默认 logger 的全部 sink 一并同步到新等级。
    if (const auto logger = spdlog::default_logger(); logger) {
        for (const auto& sink : logger->sinks()) {
            sink->set_level(level);
        }
    }
}

std::filesystem::path prepareLogFile(
    const std::filesystem::path& logDir,
    std::uintmax_t maxTotalBytes) {

    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
    };

    std::vector<Entry> entries;

    // 1. Iterate directory for files whose name contains ".log"
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(logDir, ec);
         it != std::filesystem::directory_iterator();
         it.increment(ec)) {

        if (ec) {
            std::cerr << "prepareLogFile: directory_iterator error: "
                      << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        const auto& entry = *it;

        if (ec) {
            ec.clear();
            continue;
        }

        if (!entry.is_regular_file(ec)) {
            if (ec) ec.clear();
            continue;
        }

        const auto& filename = entry.path().filename().string();
        if (filename.find(".log") == std::string::npos) {
            continue;
        }

        // 2. Collect file metadata; skip on failure
        std::uintmax_t fileSize = 0;
        std::filesystem::file_time_type mtime;

        try {
            fileSize = std::filesystem::file_size(entry.path());
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "prepareLogFile: file_size failed for "
                      << entry.path() << ": " << e.what() << std::endl;
            continue;
        }

        try {
            mtime = std::filesystem::last_write_time(entry.path());
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "prepareLogFile: last_write_time failed for "
                      << entry.path() << ": " << e.what() << std::endl;
            continue;
        }

        entries.push_back(Entry{entry.path(), mtime, fileSize});
    }

    // 3. Sort by last_write_time ascending (oldest first)
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.mtime < b.mtime;
              });

    // 4. Sum all file sizes
    std::uintmax_t total = 0;
    for (const auto& e : entries) {
        total += e.size;
    }

    // 5. Delete oldest files while total exceeds limit
    auto deletion_index = entries.begin();
    while (total > maxTotalBytes && deletion_index != entries.end()) {
        std::error_code rm_ec;
        std::filesystem::remove(deletion_index->path, rm_ec);
        if (rm_ec) {
            std::cerr << "prepareLogFile: failed to remove "
                      << deletion_index->path << ": " << rm_ec.message()
                      << std::endl;
            // Continue anyway — best-effort deletion
        } else {
            total -= deletion_index->size;
        }
        ++deletion_index;
    }

    // 6. Generate timestamped filename
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "seriona-%Y%m%d%H%M%S.log", &tm_buf);

    return logDir / std::string(buf);
}

std::shared_ptr<spdlog::logger> createDedicatedLogger(
    const std::string& logger_name,
    const std::filesystem::path& log_file_path,
    spdlog::level::level_enum logger_level) {
    
    constexpr const char* pattern =
        "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v";
    
    try {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_path.string(), 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern(pattern);
        
        auto logger = std::make_shared<spdlog::logger>(
            logger_name, file_sink);
        logger->set_level(logger_level);
        logger->flush_on(spdlog::level::warn);
        
        spdlog::register_logger(logger);
        return logger;
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "Failed to create dedicated logger '" << logger_name 
                  << "' at " << log_file_path << ": " << e.what() << std::endl;
        return nullptr;
    }
}

}  // namespace logging
}  // namespace seriona
