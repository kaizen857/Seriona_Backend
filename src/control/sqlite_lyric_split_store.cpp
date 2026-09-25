#include "seriona/control/lyric_split_store.h"

#include "lyric_split.h"

#include <sqlite3.h>
#include <xxhash.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::control {

LyricSplitStoreError::LyricSplitStoreError(LyricSplitStoreErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

namespace {

// 路径文本恒为 UTF-8：generic_string() 在 Windows 按 CP_ACP 转换，字符不可表示时抛
// std::system_error（ERROR_NO_UNICODE_TRANSLATION）；generic_u8string() 永不抛，
// POSIX 上字节级不变。SQLite 键/值与读回路径构造必须经这两个函数往返。
[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

// 与 src/scanner/hash_utils.cpp 的 canonicalHex 一致：XXH3-128 -> canonical -> 32 位 hex。
[[nodiscard]] std::string canonicalHex(const XXH128_hash_t hash) {
  XXH128_canonical_t canonical{};
  XXH128_canonicalFromHash(&canonical, hash);

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : canonical.digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

[[nodiscard]] std::string textHash(const std::string_view rawText) {
  return canonicalHex(XXH3_128bits(rawText.data(), rawText.size()));
}

[[nodiscard]] std::int64_t systemTimeToMs(const std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point msToSystemTime(const std::int64_t milliseconds) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

[[nodiscard]] std::string sourceText(const LyricSplitSource source) {
  switch (source) {
  case LyricSplitSource::Auto:
    return "auto";
  case LyricSplitSource::Manual:
    return "manual";
  }
  throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, "unknown lyric split source"};
}

[[nodiscard]] LyricSplitSource sourceFromText(const std::string& value) {
  if (value == "auto") {
    return LyricSplitSource::Auto;
  }
  if (value == "manual") {
    return LyricSplitSource::Manual;
  }
  throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, "unknown lyric split source in table: " + value};
}

[[nodiscard]] LyricSplitStoreError sqliteError(sqlite3* db, const std::string& action) {
  return LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, action + ": " + sqlite3_errmsg(db)};
}

void exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
    sqlite3_free(message);
    throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, detail};
  }
}

