# Seriona 文件监视「目录移出监视根」修复方案

**文档日期**：2026-08-09
**关联文档**：`docs/folder-watch-audit-2026-08-08.md`（根因审查）、`/tmp/opencode/seriona-watch-audit-report.md`（6 次实验证据，审计程序 `tools/watch_root_move_audit.cpp`）
**性质**：修复方案设计（含网络调研佐证）；**已按本方案实施（波 1-4 提交 856c1a3/e1d7e43/a5d4da7/24186ef，波 5 实施中），实施状态见文首「实施状态标注」**
**目标**：**最优、且不影响其他功能正常运作**地修复"将监视根下整个子文件夹移出后，播放列表不更新"

---

## 实施状态标注（2026-08-10 追加）

> 本文档为 2026-08-09 的修复方案设计。后续按计划 `watcher-move-out-fix`（方案 B：事件驱动精准增量）执行实施，本标注记录实施进度与剩余二期项，避免误读为"尚未实施"。

### 已实施（波 1-4，已提交；波 5 进行中）

- **波 1（L1 事件层）**：vendored `watcher.hpp` 转发 `IN_MOVE_SELF`（`is_self_info` 移除该位，注释 `// Seriona patch: forward IN_MOVE_SELF (upstream #122)`）；orchestrator `pathChangeRequestsScan` 放行 `WatchEffectKind::Other`（IN_MOVE_SELF 映射）；`WatchRuntimeState` 保留完整 `WatchEvent`（含 path/pathKind/effectKind/associated）入队，容量上限 1024、溢出置回落标记，入队 bump `dirtyGeneration`+`notify_one`。
- **波 2（SQLite 精准 API）**：`deleteLocationsByPathPrefix`（含 cue `source_file_path` 双维度）+ `replaceLocationsBySubtree`（rename=删旧+前缀改写重 upsert，同事务）；不动 schema `user_version=3`。
- **波 3（树补丁）**：`PlaylistTreeBuilder` 长生命周期成员 + 首次全量建树种子化；`removeSubtree`/`renameSubtree`/`upsertSong`。
- **波 4（编排 + 兜底）**：事件分类器（rename 对 / IN_MOVE_SELF / create / modify / destroy × file/dir）+ 批内去重归并 + 根自身防护 + 先树后库 + 每批发布完整快照（控制层整树替换不变）；**C1 消息匹配修复**（`w/`、`e/`、`e@` + 保留 warning/error 子串、显式排除 `s/`）；**周期对账**（`reconcileInterval` 默认 60s 可注入，目录树哈希探测仅变化才重扫、无变化零发布）。
- **波 5（进行中，未提交）**：5.1 真实 wtr 集成测试、5.2 审计场景判定翻转、5.3 DESIGN.md + 本文档更新。

### 二期未做（记录在案，不在一期）

- **C2 队列满合并**（pending 标记合并，一期仅溢出回落标记）。
- **C3 ENOSPC / watch 注册失败降级**（C1 已令 `w/sys/not_watched@` 触发全根重扫，降级日志未加）。
- **C4 根内路径过滤**（残留 watch 空转由分类器"路径磁盘不存在→丢弃幽灵事件"隐含覆盖，未做独立过滤）。
- **C5 残留 watch 释放**（移出目录 inode watch 泄漏治理，需 wtr 暴露 wd 清理接口）。
- **C6 根自身被移出**（仅加防护不实现重建 watch）。
- **fanotify A3**（非 root 环境不生效，未做）。
- **上游 PR**（vendored 补丁仅本地标注，未向上游提交）。

### 与本文档早期设计的差异（实施时按计划修正）

- 原设计（§四）"IN_MOVE_SELF 触发一次重扫"升级为**事件驱动精准增量**：分类器对可精准定位的批次做路径级更新（树补丁 + SQLite 精准 API，不触发扫描），全根重扫仅在无法分类 / watcher 消息 / 队列溢出 / 移入含未扫描文件新目录时回落。
- 场景 10（纯静默 mv 出根）判定由"PASS（残留，确认复现）"翻转为"精准删除、快照收敛、scan 不增长"——详见 `docs/folder-watch-audit-2026-08-08.md` 文首标注与 `tools/watch_root_move_audit.cpp` 判定更新。

