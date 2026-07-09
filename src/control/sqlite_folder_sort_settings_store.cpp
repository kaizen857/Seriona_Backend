#include "seriona/control/folder_sort_settings_store.h"

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

FolderSortSettingsError::FolderSortSettingsError(FolderSortSettingsErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

namespace {

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] FolderSortSettingsError sqliteError(sqlite3* db, const std::string& action) {
  return FolderSortSettingsError{FolderSortSettingsErrorCode::StorageError, action + ": " + sqlite3_errmsg(db)};
}

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
    sqlite3_free(message);
    throw FolderSortSettingsError{FolderSortSettingsErrorCode::StorageError, detail};
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

[[nodiscard]] std::filesystem::path normalizeRootPath(const std::filesystem::path& rootPath) {
  if (rootPath.empty()) {
    throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRootPath, "folder sort root path is required"};
  }
  try {
    return std::filesystem::absolute(rootPath).lexically_normal();
  } catch (const std::filesystem::filesystem_error& error) {
    throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRootPath, error.what()};
  }
}

void validateFolderNodeId(const std::string& folderNodeId) {
  if (folderNodeId.empty()) {
    throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidFolderNodeId, "folder node id is required"};
  }
}

[[nodiscard]] std::string fieldText(const FolderSortField field) {
  switch (field) {
  case FolderSortField::Title:
    return "title";
  case FolderSortField::Artist:
    return "artist";
  case FolderSortField::Album:
    return "album";
  case FolderSortField::Filename:
    return "filename";
  case FolderSortField::Year:
    return "year";
  case FolderSortField::Duration:
    return "duration";
  case FolderSortField::CreatedDate:
    return "createdDate";
  case FolderSortField::DiscNumber:
    return "discNumber";
  case FolderSortField::TrackNumber:
    return "trackNumber";
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, "unknown folder sort field"};
}

[[nodiscard]] FolderSortField parseField(const std::string& value) {
  if (value == "title") {
    return FolderSortField::Title;
  }
  if (value == "artist") {
    return FolderSortField::Artist;
  }
  if (value == "album") {
    return FolderSortField::Album;
  }
  if (value == "filename") {
    return FolderSortField::Filename;
  }
  if (value == "year") {
    return FolderSortField::Year;
  }
  if (value == "duration") {
    return FolderSortField::Duration;
  }
  if (value == "createdDate") {
    return FolderSortField::CreatedDate;
  }
  if (value == "discNumber") {
    return FolderSortField::DiscNumber;
  }
  if (value == "trackNumber") {
    return FolderSortField::TrackNumber;
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, "unknown folder sort field in rules"};
}

[[nodiscard]] std::string directionText(const FolderSortDirection direction) {
  switch (direction) {
  case FolderSortDirection::Ascending:
    return "ascending";
  case FolderSortDirection::Descending:
    return "descending";
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, "unknown folder sort direction"};
}

[[nodiscard]] FolderSortDirection parseDirection(const std::string& value) {
  if (value == "ascending") {
    return FolderSortDirection::Ascending;
  }
  if (value == "descending") {
    return FolderSortDirection::Descending;
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, "unknown folder sort direction in rules"};
}

[[nodiscard]] std::string missingValuePolicyText(const FolderSortMissingValuePolicy policy) {
  switch (policy) {
  case FolderSortMissingValuePolicy::First:
    return "first";
  case FolderSortMissingValuePolicy::Last:
    return "last";
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, "unknown folder sort missing-value policy"};
}

[[nodiscard]] FolderSortMissingValuePolicy parseMissingValuePolicy(const std::string& value) {
  if (value == "first") {
    return FolderSortMissingValuePolicy::First;
  }
  if (value == "last") {
    return FolderSortMissingValuePolicy::Last;
  }
  throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson,
                                "unknown folder sort missing-value policy in rules"};
}

[[nodiscard]] std::string serializeRules(const std::vector<FolderSortRule>& rules) {
  std::string json = "[";
  for (std::size_t index = 0; index < rules.size(); ++index) {
    if (index != 0) {
      json += ',';
    }
    json += "{\"field\":\"";
    json += fieldText(rules[index].field);
    json += "\",\"direction\":\"";
    json += directionText(rules[index].direction);
    json += "\",\"missingValuePolicy\":\"";
    json += missingValuePolicyText(rules[index].missingValuePolicy);
    json += "\"}";
  }
  json += ']';
  return json;
}

