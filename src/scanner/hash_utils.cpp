#include "seriona/scanner/hash_utils.h"

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

[[nodiscard]] bool updateHash(XXH3_state_t& state, std::string_view value) {
  return XXH3_128bits_update(&state, value.data(), value.size()) != XXH_ERROR;
}

[[nodiscard]] bool updateHash(XXH3_state_t& state, const void* data, std::size_t size) {
  return XXH3_128bits_update(&state, data, size) != XXH_ERROR;
}

[[nodiscard]] std::string fileTypeName(const std::filesystem::directory_entry& entry) {
  std::error_code error;
  if (entry.is_directory(error)) {
    return "dir";
  }
  if (!error && entry.is_regular_file(error)) {
    return "file";
  }
  if (!error && entry.is_symlink(error)) {
    return "symlink";
  }
  return "other";
}

[[nodiscard]] std::string normalizedFileTime(const std::filesystem::directory_entry& entry) {
  std::error_code error;
  const auto time = entry.last_write_time(error);
  if (error) {
    return "mtime:error";
  }
  return std::to_string(time.time_since_epoch().count());
}

[[nodiscard]] std::string fileSizeString(const std::filesystem::directory_entry& entry) {
  std::error_code error;
  if (!entry.is_regular_file(error) || error) {
    return "0";
  }
  const auto size = entry.file_size(error);
  return error ? "0" : std::to_string(size);
}

[[nodiscard]] std::string relativeUtf8(const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code error;
  auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    relative = path.filename();
  }
  const auto utf8 = relative.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::vector<std::filesystem::directory_entry> sortedChildren(const std::filesystem::path& root,
                                                                           std::vector<HashError>& errors) {
  std::vector<std::filesystem::directory_entry> children;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, root,
                                     "failed to continue directory hash traversal", error.message()));
      spdlog::debug("hash traversal: skipped entry in {} ({})", root.generic_string(), error.message());
      error.clear();
      continue;
    }
    children.push_back(*iterator);
  }
  if (error) {
    errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, root,
                                   "failed to start directory hash traversal", error.message()));
    spdlog::warn("hash traversal: failed to start in {} ({})", root.generic_string(), error.message());
  }
  std::ranges::sort(children, {}, [](const std::filesystem::directory_entry& entry) {
    return entry.path().filename().generic_u8string();
  });
  return children;
}

[[nodiscard]] DirectoryHashResult hashDirectoryRecursive(const std::filesystem::path& root,
                                                         const std::filesystem::path& path,
                                                         const HashOptions& options) {
  DirectoryHashResult result;
  if (isCancelled(options)) {
    result.errors.push_back(makeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, path,
                                          "directory hash cancelled"));
    return result;
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::exists(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                          "directory hash path disappeared", error.message()));
    spdlog::warn("dir hash: path disappeared: {} ({})", path.generic_string(), error.message());
    return result;
  }
  if (std::filesystem::is_symlink(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                          "directory hash skipped symlink"));
    spdlog::debug("dir hash: skipped symlink: {}", path.generic_string());
    return result;
  }
  if (!std::filesystem::is_directory(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                          "directory hash requires a directory"));
    spdlog::error("dir hash: not a directory: {}", path.generic_string());
    return result;
  }

  auto state = createHashState();
  if (state == nullptr) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::CacheUnavailable, path,
                                          "failed to initialize directory hash state"));
    return result;
  }

  static_cast<void>(updateHash(*state, "dir\0"));
  static_cast<void>(updateHash(*state, relativeUtf8(root, path)));
  for (const auto& child : sortedChildren(path, result.errors)) {
    if (isCancelled(options)) {
      result.errors.push_back(makeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, child.path(),
                                            "directory hash cancelled"));
      return result;
    }

    const auto childPath = child.path();
    auto childHash = std::string{};
    if (child.is_directory(error) && !child.is_symlink(error)) {
      auto childResult = hashDirectoryRecursive(root, childPath, options);
      result.errors.insert(result.errors.end(), childResult.errors.begin(), childResult.errors.end());
      if (!childResult.hash.has_value()) {
        continue;
      }
      childHash = *childResult.hash;
    } else if (child.is_regular_file(error)) {
      auto childResult = hashFileContent(childPath, options);
      result.errors.insert(result.errors.end(), childResult.errors.begin(), childResult.errors.end());
      if (!childResult.hash.has_value()) {
        continue;
      }
      childHash = *childResult.hash;
    } else {
      result.errors.push_back(makeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, childPath,
                                            "directory hash skipped unsupported child"));
      spdlog::debug("dir hash: skipped unsupported child: {}", childPath.generic_string());
      continue;
    }

    const auto childRelative = relativeUtf8(root, childPath);
    static_cast<void>(updateHash(*state, childRelative));
    static_cast<void>(updateHash(*state, "\0"));
    static_cast<void>(updateHash(*state, fileTypeName(child)));
    static_cast<void>(updateHash(*state, "\0"));
    static_cast<void>(updateHash(*state, fileSizeString(child)));
    static_cast<void>(updateHash(*state, "\0"));
    static_cast<void>(updateHash(*state, normalizedFileTime(child)));
    static_cast<void>(updateHash(*state, "\0"));
    static_cast<void>(updateHash(*state, childHash));
    static_cast<void>(updateHash(*state, "\0"));
  }

  result.hash = canonicalHex(XXH3_128bits_digest(state.get()));
  return result;
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
    spdlog::warn("file hash: path disappeared: {} ({})", path.generic_string(), error.message());
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.errors.push_back(makeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                          "file hash requires a regular file"));
    spdlog::debug("file hash: not a regular file: {}", path.generic_string());
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.errors.push_back(makeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                          "failed to open file for hashing"));
    spdlog::error("file hash: failed to open {} for hashing", path.generic_string());
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
    spdlog::error("file hash: read error for {}", path.generic_string());
    return result;
  }

  result.hash = canonicalHex(XXH3_128bits_digest(state.get()));
  spdlog::debug("file hash complete: {} -> {}", path.generic_string(), *result.hash);
  return result;
}

FileHashResult hashLyricsSidecar(const std::filesystem::path& path, const HashOptions& options) {
  auto result = hashFileContent(path, options);
  spdlog::debug("lrc hash complete: {} -> {}", path.generic_string(), result.hash.value_or("none"));
  return result;
}

DirectoryHashResult hashDirectoryMerkle(const std::filesystem::path& root, const HashOptions& options) {
  auto result = hashDirectoryRecursive(root, root, options);
  spdlog::debug("dir hash complete: {} errors={} hash={}", root.generic_string(), result.errors.size(),
                result.hash.value_or("none"));
  return result;
}

}