---

## 一、背景与问题定义

### 1.1 用户复现（已实验确证）

用户操作：将监视根下某个子文件夹整体 `mv` 到监视根外，之后不做任何操作 → 播放列表不更新。

审计实验（`tools/watch_root_move_audit.cpp`，6 次运行一致）结论链：

| 环节 | 实测结果 |
|------|----------|
| mv 出根动作本身 | **零事件**（孤立 `IN_MOVED_FROM` 进 fae 缓冲等不到 `IN_MOVED_TO`，回调被 `err_pending` 抑制；fanotify 单边事件 `ec=2` 丢弃） |
| mv 后根外写入 | 仍触发重扫（inotify watch 绑定 **inode**，移出目录的递归 watch 存活）→ 该重扫使快照**自愈**为根内真实内容 |
| **纯静默 mv（用户真实场景）** | **零事件、零扫描、快照残留已移出歌曲**（场景 10，两次稳定 PASS）——播放列表不更新 |

### 1.2 根因链

```
mv 子目录出根
  └─ 根 watch 收孤立 IN_MOVED_FROM（cookie 永不配对）→ err_pending → 回调被抑制（watcher.hpp:1564-1573, 1720-1721）
  └─ 移出目录自身 watch 收 IN_MOVE_SELF → 被 is_self_info 过滤，不派发（watcher.hpp:1689）
  └─ 无任何事件 → dirtyGeneration 不变 → debounce 不触发 → 扫描不跑 → 快照残留
```

其余传播链路（diff→快照→缓存剔除）经实验与 e2e 测试确认**正确**，断点只在"事件获取"一环。

### 1.3 上游状态（网络调研确认）

- 本仓库 vendored 的 `watcher.hpp` 与上游 **release/0.14.5 逐字节一致**（最新版）。
- 本 bug 与上游 **issue #122**（2026-05-07，仍 open）同源：「Files moved outside the watched tree are silently dropped — no destroy event fired」，官方无修复。
- 上游历史教训：**每次改动 rename 配对逻辑都引入回归**（#73→#89→#105 链条，0.13.6~0.14.4 一路修一路漏）——因此本方案**回避**修改 fae 配对逻辑。

---

## 二、修复目标与约束

### 2.1 目标（按优先级）

1. **正确性**：mv 出根（含纯静默场景）后，播放列表在一个可接受的时限内更新为根内真实内容。
2. **低回归风险**：不影响文件 create/modify/delete、根内 rename、多根、手动扫描、队列串行化等既有已验证功能。
3. **可维护**：vendored 库升级路径清晰（补丁最小、可向上游提交）。
4. **可验证**：每个改动有对应测试/实验门禁（复用现有审计程序）。

### 2.2 约束

- 生产代码禁 Qt（Seriona_Backend 仓库规则）；面向用户文档中文。
- 不引入新依赖；不改公共契约（`scanner_contracts.h` / `file_scanner_service.h` 稳定边界）。
- 不动 `SQLiteCache` schema（v3 固定）。
- 事件链路（回调→generation→debounce→scan）整体结构不动。

---

## 三、方案总览：三层修复架构

业界共识（syncthing / watchexec / jellyfin / Bun / fsnotify / notify-rs 全部采用）：**事件层修复（低延迟）+ 周期对账（兜底正确性）+ watch 生命周期治理（防泄漏/防空转）**。三者缺一不可：

| 层 | 机制 | 解决的问题 | 对应本方案 |
|----|------|-----------|-----------|
| L1 事件层 | 让"mv 出根"本身产生事件 | 实时发现移出（用户可感知的即时性） | **方案 A：IN_MOVE_SELF 转发补丁**（§四） |
| L2 兜底层 | 周期对账（stat 级比对） | 覆盖**所有**静默失效：事件黑洞、队列满、消息格式误判、watch 死亡、ENOSPC | **方案 B：周期 reconcile**（§五） |
| L3 治理层 | 根内过滤 + 残留 watch 释放 + 错误降级 | 防空转重扫、防 `max_user_watches` 耗尽、失效可见 | **方案 C：加固项**（§六） |

