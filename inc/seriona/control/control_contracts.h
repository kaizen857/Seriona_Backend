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
class LyricSplitStore;

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
  // 歌词切分条目存储（内容寻址 auto/manual 表）；null 时读写安全降级。
  std::shared_ptr<LyricSplitStore> lyricSplitStore;
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
  // 歌词切分命令（W2）：①改目标语言（载荷=lyricsTargetLanguage，取值限
  // zh/ja/ko/en，非法值走命令拒绝路径）；②③对当前曲目单行手工纠错做增/删
  // （载荷=lyricRawText + 原文译文 + lyricConvention，缺约定字段即拒绝）；
  // ④清空手工纠错。四条均为控制层实现，追加末尾保持序列化兼容。
  SetLyricsTargetLanguage,
  UpsertLyricSplitCorrection,
  RemoveLyricSplitCorrection,
  // 全库语义（清空全部手工纠错），仅供 store 层单测使用，不供前端调用；前端的
  // 「恢复自动识别」按当前曲目逐行走本组的增/删命令。追加末尾保持兼容。
  ClearLyricSplitCorrections,
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
  // 歌词切分命令载荷（W2）。全部追加末尾保持序列化兼容。
  // 改目标语言的目标值（zh/ja/ko/en）。
  std::optional<std::string> lyricsTargetLanguage;
  // 单行手工纠错的键 = 清洗行（cleanLine 之后），取自 TrackLyricsSnapshot.lines[].text。
  std::optional<std::string> lyricRawText;
  // 纠错后的原文/译文。
  std::optional<std::string> lyricOriginal;
  std::optional<std::string> lyricTranslation;
  // 本曲目的文档级约定（前端唯一来源 = TrackLyricsSnapshot.convention）。它是键的一部分，
  // 缺该字段即拒绝本命令——绝不回退为控制层自行推断的约定。
  std::optional<std::string> lyricConvention;
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

// ── 歌词切分条目（内容寻址 store 的持久化单元）────────────────────────────────
// 追加在文件末尾的独立区块：既有 ordinal 与既有 payload 一律不变
// （该头既有约定见上方 MediaControlCommand 的「Last member on purpose」注释）。
// 自动结果与手工纠错【同型】，由 `source` 区分；键 = (rawText, targetLanguage,
// convention, source) 四维（约定是必填维度，见 M2 实测）。

enum class LyricSplitSource { Auto, Manual };

struct LyricSplitEntry {           // 自动结果与手工纠错【同型】
  std::string rawText;             // ★= 清洗行（cleanLine 之后）；内容寻址的键 + 管理列表展示
  std::string targetLanguage;      // 'zh' / 'ja' / 'ko' / 'en'
  LyricSplitConvention convention{LyricSplitConvention::None};  // ★ 文档级约定，必须在键内
  std::string original;
  std::string translation;         // 空串 = 显式「此行无译文」
  LyricSplitSource source{LyricSplitSource::Auto};
  std::string algoVersion;         // 见下「算法版本」；auto 行必填，manual 行空
  // 该条目最后一次被写入的时间。**由 store 在 `putAuto`/`upsertManual` 内写 `system_clock::now()`**。
  // 仅用于诊断/未来淘汰策略，**不参与键、不参与 `load` 的命中判定**（键仍是四维 + `algo_version`）。
  std::chrono::system_clock::time_point updatedAt{};
};

// ── 当前曲目切分歌词（独立快照 + 订阅）────────────────────────────────────────
// 追加在文件末尾的独立区块：既有 ordinal 与既有 payload 一律不变。

// 译文位置策略：R7/D6 的内部扩展点。当前仅一种实现，不暴露为用户设置。
enum class TranslationPosition { LastLanguageSegment };

// 单行切分结果。刻意【不】复用 scanner::LyricLine：库树不切分（D21），
// 往共享契约上加字段只会让全库每行白背 4 个空串并破坏无关消费者。
struct SplitLyricLine {
  std::chrono::milliseconds timestamp{0};
  std::string text;          // ★= rawText 口径：cleanLine 之后的【清洗行】（不是文件原始行、不含时间戳/元数据）
  std::string original;      // 原文；未切分时 == text
  std::string translation;   // 译文；空 = 无
  bool split{false};         // 是否成功切分
  bool manualOverride{false};// 本行当前是否命中 manual 行（用户手工纠错）
  // 被 manual 覆盖【之前】的自动判定结果 —— 仅当 `manualOverride == true` 时有意义，
  // 供行级菜单的「查看本行自动判定」展示（D14 菜单第 4 项）；未覆盖时二者为空。
  // 【零额外成本】：todo 26 的流程本就在切分/查表阶段先得到 auto 结果，此处只是把它留下。
  std::string autoOriginal;
  std::string autoTranslation;
  // 【纠错管理列表的唯一来源】：列表 = 本快照中 `manualOverride == true` 的行。
  // 不新增「全库读命令」，前端也不直接调用 store（`Seriona/AGENTS.md:36` 禁前端直连 DB）。
  // 注意：这里【不】放 confidence。置信度只在算法层以字符串记号表示（LyricSplitResult.confidence）
  // 并仅用于路由判定；快照不携带它（D24：不向用户暴露置信度）。
};

struct TrackLyricsSnapshot {
  // 每次发布本快照都递增 `version` 并把 `sampledAt` 置为发布时间（同 `PlayerStateSnapshot` 的既有语义）。
  // **不得**留空不动：恒为 `{0, {}}` 会让前端的刷新判定永远失效。
  SnapshotFreshness freshness{};
  std::string trackId;
  std::string targetLanguage;   // 产出本结果时所用目标语言
  // 本曲目的【文档级约定】（D18：整首推断一次）。约定是 storage key 的组成部分，
  // 因此前端做单条 Upsert/Remove 时必须回传它 —— 这是本快照里约定的唯一来源
  // （前端不再有 `listManual()` 通路）。
  LyricSplitConvention convention{LyricSplitConvention::None};
  TranslationPosition position{TranslationPosition::LastLanguageSegment};
  std::vector<SplitLyricLine> lines;
};

// 当前曲目切分歌词的订阅面（M2 契约；归属 todo 27）。
// 追加在本文件末尾独立区块：既有声明与既有 ordinal 一律不变。
using TrackLyricsSnapshotCallback = std::function<void(TrackLyricsSnapshot)>;
using TrackLyricsSubscriptionCallback = TrackLyricsSnapshotCallback;
using TrackLyricsSubscriptionFactory = std::function<SubscriptionHandle(TrackLyricsSnapshotCallback)>;

}
