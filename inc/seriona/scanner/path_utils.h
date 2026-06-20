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
  IgnoredCue,
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
[[nodiscard]] std::filesystem::path expectedLyricsSidecarPath(const std::filesystem::path& audioPath);
[[nodiscard]] std::string serializeRelativeUtf8(const std::filesystem::path& root,
                                                const std::filesystem::path& path);
[[nodiscard]] ClassifiedPath classifyScannerPath(const std::filesystem::path& root,
                                                 const std::filesystem::path& path,
                                                 const PathClassificationConfig& config = {});
[[nodiscard]] std::vector<ClassifiedPath> discoverScannerPaths(const ScannerRoot& root,
                                                               const PathClassificationConfig& config = {});

}