void validateEntry(const LyricSplitEntry& entry) {
  if (entry.rawText.empty()) {
    throw LyricSplitStoreError{LyricSplitStoreErrorCode::InvalidEntry, "lyric split entry raw text is required"};
  }
  if (entry.targetLanguage.empty()) {
    throw LyricSplitStoreError{LyricSplitStoreErrorCode::InvalidEntry, "lyric split entry target language is required"};
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

  void bind(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
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

  [[nodiscard]] std::int64_t int64Column(const int index) const { return sqlite3_column_int64(statement_, index); }

private:
  sqlite3* db_{};
  sqlite3_stmt* statement_{};
};

[[nodiscard]] LyricSplitEntry readEntry(Statement& row) {
  const auto conventionToken = row.textColumn(2);
  const auto convention = conventionFromToken(conventionToken);
  if (!convention.has_value()) {
    throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError,
                               "unknown lyric split convention in table: " + conventionToken};
  }
  return LyricSplitEntry{.rawText = row.textColumn(4),
                         .targetLanguage = row.textColumn(1),
                         .convention = *convention,
                         .original = row.textColumn(5),
                         .translation = row.textColumn(6),
                         .source = sourceFromText(row.textColumn(3)),
                         .algoVersion = row.textColumn(7),
                         .updatedAt = msToSystemTime(row.int64Column(8))};
}

class SQLiteLyricSplitStore final : public LyricSplitStore {
public:
  explicit SQLiteLyricSplitStore(LyricSplitStoreConfig config) : databasePath_(std::move(config.databasePath)) {
    open();
    initializeSchema();
  }

  ~SQLiteLyricSplitStore() override {
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  SQLiteLyricSplitStore(const SQLiteLyricSplitStore&) = delete;
  SQLiteLyricSplitStore& operator=(const SQLiteLyricSplitStore&) = delete;

  void putAuto(LyricSplitEntry entry) override {
    validateEntry(entry);
    entry.source = LyricSplitSource::Auto;
    entry.algoVersion = std::string{kLyricSplitAlgoVersion};
    writeEntry(entry);
  }

  void upsertManual(LyricSplitEntry entry) override {
    validateEntry(entry);
    entry.source = LyricSplitSource::Manual;
    entry.algoVersion.clear();
    writeEntry(entry);
  }

  [[nodiscard]] std::optional<LyricSplitEntry> load(const std::string_view rawText,
                                                    const std::string_view targetLanguage,
                                                    const LyricSplitConvention convention) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadLocked(textHash(rawText), rawText, targetLanguage, conventionToken(convention));
  }

  void removeManual(const std::string_view rawText, const std::string_view targetLanguage,
                    const LyricSplitConvention convention) override {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement remove{db_,
                     "DELETE FROM lyric_split_entries "
                     "WHERE text_hash=?1 AND target_lang=?2 AND convention=?3 AND source='manual';"};
    remove.bind(1, textHash(rawText));
    remove.bind(2, targetLanguage);
    remove.bind(3, conventionToken(convention));
    remove.stepDone();
  }

  void clearManual() override {
    std::lock_guard<std::mutex> lock(mutex_);
    exec(db_, "DELETE FROM lyric_split_entries WHERE source='manual';");
  }

  [[nodiscard]] std::vector<LyricSplitEntry> listManual() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement select{db_,
                     "SELECT text_hash, target_lang, convention, source, raw_text, original, translation, "
                     "algo_version, updated_at_ms "
                     "FROM lyric_split_entries WHERE source='manual' "
                     "ORDER BY raw_text, target_lang, convention;"};
    std::vector<LyricSplitEntry> entries;
    while (select.stepRow()) {
      entries.push_back(readEntry(select));
    }
    return entries;
  }

  [[nodiscard]] std::string algoVersion() const override { return std::string{kLyricSplitAlgoVersion}; }

private:
  void open() {
    if (databasePath_.empty()) {
      throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, "lyric split database path is required"};
    }
    if (!databasePath_.parent_path().empty()) {
      std::filesystem::create_directories(databasePath_.parent_path());
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(pathText(databasePath_).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
      const std::string message = db == nullptr ? "failed to open lyric split database" : sqlite3_errmsg(db);
      sqlite3_close(db);
      throw LyricSplitStoreError{LyricSplitStoreErrorCode::StorageError, message};
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

  // 幂等：全部 IF NOT EXISTS，可重复调用。绝不写 PRAGMA user_version（scanner 用 kSchemaVersion=3 判定该库）。
  void initializeSchema() {
    exec(db_, R"sql(CREATE TABLE IF NOT EXISTS lyric_split_entries(
  text_hash     TEXT NOT NULL,
  target_lang   TEXT NOT NULL,
  convention    TEXT NOT NULL,
  source        TEXT NOT NULL CHECK(source IN ('auto','manual')),
  raw_text      TEXT NOT NULL,
  original      TEXT NOT NULL,
  translation   TEXT NOT NULL,
  algo_version  TEXT NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  PRIMARY KEY(text_hash, target_lang, convention, source)
);)sql");
    exec(db_, "CREATE INDEX IF NOT EXISTS lyric_split_entries_manual ON lyric_split_entries(source);");
  }

  void writeEntry(LyricSplitEntry& entry) {
    const auto updatedAt = std::chrono::system_clock::now();
    entry.updatedAt = updatedAt;

    std::lock_guard<std::mutex> lock(mutex_);
    Statement upsert{db_,
                     "INSERT INTO lyric_split_entries("
                     "text_hash, target_lang, convention, source, raw_text, original, translation, algo_version, "
                     "updated_at_ms) "
                     "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
                     "ON CONFLICT(text_hash, target_lang, convention, source) DO UPDATE SET "
                     "raw_text=excluded.raw_text, original=excluded.original, translation=excluded.translation, "
                     "algo_version=excluded.algo_version, updated_at_ms=excluded.updated_at_ms;"};
    upsert.bind(1, textHash(entry.rawText));
    upsert.bind(2, entry.targetLanguage);
    upsert.bind(3, conventionToken(entry.convention));
    upsert.bind(4, sourceText(entry.source));
    upsert.bind(5, entry.rawText);
    upsert.bind(6, entry.original);
    upsert.bind(7, entry.translation);
    upsert.bind(8, entry.algoVersion);
    upsert.bind(9, systemTimeToMs(updatedAt));
    upsert.stepDone();
  }

  [[nodiscard]] std::optional<LyricSplitEntry> loadLocked(const std::string& hash, const std::string_view rawText,
                                                          const std::string_view targetLanguage,
                                                          const std::string& convention) const {
    {
      Statement manual{db_, "SELECT text_hash, target_lang, convention, source, raw_text, original, translation, "
                            "algo_version, updated_at_ms "
                            "FROM lyric_split_entries "
                            "WHERE text_hash=?1 AND target_lang=?2 AND convention=?3 AND raw_text=?4 AND source='manual';"};
      manual.bind(1, hash);
      manual.bind(2, targetLanguage);
      manual.bind(3, convention);
      manual.bind(4, rawText);
      if (manual.stepRow()) {
        return readEntry(manual);
      }
    }

    Statement autoRow{db_,
                      "SELECT text_hash, target_lang, convention, source, raw_text, original, translation, "
                      "algo_version, updated_at_ms "
                      "FROM lyric_split_entries "
                      "WHERE text_hash=?1 AND target_lang=?2 AND convention=?3 AND source='auto' AND algo_version=?4 AND "
                      "raw_text=?5;"};
    autoRow.bind(1, hash);
    autoRow.bind(2, targetLanguage);
    autoRow.bind(3, convention);
    autoRow.bind(4, kLyricSplitAlgoVersion);
    autoRow.bind(5, rawText);
    if (autoRow.stepRow()) {
      return readEntry(autoRow);
    }
    return std::nullopt;
  }

  std::filesystem::path databasePath_;
  sqlite3* db_{};
  mutable std::mutex mutex_;
};

}

std::unique_ptr<LyricSplitStore> makeSQLiteLyricSplitStore(LyricSplitStoreConfig config) {
  return std::make_unique<SQLiteLyricSplitStore>(std::move(config));
}

}
