# 文件夹监视与曲库更新链路审查报告

**审查日期**：2026-08-08
**审查范围**：Seriona_Backend（文件夹监视/曲库/快照发布链路）+ Seriona（前端消费链路）
**审查方式**：3 路并行独立审查（watcher 事件实现 / 曲库快照链路 / 前端消费链路），结论交叉验证一致
**触发背景**：使用完整 Seriona 应用时，将监视根下某个子文件夹移出监视根后，播放列表没有更新
**结论性质**：仅审查与文档，未修改任何代码；2026-08-08 追加真实 watcher 实验验证（`tools/watch_root_move_audit.cpp`，见「六、实验验证」），核心结论据此修正

---

## 修复后结论更新（2026-08-10 追加）

> 本文档为修复前的审查与实验记录。修复方案 `watcher-move-out-fix`（波 1-4 已提交 856c1a3/e1d7e43/a5d4da7/24186ef，波 5 实施中）实施后，下方「六、实验验证」的结论需按最新行为更新；历史实验数据保留作修复前基线。

**核心翻转：场景 10（纯静默 mv 出根）不再静默，精准删除收敛。**

- **修复前**：`mv` 出根动作零事件（孤立 `IN_MOVED_FROM` 被 `err_pending` 抑制）、零扫描，快照残留已移出歌曲（场景 10 两次 PASS"残留"= 用户复现确证）。
- **修复后**：`IN_MOVE_SELF` 经 vendored `watcher.hpp` 补丁转发（原被 `is_self_info` 过滤），orchestrator 放行并完整入队；事件分类器判定 IN_MOVE_SELF 且旧路径在磁盘不存在 → **精准删除**（树 `removeSubtree` + SQLite `deleteLocationsByPathPrefix`），**不触发全根扫描（scan 不增长）**，快照与 `loadLocationsByRoot` 收敛。
- **场景 7/8/9 更新**：mv 出根后向移出目录继续写入（残留 watch 产生的旧路径 create/modify 事件）→ 分类器校验路径磁盘不存在 → **幽灵事件丢弃**，不产生幽灵条目、旧路径无残留（判定语义由"PARTIAL（事件到达但内容未变）"改为"精准删除/收敛 + 幽灵事件丢弃"）。
- **场景 2/6 更新**：文件 modify → `upsertSong` 精准更新（scan 不增长）；根内目录 rename → `renameSubtree` 精准更新（旧路径无残留、scan 不增长）——不再要求新 ScanStarted。
- **场景 1/3/4/5 判定不变**（对照组零回归）。
- 审计程序 `tools/watch_root_move_audit.cpp` 判定分支与文案按上述语义更新（对应计划 todo 13）；`DESIGN.md` §7.2 已更新为"事件驱动精准增量 + 对账兜底"新运行流程。

**单文件移出更新（2026-08-11 追加，wtr-fae-flush）**：

> 上述 `watcher-move-out-fix` 覆盖的是"目录移出根"（IN_MOVE_SELF 转发）；**单文件 mv 出根不在其覆盖范围**（wtr 只对目录加 watch，原结论为"单文件移出零事件、只能靠 60s 对账回落"）。该过期结论已由后续 `wtr-fae-flush` 计划修正：孤立 IN_MOVED_FROM 经 **fae flush ~100ms** 超时（efsw 同款、上游 #122 同源方案）以 destroy 事件即时转发，orchestrator 精准删除，单文件移出监视根**即时感知**（实测约 100-200ms 收敛、scan 增量 0），不再依赖 60s 对账兜底。

---

## 一、架构总览

文件系统监视基于 vendored 第三方库 `wtr::watch`（`third_party/watcher/include/wtr/watcher.hpp`，单头文件）。事件链路：

```
wtr::watch(根) ──回调──> enqueueWatcherEvent ──> pendingWatcherMessages + dirtyGeneration
                                                      │
               file_scanner_orchestrator.cpp:946-958   ▼
                                debounceLoop (50ms)  ──> scan(全部根, Incremental)
                                                      │
                                                      ▼
              decideScanMode(hash) ──> reconcileRoot ──> 快照全量重建 ──> PlaylistSnapshotUpdated
                                                      │
                                                      ▼
              control 层整体替换 libraryTree ──> LibraryStateSnapshot ──> 前端
                                                      │
                                                      ▼
              LibraryController(仅 libraryTree 存在时) ──> 模型全量 reset ──> ListView 刷新
```

