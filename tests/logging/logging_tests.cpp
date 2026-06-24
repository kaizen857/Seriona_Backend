#include "doctest.h"

#include "logging/logging.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <fstream>
#include <string>

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
