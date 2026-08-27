#include "seriona/scanner/hash_utils.h"

#include "seriona/scanner/directory_tree_hash.h"

#include "path_utf8.h"

#include "spdlog/spdlog.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include <xxhash.h>

#include "song_identity.cpp"

namespace seriona::scanner {
namespace {

constexpr std::uint64_t kHashSeed = 0;

struct Xxh3StateDeleter {
  void operator()(XXH3_state_t* state) const noexcept { static_cast<void>(XXH3_freeState(state)); }
};

using Xxh3State = std::unique_ptr<XXH3_state_t, Xxh3StateDeleter>;

[[nodiscard]] bool isCancelled(const HashOptions& options) noexcept {
  return options.cancellationRequested != nullptr && options.cancellationRequested->load();
}

[[nodiscard]] HashError makeHashError(HashErrorCode code, ScannerErrorCode scannerCode, const std::filesystem::path& path,
                                      std::string message, std::string detail = {}) {
  return {.code = code,
          .scannerError = {.code = scannerCode,
                           .message = std::move(message),
                           .detail = std::move(detail),
                           .path = path}};
}

[[nodiscard]] std::string canonicalHex(XXH128_hash_t hash) {
  XXH128_canonical_t canonical{};
  XXH128_canonicalFromHash(&canonical, hash);

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : canonical.digest) {
    output << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return output.str();
}

[[nodiscard]] Xxh3State createHashState() {
  Xxh3State state{XXH3_createState()};
  if (state == nullptr || XXH3_128bits_reset(state.get()) == XXH_ERROR) {
    return nullptr;
  }
  return state;
}

[[nodiscard]] bool updateHash(XXH3_state_t& state, const void* data, std::size_t size) {
  return XXH3_128bits_update(&state, data, size) != XXH_ERROR;
}

}

FileHashResult hashFileContent(const std::filesystem::path& path, const HashOptions& options) {
  FileHashResult result;
  if (isCancelled(options)) {
    result.errors.push_back(makeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, path, "file hash cancelled"));
    return result;
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::exists(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                          "file hash path disappeared", error.message()));
    spdlog::warn("file hash: path disappeared: {} ({})", pathToUtf8(path), error.message());
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                          "file hash requires a regular file"));
    spdlog::debug("file hash: not a regular file: {}", pathToUtf8(path));
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                          "failed to open file for hashing"));
    spdlog::error("file hash: failed to open {} for hashing", pathToUtf8(path));
    return result;
  }

  auto state = createHashState();
  if (state == nullptr) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::CacheUnavailable, path,
                                          "failed to initialize file hash state"));
    return result;
  }

  const auto chunkBytes = std::max<std::size_t>(1U, options.chunkBytes);
  std::vector<char> buffer(chunkBytes);
  while (input) {
    if (isCancelled(options)) {
      result.errors.push_back(makeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, path, "file hash cancelled"));
      return result;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytesRead = input.gcount();
    if (bytesRead > 0 && !updateHash(*state, buffer.data(), static_cast<std::size_t>(bytesRead))) {
      result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::CacheUnavailable, path,
                                            "failed to update file hash state"));
      return result;
    }
  }

  if (!input.eof()) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                          "failed while reading file for hashing"));
    spdlog::error("file hash: read error for {}", pathToUtf8(path));
    return result;
  }

  result.hash = canonicalHex(XXH3_128bits_digest(state.get()));
  spdlog::debug("file hash complete: {} -> {}", pathToUtf8(path), *result.hash);
  return result;
}

FileHashResult hashLyricsSidecar(const std::filesystem::path& path, const HashOptions& options) {
  auto result = hashFileContent(path, options);
  spdlog::debug("lrc hash complete: {} -> {}", pathToUtf8(path), result.hash.value_or("none"));
  return result;
}

DirectoryHashResult hashDirectoryMerkle(const std::filesystem::path& root, const HashOptions& options) {
  auto result = computeDirectoryTreeHash(root, options);
  spdlog::debug("dir hash complete: {} errors={} hash={}", pathToUtf8(root), result.errors.size(),
                result.hash.value_or("none"));
  return result;
}

}