class RulesJsonParser final {
public:
  explicit RulesJsonParser(std::string_view input) : input_(input) {}

  [[nodiscard]] std::vector<FolderSortRule> parse() {
    std::vector<FolderSortRule> rules;
    skipWhitespace();
    expect('[');
    skipWhitespace();
    if (consume(']')) {
      ensureEnd();
      return rules;
    }

    while (true) {
      rules.push_back(parseRuleObject());
      skipWhitespace();
      if (consume(']')) {
        ensureEnd();
        return rules;
      }
      expect(',');
    }
  }

private:
  [[noreturn]] void malformed(const std::string& message) const {
    throw FolderSortSettingsError{FolderSortSettingsErrorCode::InvalidRulesJson, message};
  }

  void skipWhitespace() {
    while (position_ < input_.size()) {
      const char ch = input_[position_];
      if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
        return;
      }
      ++position_;
    }
  }

  [[nodiscard]] bool consume(const char expected) {
    skipWhitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  void expect(const char expected) {
    if (!consume(expected)) {
      malformed("malformed folder sort rules json");
    }
  }

  void ensureEnd() {
    skipWhitespace();
    if (position_ != input_.size()) {
      malformed("trailing content in folder sort rules json");
    }
  }

  [[nodiscard]] std::string parseString() {
    skipWhitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      malformed("expected string in folder sort rules json");
    }
    ++position_;

    std::string value;
    while (position_ < input_.size()) {
      const char ch = input_[position_++];
      if (ch == '"') {
        return value;
      }
      if (ch != '\\') {
        value += ch;
        continue;
      }
      if (position_ >= input_.size()) {
        malformed("unterminated escape in folder sort rules json");
      }
      const char escaped = input_[position_++];
      switch (escaped) {
      case '\\':
        value += '\\';
        break;
      case '"':
        value += '"';
        break;
      case 'n':
        value += '\n';
        break;
      case 'r':
        value += '\r';
        break;
      case 't':
        value += '\t';
        break;
      default:
        malformed("unsupported escape in folder sort rules json");
      }
    }
    malformed("unterminated string in folder sort rules json");
  }

  [[nodiscard]] FolderSortRule parseRuleObject() {
    expect('{');
    std::optional<FolderSortField> field;
    std::optional<FolderSortDirection> direction;
    std::optional<FolderSortMissingValuePolicy> missingValuePolicy;

    while (true) {
      const auto key = parseString();
      expect(':');
      const auto value = parseString();
      if (key == "field") {
        field = parseField(value);
      } else if (key == "direction") {
        direction = parseDirection(value);
      } else if (key == "missingValuePolicy") {
        missingValuePolicy = parseMissingValuePolicy(value);
      } else {
        malformed("unknown key in folder sort rules json");
      }

      skipWhitespace();
      if (consume('}')) {
        break;
      }
      expect(',');
    }

    if (!field.has_value()) {
      malformed("missing folder sort rule field");
    }
    if (!direction.has_value()) {
      malformed("missing folder sort rule direction");
    }
    if (!missingValuePolicy.has_value()) {
      malformed("missing folder sort rule missing-value policy");
    }
    return FolderSortRule{.field = *field, .direction = *direction, .missingValuePolicy = *missingValuePolicy};
  }

  std::string_view input_;
  std::size_t position_{0};
};

[[nodiscard]] std::vector<FolderSortRule> parseRules(const std::string& rulesJson) {
  return RulesJsonParser{rulesJson}.parse();
}

class SQLiteFolderSortSettingsStore final : public FolderSortSettingsStore {
public:
  explicit SQLiteFolderSortSettingsStore(FolderSortSettingsStoreConfig config)
      : databasePath_(std::move(config.databasePath)) {
    open();
    initializeSchema();
  }

