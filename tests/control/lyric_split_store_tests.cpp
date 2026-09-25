#include "seriona/control/lyric_split_store.h"
#include "seriona/metadata/metadata_contracts.h"
#include "seriona/scanner/cache/sqlite_cache.h"
#include "../../src/control/control_event_loop.h"
#include "../../src/control/media_controller_module.h"
#include "../../src/control/track_lyrics_split.h"

#include <doctest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TempDatabase final {
public:
  explicit TempDatabase(std::string name)
      : directory_(std::filesystem::temp_directory_path() /
                   (std::move(name) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
        databasePath_(directory_ / "library.sqlite") {
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

class RawDatabase final {
public:
  explicit RawDatabase(const std::filesystem::path& databasePath) {
    REQUIRE(sqlite3_open_v2(databasePath.generic_string().c_str(),
                            &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) == SQLITE_OK);
  }

  ~RawDatabase() {
    if (db_ != nullptr) {
      static_cast<void>(sqlite3_close(db_));
    }
  }

  RawDatabase(const RawDatabase&) = delete;
  RawDatabase& operator=(const RawDatabase&) = delete;

  void exec(const std::string& sql) {
    char* message = nullptr;
    REQUIRE(sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK);
    sqlite3_free(message);
  }

  void execText(const std::string& sql, const std::string& value) {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
  }

  [[nodiscard]] std::int64_t queryInt(const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = sqlite3_column_int64(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    return value;
  }

  [[nodiscard]] std::int64_t countByRawText(const std::string& table, const std::string& rawText) {
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "SELECT COUNT(*) FROM " + table + " WHERE raw_text=?1;";
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, rawText.c_str(), static_cast<int>(rawText.size()), SQLITE_TRANSIENT) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = sqlite3_column_int64(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    return value;
  }

  [[nodiscard]] std::int64_t countAutoRowsMismatchingVersion(const std::string& rawText) {
    sqlite3_stmt* statement = nullptr;
    const std::string sql =
        "SELECT COUNT(*) FROM lyric_split_entries WHERE raw_text=?1 AND source='auto' AND algo_version<>?2;";
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, rawText.c_str(), static_cast<int>(rawText.size()), SQLITE_TRANSIENT) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 2, "1", 1, SQLITE_TRANSIENT) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = sqlite3_column_int64(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    return value;
  }

  [[nodiscard]] std::int64_t storedUpdatedAtMs(const std::string& rawText) {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_,
                               "SELECT updated_at_ms FROM lyric_split_entries WHERE raw_text=?1 LIMIT 1;",
                               -1,
                               &statement,
                               nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, rawText.c_str(), static_cast<int>(rawText.size()), SQLITE_TRANSIENT) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = sqlite3_column_int64(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    return value;
  }

  [[nodiscard]] std::vector<std::string> tableNames() {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_,
                               "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' "
                               "ORDER BY name;",
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

  [[nodiscard]] std::vector<std::string> primaryKeyColumns(const std::string& tableName) {
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "PRAGMA table_info('" + tableName + "');";
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    std::vector<std::pair<int, std::string>> ordered;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const auto* name = sqlite3_column_text(statement, 1);
      const auto pk = sqlite3_column_int(statement, 5);
      if (pk > 0) {
        ordered.emplace_back(pk, reinterpret_cast<const char*>(name));
      }
    }
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> columns;
    columns.reserve(ordered.size());
    for (auto& [position, name] : ordered) {
      columns.push_back(std::move(name));
    }
    return columns;
  }

  [[nodiscard]] int userVersion() {
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    const auto version = sqlite3_column_int(statement, 0);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    return version;
  }

  [[nodiscard]] bool hasTable(const std::string& tableName) {
    const auto tables = tableNames();
    return std::find(tables.begin(), tables.end(), tableName) != tables.end();
  }

private:
  sqlite3* db_{};
};

[[nodiscard]] seriona::control::LyricSplitEntry entryFor(std::string rawText,
                                                         std::string original,
                                                         std::string translation,
                                                         seriona::control::LyricSplitConvention convention,
                                                         std::string targetLanguage = "zh") {
  return seriona::control::LyricSplitEntry{.rawText = std::move(rawText),
                                           .targetLanguage = std::move(targetLanguage),
                                           .convention = convention,
                                           .original = std::move(original),
                                           .translation = std::move(translation)};
}

constexpr std::string_view kSampleRawText = "悔しいけど好きって純情 虽然不甘但还是喜欢你 这份纯情";

}

TEST_CASE("SQLite lyric split store: putAuto round-trips and persists a real timestamp") {
  TempDatabase temp{"seriona-lyric-split-roundtrip"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  CHECK(store->algoVersion() == "1");

  auto entry = entryFor(std::string{kSampleRawText},
                        "悔しいけど好きって",
                        "純情 虽然不甘但还是喜欢你 这份纯情",
                        seriona::control::LyricSplitConvention::StrongSlashSpaced);
  store->putAuto(entry);

  const auto loaded = store->load(kSampleRawText, "zh", seriona::control::LyricSplitConvention::StrongSlashSpaced);
  REQUIRE(loaded.has_value());
  CHECK(loaded->rawText == std::string{kSampleRawText});
  CHECK(loaded->targetLanguage == "zh");
  CHECK(loaded->convention == seriona::control::LyricSplitConvention::StrongSlashSpaced);
  CHECK(loaded->original == "悔しいけど好きって");
  CHECK(loaded->translation == "純情 虽然不甘但还是喜欢你 这份纯情");
  CHECK(loaded->source == seriona::control::LyricSplitSource::Auto);
  CHECK(loaded->algoVersion == "1");

  CHECK(loaded->updatedAt != std::chrono::system_clock::time_point{});
  CHECK(loaded->updatedAt.time_since_epoch().count() > 0);

  RawDatabase db{temp.path()};
  CHECK(db.storedUpdatedAtMs(std::string{kSampleRawText}) > 0);
}

TEST_CASE("SQLite lyric split store: manual wins and removeManual restores auto immediately") {
  TempDatabase temp{"seriona-lyric-split-manual-priority"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::StrongSlashSpaced;

  store->putAuto(entryFor("原文行", "原文行", "自动译文", convention));
  store->upsertManual(entryFor("原文行", "人工原文", "人工译文", convention));

  const auto manual = store->load("原文行", "zh", convention);
  REQUIRE(manual.has_value());
  CHECK(manual->source == seriona::control::LyricSplitSource::Manual);
  CHECK(manual->original == "人工原文");
  CHECK(manual->translation == "人工译文");
  CHECK(manual->algoVersion.empty());

  store->removeManual("原文行", "zh", convention);
  const auto restored = store->load("原文行", "zh", convention);
  REQUIRE(restored.has_value());
  CHECK(restored->source == seriona::control::LyricSplitSource::Auto);
  CHECK(restored->original == "原文行");
  CHECK(restored->algoVersion == "1");

  RawDatabase db{temp.path()};
  CHECK(db.countByRawText("lyric_split_entries", "原文行") == 1);
}

TEST_CASE("SQLite lyric split store: putAuto never overwrites a manual row") {
  TempDatabase temp{"seriona-lyric-split-putauto-manual"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::None;

  store->upsertManual(entryFor("一行", "人工", "人工译文", convention));
  store->putAuto(entryFor("一行", "自动", "自动译文", convention));

  const auto loaded = store->load("一行", "zh", convention);
  REQUIRE(loaded.has_value());
  CHECK(loaded->source == seriona::control::LyricSplitSource::Manual);
  CHECK(loaded->original == "人工");

  RawDatabase db{temp.path()};
  CHECK(db.countByRawText("lyric_split_entries", "一行") == 2);
}

TEST_CASE("SQLite lyric split store: clearManual clears manual only and listManual lists manual only") {
  TempDatabase temp{"seriona-lyric-split-clear-manual"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::None;

  store->putAuto(entryFor("auto-keep", "auto", "a", convention));
  store->putAuto(entryFor("manual-drop", "auto", "a", convention));
  store->upsertManual(entryFor("manual-drop", "manual", "m", convention));
  store->upsertManual(entryFor("manual-two", "manual2", "m2", convention));

  const auto listed = store->listManual();
  REQUIRE(listed.size() == 2);
  for (const auto& item : listed) {
    CHECK(item.source == seriona::control::LyricSplitSource::Manual);
  }
  CHECK(listed[0].rawText == "manual-drop");
  CHECK(listed[1].rawText == "manual-two");

  store->clearManual();
  CHECK(store->listManual().empty());

  RawDatabase db{temp.path()};
  CHECK(db.countByRawText("lyric_split_entries", "manual-drop") == 1);
  CHECK(db.countByRawText("lyric_split_entries", "manual-two") == 0);
  CHECK(db.countByRawText("lyric_split_entries", "auto-keep") == 1);
}

TEST_CASE("SQLite lyric split store: initializeSchema is idempotent and never writes user_version") {
  TempDatabase temp{"seriona-lyric-split-idempotent"};
  const auto config = seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()};

  {
    auto first = seriona::control::makeSQLiteLyricSplitStore(config);
    first->putAuto(entryFor("持久化", "持久化", "persisted", seriona::control::LyricSplitConvention::None));
  }
  {
    auto second = seriona::control::makeSQLiteLyricSplitStore(config);
    second->putAuto(entryFor("第二", "第二", "second", seriona::control::LyricSplitConvention::None));
    CHECK(second->algoVersion() == "1");
  }
  auto third = seriona::control::makeSQLiteLyricSplitStore(config);
  const auto persisted = third->load("持久化", "zh", seriona::control::LyricSplitConvention::None);
  REQUIRE(persisted.has_value());
  CHECK(persisted->translation == "persisted");

  RawDatabase db{temp.path()};
  CHECK(db.userVersion() == 0);
  CHECK(db.hasTable("lyric_split_entries"));
  const auto pk = db.primaryKeyColumns("lyric_split_entries");
  const std::vector<std::string> expectedPk{"text_hash", "target_lang", "convention", "source"};
  CHECK(pk == expectedPk);
}

TEST_CASE("SQLite lyric split store: convention dimension isolates the same raw text") {
  TempDatabase temp{"seriona-lyric-split-convention-isolation"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  store->putAuto(entryFor("同一行 原文 译文", "同一行", "原文 译文",
                          seriona::control::LyricSplitConvention::StrongSlashSpaced));
  store->putAuto(entryFor("同一行 原文 译文", "同一行 原文 译文", "",
                          seriona::control::LyricSplitConvention::None));

  const auto spaced = store->load("同一行 原文 译文", "zh", seriona::control::LyricSplitConvention::StrongSlashSpaced);
  const auto none = store->load("同一行 原文 译文", "zh", seriona::control::LyricSplitConvention::None);
  REQUIRE(spaced.has_value());
  REQUIRE(none.has_value());
  CHECK(spaced->original == "同一行");
  CHECK(spaced->translation == "原文 译文");
  CHECK(none->original == "同一行 原文 译文");
  CHECK(none->translation.empty());

  RawDatabase db{temp.path()};
  CHECK(db.countByRawText("lyric_split_entries", "同一行 原文 译文") == 2);
}

TEST_CASE("SQLite lyric split store: mismatched auto algo version is a miss but never deleted") {
  TempDatabase temp{"seriona-lyric-split-algo-version"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::None;

  store->putAuto(entryFor("版本行", "旧", "old", convention));
  REQUIRE(store->load("版本行", "zh", convention).has_value());

  RawDatabase db{temp.path()};
  db.execText("UPDATE lyric_split_entries SET algo_version='0-stale' WHERE raw_text=?1;", "版本行");

  CHECK_FALSE(store->load("版本行", "zh", convention).has_value());
  CHECK(db.countByRawText("lyric_split_entries", "版本行") == 1);
  CHECK(db.countAutoRowsMismatchingVersion("版本行") == 1);
}

TEST_CASE("SQLite lyric split store: same raw text twice keeps timestamp non-decreasing") {
  TempDatabase temp{"seriona-lyric-split-timestamp"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::None;

  store->putAuto(entryFor("时间戳", "一", "first", convention));
  const auto first = store->load("时间戳", "zh", convention);
  REQUIRE(first.has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds{2});
  store->upsertManual(entryFor("时间戳", "二", "second", convention));
  const auto second = store->load("时间戳", "zh", convention);
  REQUIRE(second.has_value());

  CHECK(first->updatedAt != std::chrono::system_clock::time_point{});
  CHECK(second->updatedAt != std::chrono::system_clock::time_point{});
  CHECK(second->updatedAt >= first->updatedAt);
}

TEST_CASE("SQLite lyric split store: shares library.sqlite with scanner cache opened first") {
  TempDatabase temp{"seriona-lyric-split-coexist-cache-first"};

  {
    seriona::scanner::cache::SQLiteCache cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }

  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  store->putAuto(entryFor("共存", "共存", "coexist", seriona::control::LyricSplitConvention::None));
  REQUIRE(store->load("共存", "zh", seriona::control::LyricSplitConvention::None).has_value());

  {
    RawDatabase db{temp.path()};
    CHECK(db.userVersion() == 3);
    CHECK(db.hasTable("lyric_split_entries"));
    CHECK(db.hasTable("lyrics"));
  }

  {
    seriona::scanner::cache::SQLiteCache cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }
}

TEST_CASE("SQLite lyric split store: shares library.sqlite with scanner cache opened second") {
  TempDatabase temp{"seriona-lyric-split-coexist-store-first"};

  {
    auto store = seriona::control::makeSQLiteLyricSplitStore(
        seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
    store->putAuto(entryFor("先建", "先建", "store-first", seriona::control::LyricSplitConvention::None));
  }

  {
    RawDatabase db{temp.path()};
    CHECK(db.userVersion() == 0);
  }

  {
    seriona::scanner::cache::SQLiteCache cache{
        seriona::scanner::cache::ScannerCacheConfig{.databasePath = temp.path()}};
    CHECK(cache.schemaVersion() == 3);
  }

  {
    RawDatabase db{temp.path()};
    CHECK(db.userVersion() == 3);
    CHECK(db.hasTable("lyric_split_entries"));
  }

  auto reopened = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto loaded = reopened->load("先建", "zh", seriona::control::LyricSplitConvention::None);
  REQUIRE(loaded.has_value());
  CHECK(loaded->translation == "store-first");
}

TEST_CASE("lyric split store injection: default dependencies inject a safe no-op store") {
  auto dependencies = seriona::control::makeDefaultMediaControllerDependencies();
  REQUIRE(dependencies.lyricSplitStore != nullptr);

  auto& store = *dependencies.lyricSplitStore;
  CHECK(store.algoVersion() == "1");
  CHECK_FALSE(store.load("注入-空路径", "zh", seriona::control::LyricSplitConvention::None).has_value());
  CHECK(store.listManual().empty());

  const auto entry = entryFor("注入-空路径", "原文", "译文", seriona::control::LyricSplitConvention::None);
  CHECK_NOTHROW(store.putAuto(entry));
  CHECK_NOTHROW(store.upsertManual(entry));
  CHECK_NOTHROW(store.removeManual("注入-空路径", "zh", seriona::control::LyricSplitConvention::None));
  CHECK_NOTHROW(store.clearManual());

  CHECK_FALSE(store.load("注入-空路径", "zh", seriona::control::LyricSplitConvention::None).has_value());
  CHECK(store.listManual().empty());
}

TEST_CASE("lyric split store injection: production dependencies inject a writable sqlite store") {
  TempDatabase temp{"seriona-lyric-split-injection-sqlite"};
  auto dependencies = seriona::control::makeProductionMediaControllerDependencies(temp.path(), {});
  REQUIRE(dependencies.lyricSplitStore != nullptr);

  const auto convention = seriona::control::LyricSplitConvention::StrongSlashSpaced;
  auto& store = *dependencies.lyricSplitStore;
  CHECK(store.algoVersion() == "1");

  store.putAuto(entryFor("注入-非空路径", "自动原文", "自动译文", convention));
  const auto autoEntry = store.load("注入-非空路径", "zh", convention);
  REQUIRE(autoEntry.has_value());
  CHECK(autoEntry->source == seriona::control::LyricSplitSource::Auto);
  CHECK(autoEntry->original == "自动原文");

  store.upsertManual(entryFor("注入-非空路径", "人工原文", "人工译文", convention));
  const auto manualEntry = store.load("注入-非空路径", "zh", convention);
  REQUIRE(manualEntry.has_value());
  CHECK(manualEntry->source == seriona::control::LyricSplitSource::Manual);
  REQUIRE(store.listManual().size() == 1);
  CHECK(store.listManual().front().rawText == "注入-非空路径");
}

TEST_CASE("SQLite lyric split store: hash collision with a different raw text is a miss") {
  TempDatabase temp{"seriona-lyric-split-collision"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto convention = seriona::control::LyricSplitConvention::None;

  store->putAuto(entryFor("碰撞-原文", "原文", "译文", convention));
  REQUIRE(store->load("碰撞-原文", "zh", convention).has_value());

  RawDatabase db{temp.path()};
  db.execText("UPDATE lyric_split_entries SET raw_text='碰撞-替换' WHERE raw_text=?1;", "碰撞-原文");

  CHECK(db.countByRawText("lyric_split_entries", "碰撞-替换") == 1);

  const auto loaded = store->load("碰撞-原文", "zh", convention);
  if (loaded.has_value()) {
    CAPTURE(loaded->rawText);
    CHECK(loaded->rawText == "碰撞-原文");
  }
  CHECK_FALSE(loaded.has_value());
  CHECK(db.countByRawText("lyric_split_entries", "碰撞-替换") == 1);
}

namespace {

using namespace std::chrono_literals;

struct LyricSplitFixture {
  seriona::control::PlayerStateSnapshot player;
  seriona::control::LibraryStateSnapshot library;
};

[[nodiscard]] LyricSplitFixture lyricSplitFixtureFor(std::string trackId,
                                                    std::vector<seriona::scanner::LyricLine> lyrics) {
  seriona::scanner::SongMetadata song;
  song.trackId = trackId;
  song.effectiveLyrics = std::move(lyrics);

  seriona::scanner::PlaylistNode node;
  node.nodeId = "node:" + trackId;
  node.kind = seriona::scanner::PlaylistNodeKind::Track;
  node.displayName = "track";
  node.song = std::move(song);

  seriona::scanner::PlaylistTreeSnapshot tree;
  tree.version = 1;
  tree.rootNodeId = node.nodeId;
  tree.nodes.push_back(std::move(node));

  seriona::control::PlayerStateSnapshot player;
  player.currentTrack = seriona::control::TrackIdentity{.trackId = trackId};

  seriona::control::LibraryStateSnapshot library;
  library.libraryTree = std::move(tree);

  return LyricSplitFixture{.player = std::move(player), .library = std::move(library)};
}

[[nodiscard]] seriona::scanner::LyricLine lyric(std::chrono::milliseconds timestamp, std::string text) {
  return seriona::scanner::LyricLine{.timestamp = timestamp, .text = std::move(text)};
}

constexpr std::string_view kSplitSampleLine = "悔しいけど好きって純情 虽然不甘但还是喜欢你 这份纯情";

// 让 inferSplitConvention 推断出 WeakSpace（"W: "）的文档：目标行 + 若干空格分隔行。
[[nodiscard]] std::vector<seriona::scanner::LyricLine> weakSpaceDocument() {
  return {lyric(0ms, std::string{kSplitSampleLine}),
          lyric(1000ms, "きみがすき 我喜欢你"),
          lyric(2000ms, "あしたも 明天也是"),
          lyric(3000ms, "そら 天空"),
          lyric(4000ms, "うみ 大海")};
}

// 两行空格分隔即可让整首推断为 WeakSpace（单行只会得 None）。
[[nodiscard]] std::vector<seriona::scanner::LyricLine> twoLineWeakSpace(std::string_view first,
                                                                      std::string_view second) {
  return {lyric(0ms, std::string{first}), lyric(1000ms, std::string{second})};
}

}

TEST_CASE("track lyrics split: auto line is computed once then served from the store") {
  TempDatabase temp{"seriona-track-split-count"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture = lyricSplitFixtureFor("track-count", {lyric(0ms, std::string{kSplitSampleLine})});

  int splitCalls = 0;
  const seriona::control::LyricLineSplitFn probe =
      [&splitCalls](std::string_view line, seriona::control::LyricSplitConvention convention,
                    std::string_view language) {
        ++splitCalls;
        return seriona::control::splitLyricLine(line, convention, language);
      };

  const auto first = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
  REQUIRE(first.has_value());
  REQUIRE(first->lines.size() == 1);
  CHECK(splitCalls == 1);
  CHECK(first->lines.front().manualOverride == false);
  CHECK(first->lines.front().autoOriginal.empty());
  CHECK(first->lines.front().autoTranslation.empty());

  const auto second = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
  REQUIRE(second.has_value());
  CHECK(splitCalls == 1);
  CHECK(second->lines.front().text == first->lines.front().text);
  CHECK(second->lines.front().original == first->lines.front().original);
}

TEST_CASE("track lyrics split: manual row wins and exposes the auto result it overrides") {
  TempDatabase temp{"seriona-track-split-manual"};
  const auto databasePath = temp.path();
  const auto fixture = lyricSplitFixtureFor("track-manual", weakSpaceDocument());
  const std::string cleaned{kSplitSampleLine};

  std::string autoOriginal;
  std::string autoTranslation;
  {
    auto store = seriona::control::makeSQLiteLyricSplitStore(
        seriona::control::LyricSplitStoreConfig{.databasePath = databasePath});
    const auto snapshot =
        seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->convention == seriona::control::LyricSplitConvention::WeakSpace);
    autoOriginal = snapshot->lines.front().original;
    autoTranslation = snapshot->lines.front().translation;

    store->upsertManual(seriona::control::LyricSplitEntry{.rawText = cleaned,
                                                         .targetLanguage = "zh",
                                                         .convention = snapshot->convention,
                                                         .original = "人工原文",
                                                         .translation = "人工译文"});

    const auto overridden =
        seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
    REQUIRE(overridden.has_value());
    const auto& row = overridden->lines.front();
    CHECK(row.manualOverride == true);
    CHECK(row.original == "人工原文");
    CHECK(row.translation == "人工译文");
    CHECK(row.autoOriginal == autoOriginal);
    CHECK(row.autoTranslation == autoTranslation);
    CHECK(row.autoOriginal == "悔しいけど好きって純情");
    CHECK(row.autoTranslation == "虽然不甘但还是喜欢你 这份纯情");
  }
  CHECK(autoOriginal == "悔しいけど好きって純情");
  CHECK(autoTranslation != "人工译文");
}

TEST_CASE("track lyrics split: language is part of the key so switching language recomputes") {
  TempDatabase temp{"seriona-track-split-language"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture = lyricSplitFixtureFor("track-language", {lyric(0ms, std::string{kSplitSampleLine})});

  int splitCalls = 0;
  const seriona::control::LyricLineSplitFn probe =
      [&splitCalls](std::string_view line, seriona::control::LyricSplitConvention convention,
                    std::string_view language) {
        ++splitCalls;
        return seriona::control::splitLyricLine(line, convention, language);
      };

  const auto zh = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
  REQUIRE(zh.has_value());
  CHECK(splitCalls == 1);
  CHECK(store->load(std::string{kSplitSampleLine}, "zh", zh->convention).has_value());

  const auto ja = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "ja", *store, probe);
  REQUIRE(ja.has_value());
  CHECK(splitCalls == 2);
  CHECK(ja->targetLanguage == "ja");
  CHECK(store->load(std::string{kSplitSampleLine}, "ja", ja->convention).has_value());
}

TEST_CASE("track lyrics split: changed lyric text is a different key and is not served stale") {
  TempDatabase temp{"seriona-track-split-text-change"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  int splitCalls = 0;
  const seriona::control::LyricLineSplitFn probe =
      [&splitCalls](std::string_view line, seriona::control::LyricSplitConvention convention,
                    std::string_view language) {
        ++splitCalls;
        return seriona::control::splitLyricLine(line, convention, language);
      };

  const auto firstFixture =
      lyricSplitFixtureFor("track-text", twoLineWeakSpace("きみがすき 我喜欢你", "そら 天空"));
  const auto first =
      seriona::control::buildTrackLyricsSnapshot(firstFixture.player, firstFixture.library, "zh", *store, probe);
  REQUIRE(first.has_value());
  CHECK(first->convention == seriona::control::LyricSplitConvention::WeakSpace);
  CHECK(splitCalls == 2);
  CHECK(first->lines.front().text == "きみがすき 我喜欢你");
  CHECK(first->lines.front().original == "きみがすき");
  CHECK(first->lines.front().translation == "我喜欢你");

  const auto changedFixture =
      lyricSplitFixtureFor("track-text", twoLineWeakSpace("うみ 大海", "そら 天空"));
  const auto changed =
      seriona::control::buildTrackLyricsSnapshot(changedFixture.player, changedFixture.library, "zh", *store, probe);
  REQUIRE(changed.has_value());
  CHECK(splitCalls == 3);
  CHECK(changed->lines.front().text == "うみ 大海");
  CHECK(changed->lines.front().original == "うみ");
  CHECK(changed->lines.front().translation == "大海");
}

TEST_CASE("track lyrics split: the same line under two inferred conventions keeps two results") {
  TempDatabase temp{"seriona-track-split-convention-docs"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  const auto weakFixture = lyricSplitFixtureFor("track-weak", weakSpaceDocument());
  const auto noneFixture = lyricSplitFixtureFor("track-none", {lyric(0ms, std::string{kSplitSampleLine})});

  const auto weak = seriona::control::buildTrackLyricsSnapshot(weakFixture.player, weakFixture.library, "zh", *store);
  const auto none = seriona::control::buildTrackLyricsSnapshot(noneFixture.player, noneFixture.library, "zh", *store);
  REQUIRE(weak.has_value());
  REQUIRE(none.has_value());

  CHECK(weak->convention == seriona::control::LyricSplitConvention::WeakSpace);
  CHECK(none->convention == seriona::control::LyricSplitConvention::None);

  const auto& weakRow = weak->lines.front();
  const auto& noneRow = none->lines.front();
  CHECK(weakRow.original == "悔しいけど好きって純情");
  CHECK(weakRow.translation == "虽然不甘但还是喜欢你 这份纯情");
  CHECK(weakRow.split == true);
  CHECK(noneRow.original == kSplitSampleLine);
  CHECK(noneRow.translation.empty());
  CHECK(noneRow.split == false);
  CHECK(weakRow.translation != noneRow.translation);

  RawDatabase db{temp.path()};
  CHECK(db.countByRawText("lyric_split_entries", std::string{kSplitSampleLine}) == 2);
}

TEST_CASE("track lyrics split: validate-failed auto rows report split as false") {
  TempDatabase temp{"seriona-track-split-validate-failed"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  const auto fixture = lyricSplitFixtureFor(
      "track-validate", {lyric(0ms, std::string{kSplitSampleLine}), lyric(1000ms, "x / y"), lyric(2000ms, "a / b"),
                         lyric(3000ms, "c / d")});
  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->convention == seriona::control::LyricSplitConvention::StrongSlashSpaced);

  const auto& row = snapshot->lines.front();
  CHECK(row.translation.empty());
  CHECK(row.split == false);
  CHECK(row.original == kSplitSampleLine);

  CHECK(snapshot->lines[1].translation == "y");
  CHECK(snapshot->lines[1].split == true);
}

TEST_CASE("track lyrics split: mismatched stored algo version is recomputed and rewritten") {
  TempDatabase temp{"seriona-track-split-algo-version"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture =
      lyricSplitFixtureFor("track-algo", twoLineWeakSpace("きみがすき 我喜欢你", "そら 天空"));

  int splitCalls = 0;
  const seriona::control::LyricLineSplitFn probe =
      [&splitCalls](std::string_view line, seriona::control::LyricSplitConvention convention,
                    std::string_view language) {
        ++splitCalls;
        return seriona::control::splitLyricLine(line, convention, language);
      };

  const auto first = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
  REQUIRE(first.has_value());
  CHECK(first->lines.front().text == "きみがすき 我喜欢你");
  CHECK(splitCalls == 2);

  RawDatabase db{temp.path()};
  db.execText("UPDATE lyric_split_entries SET algo_version='0-stale' WHERE raw_text=?1;", "きみがすき 我喜欢你");
  CHECK(db.countAutoRowsMismatchingVersion("きみがすき 我喜欢你") == 1);

  const auto second = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
  REQUIRE(second.has_value());
  CHECK(splitCalls == 3);
  CHECK(second->lines.front().original == "きみがすき");
  CHECK(second->lines.front().translation == "我喜欢你");
  CHECK(db.countAutoRowsMismatchingVersion("きみがすき 我喜欢你") == 0);
}

TEST_CASE("track lyrics split: snapshot key is the cleaned line and manual removal round-trips") {
  TempDatabase temp{"seriona-track-split-key-roundtrip"};
  const auto databasePath = temp.path();
  // raw 行带时间戳前缀：cleanLine 会剥掉它 ⇒ 键是清洗行，不是文件原始行。
  const std::string rawLine = "[00:01.000]きみがすき 我喜欢你";
  const auto fixture = lyricSplitFixtureFor("track-key", {lyric(1000ms, rawLine), lyric(5000ms, "そら 天空")});

  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = databasePath});
  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->lines.size() == 2);
  CHECK(snapshot->convention == seriona::control::LyricSplitConvention::WeakSpace);

  const auto cleaned = seriona::control::cleanLine(rawLine);
  REQUIRE(cleaned.has_value());
  CHECK(snapshot->lines.front().text == *cleaned);
  CHECK(snapshot->lines.front().text == "きみがすき 我喜欢你");
  CHECK(snapshot->lines.front().text != rawLine);

  store->upsertManual(seriona::control::LyricSplitEntry{.rawText = snapshot->lines.front().text,
                                                       .targetLanguage = "zh",
                                                       .convention = snapshot->convention,
                                                       .original = "键往返人工",
                                                       .translation = "键往返译文"});
  const auto withManual = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(withManual.has_value());
  REQUIRE(withManual->lines.size() == 2);
  CHECK(withManual->lines.front().manualOverride == true);
  CHECK(withManual->lines.front().translation == "键往返译文");

  store->removeManual(snapshot->lines.front().text, "zh", snapshot->convention);
  const auto restored = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(restored.has_value());
  REQUIRE(restored->lines.size() == 2);
  CHECK(restored->lines.front().manualOverride == false);
  CHECK(restored->lines.front().original == "きみがすき");
  CHECK(restored->lines.front().translation == "我喜欢你");
}

TEST_CASE("track lyrics split: manualOverride marks only the corrected line") {
  TempDatabase temp{"seriona-track-split-manual-scope"};
  const auto databasePath = temp.path();
  const auto fixture = lyricSplitFixtureFor(
      "track-scope", {lyric(0ms, "きみがすき 我喜欢你"), lyric(1000ms, "そら 天空"), lyric(2000ms, "うみ 大海")});

  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = databasePath});
  const auto before = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(before.has_value());
  REQUIRE(before->lines.size() == 3);
  for (const auto& row : before->lines) {
    CHECK(row.manualOverride == false);
  }
  CHECK(before->convention == seriona::control::LyricSplitConvention::WeakSpace);

  store->upsertManual(seriona::control::LyricSplitEntry{.rawText = "そら 天空",
                                                       .targetLanguage = "zh",
                                                       .convention = before->convention,
                                                       .original = "空",
                                                       .translation = "天空（人工）"});
  const auto after = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(after.has_value());
  REQUIRE(after->lines.size() == 3);
  CHECK(after->lines[0].manualOverride == false);
  CHECK(after->lines[1].manualOverride == true);
  CHECK(after->lines[2].manualOverride == false);
  CHECK(after->lines[1].text == "そら 天空");
  CHECK(after->lines[1].original == "空");
  CHECK(after->lines[1].translation == "天空（人工）");
  CHECK(after->lines[1].autoOriginal == "そら");
  CHECK(after->lines[1].autoTranslation == "天空");
  CHECK(after->lines[0].autoTranslation.empty());
  CHECK(after->lines[0].translation == "我喜欢你");
}

TEST_CASE("track lyrics split: timestamps pass through per line, including zero") {
  TempDatabase temp{"seriona-track-split-timestamps"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture = lyricSplitFixtureFor(
      "track-timestamps", {lyric(0ms, "きみがすき 我喜欢你"), lyric(5000ms, "そら 天空")});

  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->lines.size() == 2);
  CHECK(snapshot->lines[0].timestamp == 0ms);
  CHECK(snapshot->lines[1].timestamp == 5000ms);
  CHECK(snapshot->lines[0].timestamp != snapshot->lines[1].timestamp);
}

TEST_CASE("track lyrics split: reference pairing wins over the in-line separator path") {
  TempDatabase temp{"seriona-track-split-pairing"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  // 同一 timestamp 的两行：第 1 行 = 原文，第 2 行 = 译文（D22）。两行都没有行内分隔符。
  const auto fixture = lyricSplitFixtureFor(
      "track-pairing", {lyric(1000ms, "悔しいけど好きって純情"), lyric(1000ms, "虽然不甘但还是喜欢你")});

  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->lines.size() == 1);
  CHECK(snapshot->lines.front().timestamp == 1000ms);
  CHECK(snapshot->lines.front().original == "悔しいけど好きって純情");
  CHECK(snapshot->lines.front().translation == "虽然不甘但还是喜欢你");
  CHECK(snapshot->lines.front().split == true);
}

TEST_CASE("track lyrics split: no current track or no lyrics yields no snapshot") {
  TempDatabase temp{"seriona-track-split-empty"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});

  seriona::control::PlayerStateSnapshot noTrack;
  seriona::control::LibraryStateSnapshot emptyLibrary;
  CHECK_FALSE(seriona::control::buildTrackLyricsSnapshot(noTrack, emptyLibrary, "zh", *store).has_value());

  const auto withTrackNoLyrics = lyricSplitFixtureFor("track-none-lyrics", {});
  CHECK_FALSE(seriona::control::buildTrackLyricsSnapshot(withTrackNoLyrics.player, withTrackNoLyrics.library, "zh",
                                                        *store)
                  .has_value());

  const auto mismatched = lyricSplitFixtureFor("track-other", {lyric(0ms, "きみがすき 我喜欢你")});
  seriona::control::PlayerStateSnapshot otherTrack;
  otherTrack.currentTrack = seriona::control::TrackIdentity{.trackId = "track-missing"};
  CHECK_FALSE(
      seriona::control::buildTrackLyricsSnapshot(otherTrack, mismatched.library, "zh", *store).has_value());
}

namespace {

// 记录键流量的装饰器：用于断言「组装快照时查表/写 auto 用的键」与 `SplitLyricLine.text` 逐字节相同。
class RecordingLyricSplitStore final : public seriona::control::LyricSplitStore {
public:
  explicit RecordingLyricSplitStore(seriona::control::LyricSplitStore& inner) : inner_(inner) {}

  void putAuto(seriona::control::LyricSplitEntry entry) override {
    autoKeys.push_back(entry.rawText);
    inner_.putAuto(std::move(entry));
  }

  void upsertManual(seriona::control::LyricSplitEntry entry) override {
    manualKeys.push_back(entry.rawText);
    inner_.upsertManual(std::move(entry));
  }

  [[nodiscard]] std::optional<seriona::control::LyricSplitEntry> load(
      std::string_view rawText, std::string_view targetLanguage,
      seriona::control::LyricSplitConvention convention) const override {
    loadKeys.emplace_back(rawText);
    return inner_.load(rawText, targetLanguage, convention);
  }

  void removeManual(std::string_view rawText, std::string_view targetLanguage,
                    seriona::control::LyricSplitConvention convention) override {
    inner_.removeManual(rawText, targetLanguage, convention);
  }

  void clearManual() override { inner_.clearManual(); }

  [[nodiscard]] std::vector<seriona::control::LyricSplitEntry> listManual() const override {
    return inner_.listManual();
  }

  [[nodiscard]] std::string algoVersion() const override { return inner_.algoVersion(); }

  std::vector<std::string> autoKeys;
  mutable std::vector<std::string> loadKeys;
  std::vector<std::string> manualKeys;

private:
  seriona::control::LyricSplitStore& inner_;
};

}

TEST_CASE("track lyrics split: store keys are byte-identical to SplitLyricLine.text") {
  TempDatabase temp{"seriona-track-split-key-identity"};
  auto inner = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  RecordingLyricSplitStore recorder{*inner};
  const std::string rawLine = "[00:01.000]きみがすき 我喜欢你";
  const auto fixture = lyricSplitFixtureFor("track-key-identity", {lyric(1000ms, rawLine), lyric(5000ms, "そら 天空")});

  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", recorder);
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->lines.size() == 2);
  REQUIRE(recorder.loadKeys.size() == 2);
  REQUIRE(recorder.autoKeys.size() == 2);

  // 键 = 清洗行（不是带时间戳的原始行），且快照里的 text 与键逐字节相同。
  for (std::size_t i = 0; i < snapshot->lines.size(); ++i) {
    CHECK(snapshot->lines[i].text == recorder.loadKeys[i]);
    CHECK(snapshot->lines[i].text == recorder.autoKeys[i]);
  }
  CHECK(snapshot->lines.front().text == "きみがすき 我喜欢你");
  CHECK(snapshot->lines.front().text != rawLine);

  // 用快照里的 text 删纠错必须能命中该行（键往返）。
  const auto& target = snapshot->lines.front();
  inner->upsertManual(seriona::control::LyricSplitEntry{.rawText = target.text,
                                                       .targetLanguage = "zh",
                                                       .convention = snapshot->convention,
                                                       .original = "往返原文",
                                                       .translation = "往返译文"});
  REQUIRE(inner->load(target.text, "zh", snapshot->convention).has_value());
  inner->removeManual(target.text, "zh", snapshot->convention);
  const auto afterRemove = inner->load(target.text, "zh", snapshot->convention);
  REQUIRE(afterRemove.has_value());
  CHECK(afterRemove->source == seriona::control::LyricSplitSource::Auto);
}

TEST_CASE("track lyrics split: snapshot position uses the contract default") {
  TempDatabase temp{"seriona-track-split-position"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture = lyricSplitFixtureFor("track-position", weakSpaceDocument());

  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->position == seriona::control::TranslationPosition::LastLanguageSegment);
  CHECK(snapshot->trackId == "track-position");
  CHECK(snapshot->targetLanguage == "zh");
}

TEST_CASE("track lyrics split: unpaired multi-line groups expand to one row per line") {
  TempDatabase temp{"seriona-track-split-pairing-triple"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  // 同 timestamp 三行：第 1 行原文 + 后两行译文 ⇒ 合成一个 original、两行译文以 \n 连接。
  const auto fixture = lyricSplitFixtureFor(
      "track-pairing-triple", {lyric(1000ms, "悔しいけど好きって純情"), lyric(1000ms, "虽然不甘"),
                               lyric(1000ms, "但还是喜欢你")});

  const auto snapshot = seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store);
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->lines.size() == 1);
  CHECK(snapshot->lines.front().original == "悔しいけど好きって純情");
  CHECK(snapshot->lines.front().translation == "虽然不甘\n但还是喜欢你");
  CHECK(snapshot->lines.front().split == true);
}

namespace {

struct WorkerSplitOutcome {
  std::thread::id splitThread{};
  bool splitCalled{false};
  bool hasSnapshot{false};
  std::size_t lineCount{0};
};

}

TEST_CASE("track lyrics split: flow executes on the control worker thread, not the caller") {
  TempDatabase temp{"seriona-track-split-thread"};
  auto store = seriona::control::makeSQLiteLyricSplitStore(
      seriona::control::LyricSplitStoreConfig{.databasePath = temp.path()});
  const auto fixture = lyricSplitFixtureFor("track-thread", weakSpaceDocument());

  seriona::control::ControlEventLoop loop{};
  loop.start();

  const auto callerThread = std::this_thread::get_id();
  std::promise<WorkerSplitOutcome> outcomePromise;
  auto outcomeFuture = outcomePromise.get_future();

  const bool posted = loop.post([&] {
    WorkerSplitOutcome outcome;
    const seriona::control::LyricLineSplitFn probe =
        [&outcome](std::string_view line, seriona::control::LyricSplitConvention convention,
                   std::string_view language) {
          outcome.splitThread = std::this_thread::get_id();
          outcome.splitCalled = true;
          return seriona::control::splitLyricLine(line, convention, language);
        };
    const auto snapshot =
        seriona::control::buildTrackLyricsSnapshot(fixture.player, fixture.library, "zh", *store, probe);
    outcome.hasSnapshot = snapshot.has_value();
    outcome.lineCount = snapshot.has_value() ? snapshot->lines.size() : 0;
    outcomePromise.set_value(outcome);
  });
  REQUIRE(posted);
  REQUIRE(outcomeFuture.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

  const WorkerSplitOutcome outcome = outcomeFuture.get();
  CHECK(outcome.splitCalled);
  CHECK(outcome.hasSnapshot);
  CHECK(outcome.lineCount == 5);
  CHECK(outcome.splitThread != callerThread);

  loop.stop();
}
