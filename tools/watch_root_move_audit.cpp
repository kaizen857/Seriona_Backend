// 目录移出监视根审计程序（watch_root_move_audit）
//
// 独立审计工具：只链接 seriona_scanner，使用真实 FileScannerService
// （注入式工厂 makeFileScannerService + 生产回填的 EfswFolderWatcherFactory，
// 即真实的 efsw 监视器），在临时目录做文件系统操作对照实验，
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

#include <efsw/efsw.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
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
// Gate 0：efsw 语义实测矩阵（efsw-migration 计划任务 4 / Wave 2）
//
// 目的：绕过 seriona 编排层，直接用 efsw::FileWatcher（Linux 默认 inotify 后端）
// 驱动文件系统操作矩阵，逐场景记录原始事件流（相对时间戳、watchid、dir、
// filename、action、oldFilename），为迁移决策（D4：ReportCrossDirectoryMoves
// 默认值）与整体 GO/NO-GO 提供一手证据。
//
// 场景（每个场景独立 fork 子进程 + 独立临时目录）：
//   S1 目录移入（含既有子文件）          S2 文件移入
//   S3 文件/目录移出（Delete/级联）      S4 根内 rename（文件/目录）
//   S5 跨目录移动 OFF/ON（D4 依据）      S6 create/modify/delete
//   S7 UTF-8 路径（非 ASCII 根/文件名）  S8 尖峰风暴（批量/合并/竞态/溢出）
//   S9 停止/析构竞态
//
// 用法：
//   seriona_watch_root_move_audit                     原有移出审计（无参数，行为不变）
//   seriona_watch_root_move_audit --efsw-matrix DIR   efsw 矩阵，日志写 DIR/scenario-*.log
//   seriona_watch_root_move_audit --efsw-negative     无效路径负向用例（预期非 0 退出）
//
// 预期事件语义（源码核实：efsw-src/src/efsw/FileWatcherInotify.cpp）：
//   - IN_MOVED_TO 且无旧路径（从根外移入）→ Add + Modified（:552-566）
//   - IN_MOVED_FROM 配不到批内 IN_MOVED_TO → Delete（:453）
//   - 同目录 rename → 单条 Moved，oldFilename=相对旧名（:467-471）
//   - 跨目录 rename（同递归根）且 ReportCrossDirectoryMoves=ON → 单条 Moved，
//     oldFilename=绝对源路径（:525-535）；OFF → Delete(源)+Add/Modified(目标)
//   - 目录移入仅上报目录本身，不逐事件上报既有子文件（:560, :193-215）
//   - handleMissedFileActions 仅在 IN_Q_OVERFLOW 时触发（:547-548）
// ============================================================================

namespace gate0 {

// ---- 文本/时间辅助 ----------------------------------------------------------

// UTF-8 字节转换（Linux 原生路径即字节；与 src/scanner/path_utf8.h 同规则）
std::string toUtf8(const fs::path& path) {
  const std::u8string encoded = path.u8string();
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

const char* actionName(efsw::Action action) {
  switch (action) {
    case efsw::Actions::Add:
      return "Add";
    case efsw::Actions::Delete:
      return "Delete";
    case efsw::Actions::Modified:
      return "Modified";
    case efsw::Actions::Moved:
      return "Moved";
  }
  return "Unknown";
}

std::string actionLabel(efsw::Action action) {
  std::string label = actionName(action);
  label.resize(8, ' ');
  return label;
}

std::string elapsedText(const std::chrono::steady_clock::time_point& origin,
                        const std::chrono::steady_clock::time_point& now) {
  const double value = std::chrono::duration<double, std::milli>(now - origin).count();
  std::ostringstream os;
  os << std::fixed << std::setprecision(3) << value;
  return os.str();
}

std::string wallClockText() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&seconds, &tm);
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
  return buffer;
}

// ---- 原始事件记录器（efsw 在自有线程回调，必须加锁） -----------------------

struct EfswEvent {
  std::chrono::steady_clock::time_point t;
  efsw::WatchID watchid{0};
  std::string dir;
  std::string filename;
  efsw::Action action{efsw::Actions::Add};
  std::string oldFilename;
};

class EventRecorder final : public efsw::FileWatchListener {
 public:
  void handleFileAction(efsw::WatchID watchid, const std::string& dir,
                        const std::string& filename, efsw::Action action,
                        const std::string& oldFilename) override {
    if (destroyStarted_.load(std::memory_order_relaxed)) {
      ++afterDestroyStarted_;
    }
    if (watcherDestroyed_.load(std::memory_order_relaxed)) {
      ++afterWatcherDestroyed_;
    }
    std::scoped_lock lock{mutex_};
    events_.push_back(EfswEvent{std::chrono::steady_clock::now(), watchid, dir, filename, action,
                                oldFilename});
  }

  void handleMissedFileActions(efsw::WatchID watchid, const std::string& dir) override {
    std::scoped_lock lock{mutex_};
    (void)watchid;
    missedDirs_.push_back(dir);
  }

  std::vector<EfswEvent> events() const {
    std::scoped_lock lock{mutex_};
    return events_;
  }

  std::size_t eventCount() const {
    std::scoped_lock lock{mutex_};
    return events_.size();
  }

  std::vector<std::string> missedDirs() const {
    std::scoped_lock lock{mutex_};
    return missedDirs_;
  }

  // S9：停止/析构开始后到达的回调计数（允许发生，仅记录）
  void noteDestroyStarted() { destroyStarted_.store(true, std::memory_order_relaxed); }
  std::uint64_t callbacksAfterDestroyStarted() const {
    return afterDestroyStarted_.load(std::memory_order_relaxed);
  }
  // S9：watcher 析构返回后到达的回调（必须为 0；>0 = use-after-free 风险）
  void noteWatcherDestroyed() { watcherDestroyed_.store(true, std::memory_order_relaxed); }
  std::uint64_t callbacksAfterWatcherDestroyed() const {
    return afterWatcherDestroyed_.load(std::memory_order_relaxed);
  }

 private:
  mutable std::mutex mutex_;
  std::vector<EfswEvent> events_;
  std::vector<std::string> missedDirs_;
  std::atomic<bool> destroyStarted_{false};
  std::atomic<bool> watcherDestroyed_{false};
  std::atomic<std::uint64_t> afterDestroyStarted_{0};
  std::atomic<std::uint64_t> afterWatcherDestroyed_{0};
};

// ---- 事件查询辅助 -----------------------------------------------------------

std::size_t countIf(const std::vector<EfswEvent>& events,
                    const std::function<bool(const EfswEvent&)>& predicate) {
  return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), predicate));
}

bool anyIf(const std::vector<EfswEvent>& events,
           const std::function<bool(const EfswEvent&)>& predicate) {
  return countIf(events, predicate) > 0;
}

constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

std::size_t firstIndex(const std::vector<EfswEvent>& events,
                       const std::function<bool(const EfswEvent&)>& predicate) {
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (predicate(events[i])) {
      return i;
    }
  }
  return kNoIndex;
}

std::string sequenceOf(const std::vector<EfswEvent>& events, const std::string& filename) {
  std::string sequence;
  for (const auto& event : events) {
    if (event.filename != filename) {
      continue;
    }
    if (!sequence.empty()) {
      sequence += ",";
    }
    sequence += actionName(event.action);
  }
  return sequence.empty() ? "（无）" : sequence;
}

// ---- 场景上下文（每场景独立临时目录 + 每场景独立日志） ----------------------

struct ScenarioCtx {
  std::string id;
  std::string title;
  fs::path base;
  fs::path root;
  fs::path outside;
  std::ostream* log{nullptr};
  std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
  int failures{0};
  int unknowns{0};

