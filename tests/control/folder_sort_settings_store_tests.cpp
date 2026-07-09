#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

void requireNoPlaybackPersistenceTables(const std::vector<std::string>& tables) {
  for (const auto& forbidden : {"playlist", "playback_context", "playback_order", "queue", "cursor", "shuffle"}) {
    CAPTURE(forbidden);
    for (const auto& table : tables) {
      CAPTURE(table);
      CHECK(table.find(forbidden) == std::string::npos);
    }
  }
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

[[nodiscard]] std::filesystem::path normalizedRoot(const std::filesystem::path& rootPath) {
  return std::filesystem::absolute(rootPath).lexically_normal();
}

[[nodiscard]] seriona::control::FolderSortSetting settingFor(std::filesystem::path rootPath,
                                                             std::string folderNodeId,
                                                             std::vector<seriona::control::FolderSortRule> rules) {
  return seriona::control::FolderSortSetting{.rootPath = std::move(rootPath),
                                             .folderNodeId = std::move(folderNodeId),
                                             .rules = std::move(rules)};
}

[[nodiscard]] bool rulesEqual(const std::vector<seriona::control::FolderSortRule>& left,
                              const std::vector<seriona::control::FolderSortRule>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].field != right[index].field || left[index].direction != right[index].direction ||
        left[index].missingValuePolicy != right[index].missingValuePolicy) {
      return false;
    }
  }
  return true;
}

void requireFolderSortSchema(sqlite3* db, const int expectedUserVersion, const std::vector<std::string>& expectedTables) {
  const auto tables = sqliteTableNames(db);
  CHECK(tables == expectedTables);
  CHECK(sqliteUserVersion(db) == expectedUserVersion);
  requireNoPlaybackPersistenceTables(tables);

  REQUIRE(hasTable(tables, "folder_sort_rules"));
  const auto columns = sqliteColumnNames(db, "folder_sort_rules");
  CHECK(hasColumn(columns, "root_path"));
  CHECK(hasColumn(columns, "folder_node_id"));
  CHECK(hasColumn(columns, "rules_json"));
  CHECK(hasColumn(columns, "updated_at_ms"));
}

void insertMalformedRules(const std::filesystem::path& databasePath,
                          const std::filesystem::path& rootPath,
                          const std::string& folderNodeId) {
  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open_v2(databasePath.generic_string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);

  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(db,
                             "INSERT INTO folder_sort_rules(root_path, folder_node_id, rules_json, updated_at_ms) "
                             "VALUES(?1, ?2, ?3, ?4);",
                             -1,
                             &statement,
                             nullptr) == SQLITE_OK);
  const auto normalized = normalizedRoot(rootPath).generic_string();
  REQUIRE(sqlite3_bind_text(statement, 1, normalized.c_str(), static_cast<int>(normalized.size()), SQLITE_TRANSIENT) ==
          SQLITE_OK);
  REQUIRE(sqlite3_bind_text(statement, 2, folderNodeId.c_str(), static_cast<int>(folderNodeId.size()), SQLITE_TRANSIENT) ==
          SQLITE_OK);
  const std::string badJson = R"json([{"field":"title","direction":"ascending"}])json";
  REQUIRE(sqlite3_bind_text(statement, 3, badJson.c_str(), static_cast<int>(badJson.size()), SQLITE_TRANSIENT) == SQLITE_OK);
  REQUIRE(sqlite3_bind_int64(statement, 4, 1) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  REQUIRE(sqlite3_close(db) == SQLITE_OK);
}

}

TEST_CASE("folder sort settings characterization: scanner cache alone keeps runtime schema v3 unchanged") {
  TempDatabase temp{"seriona-folder-sort-settings-characterization"};

  {
    seriona::scanner::cache::SQLiteCacheV3 cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }

  SqliteHandle db{temp.path()};
  const auto tables = sqliteTableNames(db.get());
  const std::vector<std::string> expectedTables{"content", "locations", "lyrics", "scan_errors", "scan_roots"};

  CHECK(tables == expectedTables);
  CHECK(sqliteUserVersion(db.get()) == 3);
  CHECK_FALSE(hasTable(tables, "folder_sort_rules"));
  requireNoPlaybackPersistenceTables(tables);
}

