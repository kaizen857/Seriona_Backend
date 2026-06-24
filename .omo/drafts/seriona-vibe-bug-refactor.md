---
slug: seriona-vibe-bug-refactor
status: plan-written
intent: clear
pending-action: write .omo/plans/seriona-vibe-bug-refactor.md
approach: 以阶段一实时回调安全为根，阶段二 post-only 消息链路，阶段三音频资源/状态机，阶段四架构边界并行清理的 DAG 重构计划；每个任务强制测试闭环与原子提交。
---

# Draft: seriona-vibe-bug-refactor

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
- C-AUDIO-RT | miniaudio callback 只读无锁 PCM/generation 状态，设备停止等待 callback 退出 | active | DESIGN.md:405, VIBE_CODING_BUG_REPORT.md:218
- C-EVENT-SINKS | audio/metadata/control sink 全部线程安全、post-only、异常不挂死 | active | DESIGN.md:157, VIBE_CODING_BUG_REPORT.md:68
- C-AUDIO-WORKER | AudioPlaybackService 公共接口只 enqueue，FFmpeg/filter/device/seek 只在 audio worker 串行执行 | active | DESIGN.md:374, VIBE_CODING_BUG_REPORT.md:37
- C-ARCH-BOUNDARY | scanner/metadata/cache/scanner public header 边界对齐，控制层不执行重任务或平台 API | active | DESIGN.md:254, VIBE_CODING_BUG_REPORT.md:242

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
- 测试策略 | TDD：每个任务先加失败测试，再改实现，再运行该任务指定 ctest，最后提交 | 用户明确要求测试驱动闭环 | 可逆但不建议
- Git 策略 | 每个独立 Bug 修复后立即 `git add <目标文件>` + `git commit -m "fix(<scope>): ..."` | 用户明确要求原子提交且限制写命令 | 不可逆规则
- 并发策略 | 只允许文件域完全隔离的任务并行；Git add/commit 必须用锁串行化 | 避免 index.lock 与 merge 冲突 | 不可逆规则
- W8 | 排除执行范围，仅保留为已修文档审计记录 | VIBE_CODING_BUG_REPORT.md 已标记完成 | 可逆但当前不需要

## Findings (cited - path:lines)
- `DESIGN.md:157` 要求上行事件只投递到 mediaController 的 `BackendEventSink`，sink 只做 post。
- `DESIGN.md:214` 要求实时音频回调不直接执行 mediaController callback，只写原子状态或无阻塞队列。
- `DESIGN.md:405` 明确禁止实时回调中的 FFmpeg、日志、堆分配、阻塞锁和设备生命周期调用。
- `DESIGN.md:562` 要求 SQLite 缓存容量、WAL checkpoint、清理策略。
- `VIBE_CODING_BUG_REPORT.md:11` 到 `VIBE_CODING_BUG_REPORT.md:18` 指出控制层同步扫描导致命令和进度事件排队。
- `VIBE_CODING_BUG_REPORT.md:37` 到 `VIBE_CODING_BUG_REPORT.md:45` 指出音频模块缺少自己的命令串行器。
- `VIBE_CODING_BUG_REPORT.md:68` 到 `VIBE_CODING_BUG_REPORT.md:75` 指出 AudioEventDispatcher sink 数据竞争。
- `VIBE_CODING_BUG_REPORT.md:109` 到 `VIBE_CODING_BUG_REPORT.md:116` 指出 MediaController::dispatch 异常路径 promise 永久不完成。
- `VIBE_CODING_BUG_REPORT.md:132` 到 `VIBE_CODING_BUG_REPORT.md:138` 指出 Metadata/MPRIS command sink 数据竞争。
- `tests/CMakeLists.txt:17` 到 `tests/CMakeLists.txt:120` 已有 audio buffer/device/dispatcher/player/shutdown 测试目标。
- `tests/CMakeLists.txt:144` 到 `tests/CMakeLists.txt:156` 已有 metadata/control 测试目标。
- `tests/CMakeLists.txt:176` 到 `tests/CMakeLists.txt:190` 已有 scanner cache/service/watcher 测试目标。

## Decisions (with rationale)
- 阶段顺序采用用户指定优先级：实时回调安全先于 EventSink，EventSink 先于 audio worker 与架构边界清理。
- C4/W1 控制层异常与 stop 生命周期归入消息管线基础任务，必须早于 C1 扫描异步化，以免新异步路径仍有 promise 挂死风险。
- 阶段三音频 worker/seek 与阶段四 scanner/metadata/cache/header 边界可并行，但只能在各自文件域内执行；若发现公共契约必须跨域修改，executor 必须拆出串行接口任务并暂停并行。
- A2.1 纳入范围，因为报告优先级路线明确将 scanner 公共头边界纳入最终处理；W8 排除，因为已完成。

## High-accuracy review log
- Momus pass 1: REJECT。阻塞问题为 Wave 4 文件域重叠、任务计数 10/11 不一致、Task 6 公共 audio 契约变更与并行任务冲突。
- Fix applied: Wave 4 拆成 4a/4b/4c，Task 7 串行在 Task 5 后，Task 9 串行在 Task 7 后；机器摘要和成功标准统一为 11 todos；Task 6 移除 `inc/seriona/audio/audio_contracts.h` 允许修改范围并要求另拆串行契约任务。
- Momus pass 2: APPROVE。复核确认 0-10 共 11 个 todo 计数一致；Task 7/9 已串行化；Task 6 不再允许修改 `inc/seriona/audio/audio_contracts.h`；每个任务具备文件域、验收标准、happy/failure QA、证据路径、提交规则，Git 写限制和 TDD 循环明确。

## Scope IN
- W3/W4：AudioOutputDevice callback queue 生命周期、stop/wait、PCM generation/reset 安全。
- C3/C5：AudioEventDispatcher 与 metadata command sink set/clear/dispatch 并发安全。
- C4/W1：MediaController dispatch 异常完成 promise；ControlEventLoop stop 生命周期不触发 terminate。
- C1/W2：scanner 重任务和 metadata update 不在 control event loop 内同步执行。
- C2/W5/W6：AudioPlaybackService 命令队列、单 worker、seek 事务化、FFmpeg/filter/device 资源只在 worker 线程 mutation。
- W7/A2.1：SQLite cache 容量/checkpoint 策略落地；scanner TagReader/cache adapter 公共头边界收窄。

## Scope OUT (Must NOT have)
- 不修改 `DESIGN.md` 或 W8；该项已修正。
- 不引入 Qt/QML/UI 或平台 API 到公共契约、audio、scanner、实时路径。
- 不在实时 callback 中新增 FFmpeg、日志、堆分配、阻塞锁、BackendEventSink 调用或设备生命周期调用。
- 不使用 `git add .`、`git reset`、`git stash`、`git rebase`、`git push`、force 操作或交互式 Git。
- 不把 TagReader、SQLite、watcher、FFmpeg、miniaudio 私有类型泄漏进稳定公共契约头。

## Open questions
- 无阻塞问题；用户已指定阶段顺序、测试闭环、Git 写命令白名单与并发 DAG 规则。

## Approval gate
status: plan-written
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