- 监视抽象 seam：`src/scanner/file_scanner_service_internal.h`（`FolderWatcher`/`FolderWatcherFactory`/`WatchEvent`）。
- 生产实现：`file_scanner_orchestrator.cpp:909-935`（`WtrFolderWatcher` 包装 `wtr::watch`）。
- 快照契约：全量树 `PlaylistTreeSnapshot`（`scanner_contracts.h:143-157`），"移除"靠新快照中缺席隐式表达；无增量/变更集。

## 二、核心结论（背景 bug 根因）

**根因第 1 名（高，库语义 × 架构）：目录移出监视根时，mv 动作本身不产生任何用户可见事件（孤立 MOVED_FROM 静默）；若无后续事件，扫描不触发、播放列表不更新。**

- **inotify 路径（非 root，最常见）**：目录被 `mv` 出监视根时，根目录 watch 收到带 cookie 的 `IN_MOVED_FROM`。`parse_ev`（`watcher.hpp:1564-1573`）将其存入 fae 关联缓冲（仅 16 槽，`watcher.hpp:1319-1328`）等待配对的 `IN_MOVED_TO`——目标在根外、没有 watch，**永远不会配对**。`do_ev_recv` 对 `err_pending` 事件不回调用户（`watcher.hpp:1720-1721`）。单独一次移出不触发溢出 → **完全静默**。
- **fanotify 路径（root）**：`parse_ev`（`watcher.hpp:1219-1223`）要求 MOVED_FROM/TO 相邻才发关联 rename；单边 MOVED_FROM 置 `ec=2`，`do_ev_recv` 仅在 `ec==0` 时回调（`watcher.hpp:1288-1289`）→ **事件直接丢弃，连警告都没有**。
- **对比**：删除目录（rmdir）→ 无 cookie 的 `IN_DELETE` → destroy 事件正常触发扫描；根内 rename → 配对成功 → 正常。唯独"移出监视根"是事件链黑洞。

扫描触发完全依赖 watcher 事件（`debounceLoop` 仅在 `dirtyGeneration` 变化后扫描，`file_scanner_orchestrator.cpp:2076-2107`）；扫描范围为全部 `watchedRoots`（:2100），不存在"旧路径不存在导致扫描跳过"问题——问题是**根本没有事件**。

**传播链路其余部分经验证正确**：一旦扫描被触发，目录消失会正确传播——`discoverScannerPaths` 枚举缺失 → `planIncrementalScan` deleted 分支（:530-534）→ `retainedLocationIds` 剪枝 → `pruneDeletedLocationsNoTransaction` 删缓存（`sqlite_cache.cpp:461-477`）→ 快照由当前枚举重建（:1113-1116）→ 播放列表正确清空（有 e2e 测试背书：`scanner_incremental_e2e_tests.cpp:368-398`）。**断点只在"事件获取"一环。**

**实测修正（2026-08-08 审计实验，见「六、实验验证」）**：
- mv 出根动作本身确认无事件（孤立 IN_MOVED_FROM → err_pending 抑制回调），与上述机制分析一致；
- 但"扫描永远不会被触发"的推论不完整：inotify watch 绑定 **inode 而非路径**，移出目录的递归 watch 依然存活，**根外写入仍会触发重扫**（场景 7/8/9 稳定复现）；
- 该重扫按根路径执行（`reconcileRoot`），移出目录不在根内 → FileScanned=0、快照冻结且**无污染**——"播放列表不更新"的观察面成立，但机制是"重扫找不到移出内容"而非"没有扫描"；
- mv 后首次重扫会**自愈**：快照正确清除已移出歌曲（1 首→0 首）。**"播放列表残留"在纯静默场景下确认成立**（场景 10 实测，见「六、实验验证」）：移走整个子文件夹且随后根内根外均无任何事件 → 零扫描 → 快照残留已移出歌曲——这正是用户原始复现（完整应用中移走子文件夹后播放列表不更新）。