  void line(const std::string& text) {
    if (log != nullptr) {
      (*log) << text << "\n";
      log->flush();
    }
  }
  void expect(const std::string& text) { line("  [expect] " + text); }
  void note(const std::string& text) { line("  [note] " + text); }
  void check(const std::string& label, bool ok, const std::string& observed) {
    if (!ok) {
      ++failures;
    }
    line(std::string("  [") + (ok ? "PASS" : "FAIL") + "] " + label + " | 观察: " + observed);
  }
  void unknown(const std::string& label, const std::string& observed) {
    ++unknowns;
    line("  [UNKNOWN] " + label + " | 观察: " + observed);
  }
};

ScenarioCtx makeScenarioCtx(const std::string& id, const std::string& title) {
  ScenarioCtx ctx;
  ctx.id = id;
  ctx.title = title;
  ctx.base = fs::temp_directory_path() / ("seriona-gate0-" + id + "-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(ctx.base, ec);
  ctx.root = ctx.base / "root";
  ctx.outside = ctx.base / "outside";
  fs::create_directories(ctx.root, ec);
  fs::create_directories(ctx.outside, ec);
  return ctx;
}

void cleanupScenario(const ScenarioCtx& ctx) {
  if (::getenv("SERIONA_GATE0_KEEP") != nullptr) {
    return;
  }
  std::error_code ec;
  fs::remove_all(ctx.base, ec);
}

void writeText(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out << content;
}

// ---- efsw 会话（真实 FileWatcher + 选项矩阵） -------------------------------

struct EfswSession {
  std::unique_ptr<efsw::FileWatcher> watcher;
  std::unique_ptr<EventRecorder> recorder;
  efsw::WatchID watchid{-1};
  std::chrono::steady_clock::time_point t0;
  bool started{false};
};

EfswSession startEfswSession(ScenarioCtx& ctx, const fs::path& root, bool crossDirectoryMoves) {
  EfswSession session;
  session.recorder = std::make_unique<EventRecorder>();
  session.watcher = std::make_unique<efsw::FileWatcher>();
  std::vector<efsw::WatcherOption> options;
  if (crossDirectoryMoves) {
    options.emplace_back(efsw::Options::ReportCrossDirectoryMoves, 1);
  }
  const std::string rootUtf8 = toUtf8(root);
  session.t0 = std::chrono::steady_clock::now();
  session.watchid = session.watcher->addWatch(rootUtf8, session.recorder.get(), true, options);
  if (session.watchid < 0) {
    // 经 ctx.check 上报失败（failures++ → 子进程 exit 1 → 场景 FAIL），而非仅打印一行。
    ctx.check("addWatch 成功", false,
              "watchid=" + std::to_string(session.watchid) +
                  " lastError=" + efsw::Errors::Log::getLastErrorLog());
    return session;
  }
  efsw::Errors::Log::clearLastError();
  session.watcher->watch();
  session.started = true;
  std::this_thread::sleep_for(60ms);  // 让 reader 线程进入 select 循环
  return session;
}

// ---- 等待/沉降/导出 ---------------------------------------------------------

bool waitForEvents(EventRecorder& recorder, std::size_t from,
                   const std::function<bool(const std::vector<EfswEvent>&)>& predicate,
                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    {
      const auto events = recorder.events();
      const auto begin = std::min(from, events.size());
      const std::vector<EfswEvent> slice(events.begin() + static_cast<std::ptrdiff_t>(begin),
                                         events.end());
      if (predicate(slice)) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(5ms);
  }
}

void settleQuiet(EventRecorder& recorder, std::chrono::milliseconds quiet,
                 std::chrono::milliseconds maxWait) {
  auto lastCount = recorder.eventCount();
  auto lastChange = std::chrono::steady_clock::now();
  const auto deadline = lastChange + maxWait;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
    const auto count = recorder.eventCount();
    if (count != lastCount) {
      lastCount = count;
      lastChange = std::chrono::steady_clock::now();
      continue;
    }
    if (std::chrono::steady_clock::now() - lastChange >= quiet) {
      return;
    }
  }
}

std::vector<EfswEvent> eventsSince(const EventRecorder& recorder, std::size_t from) {
  const auto events = recorder.events();
  const auto begin = std::min(from, events.size());
  return std::vector<EfswEvent>(events.begin() + static_cast<std::ptrdiff_t>(begin), events.end());
}

void dumpEvents(ScenarioCtx& ctx, const EfswSession& session, std::size_t from,
                const std::string& label) {
  const auto events = session.recorder->events();
  const auto begin = std::min(from, events.size());
  ctx.line("  --- 原始事件流（" + label + "；新增 " + std::to_string(events.size() - begin) +
           " 条）---");
  for (std::size_t i = begin; i < events.size(); ++i) {
    const auto& event = events[i];
    std::ostringstream os;
    os << "  [+" << elapsedText(session.t0, event.t) << "]"
       << " watchid=" << event.watchid << " action=" << actionLabel(event.action)
       << " dir=" << event.dir << " filename=" << event.filename;
    if (!event.oldFilename.empty()) {
      os << " oldFilename=" << event.oldFilename;
    }
    ctx.line(os.str());
  }
}

// ---- S1：目录移入（含既有子文件） ------------------------------------------

void scenarioS1(ScenarioCtx& ctx) {
  ctx.expect("目录移入：父 watch 收到 Add(incoming)（IN_MOVED_TO 无旧路径时伴随 Modified）");
  ctx.expect("既有子文件（含子目录内文件）不逐事件上报 → 迁移需要子树枚举");
  ctx.expect("移入目录的递归 watch 在 Add 回调前已注册 → 之后写入可被捕获");

  const fs::path incoming = ctx.outside / "incoming";
  writeText(incoming / "child1.wav", "c1");
  writeText(incoming / "child2.wav", "c2");
  writeText(incoming / "sub" / "deep.wav", "d");

  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }
  const std::size_t base = session.recorder->eventCount();

  std::error_code ec;
  fs::rename(incoming, ctx.root / "incoming", ec);
  ctx.line("  [op] rename(outside/incoming -> root/incoming) ec=" + ec.message());
  waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
    return anyIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "incoming";
    });
  }, 3s);
  settleQuiet(*session.recorder, 200ms, 2s);

  const auto events = eventsSince(*session.recorder, base);
  dumpEvents(ctx, session, base, "目录移入后");

  const auto dirAdds = countIf(events, [](const EfswEvent& event) {
    return event.action == efsw::Actions::Add && event.filename == "incoming";
  });
  const auto dirModified = countIf(events, [](const EfswEvent& event) {
    return event.action == efsw::Actions::Modified && event.filename == "incoming";
  });
  const auto childEvents = countIf(events, [](const EfswEvent& event) {
    return event.filename == "child1.wav" || event.filename == "child2.wav" ||
           event.filename == "deep.wav";
  });
  ctx.check("目录 Add 事件已产生", dirAdds >= 1, "Add(incoming)=" + std::to_string(dirAdds));
  ctx.check("子文件未被逐事件上报（证明需子树枚举）", childEvents == 0,
            "子文件事件数=" + std::to_string(childEvents));
  ctx.note("incoming 事件序列=" + sequenceOf(events, "incoming") +
           "；Modified(incoming)=" + std::to_string(dirModified));

  const std::size_t lateBase = session.recorder->eventCount();
  writeText(ctx.root / "incoming" / "late.wav", "late");
  waitForEvents(*session.recorder, lateBase, [](const std::vector<EfswEvent>& events) {
    return anyIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "late.wav";
    });
  }, 3s);
  settleQuiet(*session.recorder, 150ms, 1500ms);
  const auto lateEvents = eventsSince(*session.recorder, lateBase);
  dumpEvents(ctx, session, lateBase, "移入目录内后续写入");

  const std::string expectedDir = toUtf8(ctx.root / "incoming") + "/";
  const bool lateAddWithNewDir = anyIf(lateEvents, [&](const EfswEvent& event) {
    return event.action == efsw::Actions::Add && event.filename == "late.wav" &&
           event.dir == expectedDir;
  });
  ctx.check("递归 watch 生效：late.wav Add 且 dir=移入目录", lateAddWithNewDir,
            "期望 dir=" + expectedDir);
}

