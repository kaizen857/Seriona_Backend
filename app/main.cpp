#include "terminal_controller.h"

#include "spdlog/spdlog.h"

#include <filesystem>
#include <iostream>

namespace {

void printUsage(const char* programName) {
  std::cerr << "Usage: " << programName << " /path/to/music-root-or-file\n";
}

}

int main(int argc, char** argv) {
  if (argc != 2) {
    printUsage(argv[0]);
    return 2;
  }

  const std::filesystem::path musicPath{argv[1]};
  if (!std::filesystem::exists(musicPath)) {
    std::cerr << "seriona: path does not exist: " << musicPath << '\n';
    return 1;
  }

  try {
    return seriona::app::runTerminalController(musicPath);
  } catch (const std::exception& e) {
    spdlog::critical("unrecoverable exception: {}", e.what());
    spdlog::shutdown();
    return 1;
  } catch (...) {
    spdlog::critical("unrecoverable unknown exception");
    spdlog::shutdown();
    return 1;
  }
}