**根因第 2 名（中，实现）：watcher 消息格式过滤与真实库格式不匹配，`w/` 前缀警告被静默吞掉。**

`watcherMessageRequestsRootReconciliation`（`file_scanner_orchestrator.cpp:865-869`）检查 `starts_with("w_")`（下划线），而库 `to_str` 生成的是 `"w/sys/not_watched@"`、`"w/self/q_overflow@"`、`"w/sys/partial@"`（`watcher.hpp:902-929`，斜杠）。后果：
- `w/sys/not_watched@`（`inotify_add_watch` 失败、watch 上限耗尽）→ 不触发 reconciliation → 监视注册失败被静默吞掉；
- `w@`/`w/sys/bad_fd@`/`w/sys/partial@` 同样被忽略；仅含 `overflow` 子串的消息有兜底；
- 测试用 `"w_sys_q_overflow"`（`scanner_watcher_tests.cpp:296`）——**与真实库格式不符**，掩盖了此缺陷。

**根因第 3 名（中，架构）：无任何"根状态对账"兜底。** 事件驱动是全链路唯一触发器：任何一环静默失败（根因 1、队列满丢弃、消息格式误判、watcher 异常死亡后不自动重启），库内容与快照**永久偏离**直到手动重扫或重启。无定时对账、无启动对账钩子。

## 三、问题清单（按链路分层）

每条：位置 | 类型 | 严重度 | 影响 | 建议方向。

### A. 事件获取（wtr.watcher 库层，vendored `third_party/watcher/include/wtr/watcher.hpp`）

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| A1 | `watcher.hpp:1564-1573, 1720-1721`（inotify）；`1219-1223, 1288-1289`（fanotify） | **目录移出监视根：MOVED_FROM 无配对 → inotify 挂起不回调 / fanotify ec=2 丢弃，均静默**（背景 bug 直接根因） | 高 | 库语义 | 库补丁：对孤立 MOVED_FROM 合成 destroy/rename 回调；或 orchestrator 侧周期对账兜底（见修复建议 R1/R3） |
| A2 | `watcher.hpp:1553-1559` | 挂起的 MOVED_FROM 可能被后续无关 MOVED_TO 按 cookie 错误配对（极端时路径映射错乱）；槽被挤出后配对丢失 → `w/sys/partial@` 空路径事件 | 低 | 库语义 | 接受或升级库 |
| A3 | `watcher.hpp:1319-1328, 1567-1573` | fae 仅 16 槽；>16 个未配对 rename 并发时旧槽被挤出 → `w_self_q_overflow` 消息（依赖 overflow 子串兜底触发全根扫描） | 低 | 库语义 | 接受；事件本身已不可靠，强化兜底 |
| A4 | `watcher.hpp:1686-1691`（IN_IGNORED/IN_MOVE_SELF 过滤）+ inode watch 不释放 | 移出目录的 watch 仍附着（inode 级）、wd_to_p 映射保留旧路径 → 长期大量移出 → inotify watch 泄漏 → 达 `max_user_watches` 后新目录注册失败（ENOSPC）→ not_watched 又被 Seriona 忽略（C1）→ **静默失监视** | 中 | 库语义 | 库补丁（IN_MOVE_SELF 触发目录重建）；Seriona 侧启动时重建 watch/统计 |
| A5 | `watcher.hpp:2269-2275`（polling 适配器 prune） | destroy 事件 path_type 用**根路径**而非被删路径判断 | 低 | 库语义 | Linux 生产路径不用 polling，仅记录 |

### B. 事件分类（`file_scanner_orchestrator.cpp:820-907`）

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| B1 | 865-869 | `starts_with("w_")` 与库真实格式 `w/` 不匹配；`contains("warning")`/`contains("error")` 为死代码；`e@`（无斜杠）不匹配 `e/` | 中 | 实现 | 改为匹配 `w/`、`e/`、`e@`；同步修正测试 `scanner_watcher_tests.cpp:296` 消息格式 |
| B2 | 894-907（+946-958） | watcher 异常死亡（`e/self/die@`）会触发 reconciliation + 扫描，但**不会重启 watcher** → 一次扫描后永久静默 | 中 | 架构 | 检测 die/error 后自动重新 `startWatching` 或上报上层 |
| B3 | 871-892 | 事件路径被丢弃（只计 generation），扫描范围固定为全部根；多根时任一根事件触发所有根扫描 | 中 | 架构 | 建立事件→受影响根/子树映射，支持局部增量扫描 |

