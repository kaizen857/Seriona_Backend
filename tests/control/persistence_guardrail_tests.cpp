#include "control_test_harness.h"

#include "seriona/control/folder_sort_settings_store.h"
#include "seriona/control/media_controller.h"
#include "seriona/scanner/cache/sqlite_cache_v3.h"

#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace control = seriona::control;
namespace control_test = seriona::control::test;
namespace scanner_cache = seriona::scanner::cache;

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

enum class OpenMode { ReadOnly, ReadWriteCreate };

class SqliteHandle final {
public:
  SqliteHandle(const std::filesystem::path& databasePath, const OpenMode mode = OpenMode::ReadOnly) {
    const auto flags = mode == OpenMode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    REQUIRE(sqlite3_open_v2(databasePath.generic_string().c_str(), &db_, flags, nullptr) == SQLITE_OK);
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

void execSql(sqlite3* db, const char* sql) {
  char* message = nullptr;
  const auto result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
  const std::string detail = message == nullptr ? std::string{} : std::string{message};
  INFO(detail);
  sqlite3_free(message);
  REQUIRE(result == SQLITE_OK);
}

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

[[nodiscard]] int tableRowCount(sqlite3* db, const std::string& tableName) {
  sqlite3_stmt* statement = nullptr;
  const std::string sql = "SELECT COUNT(*) FROM " + tableName + ";";
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const auto count = sqlite3_column_int(statement, 0);
  REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  return count;
}

[[nodiscard]] std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

enum class ForbiddenMatchKind { Exact, Prefix, Contains };

struct ForbiddenTablePattern {
  std::string_view pattern;
  std::string_view token;
  ForbiddenMatchKind kind;
};

struct ForbiddenTableViolation {
  std::string table;
  std::string pattern;
};

[[nodiscard]] constexpr std::array<ForbiddenTablePattern, 6> forbiddenTablePatterns() noexcept {
  return {{{.pattern = "playlist%", .token = "playlist", .kind = ForbiddenMatchKind::Prefix},
           {.pattern = "queue%", .token = "queue", .kind = ForbiddenMatchKind::Prefix},
           {.pattern = "playback_context", .token = "playback_context", .kind = ForbiddenMatchKind::Exact},
           {.pattern = "playback_order_items", .token = "playback_order_items", .kind = ForbiddenMatchKind::Exact},
           {.pattern = "%cursor%", .token = "cursor", .kind = ForbiddenMatchKind::Contains},
           {.pattern = "%shuffle%", .token = "shuffle", .kind = ForbiddenMatchKind::Contains}}};
}

[[nodiscard]] bool matchesForbiddenPattern(const std::string& tableName, const ForbiddenTablePattern& pattern) {
  const auto lowerTable = toLowerAscii(tableName);
  const std::string token{pattern.token};
  switch (pattern.kind) {
  case ForbiddenMatchKind::Exact:
    return lowerTable == token;
  case ForbiddenMatchKind::Prefix:
    return lowerTable.rfind(token, 0) == 0;
  case ForbiddenMatchKind::Contains:
    return lowerTable.find(token) != std::string::npos;
  }
  return false;
}

[[nodiscard]] std::vector<ForbiddenTableViolation> forbiddenPersistenceViolations(
    const std::vector<std::string>& tableNames) {
  std::vector<ForbiddenTableViolation> violations;
  for (const auto& table : tableNames) {
    for (const auto& pattern : forbiddenTablePatterns()) {
      if (matchesForbiddenPattern(table, pattern)) {
        violations.push_back(ForbiddenTableViolation{.table = table, .pattern = std::string{pattern.pattern}});
      }
    }
  }
  return violations;
}

void requireNoForbiddenPersistenceTables(const std::vector<std::string>& tableNames) {
  const auto violations = forbiddenPersistenceViolations(tableNames);
  for (const auto& violation : violations) {
    CAPTURE(violation.table);
    CAPTURE(violation.pattern);
  }
  REQUIRE(violations.empty());
}

void requireFolderSortTableShape(sqlite3* db) {
  const std::vector<std::string> expectedColumns{"root_path", "folder_node_id", "rules_json", "updated_at_ms"};
  CHECK(sqliteColumnNames(db, "folder_sort_rules") == expectedColumns);
}

void requirePersistenceGuardrailSchema(sqlite3* db,
                                       const int expectedUserVersion,
                                       const std::vector<std::string>& expectedTables) {
  const auto tables = sqliteTableNames(db);
  CHECK(tables == expectedTables);
  CHECK(sqliteUserVersion(db) == expectedUserVersion);
  requireNoForbiddenPersistenceTables(tables);
  requireFolderSortTableShape(db);
}

[[nodiscard]] std::filesystem::path normalizedRoot(const std::filesystem::path& rootPath) {
  return std::filesystem::absolute(rootPath).lexically_normal();
}

[[nodiscard]] control::FolderSortSetting settingFor(std::filesystem::path rootPath, std::string folderNodeId) {
  return control::FolderSortSetting{
      .rootPath = std::move(rootPath),
      .folderNodeId = std::move(folderNodeId),
      .rules = {{.field = control::FolderSortField::Title,
                 .direction = control::FolderSortDirection::Ascending,
                 .missingValuePolicy = control::FolderSortMissingValuePolicy::Last}}};
}

void initializeEmptySqliteDatabase(const std::filesystem::path& databasePath) {
  SqliteHandle db{databasePath, OpenMode::ReadWriteCreate};
  CHECK(sqliteUserVersion(db.get()) == 0);
  CHECK(sqliteTableNames(db.get()).empty());
}

void initializeScannerV3Database(const std::filesystem::path& databasePath) {
  scanner_cache::SQLiteCacheV3 cache{scanner_cache::ScannerCacheConfig{.databasePath = databasePath}};
  CHECK(cache.schemaVersion() == 3);
}

void runStoreInitialization(const std::filesystem::path& databasePath) {
  auto store = control::makeSQLiteFolderSortSettingsStore(control::FolderSortSettingsStoreConfig{.databasePath = databasePath});
  REQUIRE(store != nullptr);
}

void runControllerApplySortCommand(const std::filesystem::path& databasePath, const std::filesystem::path& rootPath) {
  auto sqliteStore = control::makeSQLiteFolderSortSettingsStore(
      control::FolderSortSettingsStoreConfig{.databasePath = databasePath});

  control::MediaControllerDependencies dependencies{};
  dependencies.audio = std::make_shared<control_test::FakeAudioPlaybackService>();
  dependencies.scanner = std::make_shared<control_test::FakeFileScannerService>();
  dependencies.metadata = std::make_unique<control_test::FakeMetadataSharingService>();
  dependencies.folderSortSettingsStore = std::shared_ptr<control::FolderSortSettingsStore>{std::move(sqliteStore)};

  auto controller = control::makeMediaController(std::move(dependencies),
                                                 control::MediaControllerOptions{.runInlineForTests = true});
  controller->start();

  control::MediaControlCommand command{};
  command.kind = control::MediaControlCommandKind::ApplyFolderSortRules;
  command.folderSortSetting = settingFor(rootPath, "dir:albums");

  const auto result = controller->submitCommand(command);
  CHECK(result.accepted);
  CHECK(result.code == control::MediaControllerErrorCode::None);

  controller->shutdown();
}

[[nodiscard]] std::string joinedTables(const std::vector<std::string>& tables) {
  std::string result;
  for (const auto& table : tables) {
    if (!result.empty()) {
      result += ',';
    }
    result += table;
  }
  return result;
}

void removeSqliteArtifacts(const std::filesystem::path& databasePath) {
  std::error_code error;
  std::filesystem::remove(databasePath, error);
  std::filesystem::remove(databasePath.string() + "-wal", error);
  std::filesystem::remove(databasePath.string() + "-shm", error);
}

}

TEST_CASE("persistence guardrail baseline: folder sort store initialization creates only sort schema") {
  TempDatabase temp{"seriona-control-persistence-guardrail-baseline"};
  initializeEmptySqliteDatabase(temp.path());

  runStoreInitialization(temp.path());

  SqliteHandle db{temp.path()};
  requirePersistenceGuardrailSchema(db.get(), 0, {"folder_sort_rules"});
  CHECK(tableRowCount(db.get(), "folder_sort_rules") == 0);
}

TEST_CASE("persistence guardrail: controller sort command on fresh DB persists only folder settings") {
  TempDatabase temp{"seriona-control-persistence-guardrail-fresh"};
  initializeEmptySqliteDatabase(temp.path());

  const auto root = temp.path().parent_path() / "library";
  runControllerApplySortCommand(temp.path(), root);

  SqliteHandle db{temp.path()};
  requirePersistenceGuardrailSchema(db.get(), 0, {"folder_sort_rules"});
  CHECK(tableRowCount(db.get(), "folder_sort_rules") == 1);

  auto store = control::makeSQLiteFolderSortSettingsStore(control::FolderSortSettingsStoreConfig{.databasePath = temp.path()});
  const auto loaded = store->load(normalizedRoot(root), "dir:albums");
  REQUIRE(loaded.has_value());
  CHECK(loaded->rootPath == normalizedRoot(root));
  CHECK(loaded->folderNodeId == "dir:albums");
  REQUIRE(loaded->rules.size() == 1);
  CHECK(loaded->rules.front().field == control::FolderSortField::Title);
}

TEST_CASE("persistence guardrail: controller sort command on scanner-v3 DB does not migrate or bump user_version") {
  TempDatabase temp{"seriona-control-persistence-guardrail-scanner-v3"};
  initializeScannerV3Database(temp.path());

  {
    SqliteHandle before{temp.path()};
    CHECK(sqliteUserVersion(before.get()) == 3);
    CHECK(sqliteTableNames(before.get()) == std::vector<std::string>{"content", "locations", "lyrics", "scan_errors", "scan_roots"});
    requireNoForbiddenPersistenceTables(sqliteTableNames(before.get()));
  }

  runControllerApplySortCommand(temp.path(), temp.path().parent_path() / "library");

  SqliteHandle db{temp.path()};
  requirePersistenceGuardrailSchema(db.get(),
                                    3,
                                    {"content", "folder_sort_rules", "locations", "lyrics", "scan_errors", "scan_roots"});
  CHECK(tableRowCount(db.get(), "folder_sort_rules") == 1);
}

TEST_CASE("persistence guardrail detector flags injected playback-state tables") {
  TempDatabase temp{"seriona-control-persistence-guardrail-injected-forbidden"};
  SqliteHandle db{temp.path(), OpenMode::ReadWriteCreate};

  execSql(db.get(), R"sql(
CREATE TABLE folder_sort_rules(root_path TEXT NOT NULL, folder_node_id TEXT NOT NULL, rules_json TEXT NOT NULL, updated_at_ms INTEGER NOT NULL);
CREATE TABLE playlist_shadow(id INTEGER PRIMARY KEY);
CREATE TABLE queue_items(id INTEGER PRIMARY KEY);
CREATE TABLE playback_context(id INTEGER PRIMARY KEY);
CREATE TABLE playback_order_items(id INTEGER PRIMARY KEY);
CREATE TABLE current_cursor(id INTEGER PRIMARY KEY);
CREATE TABLE shuffle_history(id INTEGER PRIMARY KEY);
)sql");

  const auto violations = forbiddenPersistenceViolations(sqliteTableNames(db.get()));
  CHECK(violations.size() == forbiddenTablePatterns().size());
  for (const auto& pattern : forbiddenTablePatterns()) {
    CAPTURE(pattern.pattern);
    CHECK(std::any_of(violations.begin(), violations.end(), [pattern](const ForbiddenTableViolation& violation) {
      return violation.pattern == pattern.pattern;
    }));
  }
}

TEST_CASE("persistence guardrail proof fixture creates scanner-v3 database for external inspection") {
  const char* proofPath = std::getenv("SERIONA_PERSISTENCE_GUARDRAIL_PROOF_DB");
  if (proofPath == nullptr || std::string_view{proofPath}.empty()) {
    return;
  }

  const std::filesystem::path databasePath{proofPath};
  if (!databasePath.parent_path().empty()) {
    std::filesystem::create_directories(databasePath.parent_path());
  }
  removeSqliteArtifacts(databasePath);

  initializeScannerV3Database(databasePath);
  runControllerApplySortCommand(databasePath, databasePath.parent_path() / "library");

  SqliteHandle db{databasePath};
  const auto tables = sqliteTableNames(db.get());
  const auto violations = forbiddenPersistenceViolations(tables);
  const auto userVersion = sqliteUserVersion(db.get());

  std::cout << "TASK11_DIRECT_PROOF db=" << databasePath.generic_string()
            << " forbidden_table_count=" << violations.size()
            << " user_version=" << userVersion
            << " tables=" << joinedTables(tables) << '\n';

  CHECK(violations.empty());
  CHECK(userVersion == 3);
  CHECK(tables == std::vector<std::string>{"content", "folder_sort_rules", "locations", "lyrics", "scan_errors", "scan_roots"});
}
