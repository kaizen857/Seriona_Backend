#include "doctest.h"

#include "logging/logging.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <thread>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::string content;
    std::string line;
    while (std::getline(in, line)) {
        content += line + "\n";
    }
    return content;
}

// Helper: create a file at `path` with `size` bytes of actual content,
// using a specified last_write_time so we can control sort order.
void createLogFile(const std::filesystem::path& path,
                   std::uintmax_t size,
                   const std::filesystem::file_time_type& mtime) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    // Write 'X' repeated to reach requested size
    const std::string data(size, 'X');
    out.write(data.data(), static_cast<std::streamsize>(size));
    out.close();
    std::filesystem::last_write_time(path, mtime);
}

// Helper: create a file with `size` bytes and its natural current mtime.
void createLogFile(const std::filesystem::path& path,
                   std::uintmax_t size) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    const std::string data(size, 'X');
    out.write(data.data(), static_cast<std::streamsize>(size));
    out.close();
}

// Helper: count how many regular files exist in a directory.
int countRegularFiles(const std::filesystem::path& dir) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("logging bootstrap initializes and writes all five levels") {
    const std::string logPath = "/tmp/seriona_logging_test.log";

    // Clean up from previous runs
    std::remove(logPath.c_str());
    std::remove((logPath + ".1").c_str());
    std::remove((logPath + ".2").c_str());
    std::remove((logPath + ".3").c_str());

    seriona::logging::initialize(spdlog::level::info, logPath);

    spdlog::debug("debug message");
    spdlog::info("info message");
    spdlog::warn("warn message");
    spdlog::error("error message");
    spdlog::critical("critical message");

    // Flush to ensure file is written before reading
    spdlog::default_logger()->flush();

    const std::string content = readFile(logPath);

    // File sink is trace-level, so all five messages must appear
    CHECK(content.find("[debug]") != std::string::npos);
    CHECK(content.find("[info]") != std::string::npos);
    CHECK(content.find("[warning]") != std::string::npos);
    CHECK(content.find("[error]") != std::string::npos);
    CHECK(content.find("[critical]") != std::string::npos);

    // Verify exact message text
    CHECK(content.find("debug message") != std::string::npos);
    CHECK(content.find("info message") != std::string::npos);
    CHECK(content.find("warn message") != std::string::npos);
    CHECK(content.find("error message") != std::string::npos);
    CHECK(content.find("critical message") != std::string::npos);

    // Clean up
    std::remove(logPath.c_str());
    std::remove((logPath + ".1").c_str());
    std::remove((logPath + ".2").c_str());
    std::remove((logPath + ".3").c_str());
}

// -----------------------------------------------------------------------
// prepareLogFile() tests
// -----------------------------------------------------------------------

namespace fs = std::filesystem;

TEST_CASE("prepareLogFile over 50MB limit deletes oldest files") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_A";
    fs::create_directories(tmpDir);

    // Base time for deterministic ordering
    auto baseTime = fs::file_time_type::clock::now();
    // Create 3 files of 100 bytes each, spaced 1s apart so mtime differs
    createLogFile(tmpDir / "oldest.log",   100, baseTime - std::chrono::seconds(3));
    createLogFile(tmpDir / "middle.log",   100, baseTime - std::chrono::seconds(2));
    createLogFile(tmpDir / "newest.log",   100, baseTime - std::chrono::seconds(1));

    REQUIRE(countRegularFiles(tmpDir) == 3);

    // limit=200: oldest (100) deleted → total 200, under limit → stop.
    // Both remaining files should stay.
    auto result = seriona::logging::prepareLogFile(tmpDir, 200);

    CHECK(countRegularFiles(tmpDir) == 2);
    CHECK_FALSE(fs::exists(tmpDir / "oldest.log"));
    CHECK(fs::exists(tmpDir / "middle.log"));
    CHECK(fs::exists(tmpDir / "newest.log"));

    fs::remove_all(tmpDir);
}