### C. 入队与 debounce（`file_scanner_orchestrator.cpp:937-958, 2076-2122`）

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| C1 | 946-958 + 909-927 + `watcher.hpp:2405-2409` | 回调链无异常保护：`watchEventFrom`/`enqueueWatcherEvent` 抛异常 → wtr async future 异常 → `WtrFolderWatcher::close() noexcept` → **std::terminate 进程崩溃** | 中 | 实现 | 回调入口 try/catch，失败降级为触发一次扫描 |
| C2 | 2090-2098 | 事件持续到达（大目录复制）时 debounce 无限顺延，扫描 head-of-line 阻塞 | 低 | 架构 | 加最大等待上限强制扫描 |
| C3 | 1019, 1025-1035 | `scan()` 队列满（容量 16）时请求被拒绝仅发 ScanError，**不重试** → 高负载下 watcher 触发的扫描静默丢失 | 中 | 实现 | 队列满时合并请求或保留 pending 标记 |

### D. 扫描触发与模式决策

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| D1 | 256-274 + `directory_tree_hash.cpp:201-206` | tree hash 只含路径/类型（不含 mtime/size）→ 任何文件 create/destroy/rename 使 hash 变化 → `decideScanMode` 返回 **Full 全量扫描**（:273） | 中 | 架构 | watcher 场景直接走增量计划（`planIncrementalScan` 已具备 deleted/added/changed 能力） |
| D2 | 530-534 + `sqlite_cache.cpp:461-477` | "目录消失 → 快照/缓存"传播链路本身正确（前提是扫描被触发，断点见 A1）——仅记录，无缺陷 | — | — | — |
| D3 | 1897-1899 | `recordScanRootDecision` 在 `directoryTreeHash` 为 nullopt 时直接 return → 缓存既不写入也不剪枝，而快照照常发布 → 缓存与快照不一致窗口 | 低 | 实现 | 哈希缺失时至少保留剪枝或强制下次 Full |
| D4 | 1908-1910 | `retainedLocationIds` 依赖 duration+contentHash，任一缺失即被剪掉 → 下次增量全量重读该文件（cache churn） | 低 | 实现 | 区分"不可保留"与"删除"，缺元数据应标记失效而非剪枝 |

### E. 计划层与发布（`file_scanner_orchestrator.cpp` / `control_state_reducer.cpp`）

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| E1 | 323, 532 | **`IncrementalScanPlan::deleted` 是死字段**：计算了删除集合却无任何消费点；删除语义完全隐式，仅靠 `retainedLocationIds` 剪枝间接完成 | 中 | 实现 | 让 `IncrementalExecutionPlan` 携带 deleted 并显式删除/发布，消除隐式语义 |
| E2 | `control_state_reducer.cpp:677-680` | 事件版本门（`monotonicVersion <= lastScannerVersion_` 丢弃）假设单 scanner 实例生命周期；若控制器热替换/重建 scanner 服务（版本从 0 重新计数），新实例全部事件被静默丢弃 | 低 | 架构 | 版本门按"服务代际"判断或替换时显式重置 |
| E3 | `scanner_contracts.h:143-157` | 契约无增量/变更集/心跳，订阅方无法区分"没变化"与"没扫描"；任何触发缺失都表现为"整个播放列表不更新" | 低 | 架构 | 可选：增加变更集或 ScanCompleted 带"无变化"确认 |
| E4 | 1250-1274 | `stopWatching` 锁外 close watchers（阻塞）+ join debounceThread_；若 debounceLoop 正阻塞在 runScan（同步全量扫描）→ 阻塞至扫描完成（大库秒级~分钟级） | 中 | 架构 | stopWatching 加超时/异步；接入已有 cancellationRequested_ |

### F. 前端消费链路（Seriona，`src/app/`、`qml/components/`）