> **为什么不能只做 L1 或只做 L2**：
> - 只做 L1：L2 的调研结论（jellyfin #16874 实证）——任何单点静默失效（如 Docker 下 watch 上限耗尽、消息格式误判）都会再次造成"永久偏离"，且 L1 无法覆盖。
> - 只做 L2：mv 出根后最长要等一个对账周期才更新（可接受但不实时）；且 L1 补丁极小，做了不亏。

---

## 四、L1 事件层修复（主修复）：IN_MOVE_SELF 转发

### 4.1 机制

Linux 内核语义（man7 inotify(7)）：被监视对象**自身被 rename**（同文件系统任意位置，包括移出监视树）时，其 inode 绑定的 watch 必产生 `IN_MOVE_SELF`。递归监视下，根内每个子目录都有独立 watch：

- **mv 子目录出根 → 该目录的 watch 必收 IN_MOVE_SELF**（确定性事件，无 cookie、无配对、无超时窗口）
- 事件路径 = 移出目录的**旧路径**（仍在根内，或可仅用其触发重扫）
- orchestrator 事件链路本就**丢弃路径、只计 generation**（`collectActionableWatcherEvent` + `dirtyGeneration`，`file_scanner_orchestrator.cpp:894-956`）——**只要事件被派发，重扫即触发，orchestrator 侧改动极小**

### 4.2 改动点

**A1. vendored `watcher.hpp`（inotify 适配器）——过滤条件一行改动**

当前（`watcher.hpp:1685-1691`）：
```cpp
auto is_parity_lost = [](unsigned msk) -> bool
{ return msk & IN_DELETE_SELF && ! (msk & IN_MOVE_SELF); };
auto is_real_event = [](unsigned msk) -> bool
{
  bool has_any = msk & ke_in_ev::recv_mask;
  bool is_self_info = msk & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF);
  return has_any && ! is_self_info;
};
```

改为把 `IN_MOVE_SELF` 移出 `is_self_info`（保留 `IN_IGNORED | IN_DELETE_SELF` 过滤）：
```cpp
bool is_self_info = msk & (IN_IGNORED | IN_DELETE_SELF);   // IN_MOVE_SELF 不再被吞
```

`recv_mask` 已包含 `IN_MOVE_SELF`（`watcher.hpp:1393`），**无需改 mask**。`is_parity_lost` 已区分 `IN_DELETE_SELF`/`IN_MOVE_SELF`，不受影响。派发路径中 `IN_MOVE_SELF` 会被映射为 rename 类事件并携带旧路径（wd 映射）——需在实现时确认 `parse_ev` 对 `IN_MOVE_SELF` 的类型映射（预期：走 `is_rename`/Renamed 分支或单独标记，见 §8 验证）。

**A2. orchestrator `watchEventFrom` / `pathChangeRequestsScan`**

- 确认 `IN_MOVE_SELF` 映射后的 `WatchEventType` 能被 `pathChangeRequestsScan` 判定为 actionable（预期 true）。若映射为 Unknown 类型，则在 `pathChangeRequestsScan` 中对 Self/Renamed 类型放行（改动 ≤3 行）。
- **事件路径无需任何使用**（链路本就丢弃路径）。

**A3. fanotify 适配器（root 场景，可选）**

`FAN_MOVE_SELF`（内核 5.1+）语义同 inotify。当前 fanotify 适配器对单边 MOVED_FROM 直接 `ec=2` 丢弃（`watcher.hpp:1219-1223, 1288-1289`）。同 A1 思路：确认 `FAN_MOVE_SELF` 是否被过滤，若过滤则放行。非 root 环境不生效，**不作为主验证路径**。

### 4.3 为什么这是最优选型（vs 候选方案）

