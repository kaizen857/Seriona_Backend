#pragma once

#include "seriona/scanner/scanner_contracts.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

enum class PathEntryKind {
  DirectoryRoot,
  SingleFileRoot,
  Directory,
  AudioCandidate,
  LyricsSidecar,
  CueSheet,
  Unsupported,
  NonRegular,
  Symlink,
  Missing,
  PermissionDenied,
  Error,
};

struct PathClassificationConfig {
  std::vector<std::string> allowedExtensions{};
  bool followSymlinks{false};
  bool readExternalLyrics{true};
};

struct PathClassificationError {
  ScannerErrorCode code{ScannerErrorCode::UnsupportedFile};
  std::filesystem::path path;
  std::string message;
  std::string detail;
};

struct ClassifiedPath {
  std::filesystem::path path;
  std::filesystem::path relativePath;
  std::string relativeUtf8;
  std::string displayName;
  PathEntryKind kind{PathEntryKind::Unsupported};
  std::optional<std::filesystem::path> sidecarLyricsPath;
  std::vector<PathClassificationError> errors{};
};

[[nodiscard]] const std::vector<std::string>& defaultAudioExtensions();
[[nodiscard]] bool isSupportedAudioExtension(const std::filesystem::path& path,
                                             const std::vector<std::string>& allowedExtensions = {});
[[nodiscard]] bool isExcludedContainerExtension(const std::filesystem::path& path);
[[nodiscard]] bool isCueSheetPath(const std::filesystem::path& path);
[[nodiscard]] bool isLyricsSidecarPath(const std::filesystem::path& path);
// Sidecar cover predicate mirrored from TagReader (src/cover/SidecarCover.cpp):
// stem in {cover, front, folder, album, artwork} (ASCII case-insensitive) and
// extension in {.png, .jpg, .jpeg, .bmp, .webp, .gif, .tiff}. Random images do not match.
[[nodiscard]] bool isCoverSidecar(const std::filesystem::path& path);
// 库相关性：受支持音频、.cue 工程、.lrc 歌词侧车、封面侧车。调用方必须共用本谓词——
// 同一过滤规则分处两地会漂移并静默漏掉真实变化（Bazel PR #22615）。
[[nodiscard]] bool isLibraryRelevantPath(const std::filesystem::path& path,
                                         const std::vector<std::string>& allowedExtensions = {});
[[nodiscard]] std::filesystem::path expectedLyricsSidecarPath(const std::filesystem::path& audioPath);
[[nodiscard]] std::string serializeRelativeUtf8(const std::filesystem::path& root,
                                                const std::filesystem::path& path);
[[nodiscard]] ClassifiedPath classifyScannerPath(const std::filesystem::path& root,
                                                 const std::filesystem::path& path,
                                                 const PathClassificationConfig& config = {});
[[nodiscard]] std::vector<ClassifiedPath> discoverScannerPaths(const ScannerRoot& root,
                                                               const PathClassificationConfig& config = {});

}