| # | 位置 | 问题 | 严重度 | 类型 | 建议方向 |
|---|------|------|--------|------|----------|
| F1 | `library_model.cpp:1140-1142` | **libraryTree 可选门控**：仅当 `snapshot.libraryTree.has_value()` 才刷新树。若后端发布不带树的快照（只带 scanStatus/进度），前端静默跳过且无任何自愈（无 version 比较——`m_version` 只记录不参与判断 :898；无超时；`BackendBridge::librarySnapshot()` 存在但从不用于重取） | 高 | 架构 | 与后端约定"含树变更必须携带 libraryTree"；前端在 `scanStatus→Completed` 时主动重取最新树（pull 兜底） |
| F2 | `backend_bridge.cpp:389-400, 439-443` + `library_model.cpp:898` | 无乱序/版本回退防护：后端回调跨线程并发 → QueuedConnection 投递顺序无保证 → 旧树可覆盖新树，移除项"复活"；`LibraryStateSnapshot.version`/`PlaylistTreeSnapshot.version` 均未用于单调性检查 | 中 | 架构 | Bridge/AppFacade 加"丢弃 version ≤ 已应用版本"的单调过滤 |
| F3 | `library_model.cpp:878-881` | `m_playingTrackId.clear()` 在 `nodeIdForTrackId(m_playingTrackId)` 计算**之前** → 本次重建投影 isPlaying 标志恒为 false；高亮仅靠 `app_facade.cpp:52` 事后补回，依赖信号顺序 | 中 | 实现 | 先计算 playingNodeId 再 clear，或由 controller forceReapply 统一恢复 |
| F4 | `library_model.cpp:870-899` + `Sidebar.qml:389-397, 652` | 全量 reset 无相同快照短路：每次带树快照（version 是否推进、内容是否相同）都整表 `beginResetModel` → ListView 滚动跳顶、delegate 状态丢失（`positionViewAtIndex` 仅由 scrollRequestChanged 驱动） | 低 | 实现 | 对相同 version + 相同内容做短路（至少 version 短路） |
| F5 | `library_model.cpp:1107-1122, 1793-1854` | 游标回退（folderWasCleared→My Music、selected→firstVisible）逻辑正确且有测试，但**只在 F1 门控通过时触发**；若更新被丢弃，selected/currentFolder 停留在已消失目录上（`rowForNodeId=-1`） | 低 | 实现 | 跟随 F1 修复；scanStatus=Completed 时强制 reconcile 一次 |
| F6 | `backend_bridge.cpp:389-400` | 库快照无合并窗口/背压（对比：通知队列有 64 条上限 `backend_bridge.cpp:28`）：扫描期间高频快照每帧整表替换，级联放大 F4 抖动、扩大 F2 乱序窗口 | 低 | 架构 | bridge 层同帧合并最近一帧快照 |

## 四、测试覆盖缺口

### 后端（`Seriona_Backend/tests/`）

已覆盖：文件级 create/modify/destroy/rename→扫描（fake watcher，`scanner_watcher_tests.cpp:199-235`）、lrc 更新（:237-274）、overflow 消息 reconciliation（:276-309，**注意消息格式与库不符**）、lifecycle 不触发（:311-346）、stop 语义（:348-371）、启动失败（:373-394）、手动与 watcher 扫描串行化（:396-432）、文件删除→快照+缓存收敛 e2e（`scanner_incremental_e2e_tests.cpp:368-398`）、control 层整树替换语义（`media_controller_tests.cpp:1792-1873`）。

| 缺失场景 | 与 bug 相关性 |
|----------|--------------|
| **子目录移出监视根（mv 到根外）** | 直接对应复现 bug，CTest 无覆盖；审计实验已覆盖两个变体：mv+根外写（场景 7，快照自愈）与**纯静默 mv（场景 10，快照残留=用户复现确证）** |
| 目录删除（rmdir）、目录根内 rename、根自身移出/删除 | 高（同为目录级事件；审计实验已覆盖 rmdir/根内 rename/mv 出根） |
| 真实 WtrFolderWatcher 集成（真实 wtr + 真实文件系统操作） | 高（所有 watcher 测试经 fake `CapturingWatcherFactory`；审计工具程序已部分覆盖，尚未收敛为 CTest 用例） |
| 真实消息格式串（`w/sys/not_watched@`、`w/sys/partial@`、`e/self/die@`） | 高（当前测试格式错误，掩盖 B1） |
| 孤立 IN_MOVED_FROM（partial rename）语义 | 高（已由审计实验锁定：mv 动作本身无事件；移出目录 watch 残留、根外写入有事件） |
| 移出目录后根外修改/删除（inotify watch 残留行为） | 中（已由审计实验覆盖：watch 残留存活、写入触发重扫、快照冻结） |
| 移出后再移回（fae 槽位恢复路径） | 中 |
| 多根、队列满、持续事件流 debounce | 中 |
| scanner 服务重建后 reducer 版本门（E2） | 中 |
| 缓存剪枝单测 | 部分：`seriona_scanner_cache_tests`/`cache_content_tests`/`phase1_integration` 在 `tests/CMakeLists.txt:307-365` 被禁用 |

