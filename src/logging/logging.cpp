#include "logging/logging.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

namespace seriona {
namespace logging {
namespace {

// spdlog 内置 rotating_file_sink 在 Windows 上以窄字符 fopen（ANSI 代码页）
// 打开文件：UTF-8 路径一旦包含无法映射到代码页的字符即抛
// "No mapping for the Unicode character exists in the target multi-byte code page"
// （EILSEQ / ERROR_NO_UNICODE_TRANSLATION），中文等非 ASCII 安装目录必现。
// 本 sink 改用 std::ofstream(std::filesystem::path)：MSVC 走宽字符（CreateFileW）
// 通道，libc++/libstdc++ 走 UTF-8 字节直通——三端统一，任意 Unicode 路径可用。
// 轮转改名与删除同样经 std::filesystem（宽字符安全），不复用 spdlog 内部的窄 rename。
class Utf8RotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  Utf8RotatingFileSink(std::filesystem::path base_path, std::size_t max_size, std::size_t max_files)
      : base_path_(std::move(base_path)), max_size_(max_size), max_files_(max_files) {
    if (max_size_ == 0) {
      throw spdlog::spdlog_ex("rotating file sink: max_size must be positive");
    }
    openFile();
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if (current_size_ + formatted.size() > max_size_) {
      rotate();
    }
    file_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    current_size_ += formatted.size();
    if (!file_) {
      throw spdlog::spdlog_ex("rotating file sink: write failed for " + pathText(base_path_));
    }
  }

  void flush_() override {
    if (file_.is_open()) {
      file_.flush();
    }
  }

 private:
  void openFile() {
    file_.open(base_path_, std::ios::binary | std::ios::app);
    if (!file_) {
      throw spdlog::spdlog_ex("failed opening file " + pathText(base_path_) +
                              " for writing: " + std::strerror(errno));
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(base_path_, ec);
    current_size_ = ec ? 0 : static_cast<std::size_t>(size);
  }

  // spdlog 轮转语义：写满 max_size 后当前文件更名为 base.1（最新轮转），
  // 既有 base.i 依次后移为 base.(i+1)，最老 base.max_files 删除；随后重开 base 追加。
  // 目标存在时先删再 rename（Windows/POSIX 覆盖语义不一致）；失败均 best-effort。
  void rotate() {
    file_.close();
    if (max_files_ > 1) {
      std::error_code ec;
      std::filesystem::remove(rotatedPath(max_files_), ec);
      for (std::size_t i = max_files_ - 1; i >= 1; --i) {
        const auto from = rotatedPath(i);
        std::error_code existsEc;
        if (std::filesystem::exists(from, existsEc)) {
          std::filesystem::remove(rotatedPath(i + 1), ec);
          std::filesystem::rename(from, rotatedPath(i + 1), ec);
        }
      }
    }
    std::error_code ec;
    std::filesystem::remove(rotatedPath(1), ec);
    std::filesystem::rename(base_path_, rotatedPath(1), ec);
    openFile();
  }

  [[nodiscard]] std::filesystem::path rotatedPath(std::size_t index) const {
    auto path = base_path_;
    path += "." + std::to_string(index);
    return path;
  }

  std::filesystem::path base_path_;
  std::size_t max_size_{0};
  std::size_t max_files_{0};
  std::size_t current_size_{0};
  std::ofstream file_;
};

}  // namespace

void initialize(spdlog::level::level_enum console_level,
                const std::filesystem::path& log_file_path,
                spdlog::level::level_enum logger_level) {
    constexpr const char* pattern =
        "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v";

    auto console_sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(console_level);
    console_sink->set_pattern(pattern);

    std::shared_ptr<Utf8RotatingFileSink> file_sink;
    try {
        file_sink = std::make_shared<Utf8RotatingFileSink>(
            log_file_path, 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::trace);
        file_sink->set_pattern(pattern);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "spdlog: unable to create rotating file sink at "
                  << pathText(log_file_path) << ": " << e.what() << "\n"
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

        const auto& filename = pathText(entry.path().filename());
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
        auto file_sink = std::make_shared<Utf8RotatingFileSink>(
            log_file_path, 1024 * 1024 * 5, 3);
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
                  << "' at " << pathText(log_file_path) << ": " << e.what() << std::endl;
        return nullptr;
    }
}

}  // namespace logging
}  // namespace seriona
