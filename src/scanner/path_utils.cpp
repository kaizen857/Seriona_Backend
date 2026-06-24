#include "seriona/scanner/path_utils.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace seriona::scanner {
namespace {

[[nodiscard]] std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

[[nodiscard]] std::string normalizedExtension(const std::filesystem::path& path) {
  return lowerAscii(path.extension().generic_string());
}

[[nodiscard]] std::unordered_set<std::string> extensionSet(const std::vector<std::string>& extensions) {
  std::unordered_set<std::string> result;
  result.reserve(extensions.size());
  for (auto extension : extensions) {
    extension = lowerAscii(std::move(extension));
    if (!extension.empty() && extension.front() != '.') {
      extension.insert(extension.begin(), '.');
    }
    if (!extension.empty()) {
      result.insert(std::move(extension));
    }
  }
  return result;
}

[[nodiscard]] PathClassificationError makeError(ScannerErrorCode code, std::filesystem::path path, std::string message,
                                                std::string detail = {}) {
  return {.code = code, .path = std::move(path), .message = std::move(message), .detail = std::move(detail)};
}

[[nodiscard]] std::filesystem::path weaklyCanonicalParentJoinedPath(const std::filesystem::path& path) {
  std::error_code error;
  const auto parent = std::filesystem::weakly_canonical(path.parent_path(), error);
  if (error) {
    return path.lexically_normal();
  }
  return (parent / path.filename()).lexically_normal();
}

[[nodiscard]] std::string displayNameFor(const std::filesystem::path& path) {
  const auto filename = path.filename().generic_string();
  if (!filename.empty()) {
    return filename;
  }
  return path.generic_string();
}

[[nodiscard]] ClassifiedPath makeTraversalError(const std::filesystem::path& rootPath, const std::error_code& error) {
  ClassifiedPath result;
  result.path = rootPath;
  result.displayName = displayNameFor(rootPath);
  result.kind = PathEntryKind::PermissionDenied;
  result.errors.push_back(makeError(ScannerErrorCode::PermissionDenied, rootPath,
                                    "failed to continue scanner traversal", error.message()));
  return result;
}

[[nodiscard]] std::filesystem::directory_options directoryOptions(const PathClassificationConfig& config) {
  auto options = std::filesystem::directory_options::skip_permission_denied;
  if (config.followSymlinks) {
    options |= std::filesystem::directory_options::follow_directory_symlink;
  }
  return options;
}

void appendClassifiedPath(std::vector<ClassifiedPath>& entries, const std::filesystem::path& root,
                          const std::filesystem::path& path, const PathClassificationConfig& config) {
  auto classified = classifyScannerPath(root, path, config);
  if (classified.kind != PathEntryKind::Directory) {
    entries.push_back(std::move(classified));
  }
}

}

const std::vector<std::string>& defaultAudioExtensions() {
  static const std::vector<std::string> extensions{
      ".mp3",   ".flac", ".wav",   ".aiff", ".aif",  ".aifc", ".oga",   ".opus",  ".spx",
      ".m4a",   ".m4b",  ".m4r",   ".isma", ".aac",  ".alac", ".ape",   ".wv",    ".wma",
      ".tta",   ".tak",  ".shn",   ".caf",  ".au",   ".snd",  ".voc",   ".vqf",   ".qoa",
      ".ac3",   ".eac3", ".ac4",   ".dts",  ".dtshd", ".truehd", ".mlp", ".spdif", ".loas",
      ".latm",  ".adts", ".mp2",   ".mpa",  ".amr",  ".awb",  ".gsm",   ".qcp",   ".dsf",
      ".dff",   ".dss",  ".oma",   ".aa",   ".aax",  ".adx",  ".brstm", ".bfstm", ".bcstm",
      ".hca",   ".vag",  ".xvag",  ".rsd",  ".rso",  ".xa",   ".xwma",  ".fsb",   ".mod",
      ".s3m",   ".xm",   ".it",    ".mptm", ".stm",  ".669",  ".mtm",   ".ult",   ".far",
      ".amf",   ".ams",  ".dbm",   ".dsm",  ".med",  ".okt",  ".s8",    ".u8",    ".s16le",
      ".s16be", ".u16le", ".u16be", ".s24le", ".s24be", ".u24le", ".u24be", ".s32le", ".s32be",
      ".u32le", ".u32be", ".f32le", ".f32be", ".f64le", ".f64be", ".alaw", ".mulaw", ".g722",
      ".g723",  ".g726", ".g728",  ".g729", ".sbc",  ".aptx", ".aptxhd", ".lc3",   ".codec2",
      ".c2",    ".ogg",  ".mka",   ".weba"};
  return extensions;
}

