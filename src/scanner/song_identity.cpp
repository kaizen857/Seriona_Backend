#include "seriona/scanner/song_identity.h"

#include "path_utf8.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

#include <xxhash.h>

namespace seriona::scanner {
namespace {

constexpr std::uint64_t kSongIdentitySeed = 0U;

[[nodiscard]] std::string hexId(std::uint64_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string output(16U, '0');
  for (std::size_t index = 0; index < 16U; ++index) {
    const auto shift = static_cast<unsigned int>((15U - index) * 4U);
    output[index] = kDigits[(value >> shift) & 0x0FU];
  }
  return output;
}

[[nodiscard]] std::string canonicalPathText(const std::filesystem::path& path) {
  return pathToUtf8(path.lexically_normal());
}

[[nodiscard]] std::string normalizedWhitespace(std::string_view text) {
  std::string output;
  output.reserve(text.size());

  bool pendingSpace = false;
  for (const unsigned char raw : text) {
    if (std::isspace(raw) != 0) {
      pendingSpace = !output.empty();
      continue;
    }
    if (pendingSpace) {
      output.push_back(' ');
      pendingSpace = false;
    }
    output.push_back(static_cast<char>(std::tolower(raw)));
  }

  return output;
}

[[nodiscard]] std::string toStableText(std::uint64_t value) {
  return std::to_string(value);
}

[[nodiscard]] std::string toStableText(std::filesystem::file_time_type::rep value) {
  return std::to_string(value);
}

[[nodiscard]] std::string hashTextParts(std::initializer_list<std::string_view> parts) {
  XXH64_hash_t hash = kSongIdentitySeed;
  for (const auto part : parts) {
    hash = XXH64(part.data(), part.size(), hash);
    hash = XXH64("\0", 1U, hash);
  }
  return hexId(hash);
}

}

std::string normalizeForId(std::string_view text) {
  return normalizedWhitespace(text);
}

std::string computeContentId(std::chrono::milliseconds durationMs, std::string_view title, std::string_view artist) {
  const auto normalizedTitle = normalizeForId(title);
  const auto normalizedArtist = normalizeForId(artist);
  const auto durationCount = durationMs.count();

  return hashTextParts({toStableText(durationCount), normalizedTitle, normalizedArtist});
}

std::string computeLocationId(const std::filesystem::path& path,
                              std::uint64_t fileSize,
                              std::optional<std::filesystem::file_time_type> mtime,
                              std::optional<std::chrono::milliseconds> cueTrackOffset,
                              std::optional<std::uint32_t> cueTrackIndex) {
  const auto normalizedPath = canonicalPathText(path);
  const auto mtimeCount = mtime.has_value() ? toStableText(mtime->time_since_epoch().count()) : std::string{"missing"};
  
  if (cueTrackOffset.has_value()) {
    const auto offsetCount = toStableText(cueTrackOffset->count());
    if (cueTrackIndex.has_value()) {
      const auto indexCount = toStableText(static_cast<std::uint64_t>(*cueTrackIndex));
      return hashTextParts({normalizedPath, toStableText(fileSize), mtimeCount, "offset", offsetCount, "index", indexCount});
    }
    return hashTextParts({normalizedPath, toStableText(fileSize), mtimeCount, "offset", offsetCount});
  }
  
  return hashTextParts({normalizedPath, toStableText(fileSize), mtimeCount});
}

}
