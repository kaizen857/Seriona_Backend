#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../audio/audio_contracts.h"
#include "../scanner/scanner_contracts.h"

namespace seriona::metadata {
class MetadataSharingService;
}

namespace seriona::control {

class FolderSortSettingsStore;
class AppSettingsStore;

enum class PlaybackStatus {
  Stopped,
  Playing,
  Paused,
  Loading,
  Seeking,
  Buffering,
  Error,
};

enum class RepeatMode {
  Off,
  One,
  All,
};

enum class Capability : std::uint32_t {
  None = 0,
  CanPlay = 1U << 0U,
  CanPause = 1U << 1U,
  CanStop = 1U << 2U,
  CanSeek = 1U << 3U,
  CanSkipNext = 1U << 4U,
  CanSkipPrevious = 1U << 5U,
  CanSetRepeat = 1U << 6U,
  CanSetVolume = 1U << 7U,
  CanSelectTrack = 1U << 8U,
};

struct TrackIdentity {
  std::string trackId;
  std::filesystem::path filePath;
  std::string sourceId;
  std::string libraryId;
};

struct DisplayMetadata {
  std::string title;
  std::string artist;
  std::string album;
  std::string albumArtist;
  std::string genre;
};

struct ArtworkRef {
  std::optional<std::filesystem::path> localPath;
  std::optional<std::string> uri;
  std::optional<std::string> contentHash;
  // Retained thumbnail fallback: pending/error artwork keeps this while only
  // localPath is upgraded to the full-resolution cover.
  std::optional<std::filesystem::path> thumbnailPath;
};

struct ArtworkResolveRequest {
  std::uint64_t generation{0};
  TrackIdentity identity;
  std::filesystem::path artworkSourcePath;
  std::filesystem::path fallbackThumbnailPath;
};

enum class ArtworkResolveOutcomeKind : std::uint8_t {
  FullPath,
  NoArt,
  CoverError,
  ResolverFailure,
};

struct ArtworkResolveOutcomeView {
  ArtworkResolveOutcomeKind kind{ArtworkResolveOutcomeKind::NoArt};
  std::optional<std::filesystem::path> fullPath;
  // CoverError only: the typed cover-processing code as text; empty otherwise.
  std::string detail;
};

struct ArtworkResolveResultView {
  std::uint64_t generation{0};
  TrackIdentity identity;
  ArtworkResolveOutcomeView outcome;
};

using ArtworkResolveCallback = std::function<void(ArtworkResolveResultView)>;

class ArtworkResolveService {
public:
  virtual ~ArtworkResolveService() = default;

