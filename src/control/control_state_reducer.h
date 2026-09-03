#pragma once

#include "playback_context_builder.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

namespace seriona::control {

class ShuffleHistory {
public:
  explicit ShuffleHistory(std::size_t maxSize = 50);
  
  void push(const TrackIdentity& track);
  std::optional<TrackIdentity> pop();
  [[nodiscard]] bool contains(const TrackIdentity& track) const;
  void clear();
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  
private:
  std::deque<TrackIdentity> history_;
  std::size_t maxSize_;
};

enum class ControlIntentKind : std::uint8_t {
  LoadTrack,
  Play,
  Pause,
  Resume,
  Stop,
  Seek,
  SetVolume,
  SetMuted,
  ResolveArtwork,
  // Appended at the end: existing enumerators keep their ordinal positions.
  ConfigureOutput,
  // 过渡参数配置意图（T1）：转发 TransitionConfig 至音频服务，不触发重载。
  SetTransitionConfig,
  // 预解码意图（T8）：控制器在 EndApproaching 时选定下一曲目标 + 交接方式，
  // 转发给音频服务 prepareNext——唯一选曲仍在控制器，仅提示不推进。
  PrepareNext,
  // 过渡中止意图（T10）：重叠窗口内失效操作/版本校验失败 → 撤第二源 + 弃预解码槽
  // + 服务侧重新武装预解码预告；随后照常执行本命令自己的意图（重调度）。
  AbortTransition,
};

struct ControlIntent {
  ControlIntentKind kind{ControlIntentKind::Play};
  std::optional<audio::TrackPlaybackRequest> track;
  std::optional<std::chrono::milliseconds> position;
  std::optional<float> volume;
  std::optional<bool> muted;
  std::optional<ArtworkResolveRequest> artworkRequest;
  // Last member on purpose: appended fields never disturb designated or
  // value-initialization of existing intents.
  std::optional<audio::AudioOutputConfig> outputConfig;
  // SetTransitionConfig 载荷（T1）。追加末尾保持序列化兼容。
  std::optional<audio::TransitionConfig> transitionConfig;
  // PrepareNext 交接方式载荷（T8）：kind（直切/交叉）+ CUE 无间隙组标记。
  // 追加末尾保持序列化兼容。
  std::optional<audio::PrepareNextMeta> prepareNextMeta;
};

struct ControlReduction {
  MediaControllerCommandResult result{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}};
  std::vector<ControlIntent> intents{};
  std::vector<ControlDomainNotification> notifications{};
  bool playerStateChanged{false};
  bool libraryStateChanged{false};
};

class ControlStateReducer {
public:
  explicit ControlStateReducer(MediaControllerOptions options = {});

  [[nodiscard]] const PlayerStateSnapshot& playerState() const noexcept;
  [[nodiscard]] const LibraryStateSnapshot& libraryState() const noexcept;
  [[nodiscard]] const std::vector<ControlDomainNotification>& recentNotifications() const noexcept;

  ControlReduction reduceCommand(const MediaControlCommand& command);
  ControlReduction reduceAudioEvent(const audio::BackendEvent& event);
  ControlReduction reduceScannerEvent(const scanner::ScannerEvent& event);
  ControlReduction reduceArtworkResolved(const ArtworkResolveResultView& result);

private:
  struct PlayableTrack {
    TrackIdentity identity{};
    audio::TrackPlaybackRequest request{};
    DisplayMetadata display{};
    std::optional<ArtworkRef> artwork{};
    std::filesystem::path artworkSourcePath;
    std::filesystem::path fallbackThumbnailPath;
    // 曲目所属容器节点（目录/专辑等）；用于“当前文件夹第一首（不含子文件夹）”回绕。
    std::optional<std::string> parentNodeId{};
  };

  struct PlaybackContextState {
    PlaybackContextDescriptor descriptor{};
    std::vector<PlayableTrack> order{};
    std::size_t index{0};
  };

  // T10：EndApproaching 时刻记录的待提交推进账本。token = 提交校验快照（记录时读取
  // 的控制状态）：queueVersion/outputMode/repeatMode/shuffle/transitionConfigVersion。
  // AdvanceCompleted 到达时 token 与当前一致且 trackId == 账本目标 → commitAdvance；
  // 否则按失效处理（abort + 重新调度 LoadTrack）。
  struct PendingAdvance {
    PlayableTrack target{};
    std::uint64_t queueVersion{0};
    audio::AudioOutputMode outputMode{audio::AudioOutputMode::Mixed};
    RepeatMode repeatMode{RepeatMode::Off};
    bool shuffle{false};
    std::uint64_t transitionConfigVersion{0};
  };

