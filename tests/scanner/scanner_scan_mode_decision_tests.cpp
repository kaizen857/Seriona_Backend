#include "scanner_test_harness.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#define SERIONA_SCANNER_ORCHESTRATOR_TESTING
#include "../../src/scanner/file_scanner_orchestrator.cpp"

namespace seriona::scanner {
namespace {

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

[[nodiscard]] cache::SQLiteCache openScanRootCache(const std::filesystem::path& databasePath) {
  return cache::SQLiteCache{cache::ScannerCacheConfig{.databasePath = scanRootDatabasePath(databasePath),
                                                        }};
}

[[nodiscard]] std::filesystem::path externalDatabasePath(const test::TempScannerRoot& temp) {
  return temp.dbPath("scanner-cache.sqlite");
}

TEST_CASE("scan mode decision returns full for first root scan") {
  test::TempScannerRoot temp{"scan-mode-first-root"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto databasePath = externalDatabasePath(temp);

  const auto decision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);

  CHECK(std::filesystem::exists(audioPath));
  CHECK(decision.mode == ScanMode::Full);
  CHECK(decision.directoryTreeHash.has_value());
}

TEST_CASE("scan mode decision returns incremental when cached directory tree hash matches") {
  test::TempScannerRoot temp{"scan-mode-unchanged-root"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto databasePath = externalDatabasePath(temp);
  const auto firstDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);
  REQUIRE(firstDecision.directoryTreeHash.has_value());
  auto cache = openScanRootCache(databasePath);
  cache.updateScanRoot(scanRootRecord(rootPathFor(ScannerRoot{.path = temp.path()}), firstDecision, 1U,
                                      std::chrono::milliseconds{5}));

  const auto secondDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);

  CHECK(std::filesystem::exists(audioPath));
  CHECK(secondDecision.mode == ScanMode::Incremental);
  CHECK(secondDecision.directoryTreeHash == firstDecision.directoryTreeHash);
}

TEST_CASE("scan mode decision returns full when directory tree hash changes") {
  test::TempScannerRoot temp{"scan-mode-changed-root"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto databasePath = externalDatabasePath(temp);
  const auto firstDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);
  REQUIRE(firstDecision.directoryTreeHash.has_value());
  auto cache = openScanRootCache(databasePath);
  const auto rootPath = rootPathFor(ScannerRoot{.path = temp.path()});
  cache.updateScanRoot(scanRootRecord(rootPath, firstDecision, 1U, std::chrono::milliseconds{5}));

  const auto newAudioPath = test::writeAudioFixture(temp.path(), "new-song.flac");
  const auto changedDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);

  CHECK(std::filesystem::exists(audioPath));
  CHECK(std::filesystem::exists(newAudioPath));
  CHECK(changedDecision.mode == ScanMode::Full);
  REQUIRE(changedDecision.directoryTreeHash.has_value());
  CHECK(*changedDecision.directoryTreeHash != *firstDecision.directoryTreeHash);
  const auto staleScanRoot = cache.loadScanRoot(rootPath);
  REQUIRE(staleScanRoot.has_value());
  CHECK(staleScanRoot->directoryTreeHash == *firstDecision.directoryTreeHash);

  cache.updateScanRoot(scanRootRecord(rootPath, changedDecision, 2U, std::chrono::milliseconds{7}));
  const auto refreshedDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);

  CHECK(refreshedDecision.mode == ScanMode::Incremental);
  CHECK(refreshedDecision.directoryTreeHash == changedDecision.directoryTreeHash);
}

TEST_CASE("scan mode decision preserves explicit full request") {
  test::TempScannerRoot temp{"scan-mode-explicit-full"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto databasePath = externalDatabasePath(temp);
  const auto firstDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, databasePath);
  REQUIRE(firstDecision.directoryTreeHash.has_value());
  auto cache = openScanRootCache(databasePath);
  cache.updateScanRoot(scanRootRecord(rootPathFor(ScannerRoot{.path = temp.path()}), firstDecision, 1U,
                                      std::chrono::milliseconds{5}));

  const auto explicitDecision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Full, databasePath);

  CHECK(std::filesystem::exists(audioPath));
  CHECK(explicitDecision.mode == ScanMode::Full);
  CHECK(explicitDecision.directoryTreeHash == firstDecision.directoryTreeHash);
}

TEST_CASE("scan mode decision falls back to full when directory tree hash is unavailable") {
  test::TempScannerRoot temp{"scan-mode-missing-root"};
  const auto missingRoot = temp.path() / "missing";
  const auto databasePath = externalDatabasePath(temp);

  const auto decision = decideScanMode(ScannerRoot{.path = missingRoot}, ScanMode::Incremental, databasePath);

  CHECK(decision.mode == ScanMode::Full);
  CHECK_FALSE(decision.directoryTreeHash.has_value());
}

TEST_CASE("scan mode decision falls back to full when scan-root cache cannot be opened") {
  test::TempScannerRoot temp{"scan-mode-cache-open-error"};
  const auto audioPath = test::writeAudioFixture(temp.path(), "song.flac");
  const auto blockedDatabasePath = temp.path() / "blocked" / "scanner-cache.sqlite";
  std::filesystem::create_directories(blockedDatabasePath.parent_path());
  writeText(blockedDatabasePath.parent_path() / "scanner-cache.sqlite.scan-roots.sqlite", "not sqlite");

  const auto decision = decideScanMode(ScannerRoot{.path = temp.path()}, ScanMode::Incremental, blockedDatabasePath);

  CHECK(std::filesystem::exists(audioPath));
  CHECK(decision.mode == ScanMode::Full);
  CHECK(decision.directoryTreeHash.has_value());
}

}
}