  virtual void request(ArtworkResolveRequest request) noexcept = 0;
  virtual void setResultCallback(ArtworkResolveCallback callback) noexcept = 0;
  virtual void stop() noexcept = 0;
};

struct PlaybackTimeline {
  std::chrono::milliseconds position{0};
  std::optional<std::chrono::milliseconds> duration;
  std::optional<std::chrono::milliseconds> buffered;
  std::optional<std::chrono::milliseconds> seekableFrom;
  std::optional<std::chrono::milliseconds> seekableTo;
};

struct PlaybackSnapshot {
  PlaybackStatus state{PlaybackStatus::Stopped};
  std::optional<std::string> errorCode;
  std::optional<std::string> errorMessage;
};

struct SnapshotFreshness {
  std::uint64_t version{0};
  std::chrono::steady_clock::time_point sampledAt{};
};

struct PlaybackCapabilities {
  bool canPlay{false};
  bool canPause{false};
  bool canStop{false};
  bool canSeek{false};
  bool canSkipNext{false};
  bool canSkipPrevious{false};
  bool canSetRepeat{false};
  bool canSetShuffle{false};
  bool canSetVolume{false};
  bool canSelectTrack{false};
};

// 临时播放队列条目（T7）。跨端契约字段名定死：trackId/nodeId —— 前端（T14/T15）
// 按 queueEntries: [{trackId, nodeId}] 同名断言。
struct QueueEntry {
  std::string trackId;
  std::string nodeId;
};

struct PlayerStateSnapshot {
  SnapshotFreshness freshness{};
  std::optional<TrackIdentity> currentTrack;
  std::optional<DisplayMetadata> display;
  std::optional<ArtworkRef> artwork;
  PlaybackSnapshot playback{};
  RepeatMode repeatMode{RepeatMode::Off};
  bool shuffle{false};
  PlaybackCapabilities capabilities{};
  PlaybackTimeline timeline{};
  float volume{1.0F};
  bool muted{false};
  // 临时播放队列（不持久化，重启清空）。字段名/结构为跨端定死契约：
  // queueEntries: [{trackId, nodeId}]（T14/T15 按同名断言）。
  std::vector<QueueEntry> queueEntries;
};

enum class LibraryScanStatus {
  Idle,
  Scanning,
  Completed,
  Stopped,
  Error,
};

struct LibraryStateSnapshot {
  std::uint64_t version{0};
  LibraryScanStatus scanStatus{LibraryScanStatus::Idle};
  std::optional<scanner::PlaylistTreeSnapshot> libraryTree;
  std::optional<scanner::ScanProgress> scanProgress;
  std::optional<scanner::ScannerError> lastError;
};

enum class FolderSortField {
  Title,
  Artist,
  Album,
  Filename,
  Year,
  Duration,
  CreatedDate,
  DiscNumber,
  TrackNumber,
};

enum class FolderSortDirection {
  Ascending,
  Descending,
};

enum class FolderSortMissingValuePolicy {
  First,
  Last,
};

struct FolderSortRule {
  FolderSortField field{FolderSortField::Title};
  FolderSortDirection direction{FolderSortDirection::Ascending};
  FolderSortMissingValuePolicy missingValuePolicy{FolderSortMissingValuePolicy::Last};
};

struct FolderSortSetting {
  std::filesystem::path rootPath;
  std::string folderNodeId;
  std::vector<FolderSortRule> rules;
};

enum class PlaybackContextScope {
  Root,
  Folder,
};

struct PlaybackContextDescriptor {
  PlaybackContextScope scope{PlaybackContextScope::Root};
  std::filesystem::path rootPath;
  std::string folderNodeId;
  std::optional<TrackIdentity> anchorTrack;
  std::vector<FolderSortRule> sortRules;
};

enum class ControlDomainNotificationKind {
  LibrarySnapshotUpdated,
  LibraryScanStarted,
  LibraryScanProgressUpdated,
  LibraryScanCompleted,
  LibraryScanStopped,
  LibraryScanError,
  PlaybackEnded,
  PlaybackError,
  OutputModeFallback,
  CommandRejected,
  FolderSortRulesApplied,
};

enum class MediaControllerErrorCode {
  None,
  ControllerStopped,
  NoPlayableTrack,
  TrackNotInLibrary,
  InvalidCommand,
  BackendRejected,
};

struct ControlDomainNotification {
  ControlDomainNotificationKind kind{ControlDomainNotificationKind::LibrarySnapshotUpdated};
  MediaControllerErrorCode errorCode{MediaControllerErrorCode::None};
  std::string message;
  std::optional<LibraryScanStatus> scanStatus;
  std::optional<FolderSortSetting> folderSortSetting;
};

struct MediaControllerCommandResult {
  bool accepted{false};
  MediaControllerErrorCode code{MediaControllerErrorCode::None};
  std::string message;
};

struct MediaControllerOptions {
  bool runInlineForTests{false};
  std::uint64_t shuffleSeed{0};
  std::size_t shuffleHistorySize{50};
};

struct MediaControllerDependencies {
  std::shared_ptr<audio::AudioPlaybackService> audio;
  std::shared_ptr<scanner::FileScannerService> scanner;
  std::unique_ptr<::seriona::metadata::MetadataSharingService> metadata;
  std::shared_ptr<FolderSortSettingsStore> folderSortSettingsStore;
  // 前端应用设置存储（输出设置/导航状态/曲目统计）；null 时读写安全降级。
  std::shared_ptr<AppSettingsStore> appSettingsStore;
  // Optional artwork resolver; when null, artwork resolve intents are dropped.
  std::shared_ptr<ArtworkResolveService> artworkResolver;
};

enum class MediaControlCommandKind {
  Play,
  Pause,
  Stop,
  TogglePlayPause,
  SeekTo,
  SeekBy,
  SetVolume,
  SetMuted,
  SetRepeatMode,
  SetShuffle,
  SkipNext,
  SkipPrevious,
  SelectTrack,
  StartPlaybackFromContext,
  ApplyFolderSortRules,
  // Appended at the end: existing enumerators keep their ordinal positions,
  // preserving serialized-command compatibility.
  ConfigureOutput,
  // 删除命令（T8）：DeleteTrack=单曲、DeleteFolder=递归删除文件夹，均直接删原文件
  // （无回收站），目标经 MediaControlCommand::targetPath 传入。追加末尾保持兼容。
  DeleteTrack,
  DeleteFolder,
  // 临时播放队列命令（T7）：PlayNextTrack=目标曲目入队首；ClearPlayQueue=清空队列；
  // RemoveFromQueue=按索引移除。追加末尾保持序列化兼容。
  PlayNextTrack,
  ClearPlayQueue,
  RemoveFromQueue,
};

struct MediaControlCommand {
  MediaControlCommandKind kind{MediaControlCommandKind::Play};
  std::optional<std::chrono::milliseconds> position;
  std::optional<std::chrono::milliseconds> delta;
  std::optional<float> volume;
  std::optional<bool> muted;
  std::optional<RepeatMode> repeatMode;
  std::optional<bool> shuffle;
  std::optional<TrackIdentity> track;
  std::optional<PlaybackContextDescriptor> playbackContext;
  std::optional<FolderSortSetting> folderSortSetting;
  // Last member on purpose: appended fields never disturb designated or
  // value-initialization of existing commands.
  std::optional<audio::AudioOutputConfig> outputConfig;
  // DeleteTrack/DeleteFolder 目标：绝对路径（DeleteTrack=音频文件，
  // DeleteFolder=文件夹）。追加末尾保持序列化兼容；字段名对前端（T16）为定死契约。
  std::optional<std::filesystem::path> targetPath;
  // RemoveFromQueue 目标索引（queueEntries 下标）。追加末尾保持序列化兼容。
  std::optional<std::size_t> queueIndex;
};

using PlayerStateSnapshotCallback = std::function<void(PlayerStateSnapshot)>;
using PlayerStateSubscriptionCallback = PlayerStateSnapshotCallback;
using MediaControlCommandSink = std::function<void(const MediaControlCommand&)>;

using LibraryStateSnapshotCallback = std::function<void(LibraryStateSnapshot)>;
using LibraryStateSubscriptionCallback = LibraryStateSnapshotCallback;
using ControlDomainNotificationCallback = std::function<void(ControlDomainNotification)>;
using ControlDomainNotificationSubscriptionCallback = ControlDomainNotificationCallback;

struct SubscriptionHandle {
  std::size_t subscriptionId{0};
  std::function<void()> unsubscribe;
};

using PlayerStateSubscriptionFactory = std::function<SubscriptionHandle(PlayerStateSnapshotCallback)>;
using MediaControlCommandSinkFactory = std::function<MediaControlCommandSink()>;
using LibraryStateSubscriptionFactory = std::function<SubscriptionHandle(LibraryStateSnapshotCallback)>;
using ControlDomainNotificationSubscriptionFactory = std::function<SubscriptionHandle(ControlDomainNotificationCallback)>;

}