bool isSupportedAudioExtension(const std::filesystem::path& path, const std::vector<std::string>& allowedExtensions) {
  if (isExcludedContainerExtension(path) || isCueSheetPath(path) || isLyricsSidecarPath(path)) {
    return false;
  }
  const auto allowed = extensionSet(allowedExtensions.empty() ? defaultAudioExtensions() : allowedExtensions);
  return allowed.contains(normalizedExtension(path));
}

bool isExcludedContainerExtension(const std::filesystem::path& path) {
  static const auto excluded = extensionSet({".mp4", ".mov", ".m4v", ".3gp", ".3g2", ".mj2", ".f4v", ".ismv",
                                            ".wmv", ".asf", ".webm", ".mkv", ".avi", ".flv", ".ts", ".m2ts",
                                            ".mpg", ".mpeg", ".vob"});
  return excluded.contains(normalizedExtension(path));
}

bool isCueSheetPath(const std::filesystem::path& path) {
  return normalizedExtension(path) == ".cue";
}

bool isLyricsSidecarPath(const std::filesystem::path& path) {
  return normalizedExtension(path) == ".lrc";
}

std::filesystem::path expectedLyricsSidecarPath(const std::filesystem::path& audioPath) {
  auto sidecar = audioPath;
  sidecar.replace_extension(".lrc");
  return sidecar;
}

std::string serializeRelativeUtf8(const std::filesystem::path& root, const std::filesystem::path& path) {
  std::error_code error;
  auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    relative = path.filename();
  }
  const auto utf8 = relative.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

