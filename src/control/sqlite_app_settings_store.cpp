#include "seriona/control/app_settings_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::control {

AppSettingsError::AppSettingsError(AppSettingsErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

namespace {

// 路径文本恒为 UTF-8：generic_string() 在 Windows 按 CP_ACP 转换，字符不可表示时抛
// std::system_error（ERROR_NO_UNICODE_TRANSLATION）；generic_u8string() 永不抛，
// POSIX 上字节级不变。
[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] AppSettingsError sqliteError(sqlite3* db, const std::string& action) {
  return AppSettingsError{AppSettingsErrorCode::StorageError, action + ": " + sqlite3_errmsg(db)};
}

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
    sqlite3_free(message);
    throw AppSettingsError{AppSettingsErrorCode::StorageError, detail};
  }
}

void validateGroup(const std::string& group) {
  if (group.empty()) {
    throw AppSettingsError{AppSettingsErrorCode::InvalidGroup, "app settings group is required"};
  }
}

void validateKey(const std::string& key) {
  if (key.empty()) {
    throw AppSettingsError{AppSettingsErrorCode::InvalidKey, "app settings key is required"};
  }
}

class Statement final {
public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw sqliteError(db_, "prepare statement");
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(const int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
      throw sqliteError(db_, "bind text");
    }
  }

  void bind(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw sqliteError(db_, "bind int64");
    }
  }

  [[nodiscard]] bool stepRow() {
    const auto result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    throw sqliteError(db_, "step row");
  }

  void stepDone() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw sqliteError(db_, "step done");
    }
  }

  [[nodiscard]] std::string textColumn(const int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
  }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

class SQLiteAppSettingsStore final : public AppSettingsStore {
public:
  explicit SQLiteAppSettingsStore(AppSettingsStoreConfig config)
      : databasePath_(std::move(config.databasePath)) {
    open();
    initializeSchema();
  }

  ~SQLiteAppSettingsStore() override {
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  SQLiteAppSettingsStore(const SQLiteAppSettingsStore&) = delete;
  SQLiteAppSettingsStore& operator=(const SQLiteAppSettingsStore&) = delete;

  void set(std::string group, std::string key, std::string value) override {
    validateGroup(group);
    validateKey(key);
    const auto updatedAt = std::chrono::system_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement{db_,
                        "INSERT INTO app_settings(group_name, key, value, updated_at_ms) "
                        "VALUES(?1, ?2, ?3, ?4) "
                        "ON CONFLICT(group_name, key) DO UPDATE SET "
                        "value=excluded.value, updated_at_ms=excluded.updated_at_ms;"};
    statement.bind(1, group);
    statement.bind(2, key);
    statement.bind(3, value);
    statement.bind(4, systemTimeToMs(updatedAt));
    statement.stepDone();
  }

  [[nodiscard]] std::optional<std::string> get(const std::string& group, const std::string& key) const override {
    validateGroup(group);
    validateKey(key);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement{db_, "SELECT value FROM app_settings WHERE group_name=?1 AND key=?2;"};
    statement.bind(1, group);
    statement.bind(2, key);
    if (!statement.stepRow()) {
      return std::nullopt;
    }
    return statement.textColumn(0);
  }

  void remove(const std::string& group, const std::string& key) override {
    validateGroup(group);
    validateKey(key);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement{db_, "DELETE FROM app_settings WHERE group_name=?1 AND key=?2;"};
    statement.bind(1, group);
    statement.bind(2, key);
    statement.stepDone();
  }

  [[nodiscard]] std::vector<AppSettingsEntry> listByGroup(const std::string& group) const override {
    validateGroup(group);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement statement{db_, "SELECT group_name, key, value FROM app_settings WHERE group_name=?1 ORDER BY key;"};
    statement.bind(1, group);
    std::vector<AppSettingsEntry> entries;
    while (statement.stepRow()) {
      entries.push_back(AppSettingsEntry{.group = statement.textColumn(0),
                                         .key = statement.textColumn(1),
                                         .value = statement.textColumn(2)});
    }
    return entries;
  }

private:
  void open() {
    if (databasePath_.empty()) {
      throw AppSettingsError{AppSettingsErrorCode::StorageError, "app settings database path is required"};
    }
    if (!databasePath_.parent_path().empty()) {
      std::filesystem::create_directories(databasePath_.parent_path());
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(pathText(databasePath_).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      const std::string message = db == nullptr ? "failed to open app settings database" : sqlite3_errmsg(db);
      sqlite3_close(db);
      throw AppSettingsError{AppSettingsErrorCode::StorageError, message};
    }

    db_ = db;
    configureConnection();
  }

  void configureConnection() {
    if (sqlite3_busy_timeout(db_, 500) != SQLITE_OK) {
      throw sqliteError(db_, "set busy timeout");
    }
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_, "PRAGMA synchronous=NORMAL;");
    exec(db_, "PRAGMA foreign_keys=ON;");
  }

  void initializeSchema() {
    exec(db_, R"sql(CREATE TABLE IF NOT EXISTS app_settings(
  group_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  PRIMARY KEY(group_name, key)
);)sql");
  }

  std::filesystem::path databasePath_;
  sqlite3* db_{};
  mutable std::mutex mutex_;
};

}

std::unique_ptr<AppSettingsStore> makeSQLiteAppSettingsStore(AppSettingsStoreConfig config) {
  return std::make_unique<SQLiteAppSettingsStore>(std::move(config));
}

}