  [[nodiscard]] std::vector<PlayableTrack> playableTracks() const;
  [[nodiscard]] std::optional<PlayableTrack> firstPlayableTrack() const;
  [[nodiscard]] std::optional<PlayableTrack> findPlayableTrack(const TrackIdentity& identity) const;
  [[nodiscard]] std::optional<PlaybackContextDescriptor> defaultContextDescriptorForTrack(const TrackIdentity& identity) const;
  [[nodiscard]] std::optional<PlaybackContextState> buildPlaybackContextState(PlaybackContextDescriptor descriptor,
                                                                              PlaybackContextBuildStatus* status = nullptr) const;
  [[nodiscard]] std::optional<std::size_t> selectedContextIndex() const;
  [[nodiscard]] std::optional<PlayableTrack> selectedPlaybackContextTrack();
  [[nodiscard]] bool activateTrackWithDefaultContext(ControlReduction& reduction, const TrackIdentity& identity, bool startPlayback);
  [[nodiscard]] std::optional<PlayableTrack> nextTrack(bool forward);
  [[nodiscard]] std::optional<PlayableTrack> shuffledTrack(const std::vector<PlayableTrack>& tracks,
                                                          bool reshuffleWhenExhausted = false);
  [[nodiscard]] std::optional<PlayableTrack> previousTrack();
  [[nodiscard]] std::optional<PlayableTrack> findPlayableTrackByTrackId(const std::string& trackId) const;
  // 消费临时队列队首（跳过不可解析条目）；队列为空返回 nullopt。不改播放上下文 index。
  [[nodiscard]] std::optional<PlayableTrack> consumeQueueFront();
  // 将临时队列同步进 PlayerStateSnapshot::queueEntries（跨端契约）。
  void syncQueueSnapshot();
  // 当前选中曲目是否为播放上下文的最后一首（不含子文件夹干扰的判断由 order 决定）。
  [[nodiscard]] bool isLastTrackInContext() const;
  // 当前播放上下文容器（文件夹 / 根）直属的第一首，不含子文件夹。
  [[nodiscard]] std::optional<PlayableTrack> firstTrackOfCurrentFolder();
  [[nodiscard]] std::vector<PlayableTrack> filterOutHistory(const std::vector<PlayableTrack>& candidates) const;
  [[nodiscard]] std::chrono::milliseconds clampPosition(std::chrono::milliseconds position) const;

  ControlReduction accept();
  ControlReduction reject(MediaControllerErrorCode code, std::string message);
  void markPlayerChanged(ControlReduction& reduction, std::chrono::steady_clock::time_point sampledAt = {});
  void addNotification(ControlReduction& reduction, ControlDomainNotification notification);
  void reconcilePlaybackContextAfterSnapshot(ControlReduction& reduction);
  void selectFirstTrackWhenIdle(ControlReduction& reduction);
  void selectTrack(ControlReduction& reduction, const PlayableTrack& track, bool startPlayback);
  void stopPlayback(ControlReduction& reduction);
  ControlReduction handleConfigureOutput(ControlReduction& reduction, const MediaControlCommand& command);
  // SetTransitionConfig：校验过渡参数并生成单意图（不重载、不改快照）。
  ControlReduction handleSetTransitionConfig(ControlReduction& reduction, const MediaControlCommand& command);

  // —— T8 预解码（EndApproaching → PrepareNext，Metis 缺口 1a 选曲侧）——
  struct NaturalEndPeek {
    PlayableTrack track{};
    // 目标来源（决定 kind 决策行）：临时队列队首 vs RepeatOne 自身重播。
    bool fromTempQueue{false};
    bool repeatSelf{false};
  };

  // —— T10 pendingAdvance 账本（Metis 缺口 1b 提交侧）——
  enum class AdvanceEventSource { PlaybackEnded, AdvanceCompleted };