### 前端（`Seriona/tests/frontend/adapter/`）

已覆盖：快照中曲目/专辑/文件夹消失→模型替换+游标回退（`tst_library_dual_cursor.cpp:148-186`、`tst_library_sort.cpp:433-458`）、空树/空库（`tst_library_tree_model.cpp:238`、`tst_library_scan_flow.cpp:142`）、新版本 reset（`tst_library_tree_model.cpp:252`）、bridge 线程（`tst_backend_bridge.cpp:334`）。

| 缺失场景 | 说明 |
|----------|------|
| **快照不带 libraryTree（仅状态/进度）** | 所有用例均显式设置 `libraryTree`（`tst_library_scan_flow.cpp:209` 等），F1 门控零测试 |
| 无树快照后的主动拉取/自愈 | 机制不存在 |
| 相同 version 快照重复发布 → 短路 | 机制不存在 |
| 乱序/旧 version 覆盖新 version | 无单调性守卫 |
| 非空→全空迁移的 libraryEmpty/libraryState 信号 | 现有用例是初始空，非迁移 |
| QML ListView reset 后滚动/空态刷新 | 20 个测试全部为 C++ QTEST_GUILESS，无 QML 级测试 |

## 五、修复建议（按优先级）

1. **R1（根因 1，必做）**：修复"移出静默"。二选一：
   - 库补丁（改动最小、最贴近内核语义）：在 `watcher.hpp` inotify `parse_ev` 对孤立 `IN_MOVED_FROM`（非 adjacent、非 fae 配对）直接合成 destroy/rename 事件；fanotify 同理（ec=2 时改发单边事件）。vendored 可改，但升级库时需保留补丁（注意跟进策略）。
   - 或 orchestrator 侧补偿：周期对账——定时重算 `computeDirectoryTreeHash` 与缓存 hash 比对，变化即触发重扫（复用 `decideScanMode` 机制，`file_scanner_orchestrator.cpp:256-274`）。
2. **R2（根因 2，低成本）**：修 `watcherMessageRequestsRootReconciliation` 消息匹配（`w/` 而非 `w_`），并修正 `scanner_watcher_tests.cpp:296` 的测试格式；消除静默失监视。
3. **R3（架构加固）**：watcher 异常死亡（`e/self/die@`）自动重启 `startWatching`（B2）；队列满不丢请求（C3）。
4. **R4（前端契约对齐）**：与后端联动确认 `LibraryStateSnapshot` 发布时 `libraryTree` 的携带策略与 version 推进语义（决定 F1 是前端修复还是契约对齐）；前端补 version 单调过滤（F2）、`scanStatus→Completed` 主动重取（F1 pull 兜底）、isPlaying 清空顺序修正（F3）。
5. **R5（质量）**：把 `tools/watch_root_move_audit.cpp` 的实验收敛为 CTest 集成测试——"建子目录→rename 移出→断言快照收敛"作为修复验证门禁；补目录级事件、真实消息格式串、`plan.deleted` 显式消费与用例（E1）。修复 A1 后须以此门禁验证：mv 出根动作本身应触发重扫（当前依赖根外写入间接触发，见「六、实验验证」）。

## 六、实验验证（真实 watcher 审计程序，2026-08-08）

### 6.1 方法