// ---- S2：文件移入 -----------------------------------------------------------

void scenarioS2(ScenarioCtx& ctx) {
  ctx.expect("文件移入：Add(file_in.wav) 后紧跟 Modified(file_in.wav)（IN_MOVED_TO 无旧路径）");
  ctx.expect("dir+filename 拼出完整目标路径");

  const fs::path source = ctx.outside / "file_in.wav";
  writeText(source, "x");

  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }
  const std::size_t base = session.recorder->eventCount();

  std::error_code ec;
  fs::rename(source, ctx.root / "file_in.wav", ec);
  ctx.line("  [op] rename(outside/file_in.wav -> root/file_in.wav) ec=" + ec.message());
  waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
    return anyIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "file_in.wav";
    });
  }, 3s);
  settleQuiet(*session.recorder, 200ms, 2s);

  const auto events = eventsSince(*session.recorder, base);
  dumpEvents(ctx, session, base, "文件移入后");

  const std::string expectedPath = toUtf8(ctx.root / "file_in.wav");
  const bool pathComplete = anyIf(events, [&](const EfswEvent& event) {
    return event.action == efsw::Actions::Add && event.filename == "file_in.wav" &&
           (event.dir + event.filename) == expectedPath;
  });
  const auto addIndex = firstIndex(events, [](const EfswEvent& event) {
    return event.action == efsw::Actions::Add && event.filename == "file_in.wav";
  });
  const auto modifiedIndex = firstIndex(events, [](const EfswEvent& event) {
    return event.action == efsw::Actions::Modified && event.filename == "file_in.wav";
  });
  ctx.check("Add 事件存在且 dir+filename=完整目标路径", pathComplete,
            "期望=" + expectedPath + "；序列=" + sequenceOf(events, "file_in.wav"));
  ctx.check("Add 之后有 Modified（成对语义）",
            addIndex != kNoIndex && modifiedIndex != kNoIndex && addIndex < modifiedIndex,
            "序列=" + sequenceOf(events, "file_in.wav"));
}

// ---- S3：文件/目录移出（Delete/级联） --------------------------------------

void scenarioS3(ScenarioCtx& ctx) {
  ctx.expect("文件移出：Delete(file_out.wav)，无 Add/Moved 回落");
  ctx.expect("目录移出：Delete 级联（被监视子目录与目录本身均产生 Delete），无 Add/Moved");
  ctx.expect("移出后 watch 已清理：向移出目录内写入不产生幽灵事件");

  writeText(ctx.root / "file_out.wav", "x");
  writeText(ctx.root / "dir_out" / "deep" / "inner.wav", "x");

  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }

  // Phase A：文件移出
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(ctx.root / "file_out.wav", ctx.outside / "file_out.wav", ec);
    ctx.line("  [op] rename(root/file_out.wav -> outside/file_out.wav) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "file_out.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件移出");
    const auto deletes = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "file_out.wav";
    });
    const auto others = countIf(events, [](const EfswEvent& event) {
      return event.action != efsw::Actions::Delete && event.filename == "file_out.wav";
    });
    ctx.check("文件移出产生 Delete", deletes >= 1, "Delete(file_out.wav)=" + std::to_string(deletes));
    ctx.check("无 Add/Moved 回落", others == 0, "非 Delete 事件=" + std::to_string(others));
  }

  // Phase B：目录移出（含已注册的子目录 watch）
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(ctx.root / "dir_out", ctx.outside / "dir_out", ec);
    ctx.line("  [op] rename(root/dir_out -> outside/dir_out) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "dir_out";
      });
    }, 3s);
    settleQuiet(*session.recorder, 250ms, 2000ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "目录移出");
    const auto dirDelete = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "dir_out";
    });
    const auto deepDelete = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "deep";
    });
    const auto movedOrAdd = countIf(events, [](const EfswEvent& event) {
      return (event.action == efsw::Actions::Add || event.action == efsw::Actions::Moved) &&
             (event.filename == "dir_out" || event.filename == "deep");
    });
    ctx.check("目录移出产生 Delete(dir_out)", dirDelete >= 1,
              "Delete(dir_out)=" + std::to_string(dirDelete));
    ctx.check("被监视子目录级联 Delete(deep)", deepDelete >= 1,
              "Delete(deep)=" + std::to_string(deepDelete));
    ctx.check("无 Add/Moved 回落", movedOrAdd == 0, "Add/Moved=" + std::to_string(movedOrAdd));
    ctx.note("dir_out 序列=" + sequenceOf(events, "dir_out") + "；deep 序列=" +
             sequenceOf(events, "deep") +
             "；级联顺序判定仅记录（源码为最深目录优先）");
  }

  // Phase C：移出目录内写入，确认 watch 已清理（无幽灵事件）
  {
    const std::size_t base = session.recorder->eventCount();
    writeText(ctx.outside / "dir_out" / "newfile.wav", "x");
    settleQuiet(*session.recorder, 300ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "移出目录内写入（幽灵事件检查）");
    ctx.check("移出目录内写入无幽灵事件（watch 已清理）", events.empty(),
              "新增事件=" + std::to_string(events.size()));
  }
}

// ---- S4：根内 rename（文件/目录） ------------------------------------------

void scenarioS4(ScenarioCtx& ctx) {
  ctx.expect("文件根内 rename：单条 Moved(new.wav, oldFilename=old.wav)，无 Delete/Add");
  ctx.expect("目录根内 rename：单条 Moved(dirB, oldFilename=dirA)");
  ctx.expect("目录 rename 后 watch 路径已更新：目录内写入事件的 dir 为新路径");

  writeText(ctx.root / "old.wav", "x");
  writeText(ctx.root / "dirA" / "inside.wav", "x");

  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }

  // Phase A：文件 rename
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(ctx.root / "old.wav", ctx.root / "new.wav", ec);
    ctx.line("  [op] rename(root/old.wav -> root/new.wav) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "new.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件根内 rename");
    const auto moved = countIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Moved && event.filename == "new.wav" &&
             event.oldFilename == "old.wav" && event.dir == toUtf8(ctx.root) + "/";
    });
    const auto addDelete = countIf(events, [](const EfswEvent& event) {
      return (event.action == efsw::Actions::Add || event.action == efsw::Actions::Delete) &&
             (event.filename == "old.wav" || event.filename == "new.wav");
    });
    ctx.check("单条 Moved(new.wav, oldFilename=old.wav)", moved == 1,
              "Moved 匹配数=" + std::to_string(moved) + "；序列=" + sequenceOf(events, "new.wav"));
    ctx.check("无 Delete/Add 回落", addDelete == 0, "Add/Delete=" + std::to_string(addDelete));
  }

  // Phase B：目录 rename
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(ctx.root / "dirA", ctx.root / "dirB", ec);
    ctx.line("  [op] rename(root/dirA -> root/dirB) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "dirB";
      });
    }, 3s);
    settleQuiet(*session.recorder, 250ms, 2000ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "目录根内 rename");
    const auto moved = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Moved && event.filename == "dirB" &&
             event.oldFilename == "dirA";
    });
    const auto addDelete = countIf(events, [](const EfswEvent& event) {
      return (event.action == efsw::Actions::Add || event.action == efsw::Actions::Delete) &&
             (event.filename == "dirA" || event.filename == "dirB");
    });
    ctx.check("单条 Moved(dirB, oldFilename=dirA)", moved == 1,
              "Moved 匹配数=" + std::to_string(moved));
    ctx.check("目录 rename 无 Delete/Add 回落", addDelete == 0,
              "Add/Delete=" + std::to_string(addDelete));
  }

  // Phase C：watch 路径更新验证
  {
    const std::size_t base = session.recorder->eventCount();
    writeText(ctx.root / "dirB" / "late2.wav", "x");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "late2.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "rename 后目录内写入");
    const std::string expectedDir = toUtf8(ctx.root / "dirB") + "/";
    const bool updated = anyIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "late2.wav" &&
             event.dir == expectedDir;
    });
    ctx.check("rename 后事件 dir=新目录路径", updated, "期望 dir=" + expectedDir);
  }
}

