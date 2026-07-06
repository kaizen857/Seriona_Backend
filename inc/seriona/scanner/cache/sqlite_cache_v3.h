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
  std::optional<std::filesystem::path> artworkPath;
  std::optional<std::filesystem::path> thumbnailPath;
  LyricsSource lyricsSource{LyricsSource::None};
  std::optional<std::filesystem::path> externalLrcPath;
  std::optional<std::int64_t> externalLrcMtimeNs;
  std::chrono::system_clock::time_point discoveredAt{};
  std::chrono::system_clock::time_point scannedAt{};
};

struct CachedScanRootV3 {
  std::filesystem::path rootPath;
  std::string directoryTreeHash;
  std::uint64_t totalFiles{0};
  ScanMode lastScanMode{ScanMode::Incremental};
  std::chrono::milliseconds lastScanDuration{0};
  std::chrono::system_clock::time_point lastScanAt{};
};

struct CachedScanErrorV3 {
  std::filesystem::path rootPath;
  std::optional<std::filesystem::path> filePath;
  ScannerErrorCode errorCode{ScannerErrorCode::MetadataReadFailed};
  std::string errorMessage;
  std::chrono::system_clock::time_point occurredAt{};
};

class SQLiteCacheV3 {
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
    friend class SQLiteCacheV3;
    explicit WriterTransaction(SQLiteCacheV3& cache);
    SQLiteCacheV3* cache_{nullptr};
    std::unique_lock<std::mutex> lock_;
    bool active_{false};
  };

  explicit SQLiteCacheV3(ScannerCacheConfig config);
  ~SQLiteCacheV3();

  SQLiteCacheV3(const SQLiteCacheV3&) = delete;
  SQLiteCacheV3& operator=(const SQLiteCacheV3&) = delete;

  [[nodiscard]] int schemaVersion() const;
  [[nodiscard]] std::string journalMode() const;
  void upsertContent(const std::string& contentId, const SongMetadata& metadata);
  [[nodiscard]] std::optional<CachedSong> loadContent(const std::string& contentId) const;
  void updateUserStats(const std::string& contentId, const CachedUserStats& userStats);
  void upsertLocation(const CachedLocation& location);
  [[nodiscard]] std::optional<CachedLocation> loadLocation(const std::string& locationId) const;
  [[nodiscard]] std::vector<CachedLocation> loadLocationsByRoot(const std::filesystem::path& rootPath) const;
  void pruneDeletedLocations(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedLocationIds);
  void replaceLyrics(const std::string& locationId, const std::string& kind, const std::vector<LyricLine>& lyrics);
  [[nodiscard]] std::vector<LyricLine> loadLyrics(const std::string& locationId, const std::string& kind) const;
  void updateScanRoot(const CachedScanRootV3& root);
  [[nodiscard]] std::optional<CachedScanRootV3> loadScanRoot(const std::filesystem::path& rootPath) const;
  void saveErrors(const std::filesystem::path& rootPath, const std::vector<CachedScanErrorV3>& errors);
  [[nodiscard]] std::vector<CachedScanErrorV3> loadErrors(const std::filesystem::path& rootPath) const;
  void clearErrors(const std::filesystem::path& rootPath);
  [[nodiscard]] WriterTransaction beginWriter();

private:
  void open();
  void initializeSchemaV3();

  [[nodiscard]] int readUserVersion() const;
  [[nodiscard]] std::string readJournalMode() const;
  [[nodiscard]] std::filesystem::path backupPath() const;

  static void configureConnection(sqlite3* db, std::chrono::milliseconds busyTimeout);
  static void exec(sqlite3* db, const char* sql);
  static std::string schemaV3Sql();
  
  void prepareStatements();
  void finalizeStatements();

  std::filesystem::path databasePath_;
  std::chrono::milliseconds busyTimeout_{500};
  void* db_{nullptr};
  mutable std::mutex writerMutex_;
  mutable std::mutex readerMutex_;
  
  void* locationStmt_{nullptr};  // sqlite3_stmt*
  void* contentStmt_{nullptr};   // sqlite3_stmt*
};

}