  ~SQLiteFolderSortSettingsStore() override {
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  SQLiteFolderSortSettingsStore(const SQLiteFolderSortSettingsStore&) = delete;
  SQLiteFolderSortSettingsStore& operator=(const SQLiteFolderSortSettingsStore&) = delete;

  void upsert(FolderSortSetting setting) override {
    const auto rootPath = normalizeRootPath(setting.rootPath);
    validateFolderNodeId(setting.folderNodeId);
    const auto rulesJson = serializeRules(setting.rules);
    const auto updatedAt = std::chrono::system_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    Statement upsert{db_,
                     "INSERT INTO folder_sort_rules(root_path, folder_node_id, rules_json, updated_at_ms) "
                     "VALUES(?1, ?2, ?3, ?4) "
                     "ON CONFLICT(root_path, folder_node_id) DO UPDATE SET "
                     "rules_json=excluded.rules_json, updated_at_ms=excluded.updated_at_ms;"};
    upsert.bind(1, rootPath.generic_string());
    upsert.bind(2, setting.folderNodeId);
    upsert.bind(3, rulesJson);
    upsert.bind(4, systemTimeToMs(updatedAt));
    upsert.stepDone();
  }

  [[nodiscard]] std::optional<FolderSortSetting> load(const std::filesystem::path& rootPath,
                                                      const std::string& folderNodeId) const override {
    const auto normalizedRoot = normalizeRootPath(rootPath);
    validateFolderNodeId(folderNodeId);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement select{db_,
                     "SELECT root_path, folder_node_id, rules_json "
                     "FROM folder_sort_rules WHERE root_path=?1 AND folder_node_id=?2;"};
    select.bind(1, normalizedRoot.generic_string());
    select.bind(2, folderNodeId);
    if (!select.stepRow()) {
      return std::nullopt;
    }
    return readSetting(select);
  }

  void remove(const std::filesystem::path& rootPath, const std::string& folderNodeId) override {
    const auto normalizedRoot = normalizeRootPath(rootPath);
    validateFolderNodeId(folderNodeId);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement remove{db_, "DELETE FROM folder_sort_rules WHERE root_path=?1 AND folder_node_id=?2;"};
    remove.bind(1, normalizedRoot.generic_string());
    remove.bind(2, folderNodeId);
    remove.stepDone();
  }

  [[nodiscard]] std::vector<FolderSortSetting> list(const std::filesystem::path& rootPath) const override {
    const auto normalizedRoot = normalizeRootPath(rootPath);

    std::lock_guard<std::mutex> lock(mutex_);
    Statement select{db_,
                     "SELECT root_path, folder_node_id, rules_json "
                     "FROM folder_sort_rules WHERE root_path=?1 ORDER BY folder_node_id;"};
    select.bind(1, normalizedRoot.generic_string());
    std::vector<FolderSortSetting> settings;
    while (select.stepRow()) {
      settings.push_back(readSetting(select));
    }
    return settings;
  }

private:
  void open() {
    if (databasePath_.empty()) {
      throw FolderSortSettingsError{FolderSortSettingsErrorCode::StorageError, "folder sort database path is required"};
    }
    if (!databasePath_.parent_path().empty()) {
      std::filesystem::create_directories(databasePath_.parent_path());
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(databasePath_.generic_string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
        SQLITE_OK) {
      const std::string message = db == nullptr ? "failed to open folder sort settings database" : sqlite3_errmsg(db);
      sqlite3_close(db);
      throw FolderSortSettingsError{FolderSortSettingsErrorCode::StorageError, message};
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
    exec(db_, R"sql(CREATE TABLE IF NOT EXISTS folder_sort_rules(
  root_path TEXT NOT NULL,
  folder_node_id TEXT NOT NULL,
  rules_json TEXT NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  PRIMARY KEY(root_path, folder_node_id)
);)sql");
  }

  [[nodiscard]] static FolderSortSetting readSetting(Statement& row) {
    return FolderSortSetting{.rootPath = row.textColumn(0),
                             .folderNodeId = row.textColumn(1),
                             .rules = parseRules(row.textColumn(2))};
  }

  std::filesystem::path databasePath_;
  sqlite3* db_{};
  mutable std::mutex mutex_;
};

}

std::unique_ptr<FolderSortSettingsStore> makeSQLiteFolderSortSettingsStore(FolderSortSettingsStoreConfig config) {
  return std::make_unique<SQLiteFolderSortSettingsStore>(std::move(config));
}

}