独立工具 `tools/watch_root_move_audit.cpp`（约 770 行，仅链接 `seriona_scanner`；真实 `FileScannerService` + 真实 `WtrFolderWatcherFactory`/wtr::watch；装配参照 `MediaController::scanLibrary`：先 `scan` 后 `startWatching`，deps 仅设 databasePath/coverExportDir）。在临时根上执行 10 场景对照实验（文件 create/modify/delete、目录 create/rmdir、根内 rename、**mv 出根 + 根外写**、多根 mv 出根、**纯静默 mv 出根**——见下）。判定信号：**ScanStarted 事件计数增量**（`snapshot.version` 每次扫描从 0 重增、恒 1，不做跨扫描单调指标）+ 快照歌曲数 + FileScanned 增量。构建：`cmake -S . -B build -DSERIONA_BUILD_TOOLS=ON -DSERIONA_BUILD_TESTS=ON && cmake --build build --target seriona_watch_root_move_audit`（target 挂 `seriona_scanner`，依赖全 PUBLIC 传递）。运行 6 次（场景 1-9：2 次核心 + 2 次监控/strace 实证；场景 10：2 次补充），退出码均 0。

**场景 10（纯静默 mv 出根，用户复现场景）**：写 `silent/song.wav` 并入快照（等歌曲数真实 +1，非仅 ScanStarted，规避竞态）→ 基线 → `rename(musicRoot/silent → silent-out)` 整个子文件夹移出根 → 沉降 300ms → 观察窗口 3s（**期间零文件系统操作**，不写/不删/不 mv/不 touch）→ 判定：ScanStarted 增量==0 且歌曲数不变（残留）为 PASS（确认用户复现），增量>0 为 FAIL（A1 机制被推翻）。

### 6.2 结果（场景 1-9 四次运行一致；场景 10 两次运行一致）

| 场景 | 判定 | ScanStarted 增量 | FileScanned 增量 | 歌曲数 |
|------|------|------------------|------------------|--------|
| 1-6 对照组（create/modify/delete/目录 create/rmdir/根内 rename） | PASS ×6 | 各 1 | 0-1 | 正常增减 |
| 7 mv 出根 + 根外写 d.wav | PARTIAL（事件到达但内容未变） | 1 | 0 | 0 冻结 |
| 8 根外继续写 e.wav | PARTIAL | 1 | 0 | 0 冻结 |
| 9-mv 多根 mv 出根 + 写 h.wav | PARTIAL（run-1 为 FAIL：快照 2→0 自愈，见 6.3-7） | 1 | 0 | 0 冻结 / 2→0 |
| **10 纯静默 mv 出根（零后续操作）** | **PASS（确认用户复现：无扫描、快照残留）** | **0** | **0** | **1 残留（冻结）** |

无等待超时、无 UNKNOWN；ScanError 仅 musicB RootUnavailable（2-3 条/次，场景 9 竞态所致，见 6.3-6）；无 wtr overflow/"no more space" 诊断（16 槽 fae 缓冲未溢出，实验事件量小）。

### 6.3 结论

1. **A1 机制确认**：mv 出根动作本身无回调、无重扫（监控实证：mv 完成到下次扫描之间无任何 scan）。wtr 当前源码行为与引用行号一致（`watcher.hpp:1564-1573, 1720-1721` / `1219-1223, 1288-1289`）。
2. **推论修正**：移出目录的 inotify watch 因 inode 绑定仍存活，**根外写入仍触发重扫**（每次 ScanStarted +1，4 次稳定）；重扫按根路径执行，结果正确反映根内内容（FileScanned=0、快照冻结、无污染）。"扫描不触发"仅对 mv 动作本身成立。
3. **快照自愈（新发现）**：mv 后首次重扫正确清除已移出歌曲（1 首→0 首；run-1 场景 9-mv 直接实证 2→0）。
4. **纯静默 mv 确认（用户复现场景）**：移走整个子文件夹后不做任何操作 → 零事件、零扫描（ScanStarted 增量=0、FileScanned 增量=0）→ **快照残留已移出歌曲**（场景 10 两次稳定 PASS，残留证据：快照仍含 `music/silent/song.wav`）。与场景 7 对比：**只有根外/根内后续事件才能触发自愈重扫，纯静默 mv 时播放列表不更新**——即用户原始复现（完整应用中移走子文件夹后播放列表不更新）的确证。
5. **watch 残留空转（强化 B3）**：`collectActionableWatcherEvent`（`file_scanner_orchestrator.cpp:894-907`）不做根内路径过滤，残留 watch 的每次根外写入都触发一次内容不变的全量重扫——长期活跃的根外写入会造成持续空转扫描，建议按根内路径过滤。
6. **场景 9 对照组 FAIL 为审计程序竞态（非后端缺陷）**：`waitUntil` 只等 ScanStarted（runScan 开头发布，`orchestrator:1051-1052`）不等 ScanCompleted，主线程在 scanWorker 执行 `reconcileRoot(musicB)` 前即 mv——strace 铁证：`rename(musicB→musicB-moved)` 后 `newfstatat(musicB)=ENOENT`。后端如实报告根不可用（ScanError code=0 "scanner path does not exist"），行为正确。
7. **场景 9 竞态的另一侧样本（run-1）**：对照组 PASS（musicB 2 首成功并入快照，补上"两根歌曲并入快照"验证）→ 9-mv FAIL（写 h.wav 触发重扫，快照 2→0 自愈，移出歌曲被清除）。与 run-2 的差异源于 6.3-6 竞态时序（waitUntil 恰好等到扫描完成 vs 未等到），非后端缺陷。

