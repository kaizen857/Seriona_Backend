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
// 库相关性：受支持音频、.cue 工程、歌词侧车（.lrc/.srt/.ass/.ttml/.txt）、封面侧车。
// 调用方必须共用本谓词——
// 同一过滤规则分处两地会漂移并静默漏掉真实变化（Bazel PR #22615）。
[[nodiscard]] bool isLibraryRelevantPath(const std::filesystem::path& path,
                                         const std::vector<std::string>& allowedExtensions = {});
[[nodiscard]] std::filesystem::path expectedLyricsSidecarPath(const std::filesystem::path& audioPath);
// 受支持的歌词侧车扩展名，按侧车优先级的降序排列：.lrc > .srt > .ass > .ttml > .txt。
// 同名多格式并存时取靠前者（确定性规则）。
[[nodiscard]] const std::vector<std::string>& lyricsSidecarExtensions();
// 该音频路径的全部候选歌词侧车（按上表优先级序）；只构造路径名，不检查存在性/可读性。
[[nodiscard]] std::vector<std::filesystem::path> candidateLyricsSidecarPaths(const std::filesystem::path& audioPath);
// 侧车路径 → 生效歌词来源；非侧车扩展名（含 .krc/.qrc/.yrc）返回 LyricsSource::None。
[[nodiscard]] LyricsSource lyricsSourceForSidecarPath(const std::filesystem::path& path);
// 该音频当前存在的最高优先级歌词侧车；无候选存在时返回空 path。只判存在性，不做解析。
// .txt 的「非空且清洗后 ≤ kPlainTextLyricsMaxLines 行」是**解析后**判据，由调用方用
// acceptsPlainTextLyricsSidecar 施加（.txt 已是末位，不通过即等价于无侧车）。
[[nodiscard]] std::filesystem::path resolveLyricsSidecarPath(const std::filesystem::path& audioPath);
// .txt 侧车行数上限（G5 收紧，避免把音乐目录里的 rip log/notes 一类文本当歌词）。
inline constexpr std::size_t kPlainTextLyricsMaxLines = 200U;
[[nodiscard]] bool acceptsPlainTextLyricsSidecar(const std::vector<LyricLine>& lines);
[[nodiscard]] std::string serializeRelativeUtf8(const std::filesystem::path& root,
                                                const std::filesystem::path& path);
[[nodiscard]] ClassifiedPath classifyScannerPath(const std::filesystem::path& root,
                                                 const std::filesystem::path& path,
                                                 const PathClassificationConfig& config = {});
[[nodiscard]] std::vector<ClassifiedPath> discoverScannerPaths(const ScannerRoot& root,
                                                               const PathClassificationConfig& config = {});

}