  // 纯只读"预览"自然播完会选中的目标：与 PlaybackEnded 推进级联逐分支同构，但
  // 不消费临时队列 / 不推进索引 / 不消耗 shuffle 随机序列（提交在任务 10 统一）。
  [[nodiscard]] std::optional<NaturalEndPeek> peekNaturalEndSelection() const;
  // 无间隙组判定：候选与当前曲同 .cue 文件（identity.filePath=scanner cue 语义）
  // 且均为 CUE 派生曲（boundedSegment）；邻接由候选=顺序下一曲天然保证。
  [[nodiscard]] bool sharesCueFileWithCurrent(const PlayableTrack& candidate) const;
  void handleEndApproaching(ControlReduction& reduction);
  // 中止在途过渡：清 pendingAdvance 账本并发出 AbortTransition 意图（撤服务侧第二源/
  // 预解码槽 + 重新武装）。窗口内失效操作（裁定基线⑦）与版本校验失败共用本路径。
  void abortPendingAdvance(ControlReduction& reduction);
  // 提交级联（PlaybackEnded 自然结束 与 AdvanceCompleted 接管 共用，行为逐分支同构）：
  // 临时队列队首 / RepeatOne / shuffle / RepeatAll / nextTrack 计算 + 索引推进 + 快照发布。
  // 差异仅在选曲应用：PlaybackEnded → selectTrack（LoadTrack+Play 意图，现行为逐事件一致）；
  // AdvanceCompleted → 曲已在服务侧加载续播，应用同字段集但不发音频意图。
  void commitAdvance(ControlReduction& reduction,
                     AdvanceEventSource source,
                     std::chrono::steady_clock::time_point sampledAt);
  // 级联选曲应用：按 source 区分 selectTrack（PBE）与无 LoadTrack 的应用（AC）。
  void applyCommittedTrack(ControlReduction& reduction, const PlayableTrack& track, AdvanceEventSource source);
  // 版本 token 是否与当前控制状态一致（record 时快照，提交时校验）。
  [[nodiscard]] bool pendingTokenMatches() const;

  PlayerStateSnapshot player_{};
  LibraryStateSnapshot library_{};
  std::optional<TrackIdentity> selectedTrack_{};
  std::optional<PlaybackContextState> playbackContext_{};
  // T8：最近一次 SetTransitionConfig 的校验通过配置（EndApproaching 决策表输入；
  // 与音频服务实际配置同源——SetTransitionConfig 单意图语义不变）。
  audio::TransitionConfig transitionConfig_{};
  // 临时播放队列（T7）：不持久化（新实例即空）；消费期间播放上下文 index 冻结，
  // 队列空后才从冻结位置推进文件夹序列。
  std::deque<QueueEntry> playbackQueue_{};
  // 当前曲目是否来自临时队列（T7）：仅此时 nextTrack 允许从冻结 index 继续；
  // 普通"上下文漂移"（选中曲目不在 order）保持旧语义（返回空，停止播放）。
  bool playingQueuedTrack_{false};
  std::optional<PlaybackStatus> visibleStateDuringSeek_{};
  std::optional<std::chrono::milliseconds> currentTrackOffset_{};
  // T10：待提交推进账本（EndApproaching 记录 → AdvanceCompleted 校验提交 / 失效操作清）。
  std::optional<PendingAdvance> pendingAdvance_{};
  // T10 N1 加固：本命令帧内 abortPendingAdvance 已压入 AbortTransition 意图的标志。
  // reject() 返回全新 reduction 会丢弃已压意图 → 载荷非法的窗内失效命令将漏撤服务侧
  // 过渡（账本已清但重叠继续跑，后续 AC 落 stale-drop）；reject() 见标志即回补意图并
  // 复位。reduceCommand 每帧开头清残余，防事件路径 abort 的置位跨命令误补。
  bool carryAbortTransitionOnReject_{false};
  // T10 版本 token 成员：临时队列/过渡配置/输出模式的变更版本（token 快照来源）。
  std::uint64_t queueVersion_{0};
  std::uint64_t transitionConfigVersion_{0};
  audio::AudioOutputMode outputMode_{audio::AudioOutputMode::Mixed};
  std::uint64_t lastAudioPlayerVersion_{0};
  std::uint64_t lastAudioServiceVersion_{0};
  std::uint64_t lastScannerVersion_{0};
  std::vector<ControlDomainNotification> recentNotifications_{};
  std::mt19937_64 shuffleRandom_;
  ShuffleHistory shuffleHistory_;
  std::size_t shuffleHistorySize_{50};
  std::uint64_t artworkGeneration_{0};
};

}