### 6.4 遗留事项

- ~~纯静默场景（mv 后无任何事件）的"播放列表残留"未被直接覆盖~~：**已覆盖**（场景 10，两次稳定 PASS：零事件、零扫描、快照残留——用户复现确证）。
- 场景 9"两根歌曲并入快照"验证：run-1 已间接完成（对照组 PASS、2 首并入）；如需稳定复现，仍需修复审计程序（waitUntil 改等 ScanCompleted）后重测。
- 审计工具为临时程序（`SERIONA_BUILD_TOOLS=ON` 才构建）；长期回归应收敛为 CTest 用例（R5）。

## 七、验证方法说明

- wtr.watcher 语义基于 vendored 源码（`third_party/watcher/`）逐行核实，非猜测。
- 未验证项：macOS FSEvents / Windows 适配器（本 bug 在 Linux 复现；两平台对"移出"的单边事件语义与 inotify 同源，需单独验证）。
- 移出目录后的 watch 残留行为（inotify 路径）已由审计实验实测验证（见「六、实验验证」）。
- 三个审查任务对根因的判断一致：**断点在事件获取一环，传播链路正确**；前端 F1/F2 为防御性缺口（后端事件丢失时前端无从自愈）。

## 附：涉及文件索引

- `Seriona_Backend/src/scanner/file_scanner_orchestrator.cpp` — 事件映射/过滤/入队/debounce/扫描触发/快照发布（核心）
- `Seriona_Backend/third_party/watcher/include/wtr/watcher.hpp` — vendored wtr.watcher（inotify/fanotify/polling 适配器）
- `Seriona_Backend/src/scanner/file_scanner_service_internal.h` — FolderWatcher/WatchEvent seam
- `Seriona_Backend/src/scanner/cache/sqlite_cache.cpp` — 缓存清理（pruneDeletedLocations）
- `Seriona_Backend/src/scanner/playlist_tree_builder.cpp`、`directory_tree_hash.cpp`、`path_utils.cpp` — 树重建/hash/枚举
- `Seriona_Backend/src/control/control_state_reducer.cpp`、`media_controller.cpp` — 消费端整树替换与发布
- `Seriona_Backend/inc/seriona/scanner/scanner_contracts.h`、`inc/seriona/control/control_contracts.h` — 契约
- `Seriona_Backend/tests/scanner/scanner_watcher_tests.cpp`、`scanner_incremental_e2e_tests.cpp` — 测试现状
- `Seriona_Backend/tools/watch_root_move_audit.cpp` — 真实 watcher 审计工具（9 场景实验，`SERIONA_BUILD_TOOLS=ON`）
- `Seriona/src/app/library_model.cpp`、`backend_bridge.cpp`、`app_facade.cpp`、`library_tree_store.cpp` — 前端消费链路
- `Seriona/qml/components/Sidebar.qml` — 播放列表视图
- `Seriona/tests/frontend/adapter/` — 前端测试