TEST_CASE("SQLite folder sort settings store persists by normalized root and folder node id") {
  TempDatabase temp{"seriona-folder-sort-settings-root-folder"};
  auto store = seriona::control::makeSQLiteFolderSortSettingsStore(
      seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});

  const auto rootA = temp.path().parent_path() / "library-a" / ".." / "library-a";
  const auto rootB = temp.path().parent_path() / "library-b";
  const std::string folderNodeId = "dir:albums";
  const std::vector<seriona::control::FolderSortRule> rootARules{
      {.field = seriona::control::FolderSortField::Title,
       .direction = seriona::control::FolderSortDirection::Ascending,
       .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::Last},
      {.field = seriona::control::FolderSortField::Artist,
       .direction = seriona::control::FolderSortDirection::Descending,
       .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::First},
  };
  const std::vector<seriona::control::FolderSortRule> rootBRules{
      {.field = seriona::control::FolderSortField::Album,
       .direction = seriona::control::FolderSortDirection::Descending,
       .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::Last},
  };

  store->upsert(settingFor(rootA, folderNodeId, rootARules));
  store->upsert(settingFor(rootB, folderNodeId, rootBRules));

  const auto loadedA = store->load(normalizedRoot(rootA), folderNodeId);
  const auto loadedB = store->load(rootB, folderNodeId);

  REQUIRE(loadedA.has_value());
  REQUIRE(loadedB.has_value());
  CHECK(loadedA->rootPath == normalizedRoot(rootA));
  CHECK(loadedB->rootPath == normalizedRoot(rootB));
  CHECK(loadedA->folderNodeId == folderNodeId);
  CHECK(loadedB->folderNodeId == folderNodeId);
  CHECK(rulesEqual(loadedA->rules, rootARules));
  CHECK(rulesEqual(loadedB->rules, rootBRules));
  REQUIRE(loadedA->rules.size() == 2);
  CHECK(loadedA->rules[1].missingValuePolicy == seriona::control::FolderSortMissingValuePolicy::First);

  const auto listedA = store->list(rootA);
  REQUIRE(listedA.size() == 1);
  CHECK(rulesEqual(listedA.front().rules, rootARules));
}

TEST_CASE("SQLite folder sort settings store survives reopen and delete keeps sibling settings") {
  TempDatabase temp{"seriona-folder-sort-settings-reopen"};
  const auto root = temp.path().parent_path() / "library";
  const std::vector<seriona::control::FolderSortRule> titleRules{
      {.field = seriona::control::FolderSortField::Title,
       .direction = seriona::control::FolderSortDirection::Ascending,
       .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::Last},
  };
  const std::vector<seriona::control::FolderSortRule> durationRules{
      {.field = seriona::control::FolderSortField::Duration,
       .direction = seriona::control::FolderSortDirection::Descending,
       .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::First},
  };

  {
    auto store = seriona::control::makeSQLiteFolderSortSettingsStore(
        seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});
    store->upsert(settingFor(root, "dir:one", titleRules));
    store->upsert(settingFor(root, "dir:two", durationRules));
  }

  auto reopened = seriona::control::makeSQLiteFolderSortSettingsStore(
      seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});
  const auto loaded = reopened->load(root, "dir:one");
  REQUIRE(loaded.has_value());
  CHECK(rulesEqual(loaded->rules, titleRules));

  reopened->remove(root, "dir:one");
  CHECK_FALSE(reopened->load(root, "dir:one").has_value());
  const auto sibling = reopened->load(root, "dir:two");
  REQUIRE(sibling.has_value());
  CHECK(rulesEqual(sibling->rules, durationRules));
}

TEST_CASE("SQLite folder sort settings store missing loads are empty and create only folder sort schema") {
  TempDatabase temp{"seriona-folder-sort-settings-missing"};
  const auto root = temp.path().parent_path() / "library";

  auto store = seriona::control::makeSQLiteFolderSortSettingsStore(
      seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});

  CHECK_FALSE(store->load(root, "dir:missing").has_value());
  CHECK(store->list(root).empty());

  SqliteHandle db{temp.path()};
  requireFolderSortSchema(db.get(), 0, {"folder_sort_rules"});
}

TEST_CASE("SQLite folder sort settings store keeps scanner user_version stable and creates no playback tables") {
  TempDatabase temp{"seriona-folder-sort-settings-scanner-db"};

  {
    seriona::scanner::cache::SQLiteCacheV3 cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }

  auto store = seriona::control::makeSQLiteFolderSortSettingsStore(
      seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});
  store->upsert(settingFor(temp.path().parent_path() / "library",
                           "dir:albums",
                           {{.field = seriona::control::FolderSortField::Title,
                             .direction = seriona::control::FolderSortDirection::Ascending,
                             .missingValuePolicy = seriona::control::FolderSortMissingValuePolicy::Last}}));

  SqliteHandle db{temp.path()};
  requireFolderSortSchema(db.get(), 3, {"content", "folder_sort_rules", "locations", "lyrics", "scan_errors", "scan_roots"});
}

TEST_CASE("SQLite folder sort settings store rejects malformed persisted rules with typed error") {
  TempDatabase temp{"seriona-folder-sort-settings-malformed"};
  const auto root = temp.path().parent_path() / "library";

  auto store = seriona::control::makeSQLiteFolderSortSettingsStore(
      seriona::control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});
  insertMalformedRules(temp.path(), root, "dir:broken");

  try {
    static_cast<void>(store->load(root, "dir:broken"));
    FAIL("persisted rules without missing-value policy must throw a typed folder sort settings error");
  } catch (const seriona::control::FolderSortSettingsError& error) {
    CHECK(error.code() == seriona::control::FolderSortSettingsErrorCode::InvalidRulesJson);
  }
}