// ---- S5：跨目录移动（ReportCrossDirectoryMoves OFF/ON；D4 决策依据） --------

void scenarioS5(ScenarioCtx& ctx, bool crossDirectoryMoves) {
  const std::string mode = crossDirectoryMoves ? "ON" : "OFF";
  ctx.expect("模式=" + mode + "：" +
             (crossDirectoryMoves
                  ? "跨目录移动 → 单条 Moved（dir=目标目录，oldFilename=绝对源路径），无 Delete/Add"
                  : "跨目录移动 → Delete(源) + Add/Modified(目标) 回落，无 Moved"));

  const fs::path dirA = ctx.root / "dirA";
  const fs::path dirB = ctx.root / "dirB";
  const fs::path srcDir = ctx.root / "srcDir";
  writeText(dirA / "file.wav", "x");
  fs::create_directories(dirB);
  writeText(srcDir / "subdir" / "leaf.bin", "x");

  EfswSession session = startEfswSession(ctx, ctx.root, crossDirectoryMoves);
  if (!session.started) {
    return;
  }
  const std::string dirAUtf8 = toUtf8(dirA) + "/";
  const std::string dirBUtf8 = toUtf8(dirB) + "/";
  const std::string srcDirUtf8 = toUtf8(srcDir) + "/";

  // Phase A：文件跨目录移动（dirA/file.wav -> dirB/file.wav）
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(dirA / "file.wav", dirB / "file.wav", ec);
    ctx.line("  [op] rename(root/dirA/file.wav -> root/dirB/file.wav) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [&](const std::vector<EfswEvent>& events) {
      if (crossDirectoryMoves) {
        return anyIf(events, [](const EfswEvent& event) {
          return event.action == efsw::Actions::Moved && event.filename == "file.wav";
        });
      }
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "file.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 250ms, 2000ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件跨目录移动（" + mode + "）");

    if (crossDirectoryMoves) {
      const auto moved = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "file.wav" &&
               event.dir == dirBUtf8 && event.oldFilename == toUtf8(dirA / "file.wav");
      });
      const auto fallback = countIf(events, [](const EfswEvent& event) {
        return (event.action == efsw::Actions::Add || event.action == efsw::Actions::Delete) &&
               event.filename == "file.wav";
      });
      ctx.check("ON：单条 Moved(file.wav, oldFilename=绝对源路径)", moved == 1,
                "Moved 匹配数=" + std::to_string(moved) + "；序列=" +
                    sequenceOf(events, "file.wav"));
      ctx.check("ON：无 Delete/Add 回落", fallback == 0,
                "Delete/Add=" + std::to_string(fallback));
    } else {
      const auto deleted = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "file.wav" &&
               event.dir == dirAUtf8;
      });
      const auto added = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "file.wav" &&
               event.dir == dirBUtf8;
      });
      const auto moved = countIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "file.wav";
      });
      ctx.check("OFF：Delete(源 dirA/file.wav) + Add(目标 dirB/file.wav)",
                deleted >= 1 && added >= 1,
                "Delete=" + std::to_string(deleted) + " Add=" + std::to_string(added) +
                    "；序列=" + sequenceOf(events, "file.wav"));
      ctx.check("OFF：无 Moved 事件", moved == 0, "Moved=" + std::to_string(moved));
    }
  }

  // Phase B：目录跨目录移动（srcDir/subdir -> dirB/subdir）
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::rename(srcDir / "subdir", dirB / "subdir", ec);
    ctx.line("  [op] rename(root/srcDir/subdir -> root/dirB/subdir) ec=" + ec.message());
    waitForEvents(*session.recorder, base, [&](const std::vector<EfswEvent>& events) {
      if (crossDirectoryMoves) {
        return anyIf(events, [](const EfswEvent& event) {
          return event.action == efsw::Actions::Moved && event.filename == "subdir";
        });
      }
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "subdir";
      });
    }, 3s);
    settleQuiet(*session.recorder, 300ms, 2500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "目录跨目录移动（" + mode + "）");
    const std::string expectedOld = toUtf8(srcDir / "subdir");

    if (crossDirectoryMoves) {
      const auto moved = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "subdir" &&
               event.dir == dirBUtf8 && event.oldFilename == expectedOld;
      });
      const auto fallback = countIf(events, [](const EfswEvent& event) {
        return (event.action == efsw::Actions::Add || event.action == efsw::Actions::Delete) &&
               event.filename == "subdir";
      });
      ctx.check("ON：单条 Moved(subdir, oldFilename=绝对源路径)", moved == 1,
                "Moved 匹配数=" + std::to_string(moved));
      ctx.check("ON：目录移动无 Delete/Add 回落", fallback == 0,
                "Delete/Add=" + std::to_string(fallback));
    } else {
      const auto deleted = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "subdir" &&
               event.dir == srcDirUtf8;
      });
      const auto added = countIf(events, [&](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "subdir" &&
               event.dir == dirBUtf8;
      });
      const auto moved = countIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "subdir";
      });
      ctx.check("OFF：Delete(源) + Add(目标)", deleted >= 1 && added >= 1,
                "Delete=" + std::to_string(deleted) + " Add=" + std::to_string(added));
      ctx.check("OFF：目录移动无 Moved 事件", moved == 0, "Moved=" + std::to_string(moved));
    }
  }

  // Phase C（仅 ON）：目录移动后 watch 路径已迁移到新位置
  if (crossDirectoryMoves) {
    const std::size_t base = session.recorder->eventCount();
    writeText(dirB / "subdir" / "late3.bin", "x");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "late3.bin";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "移动后目录内写入");
    const std::string expectedDir = toUtf8(dirB / "subdir") + "/";
    const bool updated = anyIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "late3.bin" &&
             event.dir == expectedDir;
    });
    ctx.check("目录移动后事件 dir=新路径", updated, "期望 dir=" + expectedDir);
  }
}

// ---- S6：create/modify/delete 常规序列 -------------------------------------

