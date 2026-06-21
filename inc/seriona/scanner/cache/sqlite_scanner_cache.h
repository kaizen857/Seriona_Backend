#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner::cache {

struct ScannerCacheConfig {
  std::filesystem::path databasePath;
  std::chrono::milliseconds busyTimeout{500};
};

struct CachedDirectory {
  std::filesystem::path relativePath;
  std::string hash;
  std::optional<std::filesystem::file_time_type> mtime;
};

struct CachedUserStats {
  std::uint64_t playCount{0};
  std::optional<std::uint32_t> rating;
  std::optional<std::chrono::system_clock::time_point> lastPlayed;
};

struct CachedSong {
  SongMetadata metadata;
  std::vector<LyricLine> embeddedLyrics;
  std::vector<LyricLine> externalLyrics;
  CachedUserStats userStats;
};

struct CachedRoot {
  std::filesystem::path rootPath;
  std::string directoryHash;
  std::vector<CachedDirectory> directories;
  std::vector<CachedSong> songs;
  std::vector<ScannerError> errors;
};

struct CacheCheckpointResult {
  int resultCode{0};
  int logFrames{0};
  int checkpointedFrames{0};
};

class SQLiteScannerCache {
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
    friend class SQLiteScannerCache;
    explicit WriterTransaction(SQLiteScannerCache& cache);
    SQLiteScannerCache* cache_{nullptr};
    std::unique_lock<std::mutex> lock_;
    bool active_{false};
  };

  explicit SQLiteScannerCache(ScannerCacheConfig config);
  ~SQLiteScannerCache();

  SQLiteScannerCache(const SQLiteScannerCache&) = delete;
  SQLiteScannerCache& operator=(const SQLiteScannerCache&) = delete;

  [[nodiscard]] int schemaVersion() const;
  [[nodiscard]] std::string journalMode() const;
  [[nodiscard]] std::optional<CachedRoot> loadRoot(const std::filesystem::path& rootPath) const;
  void saveRoot(const CachedRoot& root);
  void updateUserStats(const std::filesystem::path& rootPath, const std::string& trackId, CachedUserStats stats);
  void pruneMissingSongs(const std::filesystem::path& rootPath, const std::vector<std::string>& retainedTrackIds);
  [[nodiscard]] CacheCheckpointResult checkpointPassive();
  [[nodiscard]] WriterTransaction beginWriter();

private:
  void* db_{nullptr};
  mutable std::mutex writerMutex_;
};

}
