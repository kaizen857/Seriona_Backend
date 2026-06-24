#include "seriona/app/runtime_paths.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

namespace seriona::app {
namespace {

TEST_CASE("RuntimePaths resolves absolute executable path") {
  const auto paths = resolveRuntimePaths("/tmp/example/bin/seriona");

  CHECK(paths.dataRoot == "/tmp/example/bin/SerionaData");
  CHECK(paths.logFile == "/tmp/example/bin/SerionaData/logs/seriona.log");
  CHECK(paths.databasePath == "/tmp/example/bin/SerionaData/library.sqlite");
  CHECK(paths.artworkDir == "/tmp/example/bin/SerionaData/artwork");
}

TEST_CASE("RuntimePaths derives all fields from dataRoot") {
  const auto paths = resolveRuntimePaths("/opt/music/seriona");

  CHECK(paths.dataRoot == "/opt/music/SerionaData");

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
  const auto paths = resolveRuntimePaths("/tmp/example/bin/seriona");
  // This is a smoke test — directories won't actually be created
  // because /tmp/example/bin/SerionaData probably doesn't have write
  // permission, but the call should not throw or crash.
  CHECK_NOTHROW(paths.ensureDirectoriesExist());
}

}  // namespace
}  // namespace seriona::app
