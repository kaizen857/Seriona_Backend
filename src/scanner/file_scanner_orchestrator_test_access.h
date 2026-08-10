#pragma once

#include "file_scanner_service_internal.h"
#include "scanner_internal_types.h"

#include "seriona/scanner/cache/sqlite_cache.h"
#include "seriona/scanner/hash_utils.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace seriona::scanner {

// Test-only access point for observing internal pre-allocated node sequence.
// This header should ONLY be included by tests, never by production code.
//
// Usage: Set the callback via setPreallocationObserver() before scan(),
// and it will be invoked with the indexedSongs vector after pre-allocation
// but before Worker filling.

struct PreallocationSnapshot {
  std::size_t totalNodes{0};
  std::vector<NodeType> nodeTypes;
  std::vector<std::optional<CueInfo>> cueInfos;
};

using PreallocationObserver = std::function<void(const std::vector<IndexedPublishedSong>&)>;

void setPreallocationObserver(PreallocationObserver observer);
void clearPreallocationObserver();

struct WorkerTaskSnapshot {
  std::filesystem::path filePath;
  ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
  bool hasCachedLocation{false};
  std::size_t nodeIndex{0};
};

using WorkerTaskObserver = std::function<void(const std::vector<WorkerTaskSnapshot>&)>;

void setWorkerTaskObserver(WorkerTaskObserver observer);
void clearWorkerTaskObserver();

struct PublishedSongSnapshot {
  std::filesystem::path filePath;
  std::filesystem::path treeRelativePath;
  ScanItemOrigin origin{ScanItemOrigin::ScannedFull};
  std::optional<std::string> locationId;
};

using PublishedSongObserver = std::function<void(const std::vector<PublishedSongSnapshot>&)>;

void setPublishedSongObserver(PublishedSongObserver observer);
void clearPublishedSongObserver();

// Test-only CUE sheet provider: allows tests to inject controlled track data
// without requiring parseable audio files.
//
// Usage: Call setTestCueSheetProvider() with a function that returns track metadata
// for specific .cue paths. Production code (readCueSheet) checks this provider first;
// if set and returns non-empty, uses that; otherwise falls back to real TagReader.
//
// Default: nullptr (production behavior - always calls TagReader).

struct TestCueTrackData {
  std::filesystem::path audioFilePath;
  std::int64_t offset{0};
  std::int64_t duration{0};
  std::string title;
  std::string artist;
  std::string album;
  std::uint32_t trackNumber{0};
};

using TestCueSheetProvider = std::function<std::vector<TestCueTrackData>(const std::filesystem::path& cuePath)>;

void setTestCueSheetProvider(TestCueSheetProvider provider);
void clearTestCueSheetProvider();

using LrcParseObserver = std::function<void(const std::filesystem::path& path)>;

void setLrcParseObserver(LrcParseObserver observer);
void clearLrcParseObserver();

using TestLyricsSidecarHashProvider = std::function<FileHashResult(const std::filesystem::path& path,
                                                                   const HashOptions& options)>;

void setTestLyricsSidecarHashProvider(TestLyricsSidecarHashProvider provider);
void clearTestLyricsSidecarHashProvider();

struct IncrementalPlanSnapshot {
  std::vector<std::string> retainedLocationIds;
  std::vector<cache::LyricsCacheUpdate> lyricsOnlyUpdates;
};

using IncrementalPlanObserver = std::function<void(const IncrementalPlanSnapshot& snapshot)>;

void setIncrementalPlanObserver(IncrementalPlanObserver observer);
void clearIncrementalPlanObserver();

using CacheWriteObserver = std::function<void(const cache::ScanRootCacheWrite& write)>;

void setCacheWriteObserver(CacheWriteObserver observer);
void clearCacheWriteObserver();

// Test-only observer for the watcher event queue (波 1.3：完整 WatchEvent 入队管道)。
// 每次 enqueueWatcherEvent 处理完一个 actionable 事件后触发，报告：
//  - event: 入队到 pendingWatcherEvents 的完整事件（含 associated），未入队则为 nullopt
//  - dirtyGeneration: 本次入队后的代际号
//  - eventQueueSize / messageQueueSize: 事件/消息两个队列的当前深度（分队列不混淆）
//  - fallbackRescan: 事件队列超限置位的"需回落全根重扫"标记

struct WatcherEventQueueSnapshot {
  std::optional<WatchEvent> event;
  std::uint64_t dirtyGeneration{0};
  std::size_t eventQueueSize{0};
  std::size_t messageQueueSize{0};
  bool fallbackRescan{false};
};

using WatcherEventQueueObserver = std::function<void(const WatcherEventQueueSnapshot&)>;

void setWatcherEventQueueObserver(WatcherEventQueueObserver observer);
void clearWatcherEventQueueObserver();

// Test-only observer for the long-lived tree builder member (波 3a：成员生命周期+种子化)。
// runScan 完成全量建树并把结果接管为成员（treeBuilder_）后触发，报告：
//  - seeded: treeBuilder_ 成员非空（首次扫描种子化成功）
//  - generation: 成员被（重）建并替换的次数（回落重扫 +1；成员替换原子、不累积）
//  - songCount: 成员当前树中的歌曲数（应与快照歌曲数一致）

struct TreeBuilderSeededSnapshot {
  bool seeded{false};
  std::size_t generation{0};
  std::uint64_t songCount{0};
};

using TreeBuilderObserver = std::function<void(const TreeBuilderSeededSnapshot&)>;

void setTreeBuilderObserver(TreeBuilderObserver observer);
void clearTreeBuilderObserver();

} // namespace seriona::scanner
