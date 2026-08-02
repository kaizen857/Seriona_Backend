#include "seriona/scanner/directory_tree_hash.h"

#include <xxhash.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seriona::scanner {
namespace {

struct Xxh3StateDeleter {
  void operator()(XXH3_state_t* state) const noexcept { XXH3_freeState(state); }
};

using Xxh3State = std::unique_ptr<XXH3_state_t, Xxh3StateDeleter>;

constexpr char kHashSeparator = '\0';

[[nodiscard]] bool isCancelled(const HashOptions& options) noexcept {
  return options.cancellationRequested != nullptr && options.cancellationRequested->load();
}

// Local copy: some test binaries compile this file without path_utils.cpp.
[[nodiscard]] bool isLyricsSidecarPath(const std::filesystem::path& path) {
  auto extension = path.extension().generic_string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extension == ".lrc";
}

[[nodiscard]] HashError makeTreeHashError(HashErrorCode code, ScannerErrorCode scannerCode,
                                          const std::filesystem::path& path, std::string message,
                                          std::string detail = {}) {
  return {.code = code,
          .scannerError = {.code = scannerCode,
                           .message = std::move(message),
                           .detail = std::move(detail),
                           .path = path}};
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

[[nodiscard]] bool updateHashSeparator(XXH3_state_t& state) {
  return XXH3_128bits_update(&state, &kHashSeparator, sizeof(kHashSeparator)) != XXH_ERROR;
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

[[nodiscard]] std::string relativeUtf8(const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code error;
  auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    relative = path.filename();
  }
  const auto utf8 = relative.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::optional<std::string> treeEntryKind(const std::filesystem::directory_entry& entry,
                                                       std::error_code& error) {
  error.clear();
  if (entry.is_symlink(error)) {
    return "symlink";
  }
  if (error) {
    return std::nullopt;
  }
  if (entry.is_directory(error)) {
    return "dir";
  }
  if (error) {
    return std::nullopt;
  }
  if (entry.is_regular_file(error)) {
    return "file";
  }
  if (error) {
    return std::nullopt;
  }
  return "other";
}

[[nodiscard]] std::vector<std::filesystem::directory_entry> sortedChildren(const std::filesystem::path& root,
                                                                           std::vector<HashError>& errors) {
  std::vector<std::filesystem::directory_entry> children;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       iterator != end; iterator.increment(error)) {
    if (error) {
      errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, root,
                                         "failed to continue directory tree hash traversal", error.message()));
      error.clear();
      continue;
    }
    children.push_back(*iterator);
  }
  if (error) {
    errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, root,
                                       "failed to start directory tree hash traversal", error.message()));
  }
  std::ranges::sort(children, {}, [](const std::filesystem::directory_entry& entry) {
    return entry.path().filename().generic_u8string();
  });
  return children;
}

[[nodiscard]] DirectoryHashResult hashTreeRecursive(const std::filesystem::path& root, const std::filesystem::path& path,
                                                    const HashOptions& options) {
  DirectoryHashResult result;
  if (isCancelled(options)) {
    result.errors.push_back(makeTreeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, path,
                                             "directory tree hash cancelled"));
    return result;
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::exists(status)) {
    result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                             "directory tree hash path disappeared", error.message()));
    return result;
  }
  if (!std::filesystem::is_directory(status)) {
    result.errors.push_back(makeTreeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                             "directory tree hash requires a directory"));
    return result;
  }

  auto state = createHashState();
  if (state == nullptr) {
    result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::CacheUnavailable, path,
                                             "failed to initialize directory tree hash state"));
    return result;
  }

  static_cast<void>(updateHash(*state, "dir"));
  static_cast<void>(updateHashSeparator(*state));
  static_cast<void>(updateHash(*state, relativeUtf8(root, path)));
  for (const auto& child : sortedChildren(path, result.errors)) {
    if (isCancelled(options)) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::Cancelled, ScannerErrorCode::Cancelled, child.path(),
                                               "directory tree hash cancelled"));
      return result;
    }

    // Lyrics sidecar (.lrc) files are re-read from disk on every scan by the
    // lyrics reconciliation path, so their presence must not invalidate the
    // scan-mode tree hash: an lrc-only change should stay on the incremental
    // path instead of forcing a full TagReader rescan of the paired audio.
    if (isLyricsSidecarPath(child.path())) {
      continue;
    }

    auto kind = treeEntryKind(child, error);
    if (!kind.has_value()) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, child.path(),
                                               "failed to inspect directory tree hash child", error.message()));
      continue;
    }

    auto childHash = std::string{};
    if (*kind == "dir") {
      auto childResult = hashTreeRecursive(root, child.path(), options);
      result.errors.insert(result.errors.end(), childResult.errors.begin(), childResult.errors.end());
      if (!childResult.hash.has_value()) {
        continue;
      }
      childHash = *childResult.hash;
    }

    static_cast<void>(updateHash(*state, relativeUtf8(root, child.path())));
    static_cast<void>(updateHashSeparator(*state));
    static_cast<void>(updateHash(*state, *kind));
    static_cast<void>(updateHashSeparator(*state));
    static_cast<void>(updateHash(*state, childHash));
    static_cast<void>(updateHashSeparator(*state));
  }

  result.hash = canonicalHex(XXH3_128bits_digest(state.get()));
  return result;
}

}

DirectoryHashResult computeDirectoryTreeHash(const std::filesystem::path& rootPath, const HashOptions& options) {
  return hashTreeRecursive(rootPath, rootPath, options);
}

}
