#pragma once

#include "thumbnail_contracts.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace seriona::thumbnail {

struct ThumbnailCacheEntry {
  std::filesystem::path thumbnailPath;
  std::filesystem::file_time_type sourceModified{};
  std::filesystem::file_time_type thumbnailCreated{};
  std::uint64_t fileSizeBytes{0};
};

class ThumbnailService {
 public:
  explicit ThumbnailService(std::filesystem::path cacheRoot);
  ~ThumbnailService();

  ThumbnailService(const ThumbnailService&) = delete;
  ThumbnailService& operator=(const ThumbnailService&) = delete;
  ThumbnailService(ThumbnailService&&) = delete;
  ThumbnailService& operator=(ThumbnailService&&) = delete;

  ThumbnailResponse generate(const ThumbnailRequest& request);
  
  bool clearCache();
  ThumbnailCacheStats getCacheStats() const;
  
  void setMaxCacheSizeBytes(std::uint64_t maxBytes);
  void setMaxCacheAgeSeconds(std::uint64_t maxSeconds);

 private:
  std::filesystem::path getCachePath(
      const std::filesystem::path& sourcePath,
      ThumbnailSize size,
      ThumbnailFormat format) const;
  
  std::string computeSourceHash(const std::filesystem::path& sourcePath) const;
  
  std::optional<ThumbnailCacheEntry> checkCache(
      const std::filesystem::path& sourcePath,
      ThumbnailSize size,
      ThumbnailFormat format) const;
  
  ThumbnailResponse generateThumbnail(const ThumbnailRequest& request);
  
  std::uint32_t sizeToPixels(ThumbnailSize size) const;
  std::string formatToExtension(ThumbnailFormat format) const;
  
  void pruneCache();

  std::filesystem::path m_cacheRoot;
  std::uint64_t m_maxCacheSizeBytes{500 * 1024 * 1024};  // 500 MB default
  std::uint64_t m_maxCacheAgeSeconds{30 * 24 * 3600};    // 30 days default
  mutable std::mutex m_cacheMutex;
  mutable std::unordered_map<std::string, ThumbnailCacheEntry> m_cacheIndex;
  mutable std::uint64_t m_cacheHits{0};
  mutable std::uint64_t m_cacheMisses{0};
};

}  // namespace seriona::thumbnail