| 候选 | 机制 | 风险 | 判定 |
|------|------|------|------|
| **A. IN_MOVE_SELF 转发**（推荐） | 内核保证的确定性事件，无配对、无超时 | 极低：仅影响"被监视目录自身被 rename"（原本静默）场景；根内 rename 会额外触发一次**幂等**重扫（快照全量重建，结果相同） | **采用** |
| 超时配对（上游 #122 方向） | fae 条目加时间戳，超时（如 1s）后按 destroy 上报 | 高：上游 #73/#89/#105 历史证明每次改 fae 配对均引入回归；需改 fae 结构；存在"树内 rename 被误判"窗口 | 不采用（作上游 PR 参考） |
> **覆盖更新（2026-08-11，`wtr-fae-flush`）**：本行"不采用"判定已被后续 `wtr-fae-flush` 计划显式覆盖——该计划采纳"fae 条目加时间戳 + 超时 flush"方案（约 100ms，非 1s；超龄判定 `>=`，仅清 cookie 不以 destroy 上报非配对槽），但**不碰配对核心**（相邻/环缓冲配对不动），故不引入 #73/#89/#105 类回归；树内 rename 慢配对由超龄判定规避，误 flush 极端场景由既有 60s 对账与回落重扫自愈。
| 仅靠 L2 对账 | 不修事件层 | 无事件层风险，但非实时 | 与 A 并存（L2 本就必做） |
| 事件层定期重扫触发（polling 化） | 丢弃 inotify，改轮询 | 高：重写监视架构，违背"不影响其他功能" | 不采用 |

**回归面分析（A 方案）**：
- 文件级 create/modify/delete：不受影响（IN_MOVE_SELF 只在目录自身被 rename 时产生）。
- 根内目录 rename（sub→sub2）：原本产生 MOVED_FROM+TO 配对事件；现在**额外**产生一次 IN_MOVE_SELF 事件 → 两次重扫 → 第二次幂等（快照相同，不发布变化）→ 仅多一次扫描开销，行为结果不变。
- 手动扫描 / 多根 / 队列串行化：与事件层正交，不受影响。
- 目录删除（rmdir）：产生 IN_DELETE_SELF + IN_IGNORED，仍被过滤（保留 `IN_DELETE_SELF` 在 `is_self_info`）——已有 IN_DELETE 事件负责删除场景，行为不变。

### 4.4 补丁维护策略

- vendored 补丁**仅此一处**（A1）+ 可能 1 处 orchestrator 类型放行（A2）。
- 补丁以注释标注 `// Seriona patch: forward IN_MOVE_SELF ... (upstream #122)`，便于升级 diff。
- 同步向上游提交 PR（e-dant/watcher #122 关联），上游合并后可移除本地补丁。

---

## 五、L2 兜底层（必做）：周期对账 reconcile

### 5.1 设计

参照 syncthing（watcher 开启时 1-60min 周期 + 溢出即全量重扫）与 jellyfin（实时监视 + 计划扫描双轨）的业界共识：

- **机制**：在 `OrchestratedFileScannerService` 增加一个对账定时器（复用现有 `scanWorker` 线程/生命周期，不新增线程；或独立 `std::jthread`），周期默认 **60s**（配置项 `reconcileInterval`，0=禁用）。
- **动作**：周期触发时对全部 `watchedRoots` 调用现有 `scan(roots, ScanMode::Incremental)`——该路径已内置 `computeDirectoryTreeHash` 比对（`decideScanMode`，`file_scanner_orchestrator.cpp:256-274`）：**树 hash 未变 → 无文件扫描、无快照发布（零副作用）；hash 变化（含 mv 出根、外部移动）→ 正常 diff 重扫 → 快照收敛**。复用现有代码，无第二套 diff 逻辑。
- **与事件驱动的关系**：叠加而非替换。事件触发仍即时；对账是兜底。两者经现有 `scan()` 串行化队列天然互斥。
- **溢出/满队列联动**：`scan()` 队列满时（`file_scanner_orchestrator.cpp:1019, 1025-1035`）现有行为是拒绝并发 ScanError；对账周期不会因单次拒绝而丢失（下一周期再试）——从机制上消除了"事件丢失后永久偏离"。

### 5.2 改动点

