#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
  // 0 = 由生产工厂注入每次进程启动的随机种子（每次启动/每台机器序列不同）；
  // 显式非 0 = 确定性种子（测试可复现，reducer 直接以其播种）。
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
  // 过渡参数命令（T1）：SetTransitionConfig=配置播放过渡参数（淡入淡出/交叉/预
  // 加载），载荷=MediaControlCommand::transitionConfig。与 ConfigureOutput 语义隔
  // 离：仅存配置，绝不触发整轨重载/设备操作。追加末尾保持序列化兼容。
  SetTransitionConfig,
  // 均衡器参数命令（任务16/B1.2）：SetEqualizerConfig=配置均衡器参数（总开关/频段
  // 模式/前置增益/频段增益/限幅器），载荷=MediaControlCommand::equalizerConfig。与
  // ConfigureOutput/SetTransitionConfig 语义隔离：仅更新均衡处理参数，绝不触发输
  // 出重载/设备生命周期操作。追加末尾保持序列化兼容。
  SetEqualizerConfig,
  // 频谱开关命令（R2 频谱显示链路）：SetSpectrumEnabled=实时频谱分析开关（bool
  // 载荷=spectrumEnabled 字段）。纯门控转发至音频服务原子位（无 reducer 镜像、
  // 不触碰播放/均衡器状态，同 SetMuted/SetVolume 直转先例）。追加末尾保持兼容。
  SetSpectrumEnabled,
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
  // SetTransitionConfig 载荷（T1）。追加末尾保持序列化兼容。
  std::optional<audio::TransitionConfig> transitionConfig;
  // SetEqualizerConfig 载荷（任务16）：均衡器参数（默认构造 = 关闭直通，与旧行为等
  // 价）。追加末尾保持序列化兼容。
  std::optional<audio::EqualizerConfig> equalizerConfig;
  // SetSpectrumEnabled 载荷（R2）：频谱分析开关目标值。追加末尾保持序列化兼容。
  std::optional<bool> spectrumEnabled;
};

using PlayerStateSnapshotCallback = std::function<void(PlayerStateSnapshot)>;
using PlayerStateSubscriptionCallback = PlayerStateSnapshotCallback;
using MediaControlCommandSink = std::function<void(const MediaControlCommand&)>;

using LibraryStateSnapshotCallback = std::function<void(LibraryStateSnapshot)>;
using LibraryStateSubscriptionCallback = LibraryStateSnapshotCallback;
using ControlDomainNotificationCallback = std::function<void(ControlDomainNotification)>;
using ControlDomainNotificationSubscriptionCallback = ControlDomainNotificationCallback;
// 均衡器状态订阅回调（任务16）：快照=audio::EqualizerStateSnapshot（生效配置 + 按
// 当前采样率解析的增益曲线，经 setEqualizer 生效后递增 generation）。
using EqualizerStateSnapshotCallback = std::function<void(audio::EqualizerStateSnapshot)>;
using EqualizerStateSubscriptionCallback = EqualizerStateSnapshotCallback;
// 频谱订阅回调（任务16）：快照=audio::SpectrumSnapshot（120 段频带电平，实时分析输出）。
using SpectrumSnapshotCallback = std::function<void(audio::SpectrumSnapshot)>;
using SpectrumSubscriptionCallback = SpectrumSnapshotCallback;

struct SubscriptionHandle {
  std::size_t subscriptionId{0};
  std::function<void()> unsubscribe;
};

using PlayerStateSubscriptionFactory = std::function<SubscriptionHandle(PlayerStateSnapshotCallback)>;
using MediaControlCommandSinkFactory = std::function<MediaControlCommandSink()>;
using LibraryStateSubscriptionFactory = std::function<SubscriptionHandle(LibraryStateSnapshotCallback)>;
using ControlDomainNotificationSubscriptionFactory = std::function<SubscriptionHandle(ControlDomainNotificationCallback)>;
// 均衡器/频谱订阅工厂（任务16）：返回带 unsubscribe 的 SubscriptionHandle。
using EqualizerStateSubscriptionFactory = std::function<SubscriptionHandle(EqualizerStateSnapshotCallback)>;
using SpectrumSubscriptionFactory = std::function<SubscriptionHandle(SpectrumSnapshotCallback)>;

// ── 歌词切分：文档级约定记号（M2 契约；归属 todo 4）────────────────────────────
// 文档级切分约定的【稳定规范记号】。约定是整首文档推断出来的（D18），且它是切分
// 结果的【决定性输入】之一 —— 必须能进键、能比较、能持久化。记号必须与 Python
// 参考实现（tools/lyric_split_regression/lyric_split_tool.py 的
// `_convention_notation`）一一对应且可逆，否则跨语言闸门无法逐字节比对。
//
// ★ 记号形态（2026-09-24 独立复核 ★MED-R1 订正，实现前必读）：
//   `conventionToken` 返回的是【转义前原始记号】（raw），与 Python
//   `_convention_notation()` 的返回值逐字节相同：
//     None                 -> "-"
//     StrongSlashSpaced    -> "S: / "
//     StrongFullwidthBar   -> "S:｜"
//     StrongBar            -> "S:|"
//     StrongFullwidthSlash -> "S:／"
//     StrongSlash          -> "S:/"
//     StrongBackslash      -> "S:\"            （单反斜杠，共 3 字节）
//     WeakTab              -> "W:" + TAB 字符  （共 3 字节；**不是**字面 `\t` 两字符）
//     WeakFullwidthSpace   -> "W:　"           （全角空格 U+3000）
//     WeakSpace            -> "W: "
//     ScriptTransition     -> "SCRIPT"
//   写 TSV 第 3 列时再由与 Python `_escape_tsv` 逐字节等价的转义序列化（先 `\`→`\\`，
//   再 TAB→`\t`，再 LF→`\n`）：StrongBackslash 的 col3 是 4 字节 `S:\\`，
//   WeakTab 的 col3 是 4 字节 `W:\t`。**不要**把 token 本身定义成已转义形态 ——
//   那会让 WeakTab 序列化成 5 字节，静默偏离 Python 参考实现。
//   `S:`/`W:` 前缀区分强分隔符与弱边界（参考实现 CONVENTIONS 的顺序即优先级），
//   也让单空格与空串在持久化层可区分。
enum class LyricSplitConvention {
  None,
  StrongSlashSpaced,
  StrongFullwidthBar,
  StrongBar,
  StrongFullwidthSlash,
  StrongSlash,
  StrongBackslash,
  WeakTab,
  WeakFullwidthSpace,
  WeakSpace,
  ScriptTransition,
};

// 枚举 -> 上表的【原始记号】。未知枚举值不静默返回空串，而是抛 std::invalid_argument。
[[nodiscard]] std::string conventionToken(LyricSplitConvention convention);
// 【原始记号】-> 枚举。未知记号（含已转义的 `W:\t` 字面反斜杠+t）返回 std::nullopt。
[[nodiscard]] std::optional<LyricSplitConvention> conventionFromToken(std::string_view token);

}