void scenarioS6(ScenarioCtx& ctx) {
  ctx.expect("mkdir → Add；文件创建（含写入）→ Add + ≥1 Modified；改写 → ≥1 Modified；"
             "删除文件 → Delete；rmdir → Delete");

  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }

  // mkdir
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::create_directories(ctx.root / "newdir", ec);
    ctx.line("  [op] mkdir root/newdir ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "newdir";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "mkdir");
    const auto adds = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "newdir";
    });
    ctx.check("mkdir → Add(newdir)", adds >= 1, "Add=" + std::to_string(adds));
  }

  // create file (write content)
  {
    const std::size_t base = session.recorder->eventCount();
    writeText(ctx.root / "newdir" / "song.txt", "hello");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "song.txt";
      });
    }, 3s);
    settleQuiet(*session.recorder, 200ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件创建（含写入）");
    const auto adds = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "song.txt";
    });
    const auto modified = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Modified && event.filename == "song.txt";
    });
    ctx.check("文件创建 → Add(song.txt)", adds >= 1, "Add=" + std::to_string(adds));
    ctx.check("创建时写入 → ≥1 Modified", modified >= 1,
              "Modified=" + std::to_string(modified) + "；序列=" + sequenceOf(events, "song.txt"));
  }

  // modify
  {
    const std::size_t base = session.recorder->eventCount();
    std::this_thread::sleep_for(5ms);
    writeText(ctx.root / "newdir" / "song.txt", "hello world");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Modified && event.filename == "song.txt";
      });
    }, 3s);
    settleQuiet(*session.recorder, 200ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件改写");
    const auto modified = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Modified && event.filename == "song.txt";
    });
    ctx.check("改写 → ≥1 Modified", modified >= 1, "Modified=" + std::to_string(modified));
  }

  // delete file
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::remove(ctx.root / "newdir" / "song.txt", ec);
    ctx.line("  [op] rm root/newdir/song.txt ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "song.txt";
      });
    }, 3s);
    settleQuiet(*session.recorder, 200ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "文件删除");
    const auto deletes = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "song.txt";
    });
    ctx.check("删除文件 → Delete(song.txt)", deletes >= 1, "Delete=" + std::to_string(deletes));
  }

  // rmdir
  {
    const std::size_t base = session.recorder->eventCount();
    std::error_code ec;
    fs::remove(ctx.root / "newdir", ec);
    ctx.line("  [op] rmdir root/newdir ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "newdir";
      });
    }, 3s);
    settleQuiet(*session.recorder, 200ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "rmdir");
    const auto deletes = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "newdir";
    });
    ctx.check("rmdir → Delete(newdir)", deletes >= 1, "Delete=" + std::to_string(deletes));
  }
}

// ---- S7：UTF-8 路径（非 ASCII 根/目录/文件名） -----------------------------

void scenarioS7(ScenarioCtx& ctx) {
  ctx.expect("非 ASCII 根/目录/文件名：dir、filename、oldFilename 保持原 UTF-8 字节，无乱码/截断");

  const fs::path utfRoot = ctx.base / "音乐根-🎵";
  std::error_code ec;
  fs::create_directories(utfRoot, ec);
  EfswSession session = startEfswSession(ctx, utfRoot, false);
  if (!session.started) {
    return;
  }
  const std::string utfRootSlash = toUtf8(utfRoot) + "/";
  const std::string album = "專輯-音楽";
  const std::string song = "歌曲-café-Ω.wav";
  const std::string renamed = "改名-ñ.mp3";
  const fs::path albumDir = utfRoot / fs::path{album};
  const fs::path songPath = albumDir / fs::path{song};
  const fs::path renamedPath = albumDir / fs::path{renamed};

  // mkdir 非 ASCII 目录
  {
    const std::size_t base = session.recorder->eventCount();
    fs::create_directories(albumDir, ec);
    ctx.line("  [op] mkdir " + toUtf8(albumDir) + " ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "專輯-音楽";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "非 ASCII mkdir");
    const bool ok = anyIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == album &&
             event.dir == utfRootSlash;
    });
    ctx.check("非 ASCII 目录 Add 字节保真", ok, "期望 filename=" + album + " dir=" + utfRootSlash);
  }

  // create 非 ASCII 文件
  {
    const std::size_t base = session.recorder->eventCount();
    writeText(songPath, "x");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "歌曲-café-Ω.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "非 ASCII 文件创建");
    const std::string expectedFull = toUtf8(songPath);
    const bool ok = anyIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == song &&
             (event.dir + event.filename) == expectedFull;
    });
    ctx.check("非 ASCII 文件 Add 且 dir+filename=完整路径", ok, "期望=" + expectedFull);
  }

  // modify
  {
    const std::size_t base = session.recorder->eventCount();
    std::this_thread::sleep_for(5ms);
    writeText(songPath, "y");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Modified && event.filename == "歌曲-café-Ω.wav";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    const auto modified = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Modified && event.filename == "歌曲-café-Ω.wav";
    });
    ctx.check("非 ASCII 文件 Modified", modified >= 1, "Modified=" + std::to_string(modified));
  }

  // rename（同目录）
  {
    const std::size_t base = session.recorder->eventCount();
    fs::rename(songPath, renamedPath, ec);
    ctx.line("  [op] rename(" + song + " -> " + renamed + ") ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Moved && event.filename == "改名-ñ.mp3";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "非 ASCII 文件 rename");
    const bool ok = anyIf(events, [&](const EfswEvent& event) {
      return event.action == efsw::Actions::Moved && event.filename == renamed &&
             event.oldFilename == song;
    });
    ctx.check("非 ASCII rename：Moved(new, oldFilename=旧字节)", ok,
              "序列=" + sequenceOf(events, renamed));
  }

  // delete
  {
    const std::size_t base = session.recorder->eventCount();
    fs::remove(renamedPath, ec);
    ctx.line("  [op] rm " + renamed + " ec=" + ec.message());
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Delete && event.filename == "改名-ñ.mp3";
      });
    }, 3s);
    settleQuiet(*session.recorder, 150ms, 1500ms);
    const auto events = eventsSince(*session.recorder, base);
    const auto deletes = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Delete && event.filename == "改名-ñ.mp3";
    });
    ctx.check("非 ASCII 文件 Delete", deletes >= 1, "Delete=" + std::to_string(deletes));
  }
}

// ---- S8：尖峰风暴（批量创建/合并/竞态/溢出） -------------------------------

