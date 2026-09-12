#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace seriona::scanner::cache {

struct CachedSong {
  SongMetadata metadata;
  std::vector<LyricLine> embeddedLyrics;
  std::vector<LyricLine> externalLyrics;
};

struct CachedUserStats {
  std::uint32_t playCount{0};
  std::uint32_t rating{0};
  std::optional<std::chrono::system_clock::time_point> lastPlayed;
};

struct ScannerCacheConfig {
  std::filesystem::path databasePath;
};

struct CachedLocation {
  std::string locationId;
  std::string contentId;
  std::filesystem::path rootPath;
  std::filesystem::path filePath;
  std::uint64_t fileSizeBytes{0};
  std::int64_t fileMtimeNs{0};
  std::filesystem::path sourceFilePath;
  std::optional<std::chrono::milliseconds> cueTrackOffset;
  std::optional<std::uint32_t> cueTrackIndex;
  std::optional<std::chrono::milliseconds> cueTrackDuration;
  std::optional<std::uint64_t> cueFileSizeBytes;
  std::optional<std::int64_t> cueFileMtimeNs;
  std::optional<std::uint64_t> sourceFileSizeBytes;
  std::optional<std::int64_t> sourceFileMtimeNs;
  std::optional<std::filesystem::path> artworkPath;
  std::optional<std::filesystem::path> thumbnailPath;
  LyricsSource lyricsSource{LyricsSource::None};
  std::optional<std::filesystem::path> externalLrcPath;
  std::optional<std::int64_t> externalLrcMtimeNs;
  std::optional<std::string> externalLrcHash;
  std::chrono::system_clock::time_point discoveredAt{};
  std::chrono::system_clock::time_point scannedAt{};
};

struct CachedScanRoot {
  std::filesystem::path rootPath;
  std::string directoryTreeHash;
  std::uint64_t totalFiles{0};
  ScanMode lastScanMode{ScanMode::Incremental};
  std::chrono::milliseconds lastScanDuration{0};
  std::chrono::system_clock::time_point lastScanAt{};
};

struct CachedScanError {
  std::filesystem::path rootPath;
  std::optional<std::filesystem::path> filePath;
  ScannerErrorCode errorCode{ScannerErrorCode::MetadataReadFailed};
  std::string errorMessage;
  std::chrono::system_clock::time_point occurredAt{};
};

struct CacheWriteSong {
  CachedSong song;
  CachedLocation location;
};

struct LyricsCacheUpdate {
  std::string locationId;
  std::optional<std::filesystem::path> externalLrcPath;
  std::optional<std::int64_t> externalLrcMtimeNs;
  std::optional<std::string> externalLrcHash;
  std::vector<LyricLine> externalLyrics;
  LyricsSource effectiveLyricsSource{LyricsSource::ExternalLrc};
  bool removeExternalLyrics{false};
};

struct ScanRootCacheWrite {
  CachedScanRoot root;
  std::vector<CacheWriteSong> changedSongs;
  std::vector<CacheWriteSong> changedCueTracks;
  std::vector<LyricsCacheUpdate> lyricsUpdates;
  std::vector<std::string> retainedLocationIds;
};

class SQLiteCache {
public:
  class WriterTransaction {
  public:
    ~WriterTransaction();
    WriterTransaction(const WriterTransaction&) = delete;
    WriterTransaction& operator=(const WriterTransaction&) = delete;
    WriterTransaction(WriterTransaction&& other) noexcept;
    WriterTransaction& operator=(WriterTransaction&& other) noexcept;
    void commit();

  private:
    friend class SQLiteCache;
    explicit WriterTransaction(SQLiteCache& cache);
    SQLiteCache* cache_{nullptr};
    std::unique_lock<std::mutex> lock_;
    bool active_{false};
  };

  explicit SQLiteCache(ScannerCacheConfig config);
  ~SQLiteCache();

  SQLiteCache(const SQLiteCache&) = delete;
  SQLiteCache& operator=(const SQLiteCache&) = delete;

