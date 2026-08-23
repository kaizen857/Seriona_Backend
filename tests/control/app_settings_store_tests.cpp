#include "seriona/control/app_settings_store.h"
#include "seriona/scanner/cache/sqlite_cache.h"

#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class TempDatabase final {
public:
  explicit TempDatabase(std::string name)
      : directory_(std::filesystem::temp_directory_path() /
                   (std::move(name) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        databasePath_(directory_ / "seriona-test.sqlite") {
    std::filesystem::create_directories(directory_);
  }

  ~TempDatabase() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  TempDatabase(const TempDatabase&) = delete;
  TempDatabase& operator=(const TempDatabase&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return databasePath_; }

private:
  std::filesystem::path directory_;
  std::filesystem::path databasePath_;
};

class SqliteHandle final {
public:
  explicit SqliteHandle(const std::filesystem::path& databasePath) {
    REQUIRE(sqlite3_open_v2(databasePath.generic_string().c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  }

  ~SqliteHandle() {
    if (db_ != nullptr) {
      static_cast<void>(sqlite3_close(db_));
    }
  }

  SqliteHandle(const SqliteHandle&) = delete;
  SqliteHandle& operator=(const SqliteHandle&) = delete;

  [[nodiscard]] sqlite3* get() const noexcept { return db_; }

private:
  sqlite3* db_{};
};

[[nodiscard]] std::vector<std::string> sqliteTableNames(sqlite3* db) {
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db,
                             "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;",
                             -1,
                             &statement,
                             nullptr) == SQLITE_OK);
  std::vector<std::string> names;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* name = sqlite3_column_text(statement, 0);
    names.emplace_back(reinterpret_cast<const char*>(name));
  }
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return names;
}

[[nodiscard]] int sqliteUserVersion(sqlite3* db) {
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const auto version = sqlite3_column_int(statement, 0);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return version;
}

[[nodiscard]] bool hasTable(const std::vector<std::string>& tables, const std::string& tableName) {
  return std::find(tables.begin(), tables.end(), tableName) != tables.end();
}

[[nodiscard]] std::vector<std::string> sqliteColumnNames(sqlite3* db, const std::string& tableName) {
  sqlite3_stmt* statement = nullptr;
  const std::string sql = "PRAGMA table_info('" + tableName + "');";
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  std::vector<std::string> columns;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto* name = sqlite3_column_text(statement, 1);
    columns.emplace_back(reinterpret_cast<const char*>(name));
  }
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return columns;
}

[[nodiscard]] bool hasColumn(const std::vector<std::string>& columns, const std::string& columnName) {
  return std::find(columns.begin(), columns.end(), columnName) != columns.end();
}

void requireAppSettingsSchema(sqlite3* db, const std::vector<std::string>& expectedTables) {
  const auto tables = sqliteTableNames(db);
  CHECK(tables == expectedTables);

  REQUIRE(hasTable(tables, "app_settings"));
  const auto columns = sqliteColumnNames(db, "app_settings");
  CHECK(hasColumn(columns, "group_name"));
  CHECK(hasColumn(columns, "key"));
  CHECK(hasColumn(columns, "value"));
  CHECK(hasColumn(columns, "updated_at_ms"));
}

}

TEST_CASE("SQLite app settings store persists, overwrites and lists by group") {
  TempDatabase temp{"seriona-app-settings-basic"};
  auto store = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});

  CHECK_FALSE(store->get("output", "sampleRate").has_value());
  CHECK(store->listByGroup("output").empty());

  store->set("output", "sampleRate", "48000");
  store->set("output", "outputMode", "1");
  store->set("lyrics", "delimiters", R"json([" / "])json");

  const auto rate = store->get("output", "sampleRate");
  REQUIRE(rate.has_value());
  CHECK(*rate == "48000");

  store->set("output", "sampleRate", "96000");
  const auto updated = store->get("output", "sampleRate");
  REQUIRE(updated.has_value());
  CHECK(*updated == "96000");

  const auto outputEntries = store->listByGroup("output");
  REQUIRE(outputEntries.size() == 2);
  CHECK(outputEntries[0].key == "outputMode");
  CHECK(outputEntries[0].value == "1");
  CHECK(outputEntries[1].key == "sampleRate");
  CHECK(outputEntries[1].value == "96000");
}

TEST_CASE("SQLite app settings store survives reopen and remove keeps sibling entries") {
  TempDatabase temp{"seriona-app-settings-reopen"};

  {
    auto store = seriona::control::makeSQLiteAppSettingsStore(
        seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});
    store->set("trackStats", "playCount/dir:abc:track1", "3");
    store->set("trackStats", "rating/dir:abc:track1", "5");
    store->set("library", "lastScanRoot", "/music");
  }

  auto reopened = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});
  const auto count = reopened->get("trackStats", "playCount/dir:abc:track1");
  REQUIRE(count.has_value());
  CHECK(*count == "3");

  reopened->remove("trackStats", "playCount/dir:abc:track1");
  CHECK_FALSE(reopened->get("trackStats", "playCount/dir:abc:track1").has_value());
  const auto rating = reopened->get("trackStats", "rating/dir:abc:track1");
  REQUIRE(rating.has_value());
  CHECK(*rating == "5");

  const auto root = reopened->get("library", "lastScanRoot");
  REQUIRE(root.has_value());
  CHECK(*root == "/music");
}

TEST_CASE("SQLite app settings store remove of missing entry is silent") {
  TempDatabase temp{"seriona-app-settings-remove-missing"};
  auto store = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});

  CHECK_NOTHROW(store->remove("output", "missingKey"));
  CHECK(store->listByGroup("output").empty());
}

TEST_CASE("SQLite app settings store rejects empty group or key with typed error") {
  TempDatabase temp{"seriona-app-settings-invalid"};
  auto store = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});

  CHECK_THROWS_AS(store->set("", "key", "value"), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->set("group", "", "value"), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->get("", "key"), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->get("group", ""), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->remove("", "key"), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->remove("group", ""), seriona::control::AppSettingsError);
  CHECK_THROWS_AS(store->listByGroup(""), seriona::control::AppSettingsError);

  try {
    static_cast<void>(store->get("", "key"));
    FAIL("empty group must throw a typed app settings error");
  } catch (const seriona::control::AppSettingsError& error) {
    CHECK(error.code() == seriona::control::AppSettingsErrorCode::InvalidGroup);
  }
}

TEST_CASE("SQLite app settings store creates only app settings schema on a fresh database") {
  TempDatabase temp{"seriona-app-settings-fresh-schema"};

  auto store = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});
  store->set("output", "outputMode", "0");

  SqliteHandle db{temp.path()};
  requireAppSettingsSchema(db.get(), {"app_settings"});
}

TEST_CASE("SQLite app settings store keeps scanner user_version stable and shares the database") {
  TempDatabase temp{"seriona-app-settings-scanner-db"};

  {
    seriona::scanner::cache::SQLiteCache cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }

  auto store = seriona::control::makeSQLiteAppSettingsStore(
      seriona::control::AppSettingsStoreConfig{.databasePath = temp.path()});
  store->set("output", "sampleRate", "48000");

  SqliteHandle db{temp.path()};
  CHECK(sqliteUserVersion(db.get()) == 3);
  const auto tables = sqliteTableNames(db.get());
  CHECK(hasTable(tables, "app_settings"));
  CHECK(hasTable(tables, "content"));
}
