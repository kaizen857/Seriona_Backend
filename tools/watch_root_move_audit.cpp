// 目录移出监视根审计程序（watch_root_move_audit）
//
// 独立审计工具：只链接 seriona_scanner，使用真实 FileScannerService
// （注入式工厂 makeFileScannerService + 生产回填的 WtrFolderWatcherFactory，
// 即真实的 wtr::watch 监视器），在临时目录做文件系统操作对照实验，
// 验证"事件驱动精准增量（方案 B）"的核心假设：
//   - 目录 mv 出监视根 → IN_MOVE_SELF 触发精准删除（树 removeSubtree +
//     SQLite deleteByPathPrefix），快照收敛为 0 首，scan 不增长；
//   - 文件 create/modify/delete、根内 rename → 精准 upsert/renameSubtree，
//     scan 不增长（不触发 ScanStarted）；
//   - mv 出根后的残留 watch 幽灵事件（向移出目录写入）经 exists 守卫丢弃，
//     不产生幽灵条目、旧路径无残留。
//
// 事件流事实依据（file_scanner_orchestrator.cpp）：
//   - 每次扫描（无论手动 scan 还是 watcher 触发的重扫）都会先发布
//     ScannerEventType::ScanStarted（:1052），因此 ScanStarted 事件计数
//     增量是最可靠的重扫信号；方案 B 下精准更新（upsert/rename/删除）
//     不触发 ScanStarted。
//   - watcher 事件经 debounce（默认 50ms）后进入分类器，分类器对
//     create/modify 先做 exists 守卫（事件路径不在磁盘 → 幽灵事件丢弃），
//     精准更新后发布完整快照（version 递增）——快照 version 变化是
//     "精准更新落地"的信号（相对 ScanStarted 增量的辅助指标）。
//   - 注意：新建目录并写入文件（如场景 4/6/9/10 的 setup）会触发"移入含
//     未扫描文件的新目录"回落全根重扫，可能在场景窗口内多 1 次 ScanStarted；
//     判定前需用基线稳定沉降把 setup 回落计入 before，使 mv/精准操作窗口
//     的 ScanStarted 增量纯净。
//
// 构建（Seriona_Backend 根目录）：
//   cmake -S . -B build -DSERIONA_BUILD_TOOLS=ON -DSERIONA_BUILD_TESTS=ON
//   cmake --build build --target seriona_watch_root_move_audit -j4
//
// 运行（场景运行与分析由独立任务负责，本程序不做运行验证）：
//   ./build/tools/seriona_watch_root_move_audit

#include "seriona/scanner/scanner_contracts.h"
#include "seriona/scanner/file_scanner_service.h"

#include "scanner/file_scanner_service_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
namespace sc = seriona::scanner;

using namespace std::chrono_literals;

// ============================================================================
// 常量
// ============================================================================

constexpr std::chrono::milliseconds kPollInterval{10};   // 等待辅助轮询间隔
constexpr std::chrono::milliseconds kSceneTimeout{10s};  // 场景等待超时
constexpr std::chrono::milliseconds kObserveWindow{2s};  // 观察窗口（> debounce 50ms）
constexpr std::chrono::milliseconds kPostMoveSettle{200ms};  // mv 后沉降（> debounce）
constexpr std::chrono::milliseconds kBaselineSettle{1200ms};  // 基线稳定沉降：把 setup（新建目录+写文件）触发的回落重扫计入 before（实测可延后约 560ms），使 mv/精准操作窗口的 ScanStarted 增量纯净
constexpr std::chrono::milliseconds kSilentSettle{300ms};  // 场景 10 mv 后沉降（> debounce 50ms + 扫描余量）
constexpr std::chrono::milliseconds kSilentObserveWindow{3s};  // 场景 10 观察窗口（期间零文件系统操作）

// ============================================================================
// 音频 fixture：最小可解析 WAV（16-bit PCM 单声道 44.1kHz，0.2s 静音）
// 移植自 tests/audio/audio_fixture_tests.cpp 的写 WAV 思路，可直接被
// TagReader/FFmpeg 解析（注意小端字节序）。
// ============================================================================

void writeMinimalWav(const fs::path& path) {
  fs::create_directories(path.parent_path());
  constexpr std::uint16_t kChannels = 1, kBits = 16;
  constexpr std::uint32_t kSampleRate = 44100, kSamples = kSampleRate / 5;
  constexpr std::uint32_t kDataBytes = kSamples * kChannels * (kBits / 8);
  std::ofstream out{path, std::ios::binary};
  out.write("RIFF", 4);
  const std::uint32_t riffSize = kDataBytes + 36;
  out.write(reinterpret_cast<const char*>(&riffSize), 4);
  out.write("WAVEfmt ", 8);
  const std::uint32_t fmtSize = 16;
  const std::uint16_t audioFmt = 1;
  out.write(reinterpret_cast<const char*>(&fmtSize), 4);
  out.write(reinterpret_cast<const char*>(&audioFmt), 2);
  out.write(reinterpret_cast<const char*>(&kChannels), 2);
  out.write(reinterpret_cast<const char*>(&kSampleRate), 4);
  const std::uint32_t byteRate = kSampleRate * kChannels * (kBits / 8);
  out.write(reinterpret_cast<const char*>(&byteRate), 4);
  const std::uint16_t blockAlign = kChannels * (kBits / 8);
  out.write(reinterpret_cast<const char*>(&blockAlign), 2);
  out.write(reinterpret_cast<const char*>(&kBits), 2);
  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&kDataBytes), 4);
  for (std::uint32_t i = 0; i != kSamples; ++i) {
    const std::uint16_t zero = 0;
    out.write(reinterpret_cast<const char*>(&zero), 2);
  }
}

// ============================================================================
// 等待辅助：轮询式等待（参照 scanner_watcher_tests.cpp 的 100×5ms 范式）
// ============================================================================

template <typename Pred>
bool waitUntil(Pred predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return predicate();
}

// ============================================================================
// 场景判定（场景 7/8/9-mv 专用：mv 出根 + 根外继续写的收敛语义）
// ============================================================================

