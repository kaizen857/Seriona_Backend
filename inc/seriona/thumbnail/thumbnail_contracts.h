#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace seriona::thumbnail {

enum class ThumbnailSize {
  Small,   // 64x64
  Medium,  // 240x240
  Large,   // 512x512
};

enum class ThumbnailFormat {
  JPEG,
  PNG,
  WebP,
};

enum class ThumbnailErrorCode {
  SourceNotFound,
  SourceUnreadable,
  InvalidImage,
  EncodingFailed,
  CacheUnavailable,
  SizeLimitExceeded,
};

struct ThumbnailRequest {
  std::filesystem::path sourcePath;
  ThumbnailSize size{ThumbnailSize::Medium};
  ThumbnailFormat format{ThumbnailFormat::JPEG};
  std::uint8_t quality{85};
  bool allowUpscaling{false};
};

struct ThumbnailResult {
  std::filesystem::path thumbnailPath;
  std::uint64_t fileSizeBytes{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  bool fromCache{false};
  std::chrono::steady_clock::time_point generatedAt{};
};

struct ThumbnailError {
  ThumbnailErrorCode code{ThumbnailErrorCode::InvalidImage};
  std::string message;
  std::optional<std::filesystem::path> sourcePath;
};

struct ThumbnailCacheStats {
  std::uint64_t totalEntries{0};
  std::uint64_t totalSizeBytes{0};
  std::uint64_t hitCount{0};
  std::uint64_t missCount{0};
};

using ThumbnailResponse = std::variant<ThumbnailResult, ThumbnailError>;

}  // namespace seriona::thumbnail