void scenarioS8(ScenarioCtx& ctx) {
  ctx.expect("批量创建 500 个文件：内核队列未溢出时事件全覆盖（Add 名单 == 创建名单）");
  ctx.expect("同一文件描述符连续写：相同 IN_MODIFY 可被内核合并（Modified 数可 << 写入次数）");
  ctx.expect("mkdir 后立即写入（递归 watch 注册前）：目录 Add 可见，内部文件事件可能缺失"
             "（LinuxProduceSyntheticEvents=OFF）");
  ctx.expect("handleMissedFileActions 仅在 IN_Q_OVERFLOW 时触发；本场景记录是否触发");

  const fs::path burstDir = ctx.root / "burst";
  fs::create_directories(burstDir);
  EfswSession session = startEfswSession(ctx, ctx.root, false);
  if (!session.started) {
    return;
  }

  {
    std::ifstream proc{"/proc/sys/fs/inotify/max_queued_events"};
    std::string value;
    std::getline(proc, value);
    ctx.note("内核 fs.inotify.max_queued_events=" +
             (value.empty() ? std::string("（读取失败）") : value));
  }

  constexpr int kFiles = 500;
  std::vector<std::string> created;
  created.reserve(kFiles);

  // Phase 1：批量创建（覆盖度）
  {
    const std::size_t base = session.recorder->eventCount();
    for (int i = 0; i < kFiles; ++i) {
      std::ostringstream name;
      name << "f" << std::setw(4) << std::setfill('0') << i << ".txt";
      created.push_back(name.str());
      writeText(burstDir / name.str(), "x");
    }
    ctx.line("  [op] 紧循环创建 " + std::to_string(kFiles) + " 个文件于 root/burst/");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      const auto adds = countIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename.size() > 4 &&
               event.filename.rfind("f", 0) == 0 && event.filename.ends_with(".txt");
      });
      return adds >= static_cast<std::size_t>(kFiles);
    }, 20s);
    settleQuiet(*session.recorder, 400ms, 4s);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "批量创建 500 文件");

    std::set<std::string> observedAdds;
    for (const auto& event : events) {
      if (event.action == efsw::Actions::Add) {
        observedAdds.insert(event.filename);
      }
    }
    std::vector<std::string> missing;
    for (const auto& name : created) {
      if (observedAdds.count(name) == 0U) {
        missing.push_back(name);
      }
    }
    const auto modifiedCount = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Modified;
    });
    ctx.check("批量创建事件全覆盖（Add 名单 == 500 个创建名）", missing.empty(),
              "缺失=" + std::to_string(missing.size()) +
                  (missing.empty() ? "" : ("（示例 " + missing.front() + "）")) +
                  "；Modified=" + std::to_string(modifiedCount));
    ctx.note("handleMissedFileActions 触发次数=" +
             std::to_string(session.recorder->missedDirs().size()) + "（0 = 内核队列未溢出）");
  }

  // Phase 2：单 fd 连续写同一文件（合并行为）
  {
    const fs::path hot = burstDir / "hot.txt";
    writeText(hot, "0");
    const std::size_t baseHot = session.recorder->eventCount();
    waitForEvents(*session.recorder, baseHot, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "hot.txt";
      });
    }, 3s);
    settleQuiet(*session.recorder, 250ms, 1500ms);

    const std::size_t baseWrites = session.recorder->eventCount();
    {
      std::ofstream out{hot, std::ios::binary | std::ios::trunc};
      for (int i = 0; i < 200; ++i) {
        out << "write-" << i << "\n";
        out.flush();
      }
    }
    settleQuiet(*session.recorder, 300ms, 2000ms);
    const auto events = eventsSince(*session.recorder, baseWrites);
    dumpEvents(ctx, session, baseWrites, "hot.txt 连续 200 次写入");
    const auto modified = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Modified;
    });
    ctx.check("连续写同一文件产生 Modified", modified >= 1,
              "Modified=" + std::to_string(modified));
    ctx.note("200 次连续写入 → Modified=" + std::to_string(modified) +
             "（< 200 即相同 IN_MODIFY 被内核合并的实测证据；IN_CLOSE_WRITE 另计）");
  }

  // Phase 3：mkdir + 立即写入（递归 watch 注册竞态）
  {
    const std::size_t base = session.recorder->eventCount();
    const fs::path raced = ctx.root / "racedir";
    fs::create_directories(raced);
    for (int i = 0; i < 100; ++i) {
      std::ostringstream name;
      name << "r" << std::setw(3) << std::setfill('0') << i << ".txt";
      writeText(raced / name.str(), "x");
    }
    ctx.line("  [op] mkdir root/racedir 后立即写入 100 个文件（无等待）");
    waitForEvents(*session.recorder, base, [](const std::vector<EfswEvent>& events) {
      return anyIf(events, [](const EfswEvent& event) {
        return event.action == efsw::Actions::Add && event.filename == "racedir";
      });
    }, 3s);
    settleQuiet(*session.recorder, 400ms, 3000ms);
    const auto events = eventsSince(*session.recorder, base);
    dumpEvents(ctx, session, base, "mkdir+立即写入竞态");
    const auto dirAdd = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename == "racedir";
    });
    const auto fileAdds = countIf(events, [](const EfswEvent& event) {
      return event.action == efsw::Actions::Add && event.filename.size() > 4 &&
             event.filename.rfind("r", 0) == 0 && event.filename.ends_with(".txt");
    });
    ctx.check("racedir 目录 Add 可见", dirAdd >= 1, "Add(racedir)=" + std::to_string(dirAdd));
    ctx.note("racedir 内文件 Add=" + std::to_string(fileAdds) +
             "/100（<100 属预期：watch 注册竞态 + LinuxProduceSyntheticEvents=OFF 时"
             "既有文件不补报；迁移设计需在目录 Add 上做子树枚举）");
  }
}

// ---- S9：停止/析构竞态 ------------------------------------------------------

void scenarioS9(ScenarioCtx& ctx) {
  ctx.expect("事件流中析构：析构有限时间返回，析构返回后无回调（无 use-after-free）");
  ctx.expect("removeWatch 生效后不再投递新回调");

  const fs::path live = ctx.root / "live";
  fs::create_directories(live);

  // Phase A：事件流中析构 FileWatcher
  {
    auto recorder = std::make_unique<EventRecorder>();
    auto watcher = std::make_unique<efsw::FileWatcher>();
    const std::string rootUtf8 = toUtf8(ctx.root);
    const efsw::WatchID watchid =
        watcher->addWatch(rootUtf8, recorder.get(), true, std::vector<efsw::WatcherOption>{});
    if (watchid < 0) {
      ctx.check("addWatch 成功（Phase A）", false, "watchid=" + std::to_string(watchid));
      return;
    }
    watcher->watch();
    std::this_thread::sleep_for(50ms);

    std::atomic<bool> stopProducer{false};
    std::atomic<int> produced{0};
    std::thread producer([&] {
      for (int i = 0; i < 500 && !stopProducer.load(); ++i) {
        std::ostringstream name;
        name << "a" << std::setw(4) << std::setfill('0') << i << ".txt";
        writeText(live / name.str(), "x");
        ++produced;
        if (i % 25 == 0) {
          std::this_thread::sleep_for(1ms);
        }
      }
    });

    std::this_thread::sleep_for(80ms);
    recorder->noteDestroyStarted();
    const std::size_t beforeDestroy = recorder->eventCount();
    const auto destroyStart = std::chrono::steady_clock::now();
    watcher.reset();  // 析构：join 读线程并清理 watches
    const auto destroyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - destroyStart)
                               .count();
    recorder->noteWatcherDestroyed();
    const int producedAtDestroy = produced.load();
    std::this_thread::sleep_for(250ms);
    stopProducer.store(true);
    producer.join();

    ctx.check("析构在 2000ms 内返回（无挂起）", destroyMs < 2000,
              "destructor=" + std::to_string(destroyMs) + "ms");
    ctx.check("析构返回后无回调（无 use-after-free）",
              recorder->callbacksAfterWatcherDestroyed() == 0,
              "post-destroy callbacks=" +
                  std::to_string(recorder->callbacksAfterWatcherDestroyed()));
    ctx.note("析构前事件=" + std::to_string(beforeDestroy) + "；析构时已创建文件=" +
             std::to_string(producedAtDestroy) + "；析构开始后到达回调=" +
             std::to_string(recorder->callbacksAfterDestroyStarted()) +
             "（析构开始后的事件允许部分投递，仅记录）");
  }

  // Phase B：removeWatch 停止投递
  {
    auto recorder = std::make_unique<EventRecorder>();
    auto watcher = std::make_unique<efsw::FileWatcher>();
    const std::string rootUtf8 = toUtf8(ctx.root);
    const efsw::WatchID watchid =
        watcher->addWatch(rootUtf8, recorder.get(), true, std::vector<efsw::WatcherOption>{});
    if (watchid < 0) {
      ctx.check("addWatch 成功（Phase B）", false, "watchid=" + std::to_string(watchid));
      return;
    }
    watcher->watch();
    std::this_thread::sleep_for(50ms);

    const fs::path live2 = ctx.root / "live2";
    fs::create_directories(live2);
    std::atomic<bool> stopProducer{false};
    std::atomic<int> produced{0};
    std::thread producer([&] {
      for (int i = 0; i < 400 && !stopProducer.load(); ++i) {
        std::ostringstream name;
        name << "b" << std::setw(4) << std::setfill('0') << i << ".txt";
        writeText(live2 / name.str(), "x");
        ++produced;
        if (i % 20 == 0) {
          std::this_thread::sleep_for(1ms);
        }
      }
    });

    waitForEvents(*recorder, 0, [](const std::vector<EfswEvent>& events) {
      return countIf(events, [](const EfswEvent& event) {
               return event.action == efsw::Actions::Add;
             }) >= 5;
    }, 3s);
    watcher->removeWatch(rootUtf8);
    settleQuiet(*recorder, 300ms, 1200ms);
    const std::size_t afterRemoval = recorder->eventCount();
    std::this_thread::sleep_for(300ms);
    stopProducer.store(true);
    producer.join();
    settleQuiet(*recorder, 300ms, 1200ms);
    const std::size_t finalCount = recorder->eventCount();
    const int producedCount = produced.load();
    watcher.reset();

    ctx.check("removeWatch 后无新回调（producer 继续写入被忽略）", finalCount == afterRemoval,
              "removeWatch+沉降后=" + std::to_string(afterRemoval) + "；最终=" +
                  std::to_string(finalCount) + "；producer 产生=" + std::to_string(producedCount));
  }
}