ClassifiedPath classifyScannerPath(const std::filesystem::path& root, const std::filesystem::path& path,
                                   const PathClassificationConfig& config) {
  const auto canonicalRoot = weaklyCanonicalParentJoinedPath(root);
  const auto canonicalPath = weaklyCanonicalParentJoinedPath(path);
  ClassifiedPath result{.path = canonicalPath,
                        .relativePath = std::filesystem::path{serializeRelativeUtf8(canonicalRoot, canonicalPath)},
                        .relativeUtf8 = serializeRelativeUtf8(canonicalRoot, canonicalPath),
                        .displayName = displayNameFor(canonicalPath),
                        .kind = PathEntryKind::Unsupported,
                        .sidecarLyricsPath = std::nullopt,
                        .errors = {}};

  std::error_code error;
  const auto status = std::filesystem::symlink_status(canonicalPath, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      result.kind = PathEntryKind::Missing;
      result.errors.push_back(makeError(ScannerErrorCode::RootUnavailable, canonicalPath, "scanner path does not exist",
                                        error.message()));
      spdlog::warn("scanner root unavailable: {} ({})", canonicalPath.generic_string(), error.message());
      return result;
    }
    result.kind = error == std::errc::permission_denied ? PathEntryKind::PermissionDenied : PathEntryKind::Error;
    result.errors.push_back(makeError(result.kind == PathEntryKind::PermissionDenied ? ScannerErrorCode::PermissionDenied
                                                                                      : ScannerErrorCode::RootUnavailable,
                                      canonicalPath, "failed to stat scanner path", error.message()));
    spdlog::warn("scanner path access failed: {} ({})", canonicalPath.generic_string(), error.message());
    return result;
  }
  if (!std::filesystem::exists(status)) {
    result.kind = PathEntryKind::Missing;
    result.errors.push_back(makeError(ScannerErrorCode::RootUnavailable, canonicalPath, "scanner path does not exist"));
    spdlog::warn("scanner root unavailable: {}", canonicalPath.generic_string());
    return result;
  }
  if (std::filesystem::is_symlink(status) && !config.followSymlinks) {
    result.kind = PathEntryKind::Symlink;
    result.errors.push_back(makeError(ScannerErrorCode::UnsupportedFile, canonicalPath, "symlink skipped by scanner policy"));
    return result;
  }

  const auto effectiveStatus = std::filesystem::status(canonicalPath, error);
  if (error) {
    result.kind = error == std::errc::permission_denied ? PathEntryKind::PermissionDenied : PathEntryKind::Error;
    result.errors.push_back(makeError(result.kind == PathEntryKind::PermissionDenied ? ScannerErrorCode::PermissionDenied
                                                                                      : ScannerErrorCode::RootUnavailable,
                                      canonicalPath, "failed to resolve scanner path", error.message()));
    spdlog::warn("scanner path resolve failed: {} ({})", canonicalPath.generic_string(), error.message());
    return result;
  }

  if (std::filesystem::is_directory(effectiveStatus)) {
    result.kind = canonicalPath == canonicalRoot ? PathEntryKind::DirectoryRoot : PathEntryKind::Directory;
    return result;
  }
  if (!std::filesystem::is_regular_file(effectiveStatus)) {
    result.kind = PathEntryKind::NonRegular;
    result.errors.push_back(makeError(ScannerErrorCode::UnsupportedFile, canonicalPath, "non-regular scanner path skipped"));
    return result;
  }
  if (isLyricsSidecarPath(canonicalPath)) {
    result.kind = config.readExternalLyrics ? PathEntryKind::LyricsSidecar : PathEntryKind::Unsupported;
    return result;
  }
  if (isCueSheetPath(canonicalPath)) {
    result.kind = PathEntryKind::IgnoredCue;
    result.errors.push_back(makeError(ScannerErrorCode::UnsupportedFile, canonicalPath, "cue sheets are ignored by scanner policy"));
    return result;
  }
  if (isSupportedAudioExtension(canonicalPath, config.allowedExtensions)) {
    result.kind = canonicalPath == canonicalRoot ? PathEntryKind::SingleFileRoot : PathEntryKind::AudioCandidate;
    const auto sidecar = expectedLyricsSidecarPath(canonicalPath);
    if (config.readExternalLyrics && std::filesystem::is_regular_file(sidecar, error)) {
      result.sidecarLyricsPath = sidecar;
    }
    return result;
  }

  result.kind = PathEntryKind::Unsupported;
  result.errors.push_back(makeError(ScannerErrorCode::UnsupportedFile, canonicalPath, "unsupported scanner file extension"));
  spdlog::debug("unsupported file skipped: {}", canonicalPath.generic_string());
  return result;
}

std::vector<ClassifiedPath> discoverScannerPaths(const ScannerRoot& root, const PathClassificationConfig& config) {
  std::vector<ClassifiedPath> entries;
  auto rootPath = weaklyCanonicalParentJoinedPath(root.path);
  auto rootClassification = classifyScannerPath(rootPath, rootPath, config);
  if (rootClassification.kind != PathEntryKind::DirectoryRoot) {
    entries.push_back(std::move(rootClassification));
    return entries;
  }

  entries.push_back(std::move(rootClassification));
  std::error_code error;
  if (root.recursive) {
    for (std::filesystem::recursive_directory_iterator iterator(rootPath, directoryOptions(config), error), end;
         iterator != end; iterator.increment(error)) {
      if (error) {
        entries.push_back(makeTraversalError(rootPath, error));
        error.clear();
        continue;
      }
      appendClassifiedPath(entries, rootPath, iterator->path(), config);
    }
  } else {
    for (std::filesystem::directory_iterator iterator(rootPath, directoryOptions(config), error), end; iterator != end;
         iterator.increment(error)) {
      if (error) {
        entries.push_back(makeTraversalError(rootPath, error));
        error.clear();
        continue;
      }
      appendClassifiedPath(entries, rootPath, iterator->path(), config);
    }
  }

  std::ranges::sort(entries, {}, &ClassifiedPath::relativeUtf8);
  return entries;
}

}
