#include "seriona/scanner/path_utils.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <fstream>
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

struct CueParseResult {
  std::vector<std::filesystem::path> references;
  std::optional<PathClassificationError> error;
};

[[nodiscard]] CueParseResult extractAudioReferencesFromCue(const std::filesystem::path& cuePath) {
  CueParseResult result;
  
  try {
    std::ifstream cueFile(cuePath);
    if (!cueFile.is_open()) {
      result.error = makeError(ScannerErrorCode::MetadataReadFailed, cuePath, 
                               "failed to open CUE sheet for parsing",
                               "file could not be opened");
      return result;
    }

    std::string line;
    const auto cueDir = cuePath.parent_path();
    bool foundFileLine = false;
    
    while (std::getline(cueFile, line)) {
      const auto filePos = line.find("FILE ");
      if (filePos == std::string::npos) {
        continue;
      }
      
      const auto firstQuote = line.find('"', filePos);
      if (firstQuote == std::string::npos) {
        continue;
      }
      
      const auto secondQuote = line.find('"', firstQuote + 1);
      if (secondQuote == std::string::npos) {
        continue;
      }
      
      const auto filename = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
      if (!filename.empty()) {
        foundFileLine = true;
        auto audioPath = cueDir / std::filesystem::path{filename};
        result.references.push_back(std::move(audioPath));
      }
    }
    
    if (cueFile.bad()) {
      result.error = makeError(ScannerErrorCode::MetadataReadFailed, cuePath,
                               "CUE sheet parsing encountered I/O error",
                               "stream read failed");
    } else if (!foundFileLine) {
      result.error = makeError(ScannerErrorCode::MetadataReadFailed, cuePath,
                               "CUE sheet contains no valid FILE lines",
                               "malformed or empty CUE file");
    }
  } catch (const std::exception& ex) {
    result.error = makeError(ScannerErrorCode::MetadataReadFailed, cuePath,
                             "CUE sheet parsing failed with exception",
                             ex.what());
    result.references.clear();
  }
  
  return result;
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
    result.kind = PathEntryKind::CueSheet;
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

  // Two-pass traversal for CUE file handling:
  // Pass 1: Collect all paths and identify CUE files
  std::vector<std::filesystem::path> allPaths;
  std::vector<std::filesystem::path> cueSheetPaths;
  std::error_code error;

  if (root.recursive) {
    for (std::filesystem::recursive_directory_iterator iterator(rootPath, directoryOptions(config), error), end;
         iterator != end; iterator.increment(error)) {
      if (error) {
        entries.push_back(makeTraversalError(rootPath, error));
        error.clear();
        continue;
      }
      const auto& path = iterator->path();
      allPaths.push_back(path);
      if (isCueSheetPath(path)) {
        cueSheetPaths.push_back(path);
      }
    }
  } else {
    for (std::filesystem::directory_iterator iterator(rootPath, directoryOptions(config), error), end; iterator != end;
         iterator.increment(error)) {
      if (error) {
        entries.push_back(makeTraversalError(rootPath, error));
        error.clear();
        continue;
      }
      const auto& path = iterator->path();
      allPaths.push_back(path);
      if (isCueSheetPath(path)) {
        cueSheetPaths.push_back(path);
      }
    }
  }

  // Pass 1b: Parse CUE files and collect referenced audio files
  std::unordered_set<std::filesystem::path> referencedAudioFiles;
  std::unordered_map<std::filesystem::path, PathClassificationError> cueParseErrors;
  for (const auto& cuePath : cueSheetPaths) {
    const auto parseResult = extractAudioReferencesFromCue(cuePath);
    if (parseResult.error.has_value()) {
      const auto canonicalCuePath = weaklyCanonicalParentJoinedPath(cuePath);
      cueParseErrors[canonicalCuePath] = *parseResult.error;
      spdlog::warn("CUE sheet parse failed for {}: {}", cuePath.generic_string(), parseResult.error->message);
    }
    for (const auto& audioPath : parseResult.references) {
      const auto canonicalRefPath = weaklyCanonicalParentJoinedPath(audioPath);
      referencedAudioFiles.insert(canonicalRefPath);
    }
  }

  // Pass 2: Classify and add paths, skipping referenced audio files
  for (const auto& path : allPaths) {
    auto classified = classifyScannerPath(rootPath, path, config);
    if (classified.kind == PathEntryKind::Directory) {
      continue;
    }
    if (classified.kind == PathEntryKind::CueSheet) {
      if (const auto errorIt = cueParseErrors.find(classified.path); errorIt != cueParseErrors.end()) {
        classified.errors.push_back(errorIt->second);
      }
    }
    if (classified.kind == PathEntryKind::AudioCandidate) {
      if (referencedAudioFiles.contains(classified.path)) {
        continue;
      }
    }
    entries.push_back(std::move(classified));
  }

  std::ranges::sort(entries, {}, &ClassifiedPath::relativeUtf8);
  return entries;
}

}