// ---- 矩阵编排（fork 子进程 + 看门狗） ---------------------------------------

using ScenarioFn = std::function<void(ScenarioCtx&)>;

struct MatrixSpec {
  std::string id;
  std::string title;
  ScenarioFn fn;
  int timeoutSeconds;
};

void runScenarioInChild(const MatrixSpec& spec, const std::string& logPath) {
  std::ofstream log{logPath, std::ios::trunc};
  ScenarioCtx ctx = makeScenarioCtx(spec.id, spec.title);
  ctx.log = &log;
  ctx.line("===== Gate 0：efsw 语义实测（efsw-migration 任务 4 / Wave 2）=====");
  ctx.line("scenario: " + spec.id);
  ctx.line("title   : " + spec.title);
  ctx.line("pid     : " + std::to_string(::getpid()));
  ctx.line("wall    : " + wallClockText());
  ctx.line("backend : efsw::FileWatcher 默认构造（Linux 构建为 inotify 后端）");
  ctx.line("temp    : " + toUtf8(ctx.base));

  spec.fn(ctx);

  cleanupScenario(ctx);
  ctx.line("");
  ctx.line("===== 场景结论 =====");
  ctx.line("checks: FAIL=" + std::to_string(ctx.failures) + " UNKNOWN=" +
           std::to_string(ctx.unknowns));
  const std::string verdict =
      ctx.failures > 0 ? "FAIL" : (ctx.unknowns > 0 ? "UNKNOWN" : "PASS");
  ctx.line("VERDICT: " + verdict);
  log.flush();
  const int code = ctx.failures > 0 ? 1 : (ctx.unknowns > 0 ? 2 : 0);
  std::_Exit(code);
}

std::vector<MatrixSpec> buildMatrix() {
  return {
      {"s1-dir-move-in", "目录移入（含既有子文件）",
       [](ScenarioCtx& ctx) { scenarioS1(ctx); }, 60},
      {"s2-file-move-in", "文件移入",
       [](ScenarioCtx& ctx) { scenarioS2(ctx); }, 60},
      {"s3-move-out", "文件/目录移出（Delete/级联/清理）",
       [](ScenarioCtx& ctx) { scenarioS3(ctx); }, 60},
      {"s4-in-root-rename", "根内 rename（文件/目录）",
       [](ScenarioCtx& ctx) { scenarioS4(ctx); }, 60},
      {"s5a-crossdir-move-off", "跨目录移动：ReportCrossDirectoryMoves=OFF",
       [](ScenarioCtx& ctx) { scenarioS5(ctx, false); }, 60},
      {"s5b-crossdir-move-on", "跨目录移动：ReportCrossDirectoryMoves=ON",
       [](ScenarioCtx& ctx) { scenarioS5(ctx, true); }, 60},
      {"s6-create-modify-delete", "create/modify/delete 常规",
       [](ScenarioCtx& ctx) { scenarioS6(ctx); }, 60},
      {"s7-utf8-paths", "UTF-8 路径（非 ASCII 根/文件名）",
       [](ScenarioCtx& ctx) { scenarioS7(ctx); }, 60},
      {"s8-burst-storm", "尖峰风暴（批量创建/合并/竞态/溢出）",
       [](ScenarioCtx& ctx) { scenarioS8(ctx); }, 120},
      {"s9-stop-race", "停止/析构竞态",
       [](ScenarioCtx& ctx) { scenarioS9(ctx); }, 90},
  };
}

struct HarnessResult {
  std::string id;
  std::string title;
  std::string logPath;
  std::string verdict;
  std::string exitDetail;
  long long durationMs{0};
};

HarnessResult runScenarioHarness(const MatrixSpec& spec, const fs::path& outDir) {
  HarnessResult result;
  result.id = spec.id;
  result.title = spec.title;
  result.logPath = toUtf8(outDir / ("scenario-" + spec.id + ".log"));

  const auto started = std::chrono::steady_clock::now();
  const pid_t pid = fork();
  if (pid == 0) {
    runScenarioInChild(spec, result.logPath);
    std::_Exit(127);
  }
  if (pid < 0) {
    result.verdict = "FORK_FAILED";
    result.exitDetail = "fork failed";
    return result;
  }

  int status = 0;
  bool timedOut = false;
  bool waitFailed = false;
  const auto deadline = started + std::chrono::seconds(spec.timeoutSeconds);
  for (;;) {
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      break;
    }
    if (waited < 0) {
      waitFailed = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      timedOut = true;
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();

  if (waitFailed) {
    result.verdict = "WAIT_FAILED";
    result.exitDetail = "waitpid failed";
  } else if (timedOut) {
    result.verdict = "HANG";
    result.exitDetail = "watchdog kill after " + std::to_string(spec.timeoutSeconds) + "s";
  } else if (WIFSIGNALED(status)) {
    result.verdict = "CRASH";
    result.exitDetail = "signal=" + std::to_string(WTERMSIG(status));
  } else if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    result.exitDetail = "exit=" + std::to_string(code);
    if (code == 0) {
      result.verdict = "PASS";
    } else if (code == 1) {
      result.verdict = "FAIL";
    } else if (code == 2) {
      result.verdict = "UNKNOWN";
    } else {
      result.verdict = "EXIT_" + std::to_string(code);
    }
  } else {
    result.verdict = "UNKNOWN_STATUS";
  }

  {
    std::ofstream note{result.logPath, std::ios::app};
    if (note) {
      note << "\n[harness] verdict=" << result.verdict << " " << result.exitDetail
           << " duration=" << result.durationMs << "ms\n";
    }
  }
  std::cout << "  " << std::setw(28) << std::left << result.id << " " << std::setw(8)
            << result.verdict << " " << std::setw(7) << std::right << result.durationMs
            << "ms (" << result.exitDetail << ")\n";
  return result;
}

void writeD4Comparison(const fs::path& outDir, const std::vector<HarnessResult>& results) {
  std::ofstream out{outDir / "d4-crossdir-move.txt", std::ios::trunc};
  if (!out) {
    return;
  }
  out << "===== D4 决策证据：跨目录移动 ReportCrossDirectoryMoves OFF vs ON =====\n";
  out << "生成时间: " << wallClockText() << "\n";
  out << "说明：以下为两次独立运行的完整原始日志（含事件流与场景结论），未做删改。\n";
  out << "构建: cmake -S . -B build/w2-t4 -DSERIONA_BUILD_TESTS=ON -DSERIONA_BUILD_TOOLS=ON;"
         " cmake --build build/w2-t4 -j8\n\n";
  const auto appendLog = [&](const std::string& label, const fs::path& path) {
    out << "----- " << label << "（" << toUtf8(path.filename()) << "）-----\n";
    std::ifstream in{path, std::ios::binary};
    if (in) {
      out << in.rdbuf();
    } else {
      out << "（日志缺失）\n";
    }
    out << "\n";
  };
  appendLog("S5a ReportCrossDirectoryMoves=OFF", outDir / "scenario-s5a-crossdir-move-off.log");
  appendLog("S5b ReportCrossDirectoryMoves=ON", outDir / "scenario-s5b-crossdir-move-on.log");
  out << "----- harness 判定 -----\n";
  for (const auto& result : results) {
    if (result.id.rfind("s5", 0) == 0) {
      out << result.id << ": " << result.verdict << " (" << result.exitDetail << ", "
          << result.durationMs << "ms)\n";
    }
  }
}

