#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace seriona::scanner {

enum class ScanMode {
  Incremental,
  Full,
};

enum class ScannerErrorCode {
  RootUnavailable,
  PermissionDenied,
  UnsupportedFile,
  MetadataReadFailed,
  CacheUnavailable,
  Cancelled,
};

enum class ScannerEventType {
  ScanStarted,
  ProgressUpdated,
  PlaylistSnapshotUpdated,
  FileScanned,
  ScanCompleted,
  ScanStopped,
  ScanError,
};

enum class PlaylistNodeKind {
  Root,
  Directory,
  Album,
  Disc,
  Track,
};

enum class LyricsSource {
  None,
  EmbeddedTag,
  ExternalLrc,
};

struct ScannerRoot {
  std::filesystem::path path;
  bool recursive{true};
};

struct ScannerConfig {
  std::chrono::milliseconds progressInterval{250};
  std::vector<std::string> allowedExtensions{};
  bool followSymlinks{false};
  bool readEmbeddedLyrics{true};
  bool readExternalLyrics{true};
};

struct ScanProgress {
  std::uint64_t filesDiscovered{0};
  std::uint64_t filesScanned{0};
  std::uint64_t filesSkipped{0};
  std::uint64_t errors{0};
  std::chrono::milliseconds elapsed{0};
  std::optional<std::filesystem::path> currentPath;
};

struct ScannerError {
  ScannerErrorCode code{ScannerErrorCode::MetadataReadFailed};
  std::string message;
  std::string detail;
  std::optional<std::filesystem::path> path;
};

struct LyricLine {
  std::chrono::milliseconds timestamp{0};
  std::string text;
};

struct SongMetadata {
  std::string trackId;
  std::filesystem::path filePath;
  std::string title;
  std::string artist;
  std::string album;
  std::string albumArtist;
  std::string genre;
  std::optional<std::uint32_t> trackNumber;
  std::optional<std::uint32_t> discNumber;
  std::optional<std::uint32_t> year;
  std::optional<std::uint32_t> sampleRate;
  std::optional<std::uint16_t> bitDepth;
  std::optional<std::uint16_t> channels;
  std::optional<std::uint64_t> fileSizeBytes;
  std::optional<std::filesystem::file_time_type> fileMtime;
  std::string contentHash;
  LyricsSource effectiveLyricsSource{LyricsSource::None};
  std::vector<LyricLine> effectiveLyrics{};
  std::optional<std::filesystem::path> externalLyricsPath;
  std::optional<std::string> externalLyricsHash;
  std::optional<std::filesystem::file_time_type> externalLyricsMtime;
  std::filesystem::path sourceFilePath;
  std::optional<std::chrono::milliseconds> offset;
  std::optional<std::chrono::milliseconds> duration;
  std::string logicalTrackId;
};

struct PlaylistNode {
  std::string nodeId;
  std::optional<std::string> parentNodeId;
  PlaylistNodeKind kind{PlaylistNodeKind::Track};
  std::string displayName;
  std::optional<SongMetadata> song;
  std::vector<std::string> childNodeIds{};
};

struct PlaylistTreeSnapshot {
  std::uint64_t version{0};
  std::chrono::steady_clock::time_point generatedAt{};
  std::vector<PlaylistNode> nodes{};
  std::optional<std::string> rootNodeId;
};

using ScannerEventPayload = std::variant<ScanProgress, PlaylistTreeSnapshot, SongMetadata, ScannerError>;

struct ScannerEvent {
  ScannerEventType type{ScannerEventType::ProgressUpdated};
  std::uint64_t monotonicVersion{0};
  std::chrono::steady_clock::time_point timestamp{};
  ScannerEventPayload payload{ScanProgress{}};
};

using ScannerEventSink = std::function<void(ScannerEvent)>;

class FileScannerService {
public:
  virtual ~FileScannerService() = default;

  virtual void setEventSink(ScannerEventSink sink) = 0;
  virtual void configure(const ScannerConfig& config) = 0;
  virtual void scan(const std::vector<ScannerRoot>& roots, ScanMode mode) = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual PlaylistTreeSnapshot snapshot() const = 0;
};

}
