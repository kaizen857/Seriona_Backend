#include "seriona/thumbnail/thumbnail_service.h"

#include <QImage>
#include <QImageReader>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace seriona::thumbnail {

namespace {

std::string computeHash(const std::string& input) {
  // Simple hash for cache key generation
  std::hash<std::string> hasher;
  std::size_t hash = hasher(input);
  std::ostringstream oss;
  oss << std::hex << hash;
  return oss.str();
}

}  // namespace

ThumbnailService::ThumbnailService(std::filesystem::path cacheRoot)
    : m_cacheRoot(std::move(cacheRoot)) {
  std::error_code ec;
  std::filesystem::create_directories(m_cacheRoot, ec);
}

ThumbnailService::~ThumbnailService() = default;

ThumbnailResponse ThumbnailService::generate(const ThumbnailRequest& request) {
  if (!std::filesystem::exists(request.sourcePath)) {
    return ThumbnailError{
        ThumbnailErrorCode::SourceNotFound,
        "Source file not found",
        request.sourcePath};
  }

  // Check cache first
  auto cached = checkCache(request.sourcePath, request.size, request.format);
  if (cached.has_value()) {
    std::lock_guard lock(m_cacheMutex);
    ++m_cacheHits;
    
    return ThumbnailResult{
        cached->thumbnailPath,
        cached->fileSizeBytes,
        sizeToPixels(request.size),
        sizeToPixels(request.size),
        true,
        std::chrono::steady_clock::now()};
  }

  std::lock_guard lock(m_cacheMutex);
  ++m_cacheMisses;

  return generateThumbnail(request);
}

std::optional<ThumbnailCacheEntry> ThumbnailService::checkCache(
    const std::filesystem::path& sourcePath,
    ThumbnailSize size,
    ThumbnailFormat format) const {
  
  auto cachePath = getCachePath(sourcePath, size, format);
  
  std::error_code ec;
  if (!std::filesystem::exists(cachePath, ec)) {
    return std::nullopt;
  }

  auto sourceModified = std::filesystem::last_write_time(sourcePath, ec);
  if (ec) {
    return std::nullopt;
  }

  auto thumbnailModified = std::filesystem::last_write_time(cachePath, ec);
  if (ec) {
    return std::nullopt;
  }

  // Cache invalid if source is newer than thumbnail
  if (sourceModified > thumbnailModified) {
    return std::nullopt;
  }

  auto fileSize = std::filesystem::file_size(cachePath, ec);
  if (ec) {
    return std::nullopt;
  }

  ThumbnailCacheEntry entry;
  entry.thumbnailPath = cachePath;
  entry.sourceModified = sourceModified;
  entry.thumbnailCreated = thumbnailModified;
  entry.fileSizeBytes = fileSize;

  return entry;
}

ThumbnailResponse ThumbnailService::generateThumbnail(const ThumbnailRequest& request) {
  QImageReader reader(QString::fromStdString(request.sourcePath.string()));
  
  if (!reader.canRead()) {
    return ThumbnailError{
        ThumbnailErrorCode::InvalidImage,
        "Cannot read source image",
        request.sourcePath};
  }

  const std::uint32_t targetSize = sizeToPixels(request.size);
  QSize sourceSize = reader.size();
  
  // Calculate scaled size maintaining aspect ratio
  QSize scaledSize = sourceSize.scaled(
      static_cast<int>(targetSize),
      static_cast<int>(targetSize),
      Qt::KeepAspectRatio);
  
  reader.setScaledSize(scaledSize);
  
  QImage image = reader.read();
  if (image.isNull()) {
    return ThumbnailError{
        ThumbnailErrorCode::InvalidImage,
        "Failed to decode source image",
        request.sourcePath};
  }

  // Don't upscale unless explicitly allowed
  if (!request.allowUpscaling && 
      (image.width() < static_cast<int>(targetSize) || 
       image.height() < static_cast<int>(targetSize))) {
    // Use original size if smaller than target
  } else {
    // Scale down if larger
    if (image.width() > static_cast<int>(targetSize) || 
        image.height() > static_cast<int>(targetSize)) {
      image = image.scaled(
          static_cast<int>(targetSize),
          static_cast<int>(targetSize),
          Qt::KeepAspectRatio,
          Qt::SmoothTransformation);
    }
  }

  auto cachePath = getCachePath(request.sourcePath, request.size, request.format);
  
  std::error_code ec;
  std::filesystem::create_directories(cachePath.parent_path(), ec);
  if (ec) {
    return ThumbnailError{
        ThumbnailErrorCode::CacheUnavailable,
        "Cannot create cache directory",
        request.sourcePath};
  }

  const char* formatStr = nullptr;
  switch (request.format) {
    case ThumbnailFormat::JPEG:
      formatStr = "JPEG";
      break;
    case ThumbnailFormat::PNG:
      formatStr = "PNG";
      break;
    case ThumbnailFormat::WebP:
      formatStr = "WEBP";
      break;
  }

  if (!image.save(QString::fromStdString(cachePath.string()), formatStr, request.quality)) {
    return ThumbnailError{
        ThumbnailErrorCode::EncodingFailed,
        "Failed to encode thumbnail",
        request.sourcePath};
  }

  auto fileSize = std::filesystem::file_size(cachePath, ec);
  if (ec) {
    fileSize = 0;
  }

  return ThumbnailResult{
      cachePath,
      fileSize,
      static_cast<std::uint32_t>(image.width()),
      static_cast<std::uint32_t>(image.height()),
      false,
      std::chrono::steady_clock::now()};
}