TEST_CASE("prepareLogFile under limit keeps all files") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_B";
    fs::create_directories(tmpDir);

    createLogFile(tmpDir / "one.log", 100);
    createLogFile(tmpDir / "two.log", 100);

    REQUIRE(countRegularFiles(tmpDir) == 2);

    auto result = seriona::logging::prepareLogFile(tmpDir, 500);

    CHECK(countRegularFiles(tmpDir) == 2);
    CHECK(fs::exists(tmpDir / "one.log"));
    CHECK(fs::exists(tmpDir / "two.log"));

    fs::remove_all(tmpDir);
}

TEST_CASE("prepareLogFile empty directory returns timestamped path") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_C";
    fs::create_directories(tmpDir);

    REQUIRE(countRegularFiles(tmpDir) == 0);

    auto result = seriona::logging::prepareLogFile(tmpDir, 500);

    CHECK(result.parent_path() == tmpDir);
    CHECK(result.has_filename());
    // Should not be empty or throw

    fs::remove_all(tmpDir);
}

TEST_CASE("prepareLogFile counts rotated .log.1 .log.2 in size and deletes them") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_D";
    fs::create_directories(tmpDir);

    auto baseTime = fs::file_time_type::clock::now();
    createLogFile(tmpDir / "seriona.log.1", 100, baseTime - std::chrono::seconds(4));
    createLogFile(tmpDir / "seriona.log.2", 100, baseTime - std::chrono::seconds(3));
    createLogFile(tmpDir / "seriona.log",   100, baseTime - std::chrono::seconds(2));

    REQUIRE(countRegularFiles(tmpDir) == 3);

    // limit=150: oldest (seriona.log.1=100) deleted → 200; still > 150 →
    // next oldest (seriona.log.2=100) deleted → 100; under limit → stop.
    auto result = seriona::logging::prepareLogFile(tmpDir, 150);

    CHECK(countRegularFiles(tmpDir) == 1);
    CHECK_FALSE(fs::exists(tmpDir / "seriona.log.1"));
    CHECK_FALSE(fs::exists(tmpDir / "seriona.log.2"));
    CHECK(fs::exists(tmpDir / "seriona.log"));

    fs::remove_all(tmpDir);
}

TEST_CASE("prepareLogFile ignores non-log files") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_E";
    fs::create_directories(tmpDir);

    auto baseTime = fs::file_time_type::clock::now();
    createLogFile(tmpDir / "notes.txt", 200, baseTime - std::chrono::seconds(4));
    createLogFile(tmpDir / "data.dat",  200, baseTime - std::chrono::seconds(3));
    createLogFile(tmpDir / "app.log",   100, baseTime - std::chrono::seconds(2));

    REQUIRE(countRegularFiles(tmpDir) == 3);

    // limit=50: only app.log is counted; oldest log file (app.log=100) > 50 →
    // deleted; total of counted files becomes 0; .txt/.dat ignored and survive.
    auto result = seriona::logging::prepareLogFile(tmpDir, 50);

    CHECK(countRegularFiles(tmpDir) == 2);
    CHECK(fs::exists(tmpDir / "notes.txt"));
    CHECK(fs::exists(tmpDir / "data.dat"));
    CHECK_FALSE(fs::exists(tmpDir / "app.log"));

    fs::remove_all(tmpDir);
}

TEST_CASE("prepareLogFile timestamped filename matches expected pattern") {
    const fs::path tmpDir = "/tmp/seriona_logging_test_F";
    fs::create_directories(tmpDir);

    auto result = seriona::logging::prepareLogFile(tmpDir, 500);

    CHECK(result.parent_path() == tmpDir);
    std::regex pattern(R"(seriona-\d{14}\.log)");
    CHECK(std::regex_match(result.filename().string(), pattern));

    // Verify the file does not actually exist on disk yet
    // (prepareLogFile only computes the path, does not create it)
    CHECK_FALSE(fs::exists(result));

    fs::remove_all(tmpDir);
}