| 位置 | 改动 |
|------|------|
| `file_scanner_service_internal.h` | `FileScannerServiceDependencies` 增加 `std::chrono::milliseconds reconcileInterval{60s}`（默认值）；`FileScannerService` 接口**不加**新虚方法（公共契约零变化） |
| `file_scanner_orchestrator.cpp` | 服务启动时（startWatching 成功后）启动对账定时器；停止时取消；触发逻辑：`scan(currentRoots, ScanMode::Incremental)`；实现用 `std::jthread` + `condition_variable`（与 `debounceThread_` 同构） |
| `file_scanner_orchestrator.cpp` | 对账仅统计日志（如 `reconcile: tree hash unchanged`），不引入新事件类型（契约不变） |

### 5.3 回归风险

- 极低：新增独立定时路径，不触碰事件回调、watcher、缓存 schema；无变化时零发布（不触发前端刷新，F4 抖动不加剧）。
- 周期扫描与手动/事件扫描共用 `scan()` 串行化队列——已有测试覆盖（`scanner_watcher_tests.cpp:396-432` 手动与 watcher 扫描串行化）。

---

## 六、L3 治理层（加固项，按风险排序）

### C1. 消息格式修复（低风险、低成本、消除一类静默失效）

`watcherMessageRequestsRootReconciliation`（`file_scanner_orchestrator.cpp:865-869`）匹配 `starts_with("w_")`，库真实格式为 `"w/"`（`watcher.hpp:902-929`）。改为同时匹配 `w/`、`e/`、`e@`；同步修正测试 `scanner_watcher_tests.cpp:296` 的消息格式。收益：`w/sys/not_watched@`（watch 注册失败/上限耗尽）、`w/sys/partial@` 等警告从"静默吞掉"变为"触发全根重扫"。

### C2. 队列满合并（中风险）

`scan()` 队列满（容量 16）拒绝请求且不重试（`file_scanner_orchestrator.cpp:1019, 1025-1035`）。改为"pending 标记合并"：队列满时置 `pendingRescan` 标志，队空后补扫一次。行为语义：从"丢请求"变"合并请求"，更安全；需同步调整队列满测试。

### C3. ENOSPC / watch 注册失败降级（低风险）

`inotify_add_watch` 失败（上限耗尽）→ 现仅发 `w/sys/not_watched@` 且被 C1 修复前吞掉。修复 C1 后该消息触发全根重扫；在此基础上增加：收到 `w/sys/not_watched@` 时打 **warn 日志 + 保持对账兜底**（L2 已覆盖正确性），必要时提示用户调 `fs.inotify.max_user_watches`。参照 jellyfin #16874 教训（静默失败 = 灾难）。

### C4. 根内路径过滤（低风险，防空转）

`collectActionableWatcherEvent`（`file_scanner_orchestrator.cpp:894-907`）对 Path 类型事件不做根内检查 → 残留 watch（移出目录）的每次根外写入都触发一次内容不变的全量重扫。增加：事件路径前缀不在任何 `watchedRoots` 内 → 不置 actionable（仅丢弃，不计 generation）。参照 syncthing `unrootedChecked` 模式。**注意**：IN_MOVE_SELF 事件路径为旧路径（在根内），不受此过滤影响；过滤只挡根外残留事件。

### C5. 残留 watch 释放（中风险，长期防泄漏）

移出目录的 inode watch 永不释放（`max_user_watches` 长跑耗尽）。参照 **Bun PR #33396** 模式：收到目录级孤立 `IN_MOVED_FROM`（A1 转发后 IN_MOVE_SELF 亦可触发）→ 对子树 wd 逐个 `inotify_rm_watch`。需要 wtr 暴露 wd 清理接口（补丁第二处）。**实施优先级最低**：C4 过滤已消除空转开销，泄漏只在"海量目录反复移出"的极端场景成为问题；且此改动触及 wtr watch 生命周期，需最严格回归。**本方案一期不实施**，仅记录为后续项。

### C6. 监视根自身被移出（边界）