std::filesystem::path ThumbnailService::getCachePath(
    const std::filesystem::path& sourcePath,
    ThumbnailSize size,
    ThumbnailFormat format) const {
  
  std::string hash = computeSourceHash(sourcePath);
  
  std::string sizeStr;
  switch (size) {
    case ThumbnailSize::Small:
      sizeStr = "small";
      break;
    case ThumbnailSize::Medium:
      sizeStr = "medium";
      break;
    case ThumbnailSize::Large:
      sizeStr = "large";
      break;
  }

  std::string ext = formatToExtension(format);
  std::string filename = hash + "_" + sizeStr + ext;
  
  return m_cacheRoot / filename;
}

std::string ThumbnailService::computeSourceHash(
    const std::filesystem::path& sourcePath) const {
  return computeHash(sourcePath.string());
}

std::uint32_t ThumbnailService::sizeToPixels(ThumbnailSize size) const {
  switch (size) {
    case ThumbnailSize::Small:
      return 64;
    case ThumbnailSize::Medium:
      return 240;
    case ThumbnailSize::Large:
      return 512;
  }
  return 240;
}

std::string ThumbnailService::formatToExtension(ThumbnailFormat format) const {
  switch (format) {
    case ThumbnailFormat::JPEG:
      return ".jpg";
    case ThumbnailFormat::PNG:
      return ".png";
    case ThumbnailFormat::WebP:
      return ".webp";
  }
  return ".jpg";
}

bool ThumbnailService::clearCache() {
  std::lock_guard lock(m_cacheMutex);
  
  std::error_code ec;
  std::filesystem::remove_all(m_cacheRoot, ec);
  
  if (ec) {
    return false;
  }
  
  std::filesystem::create_directories(m_cacheRoot, ec);
  m_cacheIndex.clear();
  m_cacheHits = 0;
  m_cacheMisses = 0;
  
  return !ec;
}

ThumbnailCacheStats ThumbnailService::getCacheStats() const {
  std::lock_guard lock(m_cacheMutex);
  
  ThumbnailCacheStats stats;
  stats.hitCount = m_cacheHits;
  stats.missCount = m_cacheMisses;
  
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(m_cacheRoot, ec)) {
    if (entry.is_regular_file(ec)) {
      ++stats.totalEntries;
      stats.totalSizeBytes += entry.file_size(ec);
    }
  }
  
  return stats;
}

void ThumbnailService::setMaxCacheSizeBytes(std::uint64_t maxBytes) {
  std::lock_guard lock(m_cacheMutex);
  m_maxCacheSizeBytes = maxBytes;
}

void ThumbnailService::setMaxCacheAgeSeconds(std::uint64_t maxSeconds) {
  std::lock_guard lock(m_cacheMutex);
  m_maxCacheAgeSeconds = maxSeconds;
}

void ThumbnailService::pruneCache() {
  // TODO: Implement LRU cache pruning based on size and age limits
}

}  // namespace seriona::thumbnail
