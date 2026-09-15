#include "seriona/scanner/directory_tree_hash.h"

#include "seriona/scanner/path_utils.h"

#include "path_utf8.h"

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

// 目录树哈希只关心「库相关性」条目：受支持音频、.cue 工程、封面侧车；`.lrc` 无条件排除——
// 歌词侧车每次扫描都会重读，其变化必须留在增量路径，不能把整根升级为 Full。谓词统一取自
// path_utils，避免同一过滤规则分处两地漂移后静默漏掉真实变化（Bazel PR #22615）。
[[nodiscard]] bool isHashRelevantEntry(const std::filesystem::path& path, const HashOptions& options) {
  if (isLyricsSidecarPath(path)) {
    return false;
  }
  if (!options.libraryRelevanceFilter) {
    return true;
  }
  return isLibraryRelevantPath(path, options.allowedExtensions);
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

struct SubtreeHash {
  std::optional<std::string> hash;
  std::vector<HashError> errors;
  bool hasRelevantEntries{false};
};

[[nodiscard]] SubtreeHash hashTreeRecursive(const std::filesystem::path& root, const std::filesystem::path& path,
                                            const HashOptions& options) {
  SubtreeHash result;
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
    // 单文件根（常规文件）：没有目录树可递归，合成一个稳定的身份哈希（规范化路径 + size +
    // mtime）。这样 Full 会写入它、decideScanMode 会比对它、内部 Reconcile 也能计算它，
    // 单文件根与目录根在扫描模式判定/脏标记收敛上语义一致；文件被修改必然改变 size/mtime →
    // 哈希变化 → 下次决策升级 Full 或对账重读。只有"路径缺失/不可读"才返回 nullopt（保持
    // 既有 Reconcile 中止语义），非常规文件仍按 UnsupportedPath 处理。
    if (!std::filesystem::is_regular_file(status)) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::UnsupportedPath, ScannerErrorCode::UnsupportedFile, path,
                                               "directory tree hash requires a directory"));
      return result;
    }
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                               "directory tree hash file root size is unreadable", sizeError.message()));
      return result;
    }
    std::error_code mtimeError;
    const auto mtime = std::filesystem::last_write_time(path, mtimeError);
    if (mtimeError) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::RootUnavailable, path,
                                               "directory tree hash file root mtime is unreadable", mtimeError.message()));
      return result;
    }

    auto fileState = createHashState();
    if (fileState == nullptr) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::CacheUnavailable, path,
                                               "failed to initialize directory tree hash state"));
      return result;
    }
    static_cast<void>(updateHash(*fileState, "file"));
    static_cast<void>(updateHashSeparator(*fileState));
    static_cast<void>(updateHash(*fileState, pathToUtf8(path.lexically_normal())));
    static_cast<void>(updateHashSeparator(*fileState));
    static_cast<void>(updateHash(*fileState, std::to_string(size)));
    static_cast<void>(updateHashSeparator(*fileState));
    // libc++ 下 file_time_type::rep 为 __int128，std::to_string 无该重载且与整型重载二义
    // （同 song_identity.cpp 的 toStableText 处理），显式收窄到 64 位；纳秒计数远在 int64 内。
    const auto mtimeTicks = static_cast<long long>(mtime.time_since_epoch().count());
    static_cast<void>(updateHash(*fileState, std::to_string(mtimeTicks)));
    result.hash = canonicalHex(XXH3_128bits_digest(fileState.get()));
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

    auto kind = treeEntryKind(child, error);
    if (!kind.has_value()) {
      result.errors.push_back(makeTreeHashError(HashErrorCode::IoFailure, ScannerErrorCode::PermissionDenied, child.path(),
                                               "failed to inspect directory tree hash child", error.message()));
      continue;
    }

    auto childHash = std::string{};
    if (*kind == "dir") {
      // 目录必须始终下探：相关性只能由子树内容判定，目录名本身没有扩展名。
      auto childResult = hashTreeRecursive(root, child.path(), options);
      result.errors.insert(result.errors.end(), childResult.errors.begin(), childResult.errors.end());
      if (!childResult.hash.has_value()) {
        continue;
      }
      // 子目录仅在「含库相关性内容」或「遍历出错」时参与哈希。真正的收敛保证来自「丢弃 ↔ 参与
      // 本身改变父帧」：不可读/空目录被丢弃，一旦出现相关条目即参与，父哈希必然变化——这正是
      // orchestrator 枚举不完整收敛（净零 prune 盲区）所依赖的通道。errors 分支是安全超集：
      // 它覆盖 treeEntryKind/stat 失败，但对「目录不可读」不生效（sortedChildren 用
      // skip_permission_denied，实测 EACCES 不产生 ec），故不承担收敛职责。
      if (!childResult.hasRelevantEntries && childResult.errors.empty()) {
        continue;
      }
      childHash = *childResult.hash;
      result.hasRelevantEntries = result.hasRelevantEntries || childResult.hasRelevantEntries;
    } else {
      // 非目录：只有库相关性条目（音频/.cue/封面）参与哈希；`.lrc` 与一切无关条目的增删改既不改变
      // 哈希，也就不会把周期探测升级为 Full 对账。
      if (!isHashRelevantEntry(child.path(), options)) {
        continue;
      }
      result.hasRelevantEntries = true;
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
  auto subtree = hashTreeRecursive(rootPath, rootPath, options);
  return {.hash = std::move(subtree.hash), .errors = std::move(subtree.errors)};
}

}