用户将**监视根本身** mv 出（罕见）：根 watch 收 IN_MOVE_SELF（A1 转发后）→ 触发重扫 → 根路径已不存在 → RootUnavailable（如实报错）。TLPI 标准做法是"根自身 IN_MOVE_SELF → zap 子树 + 上报上层"。一期行为（触发重扫+报错）可接受，L2 对账兜底；后续可加"根失联时重新 stat 并尝试重建 watch"（对应审查文档 A4/B2）。

---

## 七、实施清单与验证门禁

### 7.1 实施顺序

| 步骤 | 内容 | 依赖 |
|------|------|------|
| 1 | **L1-A1/A2**：IN_MOVE_SELF 转发补丁（wtr 一行 + orchestrator 类型放行） | 无 |
| 2 | **L2**：周期对账 reconcile（60s） | 无 |
| 3 | **C1** 消息格式修复 + 测试修正 | 无 |
| 4 | **C3** ENOSPC 降级日志 | C1 |
| 5 | **C2** 队列满合并 | 无 |
| 6 | **C4** 根内路径过滤 | 无 |
| 7 | C5/C6 后续项（不在一期） | C4 稳定后评估 |

### 7.2 验证门禁（每步必须通过）

1. **回归**：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build -j<N> && ctest --test-dir build --output-on-failure`（全量，重点 `seriona\.scanner`、`seriona\.control` 分组）。
2. **审计实验**（`tools/watch_root_move_audit.cpp`，构建后运行 2 次）——**波 5.2（todo 13）已完成**：判定按方案 B 语义更新并验证（证据 `.omo/evidence/task-13-watcher-move-out-fix.txt`）：
   - 场景 1/3/4/5 仍 **PASS**（判定不变，基于歌曲数变化；方案 B 下文件 create/delete 为精准更新、目录 create 回落重扫，歌曲数均正确变化）；
   - 场景 2（modify）改为 **PASS（upsertSong 精准更新：歌曲数不变 + scan 不增长）**——旧文案"必须新 ScanStarted"不再适用；
   - 场景 6（根内 rename）改为 **PASS（renameSubtree 精准更新/有界回落：路径收敛、旧路径无残留、歌曲数不变、scan ≤1 次）**；
   - 场景 7/8/9（mv 出根 + 根外继续写）改为 **PASS（精准删除/收敛 + 幽灵事件丢弃：快照 0 首、旧路径无残留、scan 不增长）**；
   - 场景 10（纯静默 mv）翻转为 **PASS（快照 0 首 + scan ≤1 次）**：mv 出根后零操作，快照自愈删除、无残留——用户复现修复的直接证据；
   - 判定依据 = ScanStarted 增量（before/after 基线对比、排除 setup 回落）+ 快照 version/歌曲数 + 残留路径断言；真实 wtr 对根内 rename/mv 出根会报告 file/other 事件触发一次回落重扫（集成测试接受 baseline+1），故 scan 增量≤1 判 PASS、>1 或未收敛判 FAIL（不掩盖真实失败）；
   - 实测两次运行：**全部场景 PASS、无 FAIL、无 UNKNOWN**（退出码 0）。
3. **新增 CTest**（步骤 1 后随代码）：真实 wtr 集成用例"建子目录→rename 移出→断言快照在 reconcile/事件下收敛"（收编审计程序核心场景，对应审查文档 R5）；周期对账用例（fake 时钟或短周期配置，验证无变化不发布、有变化收敛）。
4. **前端联调**（跨工作区，Seriona）：完整应用下重复用户操作（移走整个子文件夹，等待 ≤ 对账周期或即时），确认播放列表更新、无崩溃、无滚动异常。

### 7.3 回滚

- L1 补丁为一行级，可瞬时还原；L2 为配置项（`reconcileInterval=0` 禁用）；C1-C4 相互独立，均可单独 revert。
- 每步独立提交，commit message 中文，标注关联 issue（如 `watcher: 转发 IN_MOVE_SELF 修复目录移出静默（#122 同源）`）。

---

## 八、待实现时确认的细节（避免文档误导）