enum class Verdict {
  PreciseConverged,   // scan 不增长 + 快照收敛（0 首）+ 旧路径无残留：精准删除 + 幽灵事件丢弃
  ScanGrew,           // 窗口内 ScanStarted 增长：mv/根外写入触发了重扫，方案 B 精准路径不应扫描
  ResidualOrGhost,    // 快照未收敛：残留歌曲或幽灵条目
  Unknown,            // 观察数据自相矛盾
};

std::string_view verdictName(Verdict v) {
  switch (v) {
    case Verdict::PreciseConverged:
      return "PASS（精准删除/收敛 + 幽灵事件丢弃，scan 不增长）";
    case Verdict::ScanGrew:
      return "FAIL（mv/根外写入触发重扫，方案 B 精准路径不应扫描）";
    case Verdict::ResidualOrGhost:
      return "FAIL（快照未收敛/残留：幽灵条目或精准删除未生效）";
    case Verdict::Unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

// ============================================================================
// 事件记录器：sink 在 scanner 工作线程上调用，必须加锁
// ============================================================================

class EventLog {
public:
  // 事件类型数量（ScannerEventType 0..6）
  static constexpr std::size_t kTypeCount = 7;

  void record(const sc::ScannerEvent& ev) {
    std::scoped_lock lock{mutex_};
    const auto idx = static_cast<std::size_t>(ev.type);
    if (idx < counts_.size()) {
      ++counts_[idx];
    }
    if (ev.type == sc::ScannerEventType::ScanStarted) {
      ++scanStartedCount_;
    } else if (ev.type == sc::ScannerEventType::PlaylistSnapshotUpdated) {
      if (const auto* snap = std::get_if<sc::PlaylistTreeSnapshot>(&ev.payload)) {
        lastSnapshotEventMonotonic_ = ev.monotonicVersion;
        lastSnapshotVersion_ = snap->version;
        lastSnapshotTracks_ = static_cast<std::size_t>(std::count_if(
            snap->nodes.begin(), snap->nodes.end(),
            [](const sc::PlaylistNode& node) { return node.song.has_value(); }));
      }
    } else if (ev.type == sc::ScannerEventType::FileScanned) {
      if (const auto* song = std::get_if<sc::SongMetadata>(&ev.payload)) {
        recentScanned_.push_back(song->filePath.generic_string());
        if (recentScanned_.size() > 16U) {
          recentScanned_.erase(recentScanned_.begin());
        }
      }
    } else if (ev.type == sc::ScannerEventType::ScanError) {
      if (const auto* err = std::get_if<sc::ScannerError>(&ev.payload)) {
        std::string text = "code=" + std::to_string(static_cast<int>(err->code)) +
                           " message=" + err->message;
        if (!err->detail.empty()) {
          text += " detail=" + err->detail;
        }
        if (err->path.has_value()) {
          text += " path=" + err->path->generic_string();
        }
        errors_.push_back(std::move(text));
      }
    }
  }

  std::uint64_t countOf(sc::ScannerEventType type) const {
    std::scoped_lock lock{mutex_};
    const auto idx = static_cast<std::size_t>(type);
    return idx < counts_.size() ? counts_[idx] : 0;
  }

  std::uint64_t scanStartedCount() const {
    std::scoped_lock lock{mutex_};
    return scanStartedCount_;
  }

  std::uint64_t lastSnapshotVersion() const {
    std::scoped_lock lock{mutex_};
    return lastSnapshotVersion_;
  }

  std::uint64_t lastSnapshotEventMonotonic() const {
    std::scoped_lock lock{mutex_};
    return lastSnapshotEventMonotonic_;
  }

  std::size_t lastSnapshotTracks() const {
    std::scoped_lock lock{mutex_};
    return lastSnapshotTracks_;
  }

  std::vector<std::string> recentScanned() const {
    std::scoped_lock lock{mutex_};
    return recentScanned_;
  }

  std::vector<std::string> errors() const {
    std::scoped_lock lock{mutex_};
    return errors_;
  }

  // 观测基线：记录此刻的计数快照，供场景判定计算增量
  struct Baseline {
    std::array<std::uint64_t, kTypeCount> counts{};
    std::uint64_t scanStarted{0};
    std::uint64_t snapshotEventMonotonic{0};
    std::uint64_t snapshotVersion{0};
    std::size_t tracks{0};
  };

  Baseline baseline() const {
    std::scoped_lock lock{mutex_};
    Baseline b;
    b.counts = counts_;
    b.scanStarted = scanStartedCount_;
    b.snapshotEventMonotonic = lastSnapshotEventMonotonic_;
    b.snapshotVersion = lastSnapshotVersion_;
    b.tracks = lastSnapshotTracks_;
    return b;
  }

  // 由基线计算窗口内增量（比总计数更抗场景间串扰）
  static std::uint64_t deltaScanStarted(const Baseline& before, const Baseline& after) {
    return after.scanStarted - before.scanStarted;
  }

  static std::uint64_t deltaSnapshotEvent(const Baseline& before, const Baseline& after) {
    return after.snapshotEventMonotonic > before.snapshotEventMonotonic
               ? (after.snapshotEventMonotonic - before.snapshotEventMonotonic)
               : 0;
  }

  static std::uint64_t deltaCount(const Baseline& before, const Baseline& after,
                                  sc::ScannerEventType type) {
    const auto idx = static_cast<std::size_t>(type);
    return after.counts[idx] > before.counts[idx] ? after.counts[idx] - before.counts[idx] : 0;
  }

private:
  mutable std::mutex mutex_;
  std::array<std::uint64_t, kTypeCount> counts_{};
  std::uint64_t scanStartedCount_{0};
  std::uint64_t lastSnapshotEventMonotonic_{0};
  std::uint64_t lastSnapshotVersion_{0};
  std::size_t lastSnapshotTracks_{0};
  std::vector<std::string> recentScanned_;
  std::vector<std::string> errors_;
};

// ============================================================================
// 场景报告辅助
// ============================================================================

struct SceneReport {
  std::string name;
  std::string action;
  std::string baselineLine;
  std::string deltaLine;
  std::string resultLine;
  std::string verdictLine;
};

void printSceneHeader(int index, std::string_view title) {
  std::cout << "\n[场景 " << index << "] " << title << "\n";
}

void printSceneData(const SceneReport& report) {
  std::cout << "  操作    : " << report.action << "\n";
  std::cout << "  基线    : " << report.baselineLine << "\n";
  if (!report.deltaLine.empty()) {
    std::cout << "  窗口内  : " << report.deltaLine << "\n";
  }
  std::cout << "  结果    : " << report.resultLine << "\n";
  std::cout << "  判定    : " << report.verdictLine << "\n";
}

// 移动出根场景的判定与报告（场景 7/8/9-mv 共用）
// 判定语义（方案 B）：
//   - ScanStarted 增量 == 0 且快照收敛（歌曲数 0 + 移出路径无残留）-> PASS：
//     IN_MOVE_SELF 精准删除 + 残留 watch 幽灵事件（根外写入）被 exists 守卫丢弃
//   - ScanStarted 增量 > 0 -> FAIL：mv/根外写入触发了重扫（精准路径不应扫描）
//   - 快照未收敛（残留歌曲或幽灵条目）-> FAIL
void reportMoveOutScene(int index, std::string_view title,
                        const EventLog::Baseline& before, const EventLog::Baseline& after,
                        std::string_view action, sc::FileScannerService* service,
                        std::string_view residualPrefix) {
  printSceneHeader(index, title);

  const auto scanStartedDelta = EventLog::deltaScanStarted(before, after);
  const auto snapshotEventDelta = EventLog::deltaSnapshotEvent(before, after);
  const auto fileScannedDelta = EventLog::deltaCount(before, after, sc::ScannerEventType::FileScanned);

  std::size_t residualCount = 0;
  const auto snapshot = service->snapshot();
  for (const auto& node : snapshot.nodes) {
    if (node.song.has_value() &&
        node.song->filePath.generic_string().find(residualPrefix) != std::string::npos) {
      ++residualCount;
    }
  }

  const bool converged = (after.tracks == 0) && (residualCount == 0);
  Verdict verdict = Verdict::Unknown;
  if (scanStartedDelta > 0) {
    verdict = Verdict::ScanGrew;
  } else if (!converged) {
    verdict = Verdict::ResidualOrGhost;
  } else {
    verdict = Verdict::PreciseConverged;
  }

  SceneReport report;
  report.name = std::string(title);
  report.action = std::string(action);
  report.baselineLine =
      "ScanStarted=" + std::to_string(before.scanStarted) +
      " 快照事件单调=" + std::to_string(before.snapshotEventMonotonic) +
      " 快照version=" + std::to_string(before.snapshotVersion) +
      " 歌曲数=" + std::to_string(before.tracks);
  report.deltaLine =
      "ScanStarted增量=" + std::to_string(scanStartedDelta) +
      " 快照事件增量=" + std::to_string(snapshotEventDelta) +
      " FileScanned增量=" + std::to_string(fileScannedDelta);
  report.resultLine =
      "快照version=" + std::to_string(after.snapshotVersion) +
      " 歌曲数=" + std::to_string(after.tracks) +
      " 残留路径=" + std::to_string(residualCount) +
      (converged ? "（收敛）" : "（未收敛）");
  report.verdictLine = std::string(verdictName(verdict));

  printSceneData(report);
}

// ============================================================================
// 主流程
// ============================================================================

int main() {
  std::cout << "===== Seriona Watch Root Move Audit =====\n";
  std::cout << "验证假设（方案 B）：目录 mv 出监视根 -> IN_MOVE_SELF 精准删除（快照收敛、scan 不增长）；\n";
  std::cout << "                文件 create/modify/delete、根内 rename -> 精准更新（scan 不增长）；\n";
  std::cout << "                残留 watch 幽灵事件被丢弃（无幽灵条目、旧路径无残留）。\n";

  // ---------- 公共 setup ----------
  const fs::path tempRoot =
      fs::temp_directory_path() / ("seriona-watch-audit-" + std::to_string(::getpid()));
  const fs::path musicRoot = tempRoot / "music";
  const fs::path musicB = tempRoot / "musicB";
  const fs::path movedOut = tempRoot / "moved-out";
  const fs::path movedB = tempRoot / "musicB-moved";

  std::error_code setupEc;
  fs::remove_all(tempRoot, setupEc);
  fs::create_directories(musicRoot, setupEc);

  sc::FileScannerServiceDependencies deps;
  deps.databasePath = tempRoot / "library.sqlite";
  deps.coverExportDir = tempRoot / "artwork";
  // metadataReader / watcherFactory / folderThumbnailSeam 留空：
  // 生产实现自动回填 ProductionTagMetadataReader 与真实 WtrFolderWatcherFactory
  // （file_scanner_orchestrator.cpp:967-983），这正是本审计要用的真实监视器。

  EventLog log;
  auto service = sc::makeFileScannerService(deps);

  // 启动顺序与 MediaController::scanLibrary 一致（media_controller.cpp:209-227）：
  // 先 scan 再 startWatching。
  service->setEventSink([&log](const sc::ScannerEvent& ev) { log.record(ev); });

  sc::ScannerConfig config;
  config.progressInterval = 50ms;
  service->configure(config);

  const std::vector<sc::ScannerRoot> roots{{sc::ScannerRoot{musicRoot, true}}};
  service->scan(roots, sc::ScanMode::Full);
  service->startWatching(roots);

  const bool initialReady =
      waitUntil([&log] { return log.countOf(sc::ScannerEventType::ScanCompleted) >= 1; },
                kSceneTimeout);
  std::cout << "\n[setup] 临时根=" << tempRoot.generic_string()
            << " 初始扫描完成=" << (initialReady ? "是" : "否（超时）")
            << " 初始歌曲数=" << log.lastSnapshotTracks() << "\n";
  if (!initialReady) {
    std::cerr << "致命错误：初始扫描未在超时内完成，无法继续。\n";
    service->stopWatching();
    service->stop();
    return 2;
  }

  // ---------- 场景 1：文件 create（对照组） ----------
  {
    const auto before = log.baseline();
    printSceneHeader(1, "文件 create（对照组）");
    const fs::path aWav = musicRoot / "a.wav";
    writeMinimalWav(aWav);
    const bool reached =
        waitUntil([&log, &before] { return log.lastSnapshotTracks() > before.tracks; },
                  kSceneTimeout);
    const auto after = log.baseline();
    SceneReport report;
    report.action = "根内写 a.wav；等待快照歌曲数 +1";
    report.baselineLine = "ScanStarted=" + std::to_string(before.scanStarted) +
                          " 歌曲数=" + std::to_string(before.tracks);
    report.deltaLine =
        "ScanStarted增量=" + std::to_string(EventLog::deltaScanStarted(before, after)) +
        " FileScanned增量=" + std::to_string(EventLog::deltaCount(before, after, sc::ScannerEventType::FileScanned));
    report.resultLine = "歌曲数=" + std::to_string(after.tracks) +
                        (reached ? "（+1 达成）" : "（未达成）");
    report.verdictLine = reached ? "PASS（对照组：create 触发重扫，快照更新）" : "FAIL（对照组异常）";
    printSceneData(report);
  }

  // ---------- 场景 2：文件 modify（对照组） ----------
  {
    const auto before = log.baseline();
    printSceneHeader(2, "文件 modify（对照组）");
    // mtime guard：先等 5ms 再改写，保证 mtime 变化可被识别
    std::this_thread::sleep_for(5ms);
    writeMinimalWav(musicRoot / "a.wav");
    // 方案 B：modify -> upsertSong 精准更新（不触发 ScanStarted）。
    // 等待"更新落地"信号：快照 version 递增（精准更新发布）或意外重扫或歌曲数变化。
    const bool updateLanded =
        waitUntil([&log, &before] {
                    return log.scanStartedCount() > before.scanStarted ||
                           log.lastSnapshotVersion() != before.snapshotVersion ||
                           log.lastSnapshotTracks() != before.tracks;
                  },
                  kSceneTimeout);
    const auto after = log.baseline();
    const auto scanStartedDelta = EventLog::deltaScanStarted(before, after);
    const auto versionBumped = (after.snapshotVersion != before.snapshotVersion);
    const auto tracksUnchanged = (after.tracks == before.tracks);
    SceneReport report;
    report.action = "改写 a.wav（mtime guard 5ms 后）；等待精准更新落地";
    report.baselineLine = "ScanStarted=" + std::to_string(before.scanStarted) +
                          " 快照version=" + std::to_string(before.snapshotVersion) +
                          " 歌曲数=" + std::to_string(before.tracks);
    report.deltaLine = "ScanStarted增量=" + std::to_string(scanStartedDelta) +
                       " FileScanned增量=" +
                       std::to_string(EventLog::deltaCount(before, after, sc::ScannerEventType::FileScanned));
    report.resultLine = "快照version=" + std::to_string(after.snapshotVersion) +
                        " 歌曲数=" + std::to_string(after.tracks) +
                        (updateLanded ? "（更新已落地）" : "（更新未落地）");
    if (scanStartedDelta > 0) {
      report.verdictLine = "FAIL（modify 触发重扫，方案 B 精准更新未生效）";
    } else if (tracksUnchanged && versionBumped) {
      report.verdictLine = "PASS（upsertSong 精准更新：歌曲数不变、scan 不增长）";
    } else if (!tracksUnchanged) {
      report.verdictLine = "FAIL（modify 后快照歌曲数变化，异常）";
    } else {
      report.verdictLine = "UNKNOWN（无扫描且无版本变化，无法确认精准更新落地）";
    }
    printSceneData(report);
  }

  // ---------- 场景 3：文件 delete（对照组） ----------
  {
    const auto before = log.baseline();
    printSceneHeader(3, "文件 delete（对照组）");
    std::error_code ec;
    fs::remove(musicRoot / "a.wav", ec);
    const bool reached =
        waitUntil([&log, &before] { return log.lastSnapshotTracks() < before.tracks; },
                  kSceneTimeout);
    const auto after = log.baseline();
    SceneReport report;
    report.action = "删除 a.wav；等待歌曲数 -1";
    report.baselineLine = "歌曲数=" + std::to_string(before.tracks);
    report.deltaLine = "ScanStarted增量=" +
                       std::to_string(EventLog::deltaScanStarted(before, after));
    report.resultLine = "歌曲数=" + std::to_string(after.tracks) +
                        (reached ? "（-1 达成）" : "（未达成）");
    report.verdictLine = reached ? "PASS（对照组：delete 触发重扫，快照更新）" : "FAIL（对照组异常）";
    printSceneData(report);
  }

  // ---------- 场景 4：目录 create + 文件（对照组） ----------
  {
    const auto before = log.baseline();
    printSceneHeader(4, "目录 create + 文件（对照组）");
    const fs::path sub = musicRoot / "sub";
    writeMinimalWav(sub / "b.wav");
    const bool reached =
        waitUntil([&log, &before] { return log.lastSnapshotTracks() > before.tracks; },
                  kSceneTimeout);
    const auto after = log.baseline();
    SceneReport report;
    report.action = "mkdir sub + 写 sub/b.wav；等待歌曲数 +1";
    report.baselineLine = "歌曲数=" + std::to_string(before.tracks);
    report.deltaLine = "ScanStarted增量=" +
                       std::to_string(EventLog::deltaScanStarted(before, after));
    report.resultLine = "歌曲数=" + std::to_string(after.tracks) +
                        (reached ? "（+1 达成）" : "（未达成）");
    report.verdictLine = reached ? "PASS（对照组：子目录内 create 触发重扫）" : "FAIL（对照组异常）";
    printSceneData(report);
  }

  // ---------- 场景 5：目录 rmdir（对照组） ----------
  {
    const auto before = log.baseline();
    printSceneHeader(5, "目录 rmdir（对照组）");
    std::error_code ec;
    fs::remove_all(musicRoot / "sub", ec);
    const bool reached =
        waitUntil([&log, &before] { return log.lastSnapshotTracks() < before.tracks; },
                  kSceneTimeout);
    const auto after = log.baseline();
    SceneReport report;
    report.action = "清空并 remove_all(sub)；等待歌曲数 -1";
    report.baselineLine = "歌曲数=" + std::to_string(before.tracks);
    report.deltaLine = "ScanStarted增量=" +
                       std::to_string(EventLog::deltaScanStarted(before, after));
    report.resultLine = "歌曲数=" + std::to_string(after.tracks) +
                        (reached ? "（-1 达成）" : "（未达成）");
    report.verdictLine = reached ? "PASS（对照组：rmdir 触发重扫，快照更新）" : "FAIL（对照组异常）";
    printSceneData(report);
  }

  // ---------- 场景 6：根内目录 rename（对照组） ----------
  {
    const fs::path sub = musicRoot / "sub";
    const fs::path sub2 = musicRoot / "sub2";
    // 重建 sub 并写入 c.wav，等待其进入快照（歌曲数 0 -> 1）
    writeMinimalWav(sub / "c.wav");
    const bool ready = waitUntil([&log] { return log.lastSnapshotTracks() >= 1; }, kSceneTimeout);
    // 基线稳定沉降：把 setup（重建 sub+c.wav）触发的回落重扫计入 before，
    // 使 rename 窗口的 ScanStarted 增量纯净（方案 B 下 rename 本身不扫描）。
    std::this_thread::sleep_for(kBaselineSettle);
    const auto before = log.baseline();
    printSceneHeader(6, "根内目录 rename（对照组）");
    std::error_code ec;
    fs::rename(sub, sub2, ec);
    // 方案 B：根内 rename -> renameSubtree 精准更新；真实 wtr 可能因
    // "dir/rename + 后续 file/other" 回落一次重扫（集成测试接受 baseline+1）。
    // 等待"收敛"信号：快照路径已更新到新路径（旧路径无残留、新路径有歌曲）。
    const bool converged =
        waitUntil([&] {
                    const auto snap = service->snapshot();
                    std::size_t oldN = 0;
                    std::size_t newN = 0;
                    for (const auto& node : snap.nodes) {
                      if (!node.song.has_value()) {
                        continue;
                      }
                      const auto p = node.song->filePath.generic_string();
                      if (p.find((musicRoot / "sub").generic_string() + "/") != std::string::npos) {
                        ++oldN;
                      }
                      if (p.find((musicRoot / "sub2").generic_string() + "/") != std::string::npos) {
                        ++newN;
                      }
                    }
                    return (newN >= 1 && oldN == 0) || log.lastSnapshotTracks() != before.tracks;
                  },
                  kSceneTimeout);
    const auto after = log.baseline();
    // 快照路径收敛断言：旧路径 sub 无残留、新路径 sub2 有歌曲
    std::size_t oldPathResidual = 0;
    std::size_t newPathCount = 0;
    const auto snapshot = service->snapshot();
    for (const auto& node : snapshot.nodes) {
      if (!node.song.has_value()) {
        continue;
      }
      const auto p = node.song->filePath.generic_string();
      if (p.find((musicRoot / "sub").generic_string() + "/") != std::string::npos) {
        ++oldPathResidual;
      }
      if (p.find((musicRoot / "sub2").generic_string() + "/") != std::string::npos) {
        ++newPathCount;
      }
    }
    const auto scanStartedDelta = EventLog::deltaScanStarted(before, after);
    const auto tracksUnchanged = (after.tracks == before.tracks);
    const auto pathConverged = (oldPathResidual == 0) && (newPathCount >= 1);
    SceneReport report;
    report.action = "重建 sub（含 c.wav）后 rename(sub, sub2)；等待快照路径收敛";
    report.baselineLine = "ScanStarted=" + std::to_string(before.scanStarted) +
                          " 快照version=" + std::to_string(before.snapshotVersion) +
                          " 歌曲数=" + std::to_string(before.tracks) +
                          (ready ? "" : "（注意：c.wav 未在超时内进入快照）");
    report.deltaLine = "ScanStarted增量=" + std::to_string(scanStartedDelta) +
                       " FileScanned增量=" +
                       std::to_string(EventLog::deltaCount(before, after, sc::ScannerEventType::FileScanned));
    report.resultLine = "快照version=" + std::to_string(after.snapshotVersion) +
                        " 歌曲数=" + std::to_string(after.tracks) +
                        " 旧路径残留=" + std::to_string(oldPathResidual) +
                        " 新路径歌曲=" + std::to_string(newPathCount) +
                        (converged ? "（已收敛）" : "（未收敛）");
    if (!converged && !pathConverged) {
      report.verdictLine = "FAIL（rename 后快照未收敛：旧路径残留或新路径缺失）";
    } else if (scanStartedDelta > 1) {
      report.verdictLine = "FAIL（rename 触发多次重扫（增量 " + std::to_string(scanStartedDelta) +
                           " > 1），超集成测试接受上限 baseline+1）";
    } else if (!tracksUnchanged) {
      report.verdictLine = "FAIL（rename 后快照歌曲数变化，异常）";
    } else if (!pathConverged) {
      report.verdictLine = "FAIL（快照路径未收敛：旧路径残留或新路径缺失）";
    } else if (scanStartedDelta == 0) {
      report.verdictLine = "PASS（renameSubtree 精准更新：路径收敛、歌曲数不变、scan 不增长）";
    } else {
      report.verdictLine = "PASS（rename 收敛：路径更新、歌曲数不变、scan 有界（1 次回落，集成测试接受））";
    }
    printSceneData(report);
  }

  // ---------- 场景 7：目录 mv 出根（核心实验） ----------
  {
    printSceneHeader(7, "目录 mv 出监视根（核心实验）");
    const fs::path sub2 = musicRoot / "sub2";
    std::error_code ec;
    fs::rename(sub2, movedOut, ec);
    if (ec) {
      std::cout << "  操作    : rename(musicRoot/sub2 -> moved-out) 失败: " << ec.message() << "\n";
      std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
    } else {
      // 沉降：让 mv 自身可能触发的重扫（若有）在 debounce(50ms) 内完成，
      // 基线取在沉降之后、写 d.wav 之前，确保增量精确反映 d.wav 写入是否触发重扫。
      std::this_thread::sleep_for(kPostMoveSettle);
      const auto before = log.baseline();
      writeMinimalWav(movedOut / "d.wav");
      std::this_thread::sleep_for(kObserveWindow);
      const auto after = log.baseline();
      reportMoveOutScene(7, "目录 mv 出监视根（核心实验）", before, after,
                         "rename(sub2 -> moved-out)；沉降 200ms；写 moved-out/d.wav；观察 2s",
                         service.get(), movedOut.generic_string());
    }
  }

  // ---------- 场景 8：移出后根外修改 ----------
  {
    printSceneHeader(8, "移出后根外修改（moved-out 内继续写文件）");
    if (!fs::exists(movedOut)) {
      std::cout << "  操作    : moved-out 不存在（场景 7 未执行成功），跳过\n";
      std::cout << "  判定    : UNKNOWN\n";
    } else {
      const auto before = log.baseline();
      writeMinimalWav(movedOut / "e.wav");
      std::this_thread::sleep_for(kObserveWindow);
      const auto after = log.baseline();
      reportMoveOutScene(8, "移出后根外修改", before, after,
                         "写 moved-out/e.wav；观察 2s",
                         service.get(), movedOut.generic_string());
    }
  }

  // ---------- 场景 9：多根（新增第二根，mv 第二根出根） ----------
  {
    printSceneHeader(9, "多根场景（第二根 musicB）");
    std::error_code ec;
    fs::create_directories(musicB, ec);
    writeMinimalWav(musicB / "f.wav");
    // 重建监视器：startWatching 内部先 stopWatching 再对两个根重新 watch
    // （file_scanner_orchestrator.cpp:1209-1248）。注意 startWatching 本身不触发扫描，
    // f.wav 需等 g.wav 触发的重扫（对全部 watchedRoots）才会进入快照。
    service->startWatching({sc::ScannerRoot{musicRoot, true}, sc::ScannerRoot{musicB, true}});
    const auto beforeG = log.baseline();
    writeMinimalWav(musicB / "g.wav");
    const bool rescanned =
        waitUntil([&log, &beforeG] { return log.scanStartedCount() > beforeG.scanStarted; },
                  kSceneTimeout);
    const auto afterG = log.baseline();
    std::cout << "  操作    : 建 musicB，写 f.wav；startWatching({musicRoot, musicB})；写 g.wav\n";
    std::cout << "  结果    : 重扫=" << (rescanned ? "触发" : "未触发（超时）")
              << " 歌曲数=" << afterG.tracks << "\n";
    // 验证快照包含两个根下的歌曲（f.wav + g.wav）
    const auto snapshot = service->snapshot();
    std::size_t musicBInSnapshot = 0;
    std::size_t musicRootInSnapshot = 0;
    for (const auto& node : snapshot.nodes) {
      if (node.song.has_value()) {
        const auto p = node.song->filePath.generic_string();
        if (p.find("musicB") != std::string::npos) {
          ++musicBInSnapshot;
        } else {
          ++musicRootInSnapshot;
        }
      }
    }
    std::cout << "  验证    : 快照内 musicRoot 歌曲=" << musicRootInSnapshot
              << " musicB 歌曲=" << musicBInSnapshot << "\n";
    std::cout << "  判定    : " << ((rescanned && musicBInSnapshot >= 2U)
                                        ? "PASS（对照组：第二根内 create 触发重扫，两个根歌曲并入快照）"
                                        : "FAIL（对照组异常）")
              << "\n";

    // 对 musicB 整体 mv 出根，再在移出目录内写文件，判定快照是否冻结
    const fs::path musicBPath = musicB;
    fs::rename(musicBPath, movedB, ec);
    if (ec) {
      std::cout << "  操作    : rename(musicB -> musicB-moved) 失败: " << ec.message() << "\n";
      std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
    } else {
      std::this_thread::sleep_for(kPostMoveSettle);
      const auto before = log.baseline();
      writeMinimalWav(movedB / "h.wav");
      std::this_thread::sleep_for(kObserveWindow);
      const auto after = log.baseline();
      reportMoveOutScene(9, "多根场景（第二根 mv 出根）", before, after,
                         "rename(musicB -> musicB-moved)；沉降 200ms；写 musicB-moved/h.wav；观察 2s",
                         service.get(), movedB.generic_string());
    }
  }

  // ---------- 场景 10：纯静默 mv 出根（用户复现场景） ----------
  // 用户实际复现："直接移走整个子文件夹后不做任何操作，播放列表不更新"。
  // 场景 7/8 在 mv 出根后又写了 d.wav/e.wav（根外写入，触发残留 watch 幽灵事件），
  // 未覆盖"纯静默 mv"；本场景 mv 出根后观察窗口内绝不执行任何文件系统操作
  // （不写、不删、不 mv、不 touch），单独验证 IN_MOVE_SELF 精准删除
  // （快照 0 首 + scan 不增长）。
  {
    printSceneHeader(10, "纯静默 mv 出根（用户复现场景）");
    const fs::path silent = musicRoot / "silent";
    const fs::path silentOut = tempRoot / "silent-out";
    std::error_code ec;

    // 若存在先清理；全部发生在基线与 mv 之前，不影响观察窗口测量
    fs::remove_all(silent, ec);
    fs::remove_all(silentOut, ec);
    writeMinimalWav(silent / "song.wav");

    // 等待该歌曲"真实进入快照"（PlaylistSnapshotUpdated 后歌曲数 +1），
    // 而不是只等 ScanStarted：仅等 ScanStarted 会引入"扫描开始但快照未落地"
    // 的竞态，污染后续观察窗口。
    const auto tracksBeforeAdd = log.lastSnapshotTracks();
    const bool songInSnapshot = waitUntil(
        [&log, &tracksBeforeAdd] { return log.lastSnapshotTracks() > tracksBeforeAdd; },
        kSceneTimeout);

    if (!songInSnapshot) {
      std::cout << "  操作    : 写 silent/song.wav；等待歌曲入快照（基线歌曲数="
                << tracksBeforeAdd << "）\n";
      std::cout << "  结果    : 歌曲未在超时内进入快照\n";
      std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
    } else {
      // 基线稳定沉降：把 setup（新建 silent 目录+写 song.wav）触发的回落重扫
      // 计入 before，使 mv 窗口的 ScanStarted 增量纯净（mv 本身不扫描）。
      std::this_thread::sleep_for(kBaselineSettle);
      // 基线 S10：ScanStarted 计数、快照歌曲数、快照事件单调
      const auto before = log.baseline();
      // 整个子文件夹移出根；移出后严禁再对 silent-out 做任何文件系统操作
      ec = {};
      fs::rename(silent, silentOut, ec);
      if (ec) {
        std::cout << "  操作    : rename(musicRoot/silent -> silent-out) 失败: "
                  << ec.message() << "\n";
        std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
      } else {
        // 沉降 300ms（覆盖 watcherDebounce 50ms + 潜在扫描余量），
        // 随后观察窗口 3s：期间绝不执行任何文件系统操作
        std::this_thread::sleep_for(kSilentSettle);
        std::this_thread::sleep_for(kSilentObserveWindow);

        const auto after = log.baseline();
        const auto scanStartedDelta = EventLog::deltaScanStarted(before, after);
        const auto fileScannedDelta =
            EventLog::deltaCount(before, after, sc::ScannerEventType::FileScanned);
        const auto snapshotEventDelta = EventLog::deltaSnapshotEvent(before, after);

        // 残留证据：观察后从 service->snapshot() 确认快照中是否仍含
        // 路径含 "silent/song.wav" 的歌曲节点（方案 B 下应已随 mv 精准删除）
        std::size_t residualCount = 0;
        const auto snapshot = service->snapshot();
        for (const auto& node : snapshot.nodes) {
          if (node.song.has_value() &&
              node.song->filePath.generic_string().find("silent/song.wav") !=
                  std::string::npos) {
            ++residualCount;
            std::cout << "  残留证据: 快照仍含歌曲路径 "
                      << node.song->filePath.generic_string() << "\n";
          }
        }
        if (residualCount == 0) {
          std::cout << "  残留证据: 快照中未找到路径含 \"silent/song.wav\" 的歌曲节点\n";
        }

        // 判定（方案 B + 集成测试接受语义）：
        //   真实 wtr 对 mv 出根会报告 file/other 事件 → 分类器回落全根重扫一次
        //   （集成测试 scanner_wtr_integration_tests.cpp:188 接受 baseline+1），
        //   只要快照收敛（0 首、无 silent/song.wav 残留）即通过；delta==0 为
        //   精准删除（scan 不增长）的理想路径，delta==1 为接受的有界回落。
        //   delta>1 或未收敛 -> FAIL（真实信号）。
        const bool converged = (after.tracks == 0) && (residualCount == 0);
        std::string verdictLine;
        if (scanStartedDelta > 1) {
          verdictLine = "FAIL（mv 触发多次重扫（增量 " + std::to_string(scanStartedDelta) +
                        " > 1），超集成测试接受上限 baseline+1）";
        } else if (!converged) {
          verdictLine = "FAIL（快照未收敛：mv 后残留 " + std::to_string(after.tracks) +
                        " 首、残留路径 " + std::to_string(residualCount) + "）";
        } else if (scanStartedDelta == 0) {
          verdictLine = "PASS（IN_MOVE_SELF 精准删除：快照 0 首 + scan 不增长）";
        } else {
          verdictLine = "PASS（mv 收敛：快照 0 首 + scan 有界（1 次回落，集成测试接受））";
        }

        SceneReport report;
        report.name = "纯静默 mv 出根";
        report.action =
            "写 silent/song.wav 并入快照；rename(musicRoot/silent -> silent-out)；"
            "沉降 300ms；观察 3s（期间零文件系统操作）";
        report.baselineLine =
            "ScanStarted=" + std::to_string(before.scanStarted) +
            " 快照事件单调=" + std::to_string(before.snapshotEventMonotonic) +
            " 快照version=" + std::to_string(before.snapshotVersion) +
            " 歌曲数=" + std::to_string(before.tracks);
        report.deltaLine =
            "ScanStarted增量=" + std::to_string(scanStartedDelta) +
            " FileScanned增量=" + std::to_string(fileScannedDelta) +
            " 快照事件增量=" + std::to_string(snapshotEventDelta);
        report.resultLine =
            "快照version=" + std::to_string(after.snapshotVersion) +
            " 歌曲数=" + std::to_string(after.tracks) +
            " 残留路径=" + std::to_string(residualCount) +
            (converged ? "（收敛）" : "（未收敛）");
        report.verdictLine = verdictLine;
        printSceneData(report);
      }
    }
  }

  // ---------- 场景 11：单文件 mv 出根（fae flush 精准删除） ----------
  // 波 1a（wtr-fae-flush）：wtr 只对目录加 watch，单文件移出根外时父目录收到
  // 孤立 IN_MOVED_FROM（目标在根外 → 无 IN_MOVED_TO 配对），parse_ev 将其存入
  // fae 16 槽环缓冲并以 err_pending 抑制转发 → 上层感知不到，只能靠 60s 对账兜底；
  // fae ~100ms 超时 flush 以 destroy 事件发出 → orchestrator destroyByKey 精准删除
  // （快照收敛 0 首、scan 不增长）。
  // 与场景 7/8/9/10（目录 mv，可能回落重扫）区分：单文件 + 短窗口精准语义，
  // scanStartedDelta 必须 == 0（flush-destroy 精准删除，非对账/回落）。
  // 观察窗口 3s >> flush 100ms，但 << 60s 对账周期 → 收敛只能来自 flush-destroy。
  {
    printSceneHeader(11, "单文件 mv 出根（fae flush 精准删除）");
    const fs::path faeDir = musicRoot / "fae";
    const fs::path faeWav = faeDir / "01.wav";
    const fs::path faeOut = tempRoot / "fae-out";
    std::error_code ec;

    // 若存在先清理；全部发生在基线与 mv 之前，不影响观察窗口测量
    fs::remove_all(faeDir, ec);
    fs::remove_all(faeOut, ec);
    // 必须 .wav：destroy 门禁（orchestrator:2425 isSupportedAudioExtension）排除非音频
    writeMinimalWav(faeWav);

    // 等待歌曲"真实进入快照"（PlaylistSnapshotUpdated 后歌曲数 +1），
    // 而非只等 ScanStarted：仅等 ScanStarted 会引入"扫描开始但快照未落地"
    // 的竞态，污染后续观察窗口。
    const auto tracksBeforeAdd = log.lastSnapshotTracks();
    const bool songInSnapshot = waitUntil(
        [&log, &tracksBeforeAdd] { return log.lastSnapshotTracks() > tracksBeforeAdd; },
        kSceneTimeout);

    if (!songInSnapshot) {
      std::cout << "  操作    : 写 fae/01.wav；等待歌曲入快照（基线歌曲数="
                << tracksBeforeAdd << "）\n";
      std::cout << "  结果    : 歌曲未在超时内进入快照\n";
      std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
    } else {
      // 基线稳定沉降：把 setup（新建 fae 目录+写 01.wav）触发的回落重扫
      // 计入 before，使 mv 窗口的 ScanStarted 增量纯净（flush-destroy 不扫描）。
      std::this_thread::sleep_for(kBaselineSettle);
      const auto before = log.baseline();
      // 单文件 mv 出根（目标在根外 fae-out，fae 目录保留）：父目录 watch 收到
      // 孤立 IN_MOVED_FROM，fae 槽滞留 → ~100ms 超时 flush 以 destroy 发出。
      ec = {};
      fs::rename(faeWav, faeOut, ec);
      if (ec) {
        std::cout << "  操作    : rename(musicRoot/fae/01.wav -> fae-out) 失败: "
                  << ec.message() << "\n";
        std::cout << "  判定    : UNKNOWN（无法执行实验）\n";
      } else if (!fs::exists(faeOut)) {
        std::cout << "  结果    : 移出目标 fae-out 不存在（mv 未生效）\n";
        std::cout << "  判定    : UNKNOWN\n";
      } else {
        // 沉降 300ms + 观察窗口 3s（期间零文件系统操作）：
        // 3s >> fae flush 100ms，但 << 60s 对账周期 → 收敛只能来自 flush-destroy。
        std::this_thread::sleep_for(kSilentSettle);
        std::this_thread::sleep_for(kSilentObserveWindow);
        const auto after = log.baseline();
        reportMoveOutScene(11, "单文件 mv 出根（fae flush 精准删除）", before, after,
                           "写 fae/01.wav 并入快照；rename(musicRoot/fae/01.wav -> fae-out)；"
                           "沉降 300ms；观察 3s（>> flush 100ms，<< 60s 对账）",
                           service.get(), faeWav.generic_string());
      }
    }
  }

  // ---------- 收尾：汇总表 ----------
  std::cout << "\n===== 汇总（判定依据：ScanStarted 增量 / 快照版本与歌曲数 / 残留路径）=====\n";
  std::cout << "场景 1 文件create   : 对照组，预期精准更新且快照歌曲数 +1\n";
  std::cout << "场景 2 文件modify   : 精准更新（upsertSong），预期歌曲数不变、scan 不增长\n";
  std::cout << "场景 3 文件delete   : 对照组，预期精准更新且快照歌曲数 -1\n";
  std::cout << "场景 4 子目录create : 对照组，预期回落重扫且快照歌曲数 +1\n";
  std::cout << "场景 5 目录rmdir    : 对照组，预期精准更新且快照歌曲数 -1\n";
  std::cout << "场景 6 根内rename   : 精准更新（renameSubtree），预期路径收敛、歌曲数不变、scan 不增长\n";
  std::cout << "场景 7 mv出根+根外写: 核心实验，预期 IN_MOVE_SELF 精准删除（0 首）+ 幽灵事件丢弃、scan 不增长\n";
  std::cout << "场景 8 根外继续写   : 核心实验延续，预期幽灵事件丢弃、快照收敛、scan 不增长\n";
  std::cout << "场景 9 多根mv出根   : 核心实验延伸，预期精准删除 + 幽灵事件丢弃、scan 不增长\n";
  std::cout << "场景 10 纯静默mv出根: 用户复现场景，预期 IN_MOVE_SELF 精准删除（快照 0 首 + scan 不增长）\n";
  std::cout << "场景 11 单文件mv出根 : fae flush 精准删除，预期快照 0 首 + scan 不增长（3s 窗口内，非对账回落）\n";
  std::cout << "注：以上预期为方案 B 语义；实际判定以上方各场景输出为准。\n";

  const auto finalErrors = log.errors();
  if (!finalErrors.empty()) {
    std::cout << "\n===== 扫描错误事件（ScanError）=====\n";
    for (const auto& err : finalErrors) {
      std::cout << "  " << err << "\n";
    }
  }

  // 最近 FileScanned 路径（辅助观察窗口内容）
  const auto recent = log.recentScanned();
  if (!recent.empty()) {
    std::cout << "\n===== 最近 FileScanned 路径（最多 16 条）=====\n";
    for (const auto& path : recent) {
      std::cout << "  " << path << "\n";
    }
  }

  // ---------- 清理 ----------
  // 显式 stopWatching + stop：wtr close 阻塞等待监视线程退出（watcher.hpp:2405-2409），
  // 之后再销毁服务与删除临时目录，避免 watcher 扫描未结束的目录。
  service->stopWatching();
  service->stop();
  service.reset();
  std::error_code cleanupEc;
  fs::remove_all(tempRoot, cleanupEc);
  if (cleanupEc) {
    std::cerr << "警告：清理临时目录失败: " << cleanupEc.message() << "\n";
  }

  std::cout << "\n===== 审计结束 =====\n";
  return 0;
}
