#include "scanner_test_harness.h"

#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>

namespace seriona::scanner::cache {
namespace {

void createV2Database(const std::filesystem::path& dbPath) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db, "PRAGMA user_version=2;", nullptr, nullptr, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_close(db) == SQLITE_OK);
}

}

TEST_CASE("sqlite cache v3 initializes schema version three and wal journal mode") {
  test::TempScannerRoot temp{"scanner-cache-v3-basic"};
  SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath(), .busyTimeout = std::chrono::milliseconds{25}}};

  CHECK(cache.schemaVersion() == 3);
  CHECK(cache.journalMode() == "wal");
  auto writer = cache.beginWriter();
  writer.commit();
}

TEST_CASE("sqlite cache v3 rejects an existing v2 database without migration logic") {
  test::TempScannerRoot temp{"scanner-cache-v3-v2-db"};
  createV2Database(temp.dbPath());

  CHECK_THROWS_AS(([&temp] {
    SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.dbPath(), .busyTimeout = std::chrono::milliseconds{25}}};
    static_cast<void>(cache);
  }()),
                  std::runtime_error);
}

TEST_CASE("sqlite cache v3 rejects a directory path as an invalid database file") {
  test::TempScannerRoot temp{"scanner-cache-v3-invalid-path"};

  CHECK_THROWS_AS(([&temp] {
    SQLiteCacheV3 cache{ScannerCacheConfig{.databasePath = temp.path(), .busyTimeout = std::chrono::milliseconds{25}}};
    static_cast<void>(cache);
  }()),
                  std::runtime_error);
}

}