1. `parse_ev` 对 `IN_MOVE_SELF` 的类型映射：确认走 rename 分支或需新增类型（影响 A2 的 `pathChangeRequestsScan` 放行判断，预计 ≤3 行）。
2. 周期对账定时器的宿主选择：复用 `debounceThread_` 轮询（已有 50ms 唤醒，可叠加 60s 节拍，**零新增线程**）优先；独立线程为备选。若复用，需确认 `debounceLoop` 的 `changed.wait_for` 循环可容纳双条件。
3. `scan(Incremental)` 在树 hash 未变时确认**零发布**（避免对账周期内前端无谓刷新）；若现有实现无变化也发布快照，则对账触发条件改为"hash 变化才 scan"。
4. fanotify 适配器 `FAN_MOVE_SELF` 处理现状（非 root 不生效，A3 优先级低）。

---

## 九、预期效果汇总

| 用户可见行为 | 修复前 | 修复后（一期） |
|-------------|--------|---------------|
| 移走整个子文件夹，无后续操作 | 播放列表永久残留（用户 bug） | **事件层即时自愈（IN_MOVE_SELF，毫秒-秒级）**；即使事件层失效，对账 60s 内收敛 |
| 移走后根外继续写文件 | 空转重扫（内容不变） | 事件层已自愈 + C4 过滤空转 |
| 长期反复移出目录 | watch 泄漏→上限耗尽→静默失监视 | C5（二期）释放；C1+C3 使失效可见并由 L2 兜底 |
| 文件增删改/根内 rename/多根/手动扫描 | 正常 | **零回归**（对照组实验+CTest 门禁背书） |
| 前端播放列表 | 不更新 | 正常更新（快照驱动，链路未动） |

---

## 十、参考来源（网络调研）

**wtr::watch 上游**
- issue #122（本 bug 同源，open）：https://github.com/e-dant/watcher/issues/122
- issue #73/#89/#105/#109（fae/配对历史回归）：https://github.com/e-dant/watcher/issues/73 / #89 / #105 / #109
- changelog（0.13.6~0.14.5）：https://github.com/e-dant/watcher/blob/release/changelog.md
- 本地 vendored 0.14.5 与上游逐字节一致（已 diff 验证）

**inotify 语义与孤立 MOVED_FROM 处理**
- man7 inotify(7)（IN_MOVE_SELF/IN_IGNORED、Dealing with rename() events）：https://man7.org/linux/man-pages/man7/inotify.7.html
- LWN 605128（Kerrisk：孤立 MOVED_FROM 按删除处理 + rm_watch 子树）：https://lwn.net/Articles/605128/
- TLPI inotify_dtree.c（IN_MOVE_SELF → zap 子树参考实现）：https://man7.org/tlpi/code/online/book/inotify/inotify_dtree.c.html
- SO 20300955（cookie 超时队列→按删除）：https://stackoverflow.com/questions/20300955/

**业界工程实践**
- Bun PR #33396（移出目录 wd 释放模板）：https://github.com/oven-sh/bun/pull/33396
- fsnotify #172/#203/#518（移出目录追踪限制、watch 清理）：https://github.com/fsnotify/fsnotify/issues/172 等
- notify-rs #165/#700/#720（移出即自动 unwatch、未知 wd 丢弃）：https://github.com/notify-rs/notify/issues/165 等
- syncthing（watcher+周期重扫、溢出→全量重扫、unrootedChecked 过滤）：https://docs.syncthing.net/users/syncing ；https://github.com/syncthing/syncthing/blob/main/lib/fs/basicfs_watch.go
- watchexec（poll 兜底 30s/上限降级 1000ms、GlobsetFilterer）：https://github.com/watchexec/watchexec/pull/93
- jellyfin #5547/#11959/#16874（媒体库移出场景与计划扫描兜底）：https://github.com/jellyfin/jellyfin/issues/16874 等
- 内核 max_user_watches 自适应（commit 9289012）：https://github.com/torvalds/linux/commit/92890123749bafc317bbfacbe0a62ce08d78efb7
- fanotify（5.13 非特权限制、FAN_RENAME 单事件、FAN_MOVE_SELF）：https://man7.org/linux/man-pages/man2/fanotify_init.2.html ；https://lwn.net/Articles/876495/
