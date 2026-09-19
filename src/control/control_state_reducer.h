#pragma once

#include "playback_context_builder.h"

#include "seriona/audio/audio_contracts.h"
#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
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
  // 均衡器参数配置意图（B1.4）：转发 EqualizerConfig 至音频服务，不触发重载。
  SetEqualizerConfig,
  // 频谱开关配置意图（R2 频谱显示链路）：转发开关目标值至音频服务原子位。
  SetSpectrumEnabled,
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
  // SetEqualizerConfig 载荷（B1.4）：均衡器参数副本。追加末尾保持序列化兼容。
  std::optional<audio::EqualizerConfig> equalizerConfig;
  // SetSpectrumEnabled 载荷（R2）：频谱开关目标值副本。追加末尾保持序列化兼容。
  std::optional<bool> spectrumEnabled;
};

struct ControlReduction {
  MediaControllerCommandResult result{.accepted = true, .code = MediaControllerErrorCode::None, .message = {}};
  std::vector<ControlIntent> intents{};
  std::vector<ControlDomainNotification> notifications{};
  bool playerStateChanged{false};
  bool libraryStateChanged{false};
  // B1.4：SetEqualizerConfig 校验通过并已入 reducer 状态（commit 侧据此读取并发布
  // EqualizerStateSnapshot——订阅推送机制见任务 19 接线，此处仅留变更信号）。
  bool equalizerStateChanged{false};
};

class ControlStateReducer {
public:
  explicit ControlStateReducer(MediaControllerOptions options = {});

  [[nodiscard]] const PlayerStateSnapshot& playerState() const noexcept;
  [[nodiscard]] const LibraryStateSnapshot& libraryState() const noexcept;
  // B1.4：均衡器生效快照（generation=0 为空快照；每次 SetEqualizerConfig 校验通过
  // 且入 state 后 generation++）。sampleRate 由音频服务生效路径以实际输出率填充。
  [[nodiscard]] const audio::EqualizerStateSnapshot& equalizerState() const noexcept;
  [[nodiscard]] const std::vector<ControlDomainNotification>& recentNotifications() const noexcept;

  ControlReduction reduceCommand(const MediaControlCommand& command);
  ControlReduction reduceAudioEvent(const audio::BackendEvent& event);
  ControlReduction reduceScannerEvent(const scanner::ScannerEvent& event);
  ControlReduction reduceArtworkResolved(const ArtworkResolveResultView& result);

  // 文件夹排序规则变更事件（MediaController 在持久化成功后投递的纯数据事件，不做 I/O。
  // 排序命令本身由 service 层落库，reducer 只负责"让播放顺序跟随排序"这一策略）：
  // 与当前播放上下文的 (rootPath, folderNodeId) 匹配时更新 descriptor.sortRules 并重建
  // order + 按身份重锚，使"下一首"立即跟随新排序；不匹配则零影响。
  ControlReduction applyContextSortRules(std::filesystem::path rootPath,
                                         std::string folderNodeId,
                                         std::vector<FolderSortRule> rules);
  // 启动时批量注入已持久化的规则（这些规则由先前的 ApplyFolderSortRules 落库，本次进程
  // 尚无对应命令）：填充 activeSortRules_ 并对当前树应用一次，使重启后仍按用户设置的顺序播放。
  ControlReduction injectSavedContextSortRules(std::vector<FolderSortSetting> settings);

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
    // 账本目标来自临时队列队首：AdvanceCompleted 提交时需消费该条目（追加末尾，
    // 不扰动既有指定初始化的字段顺序）。
    bool fromTempQueue{false};
  };

  [[nodiscard]] std::vector<PlayableTrack> playableTracks() const;
  [[nodiscard]] std::optional<PlayableTrack> firstPlayableTrack() const;
  [[nodiscard]] std::optional<PlayableTrack> findPlayableTrack(const TrackIdentity& identity) const;
  [[nodiscard]] std::optional<PlaybackContextDescriptor> defaultContextDescriptorForTrack(const TrackIdentity& identity) const;
  // 生效中的文件夹排序规则（key = rootPath + '\n' + folderNodeId）：由
  // StartPlaybackFromContext（命令携带规则）与 applyContextSortRules（排序变更）写入；
  // 兜底上下文构建据此携带规则，避免 Play/TogglePlayPause/SelectTrack 退化为 DFS 树序。
  [[nodiscard]] static std::string sortRulesKey(const std::filesystem::path& rootPath,
                                                const std::string& folderNodeId);
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
  // INV-COMMIT 采纳路径：按 trackId 解析接管曲目（先查播放上下文 order 以保留完整
  // display/artwork/request，再回落曲库）。曲目已不在曲库时返回 nullopt，调用方停止播放。
  [[nodiscard]] std::optional<PlayableTrack> resolveTrackForAdoption(const std::string& trackId) const;
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
  // 播放上下文 order 的唯一重建入口：按身份重锚当前曲（身份消失时不按旧位置取值）、
  // 递增 orderGeneration_、并调用 onSuccessorRelationChanged。
  void rebuildOrder(ControlReduction& reduction);
  // 决策⑦：排序施加在控制层持有的树副本（library_.libraryTree）上，使前端（消费该快照渲染）
  // 与播放（对该树做前序 DFS）读到同一顺序，「可见序 == 播放序」由构造保证。
  // 仅对 activeSortRules_ 中有规则的文件夹排序其 childNodeIds，其余保持树序（决策⑥）。
  void applyTreeSortOrder(ControlReduction& reduction);
  // 后继关系可能已改变：已武装的预解码目标若不再是"当前后继"，撤账本 + AbortTransition，
  // 避免"音频预解码 X、账本却是 Y"的分叉。
  void onSuccessorRelationChanged(ControlReduction& reduction);
  void selectFirstTrackWhenIdle(ControlReduction& reduction);
  void selectTrack(ControlReduction& reduction, const PlayableTrack& track, bool startPlayback);
  void stopPlayback(ControlReduction& reduction);
  ControlReduction handleConfigureOutput(ControlReduction& reduction, const MediaControlCommand& command);
  // SetTransitionConfig：校验过渡参数并生成单意图（不重载、不改快照）。
  ControlReduction handleSetTransitionConfig(ControlReduction& reduction, const MediaControlCommand& command);
  // B1.4 SetEqualizerConfig：校验均衡器参数 → 单意图（不重载）→ 快照入状态。
  ControlReduction handleSetEqualizerConfig(ControlReduction& reduction, const MediaControlCommand& command);
  // R2 SetSpectrumEnabled：校验载荷 → 单意图转发（纯门控，无 reducer 镜像）。
  ControlReduction handleSetSpectrumEnabled(ControlReduction& reduction, const MediaControlCommand& command);

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
  // B1.4：均衡器生效快照（generation=0 空快照 → 每次校验通过的 SetEqualizerConfig
  // 重建并 ++generation；curvePoints/curveFrequencies 由 reducer 纯函数按 20–20k
  // 181 点填充，sampleRate 在 reducer 侧恒 0=未定，由音频服务生效路径回填实际率）。
  audio::EqualizerStateSnapshot equalizer_{};
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
  // 播放上下文 order 的世代：每次 rebuildOrder 递增（仅用于诊断与"顺序已变"的判定）。
  std::uint64_t orderGeneration_{0};
  std::map<std::string, std::vector<FolderSortRule>> activeSortRules_{};
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
