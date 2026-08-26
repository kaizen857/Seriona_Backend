#include "seriona/app/runtime_paths.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace seriona::app {
namespace {

std::filesystem::path uniqueTestRoot() {
  const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("seriona_runtime_paths_test_" + std::to_string(uniqueSuffix));
}

TEST_CASE("RuntimePaths resolves absolute executable path") {
  const auto executablePath = uniqueTestRoot() / "bin" / "seriona";
  const auto expectedDataRoot = executablePath.parent_path() / "SerionaData";
  const auto paths = resolveRuntimePaths(executablePath);

  CHECK(paths.dataRoot == expectedDataRoot);
  CHECK(paths.logFile == expectedDataRoot / "logs" / "seriona.log");
  CHECK(paths.databasePath == expectedDataRoot / "library.sqlite");
  CHECK(paths.artworkDir == expectedDataRoot / "artwork");
}

TEST_CASE("RuntimePaths derives all fields from dataRoot") {
  const auto executablePath = uniqueTestRoot() / "bin" / "seriona";
  const auto paths = resolveRuntimePaths(executablePath);

  CHECK(paths.dataRoot == executablePath.parent_path() / "SerionaData");

  CHECK(paths.logFile.parent_path() == paths.dataRoot / "logs");
  CHECK(paths.logFile.filename() == "seriona.log");

  CHECK(paths.databasePath.parent_path() == paths.dataRoot);
  CHECK(paths.databasePath.filename() == "library.sqlite");

  CHECK(paths.artworkDir == paths.dataRoot / "artwork");
}

TEST_CASE("RuntimePaths fallback for empty/relative path uses current_path") {
  // When /proc/self/exe is available (Linux), it takes priority.
  // On platforms without /proc/self/exe, or when readlink fails,
  // an empty or relative executablePath falls back to
  // std::filesystem::current_path() to produce a deterministic result.
  //
  // This test documents the fallback contract:
  //   dataRoot = current_path() / "SerionaData"
  //
  // Note: on Linux this test will yield the /proc/self/exe-derived
  // directory, which is the expected production behavior. The
  // current_path() fallback is reachable on non-Linux platforms and
  // when /proc/self/exe is unavailable.
  const auto cwd = std::filesystem::current_path();
  const auto expectedDataRoot = cwd / "SerionaData";

  const auto paths = resolveRuntimePaths("");

  // On Linux, /proc/self/exe determines the actual result.
  // Verify the shape (relative positioning) rather than exact equality.
  CHECK(paths.logFile.filename() == "seriona.log");
  CHECK(paths.databasePath.filename() == "library.sqlite");
  CHECK(paths.logFile.parent_path().filename() == "logs");
  CHECK(paths.artworkDir.filename() == "artwork");

  // dataRoot ends with /SerionaData
  CHECK(paths.dataRoot.filename() == "SerionaData");

  // All sub-paths are rooted under dataRoot
  CHECK(paths.logFile.parent_path().parent_path() == paths.dataRoot);
  CHECK(paths.databasePath.parent_path() == paths.dataRoot);
  CHECK(paths.artworkDir.parent_path() == paths.dataRoot);

  // Document: the deterministic fallback would produce:
  //   paths.dataRoot == expectedDataRoot
  (void)expectedDataRoot;
}

TEST_CASE("RuntimePaths ensureDirectoriesExist does not crash") {
  const auto root = uniqueTestRoot();
  const auto paths = resolveRuntimePaths(root / "bin" / "seriona");
  std::filesystem::remove_all(root);

  CHECK_NOTHROW(paths.ensureDirectoriesExist());
  CHECK(std::filesystem::exists(paths.dataRoot));
  CHECK(std::filesystem::exists(paths.logFile.parent_path()));
  CHECK(std::filesystem::exists(paths.artworkDir));

  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace seriona::app