int runEfswMatrix(const fs::path& outDir) {
  std::error_code ec;
  fs::create_directories(outDir, ec);
  if (ec) {
    std::cerr << "无法创建输出目录 " << toUtf8(outDir) << ": " << ec.message() << "\n";
    return 2;
  }
  std::cout << "===== Gate 0：efsw 语义实测矩阵 =====\n";
  std::cout << "输出目录: " << toUtf8(outDir) << "\n";

  const auto specs = buildMatrix();
  std::vector<HarnessResult> results;
  results.reserve(specs.size());
  for (const auto& spec : specs) {
    results.push_back(runScenarioHarness(spec, outDir));
  }
  writeD4Comparison(outDir, results);

  std::cout << "\n===== 矩阵汇总 =====\n";
  std::size_t passCount = 0;
  for (const auto& result : results) {
    const bool pass = result.verdict == "PASS";
    passCount += pass ? 1 : 0;
    std::cout << "  " << std::setw(28) << std::left << result.id << " " << std::setw(8)
              << result.verdict << " " << std::setw(7) << std::right << result.durationMs
              << "ms " << result.exitDetail << "\n";
  }
  std::cout << "overall: " << passCount << "/" << results.size()
            << (passCount == results.size() ? " PASS" : " 存在 FAIL/HANG/CRASH") << "\n";
  std::cout << "d4: " << toUtf8(outDir / "d4-crossdir-move.txt") << "\n";
  return passCount == results.size() ? 0 : 1;
}

int runEfswNegative() {
  std::cout << "===== Gate 0 负向用例：不存在路径上的监视请求 =====\n";
  const fs::path missing = fs::temp_directory_path() /
                           ("seriona-gate0-negative-" + std::to_string(::getpid()) +
                            "/does-not-exist");
  std::error_code ec;
  fs::remove_all(missing.parent_path(), ec);
  const std::string missingUtf8 = toUtf8(missing);
  std::cout << "path: " << missingUtf8 << "\n";
  std::cout << "exists: " << (fs::exists(missing) ? "yes" : "no") << "\n";

  EventRecorder recorder;
  efsw::FileWatcher watcher;
  efsw::Errors::Log::clearLastError();
  std::cout << "call: addWatch(path, listener, recursive=true)\n";
  const efsw::WatchID watchid = watcher.addWatch(missingUtf8, &recorder, true);
  const efsw::Error errorCode = efsw::Errors::Log::getLastErrorCode();
  const std::string errorLog = efsw::Errors::Log::getLastErrorLog();
  const auto dirs = watcher.directories();
  std::cout << "return: watchid=" << watchid << "\n";
  std::cout << "efsw error code=" << static_cast<int>(errorCode)
            << " (FileNotFound=-1)\n";
  std::cout << "efsw error log=" << errorLog << "\n";
  std::cout << "registered watches after failure=" << dirs.size() << "\n";

  // efsw 1.7.2 的 Log::createLastError 只写文案、不更新 LastErrorCode（Log.cpp:22-60），
  // getLastErrorCode() 恒为 NoError；权威信号是 addWatch 的返回值（错误枚举取负）。
  const bool rejected = watchid < 0 && !errorLog.empty() && dirs.empty();
  if (!rejected) {
    std::cout << "VERDICT: FAIL（无效路径未被明确拒绝）\n";
    return 4;
  }
  std::cout << "VERDICT: PASS（watchid<0 + 明确错误文案 + 未注册 watch；无崩溃/无挂起）\n";
  std::cout << "note: getLastErrorCode()=0 为 efsw 1.7.2 API 缺陷（LastErrorCode 永不更新），"
               "迁移实现必须以 addWatch 返回值为准\n";
  std::cout << "note: 本用例按设计以非 0 退出码 3 结束（负向信号即预期结果）\n";
  return 3;
}

}  // namespace gate0

// ============================================================================
// 主流程（原有移出审计；无参数模式行为保持不变）
// ============================================================================

int runProductionAudit() {
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
  // 生产实现自动回填 ProductionTagMetadataReader 与真实 EfswFolderWatcherFactory
  // （file_scanner_orchestrator.cpp:1205），这正是本审计要用的真实监视器。

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
    // 方案 B：根内 rename -> renameSubtree 精准更新；真实监视器可能因
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
    // 竞态修复：不能只等"扫描开始"就立刻取快照——扫描开始 -> 完成入库之间存在窗口，
    // 快照可能读到 0 首而假 FAIL（C1 收尾验证复现 1 次）。改为循环等待直到判定条件
    // 实际成立：扫描确被触发（scanStartedCount 增量）且当前快照内 musicB 歌曲 >= 2；
    // 超时则以实际值判定 FAIL，真实回归不会被掩盖。
    const bool controlReady =
        waitUntil([&] {
                    if (log.scanStartedCount() <= beforeG.scanStarted) {
                      return false;
                    }
                    std::size_t musicBCount = 0;
                    for (const auto& node : service->snapshot().nodes) {
                      if (!node.song.has_value()) {
                        continue;
                      }
                      if (node.song->filePath.generic_string().find("musicB") !=
                          std::string::npos) {
                        ++musicBCount;
                      }
                    }
                    return musicBCount >= 2U;
                  },
                  kSceneTimeout);
    const auto afterG = log.baseline();
    const bool rescanned = log.scanStartedCount() > beforeG.scanStarted;
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
    std::cout << "  判定    : " << ((controlReady && rescanned && musicBInSnapshot >= 2U)
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
        //   真实监视器对 mv 出根会报告 file/other 事件 → 分类器回落全根重扫一次
        //   （scanner_efsw_integration_tests.cpp 接受 baseline+1），
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

  // ---------- 场景 11：单文件 mv 出根（精准删除） ----------
  // 单文件移出根外时父目录收到孤立 IN_MOVED_FROM（目标在根外 → 无 IN_MOVED_TO
  // 配对），监视器将其上报为 destroy 事件 → orchestrator destroyByKey 精准删除
  // （快照收敛 0 首、scan 不增长）。
  // 与场景 7/8/9/10（目录 mv，可能回落重扫）区分：单文件 + 短窗口精准语义，
  // scanStartedDelta 必须 == 0（destroy 精准删除，非对账/回落）。
  // 观察窗口 3s >> 监视/去抖延迟，但 << 60s 对账周期 → 收敛只能来自 destroy 精准删除。
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
  // 显式 stopWatching + stop：停止监视器并等待线程退出，
  // 之后再销毁服务与删除临时目录，避免监视器扫描未结束的目录。
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

// ============================================================================
// 入口分发
//   无参数                      → 原有移出审计（11 场景）
//   --efsw-matrix <输出目录>    → Gate 0 efsw 语义矩阵（scenario-*.log + d4）
//   --efsw-negative             → 无效路径负向用例（明确报错 + 非 0 退出）
// ============================================================================

int main(int argc, char** argv) {
  if (argc <= 1) {
    return runProductionAudit();
  }
  const std::string first = argv[1];
  if (first == "--efsw-matrix") {
    if (argc != 3) {
      std::cerr << "用法: seriona_watch_root_move_audit --efsw-matrix <输出目录>\n";
      return 2;
    }
    return gate0::runEfswMatrix(fs::path{argv[2]});
  }
  if (first == "--efsw-negative") {
    if (argc != 2) {
      std::cerr << "用法: seriona_watch_root_move_audit --efsw-negative\n";
      return 2;
    }
    return gate0::runEfswNegative();
  }
  if (first == "--help" || first == "-h") {
    std::cout << "用法:\n"
              << "  seriona_watch_root_move_audit                     原有移出审计（11 场景）\n"
              << "  seriona_watch_root_move_audit --efsw-matrix DIR   Gate 0 efsw 矩阵"
                 "（日志写 DIR/scenario-*.log）\n"
              << "  seriona_watch_root_move_audit --efsw-negative     无效路径负向用例"
                 "（明确报错 + 非 0 退出）\n";
    return 0;
  }
  std::cerr << "未知参数: " << first << "（--help 查看用法）\n";
  return 2;
}