  [[nodiscard]] int schemaVersion() const;
  [[nodiscard]] std::string journalMode() const;
  void upsertContent(const std::string& contentId, const SongMetadata& metadata);
  [[nodiscard]] std::optional<CachedSong> loadContent(const std::string& contentId) const;
  void updateUserStats(const std::string& contentId, const CachedUserStats& userStats);
  void upsertLocation(const CachedLocation& location);
  [[nodiscard]] std::optional<CachedLocation> loadLocation(const std::string& locationId) const;
  [[nodiscard]] std::vector<CachedLocation> loadLocationsByRoot(const std::filesystem::path& rootPath) const;
  void pruneDeletedLocations(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedLocationIds);
  std::int64_t deleteLocationsByPathPrefix(const std::string& rootPath, const std::string& filePathPrefix);
  std::int64_t deleteLocationsByPathPrefixNoTransaction(const std::string& rootPath, const std::string& filePathPrefix);
  [[nodiscard]] std::int64_t countLocationsByPathPrefix(const std::string& rootPath,
                                                        const std::string& filePathPrefix) const;
  // 单事务：删除 root 下 file_path/source_file_path 以 filePathPrefix 开头的行，再写入 songs，
  // 最后恢复 restored（scope 外 cue 轨的补偿行，见 B2）。scoped 子树对账的原子合并入口。
  void replaceLocationsByPathPrefixWithSongs(const std::string& rootPath,
                                             const std::string& filePathPrefix,
                                             const std::vector<CacheWriteSong>& songs,
                                             const std::vector<CacheWriteSong>& restored);
  // 单事务：把 root 下 file_path/source_file_path 以 oldPrefix（= 或 前缀/）开头的行改写为 newPrefix 前缀。
  // 数据来自既有缓存行改写，绝不重读元数据/触发扫描。返回受影响行数。
  std::int64_t replaceLocationsBySubtree(const std::string& rootPath, const std::string& oldPrefix, const std::string& newPrefix);
  void replaceLyrics(const std::string& locationId, const std::string& kind, const std::vector<LyricLine>& lyrics);
  [[nodiscard]] std::vector<LyricLine> loadLyrics(const std::string& locationId, const std::string& kind) const;
  // 单事务批量歌词对账（.lrc 事件专用；不得构造 ScanRootCacheWrite，其空 retained 会清空 root）。
  void applyLyricsCacheUpdates(const std::vector<LyricsCacheUpdate>& updates);
  void updateScanRoot(const CachedScanRoot& root);
  void recordScanRootCacheWrite(const ScanRootCacheWrite& write);
  [[nodiscard]] std::optional<CachedScanRoot> loadScanRoot(const std::filesystem::path& rootPath) const;
  // 删除 root 的 scan_roots 行；locations/scan_errors 经外键级联清理（显式根移除专用）。
  void deleteScanRoot(const std::filesystem::path& rootPath);
  void saveErrors(const std::filesystem::path& rootPath, const std::vector<CachedScanError>& errors);
  [[nodiscard]] std::vector<CachedScanError> loadErrors(const std::filesystem::path& rootPath) const;
  void clearErrors(const std::filesystem::path& rootPath);
  [[nodiscard]] WriterTransaction beginWriter();

private:
  void open();
  void initializeSchema();

  [[nodiscard]] int readUserVersion() const;
  [[nodiscard]] std::string readJournalMode() const;
  [[nodiscard]] std::filesystem::path backupPath() const;

  static void configureConnection(sqlite3* db, std::chrono::milliseconds busyTimeout);
  static void exec(sqlite3* db, const char* sql);
  static std::string schemaSql();
  
  void prepareStatements();
  void finalizeStatements();

  void upsertContentNoTransaction(const std::string& contentId, const SongMetadata& metadata);
  void upsertLocationNoTransaction(const CachedLocation& location);
  void pruneDeletedLocationsNoTransaction(const std::filesystem::path& rootPath,
                                          const std::vector<std::string>& retainedLocationIds);
  void replaceLyricsNoTransaction(const std::string& locationId, const std::string& kind,
                                  const std::vector<LyricLine>& lyrics);
  void updateScanRootNoTransaction(const CachedScanRoot& root);
  void saveErrorsNoTransaction(const std::filesystem::path& rootPath, const std::vector<CachedScanError>& errors);
  void clearErrorsNoTransaction(const std::filesystem::path& rootPath);
  void applyLyricsCacheUpdateNoTransaction(const LyricsCacheUpdate& update);

  std::filesystem::path databasePath_;
  std::chrono::milliseconds busyTimeout_{500};
  void* db_{nullptr};
  mutable std::mutex writerMutex_;
  mutable std::mutex readerMutex_;
  
  void* locationStmt_{nullptr};  // sqlite3_stmt*
  void* contentStmt_{nullptr};   // sqlite3_stmt*
};

}
