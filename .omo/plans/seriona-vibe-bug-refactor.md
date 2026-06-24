# seriona-vibe-bug-refactor - Work Plan

## TL;DR (For humans)
**What you'll get:** 一个按依赖顺序修复音频实时回调、事件管线、音频状态机和架构边界的底层重构计划；每个修复都要求先写失败测试、通过验证后原子提交。

**Why this approach:** 先拆除实时线程和 callback 生命周期风险，再修 post-only 消息链路，最后并行处理音频 worker 与 scanner/metadata/cache 边界，避免上层修复被底层竞态掩盖。

**What it will NOT do:** 不改 UI/QML，不把平台或第三方私有类型泄漏进公共契约，不修改已经完成的 `DESIGN.md` W8 文档项。

**Effort:** XL
**Risk:** High - 涉及跨线程生命周期、公共接口语义和多模块测试闭环。
**Decisions to sanity-check:** W8 明确排除；A2.1 scanner 公共头边界纳入；所有 Git 写入只允许目标文件 add 和 commit。

Your next move: 确认是否唤醒 plan executor 从第一步开始执行。Full execution detail follows below.

---

> TL;DR (machine): XL/high-risk DAG refactor; 11 todos including preflight/final integration; strict TDD, isolated file domains, serialized git commits.

## Scope
### Must have
- 任务 DAG 必须按阶段执行：Phase 1 实时线程安全 -> Phase 2 EventSink/消息管线 -> Phase 3 音频资源与状态机、Phase 4 架构边界清理并行 -> final verification。
- 每个任务只能修改其 `Allowed file domain` 列出的文件；如需跨域修改，executor 必须停止并拆分/更新计划，不得临时扩大范围。
- 每个任务必须先新增或调整失败测试，再修改实现，再运行指定构建/测试命令；失败时原地循环修复直到通过，才能进入下一任务。
- 每个独立 Bug 修复通过测试后必须立即提交；Git 写命令仅允许 `git add <目标文件>` 与 `git commit -m "<修复说明>"`。
- 并行任务的源码文件域必须完全隔离；并行执行时 Git add/commit 必须通过单一锁串行化，避免 `index.lock` 与 index 交叉污染。
- 所有事件 sink 都必须按值语义、线程安全、锁外调用、post-only；控制层串行执行器只做状态归并和轻量调度。
### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不修改 `src/` 或 `inc/` 以外无关文件，除非该任务明确列出测试或构建清单文件。
- 不引入 Qt/QML/UI 依赖；不把 MPRIS、sdbus-c++、Windows SMTC、SQLite、TagReader、watcher、FFmpeg、miniaudio 私有类型泄漏进稳定公共契约。
- 不在 `AudioOutputDevice::renderCallback()` 或等价实时路径中做 FFmpeg、日志、动态分配、阻塞锁、`BackendEventSink`、设备 start/stop/uninit。
- 不用 `git add .`、`git reset`、`git stash`、`git rebase`、`git push`、force、interactive Git 或自动清理未列入任务的用户改动。
- 不把全部测试推迟到最后；每个任务自己的验证通过前不得开始依赖任务。

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD with doctest/CTest. For each task: add failing regression test first, run the task-specific `ctest -R ... --output-on-failure` to confirm failure when practical, implement, rerun until green, then run the broader module regex listed in the task.
- Baseline setup command before task 1: `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` and `cmake --build build` if no valid build tree exists.
- Evidence: each task writes command transcript and short result note to `.omo/evidence/task-<N>-seriona-vibe-bug-refactor.md` before commit.
- Required loop gate: if any compile/test command fails, executor stays on the same task, edits only that task's file domain, updates evidence, and retries; it cannot mark the task done or commit until green.
- Final verification: after all tasks, run `cmake --build build` and `ctest --test-dir build --output-on-failure`, then run read-only plan compliance/code quality/scope fidelity reviews.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 0: Preflight only; no product code changes; records dirty worktree and build baseline.
- Wave 1: Task 1 only; Phase 1 real-time callback safety foundation.
- Wave 2: Tasks 2, 3, and 4 can run in parallel after Task 1 because their file domains are audio dispatcher, metadata, and control respectively; Git commit steps must be serialized.
- Wave 3: Task 5 only; scanner async/control unblocking depends on control dispatch safety from Task 4.
- Wave 4a: Tasks 6 and 8 can run in parallel after their dependencies because their file domains are audio service/state machine and scanner cache/header. Git commit steps must be serialized.
- Wave 4b: Task 7 runs after Task 5 and Task 3/4 because it integrates metadata async delivery through `src/control/media_controller.cpp`.
- Wave 4c: Task 9 runs after Task 7 because it also may integrate snapshot publishing through `src/control/media_controller.cpp`; it must not run in parallel with Task 7.
- Wave 5: Task 10 only; full integration and final verification after all previous tasks.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 0 | none | 1 | none |
| 1 | 0 | 2,3,4,6 | none |
| 2 | 1 | 6 | 3,4 |
| 3 | 1 | 7,8,10 | 2,4 |
| 4 | 1 | 5,7,9,10 | 2,3 |
| 5 | 4 | 10 | none |
| 6 | 1,2 | 10 | 8 |
| 7 | 3,4,5 | 9,10 | none |
| 8 | 3 | 10 | 6 |
| 9 | 4,7 | 10 | none |
| 10 | 5,6,7,8,9 | final | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 0. Preflight: snapshot worktree and build baseline without source edits
  What to do / Must NOT do: Run read-only `git status --short`, inspect existing build availability, configure/build tests if needed. Do not edit product code and do not commit. Record any unrelated dirty files so later tasks avoid them.
  Allowed file domain: `.omo/evidence/task-0-seriona-vibe-bug-refactor.md` only.
  Parallelization: Wave 0 | Blocked by: none | Blocks: 1
  References: `CMakeLists.txt:1`, `CMakeLists.txt:201`, `tests/CMakeLists.txt:1`, `AGENTS.md` build commands in repo instructions.
  Acceptance criteria: `.omo/evidence/task-0-seriona-vibe-bug-refactor.md` contains exact output summaries for `git status --short`, configure/build commands used or explicit note that existing build tree is valid.
  QA scenarios: happy = `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build`; failure = missing dependency is recorded with exact CMake error and task stops for user/environment fix. Evidence `.omo/evidence/task-0-seriona-vibe-bug-refactor.md`.
  Commit: N | no source change.

- [x] 1. Phase 1: make audio callback queue lifetime generation-safe
  What to do / Must NOT do: Add tests that simulate callback/read racing with seek/reset/stop, then refactor `AudioOutputDevice` and `PcmBufferQueue` so callback observes a stable generation/queue state and `stop`/`uninitialize` cannot race a dangling `PcmBufferQueue*`. Preserve RT path: callback may only read atomic/shared immutable state, read PCM, apply volume/mute, update atomics, and fill silence. Do not add mutex locks, allocation, logging, FFmpeg, sink dispatch, or device lifecycle calls inside render callback.
  Allowed file domain: `inc/seriona/audio/buffer/pcm_buffer_queue.h`, `src/audio/buffer/pcm_buffer_queue.cpp`, `inc/seriona/audio/device/audio_output_device.h`, `src/audio/device/audio_output_device.cpp`, `tests/audio/pcm_buffer_queue_tests.cpp`, `tests/audio/audio_output_device_tests.cpp`, optional `tests/CMakeLists.txt` only if a new test executable is strictly required.
  Parallelization: Wave 1 | Blocked by: 0 | Blocks: 2,3,4,6
  References: `DESIGN.md:405`, `DESIGN.md:411`, `VIBE_CODING_BUG_REPORT.md:218`, `VIBE_CODING_BUG_REPORT.md:224`, `inc/seriona/audio/device/audio_output_device.h:80`, `src/audio/buffer/pcm_buffer_queue.cpp:70`, `tests/CMakeLists.txt:17`, `tests/CMakeLists.txt:21`.
  Acceptance criteria: tests prove callback after stop/uninitialize cannot read freed queue; seek/reset generation prevents pre-seek PCM from being observed after reset; render callback code review evidence confirms no forbidden RT operations.
  QA scenarios: happy = `cmake --build build --target seriona_pcm_buffer_queue_tests seriona_audio_output_device_tests && ctest --test-dir build -R 'seriona_(pcm_buffer_queue|audio_output_device)' --output-on-failure`; failure = intentionally failing regression initially demonstrates old race/semantic gap or, if nondeterministic, a deterministic fake backend/callback test asserts lifecycle ordering. Evidence `.omo/evidence/task-1-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(audio): make callback queue lifetime generation-safe`

- [x] 2. Phase 2: make AudioEventDispatcher sink thread-safe and lock-outside-call
  What to do / Must NOT do: Add concurrent set/clear/dispatch regression tests, then protect `AudioEventDispatcher` sink and event versioning with a safe synchronization strategy. `dispatch()` must copy the sink under lock, release lock, then invoke; `shutdown()`/`clearEventSink()` must not race dispatch; service destruction must stop progress worker before clearing sinks. Do not call sink while holding the mutex and do not turn dispatch into a blocking control operation.
  Allowed file domain: `inc/seriona/audio/events/audio_event_dispatcher.h`, `src/audio/events/audio_event_dispatcher.cpp`, `src/audio/audio_playback_service.cpp` only for destructor/lifecycle ordering, `tests/audio/audio_event_dispatcher_tests.cpp`, `tests/audio/audio_event_dispatcher_shutdown_tests.cpp`, `tests/audio/audio_shutdown_lifecycle_tests.cpp`.
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 6 | Can parallelize with: 3,4
  References: `DESIGN.md:232`, `VIBE_CODING_BUG_REPORT.md:68`, `VIBE_CODING_BUG_REPORT.md:71`, `VIBE_CODING_BUG_REPORT.md:100`, `src/audio/events/audio_event_dispatcher.cpp:10`, `src/audio/events/audio_event_dispatcher.cpp:16`, `tests/CMakeLists.txt:35`, `tests/CMakeLists.txt:39`, `tests/CMakeLists.txt:108`.
  Acceptance criteria: stress test repeatedly races dispatch with clear/set without crash/data race symptoms; sink invocation happens outside lock; monotonicVersion remains unique/monotonic under concurrent dispatch or dispatch is explicitly serialized.
  QA scenarios: happy = `cmake --build build --target seriona_audio_event_dispatcher_tests seriona_audio_event_dispatcher_shutdown_tests seriona_audio_shutdown_lifecycle_tests && ctest --test-dir build -R 'seriona_audio_event_dispatcher|seriona_audio_shutdown_lifecycle' --output-on-failure`; failure = regression test demonstrates old unsynchronized clear/dispatch can drop/crash or violates expected lifecycle ordering. Evidence `.omo/evidence/task-2-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(audio): synchronize event dispatcher sink lifecycle`

- [x] 3. Phase 2: make metadata command sink thread-safe and post-only
  What to do / Must NOT do: Add tests for MPRIS command handler racing with unsubscribe/update, then make `CommandSinkState` synchronize set/clear/read and copy callbacks before lock-free invocation. Metadata must generate `MediaControlCommand` values and post them to control; it must not directly mutate control/audio/scanner state. Do not expose `sdbus-c++` or platform private types in public contracts.
  Allowed file domain: `src/metadata/metadata_mpris_private.h`, `src/metadata/metadata_mpris_linux.cpp`, `src/metadata/metadata_mpris_backend.cpp`, `src/metadata/metadata_mpris.cpp`, `tests/metadata/metadata_mpris_tests.cpp`, `tests/metadata/metadata_service_tests.cpp` if needed for sink lifecycle coverage.
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 7,8,10 | Can parallelize with: 2,4
  References: `DESIGN.md:101`, `DESIGN.md:161`, `VIBE_CODING_BUG_REPORT.md:132`, `VIBE_CODING_BUG_REPORT.md:135`, `tests/CMakeLists.txt:134`, `tests/CMakeLists.txt:144`.
  Acceptance criteria: unsubscribe concurrent with MPRIS command dispatch is safe; callback copied before invocation; public metadata contracts remain platform-neutral.
  QA scenarios: happy = `cmake --build build --target seriona_metadata_mpris_tests seriona_metadata_service_tests && ctest --test-dir build -R 'seriona_metadata_(mpris|service)' --output-on-failure`; failure = regression test simulates command after unsubscribe and expects no callback/use-after-clear. Evidence `.omo/evidence/task-3-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(metadata): synchronize media command sink`

- [x] 4. Phase 2: harden control dispatch exception and stop lifecycle
  What to do / Must NOT do: Add tests where posted control work throws and where stop is requested from/near worker context, then ensure `MediaController::dispatch()` always fulfills promises via value or exception conversion and `ControlEventLoop::stop()`/destructor cannot leave a joinable thread that terminates. Do not swallow exceptions in a way that leaves callers blocked; do not add blocking heavy work to the control event loop.
  Allowed file domain: `src/control/control_event_loop.h`, `src/control/control_event_loop.cpp`, `src/control/media_controller.cpp`, `tests/control/media_controller_tests.cpp`, `tests/control/control_contract_tests.cpp`, `tests/control/control_test_harness.h`, `tests/control/control_test_harness.cpp`.
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 5,9,10 | Can parallelize with: 2,3
  References: `DESIGN.md:157`, `VIBE_CODING_BUG_REPORT.md:109`, `VIBE_CODING_BUG_REPORT.md:111`, `VIBE_CODING_BUG_REPORT.md:206`, `tests/CMakeLists.txt:149`, `tests/CMakeLists.txt:153`.
  Acceptance criteria: throwing work returns deterministic failure or propagates through `future.get()` without deadlock; `ControlEventLoop` destruction never calls `std::terminate`; test has timeout-safe assertions.
  QA scenarios: happy = `cmake --build build --target seriona_media_controller_tests seriona_control_contract_tests && ctest --test-dir build -R 'seriona_(media_controller|control_contract)' --output-on-failure`; failure = regression test reproduces old promise hang using bounded wait and fails before fix. Evidence `.omo/evidence/task-4-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(control): complete dispatch promises on exceptions`

- [x] 5. Phase 2/4 bridge: move scanner work out of ControlEventLoop
  What to do / Must NOT do: Add a fake slow scanner test proving `Pause`/position event processing does not wait for scan completion, then refactor control/scanner interaction so `MediaController::scanLibrary()` posts only a lightweight start request and scanner does TagReader/SQLite/tree work on scanner-owned worker/scheduler. Scanner events must return by value to control. Do not run filesystem traversal, TagReader, SQLite, or watcher debounce inside `ControlEventLoop`.
  Allowed file domain: `inc/seriona/scanner/file_scanner_service.h`, `inc/seriona/scanner/scanner_contracts.h` only if async contract is required, `src/scanner/file_scanner_service.cpp`, `src/scanner/file_scanner_orchestrator.cpp`, `inc/seriona/scanner/scan_scheduler.h`, `src/scanner/scan_scheduler.cpp`, `src/control/media_controller.cpp`, `tests/control/media_controller_tests.cpp`, `tests/control/control_test_harness.*`, `tests/scanner/scanner_scheduler_tests.cpp`, `tests/scanner/scanner_service_tests.cpp`.
  Parallelization: Wave 3 | Blocked by: 4 | Blocks: 10
  References: `DESIGN.md:254`, `VIBE_CODING_BUG_REPORT.md:11`, `VIBE_CODING_BUG_REPORT.md:14`, `VIBE_CODING_BUG_REPORT.md:153`, `VIBE_CODING_BUG_REPORT.md:297`, `tests/CMakeLists.txt:153`, `tests/CMakeLists.txt:172`, `tests/CMakeLists.txt:185`.
  Acceptance criteria: while fake scanner blocks, a control command and a fake `PlaybackPositionUpdated` event are processed within bounded time; scanner completion still updates library snapshot; scanner public stable headers do not leak TagReader/SQLite/watcher internals.
  QA scenarios: happy = `cmake --build build --target seriona_media_controller_tests seriona_scanner_scheduler_tests seriona_scanner_service_tests && ctest --test-dir build -R 'seriona_(media_controller|scanner_scheduler|scanner_service)' --output-on-failure`; failure = old implementation causes bounded wait timeout in new control blocking regression. Evidence `.omo/evidence/task-5-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(control): schedule scanner work off the event loop`

- [x] 6. Phase 3: introduce audio command worker and transactional seek
  What to do / Must NOT do: Add tests for nonblocking audio public commands, ordered `seek/pause/stop`, stale generation cancellation, and seek failure rollback/error. Refactor `SingleTrackAudioPlaybackService` so public methods enqueue commands and return quickly; exactly one audio control worker mutates FFmpeg source, filter pipeline, PCM queue, clock, output device, and state machine. Seek must prepare new decoder/filter/queue generation before publishing or must fail without half-applied state. Do not let control thread perform FFmpeg/filter/device work. Preserve current public `AudioPlaybackService` and `BackendEvent` contracts; if executor believes `inc/seriona/audio/audio_contracts.h` must change, it must stop and request a separate serial contract task instead of editing it inside this parallelizable task.
  Allowed file domain: `inc/seriona/audio/audio_playback_service.h`, `src/audio/audio_playback_service.cpp`, `inc/seriona/audio/playback_state_machine.h`, `src/audio/playback_state_machine.cpp`, `tests/audio/audio_player_single_track_tests.cpp`, `tests/audio/audio_player_small_buffer_tests.cpp`, `tests/audio/audio_error_matrix_tests.cpp`, `tests/audio/audio_shutdown_lifecycle_tests.cpp`, `tests/audio/playback_state_machine_tests.cpp`.
  Parallelization: Wave 4a | Blocked by: 1,2 | Blocks: 10 | Can parallelize with: 8 only
  References: `DESIGN.md:374`, `DESIGN.md:395`, `VIBE_CODING_BUG_REPORT.md:37`, `VIBE_CODING_BUG_REPORT.md:40`, `VIBE_CODING_BUG_REPORT.md:236`, `VIBE_CODING_BUG_REPORT.md:283`, `tests/CMakeLists.txt:31`, `tests/CMakeLists.txt:43`, `tests/CMakeLists.txt:95`, `tests/CMakeLists.txt:108`.
  Acceptance criteria: public `play/pause/stop/seek/loadTrack` do not synchronously run decode/device work; worker serializes mutations; ordered command tests pass; seek failure leaves previous context intact or transitions cleanly to Error with no dangling device/queue.
  QA scenarios: happy = `cmake --build build --target seriona_playback_state_machine_tests seriona_audio_player_single_track_tests seriona_audio_player_small_buffer_tests seriona_audio_error_matrix_tests seriona_audio_shutdown_lifecycle_tests && ctest --test-dir build -R 'seriona_(playback_state_machine|audio_player_single_track|audio_player_small_buffer|audio_error_matrix|audio_shutdown_lifecycle)' --output-on-failure`; failure = fake slow FFmpeg/device path demonstrates command returns too slowly or stale seek overwrites newer stop before fix. Evidence `.omo/evidence/task-6-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(audio): serialize playback commands on audio worker`

- [x] 7. Phase 4: make metadata snapshot updates asynchronous from control
  What to do / Must NOT do: Add a slow metadata backend test proving `publishPlayerSnapshot()`/control command processing does not wait for platform update, then move metadata update work into metadata-owned queue/worker or coalescing async adapter. Control may enqueue latest `PlatformMediaState` only. Do not call D-Bus/SMTC or metadata backend synchronously from the control event loop.
  Allowed file domain: `inc/seriona/metadata/metadata_contracts.h` only if async API contract must be clarified, `src/metadata/metadata_service.cpp`, `src/metadata/metadata_service_backend.cpp`, `src/metadata/metadata_synchronizer.cpp`, `src/control/media_controller.cpp`, `tests/metadata/metadata_service_tests.cpp`, `tests/metadata/metadata_service_recording_tests.cpp`, `tests/control/media_controller_tests.cpp`.
  Parallelization: Wave 4b | Blocked by: 3,4,5 | Blocks: 9,10 | Can parallelize with: none
  References: `DESIGN.md:161`, `VIBE_CODING_BUG_REPORT.md:190`, `VIBE_CODING_BUG_REPORT.md:212`, `VIBE_CODING_BUG_REPORT.md:215`, `tests/CMakeLists.txt:134`, `tests/CMakeLists.txt:139`, `tests/CMakeLists.txt:153`.
  Acceptance criteria: slow metadata update cannot block control event loop progress; latest state coalescing is deterministic; unsubscribe/shutdown drains or cancels worker safely.
  QA scenarios: happy = `cmake --build build --target seriona_metadata_service_tests seriona_metadata_service_recording_tests seriona_media_controller_tests && ctest --test-dir build -R 'seriona_(metadata_service|metadata_service_recording|media_controller)' --output-on-failure`; failure = old synchronous metadata backend causes bounded control test timeout. Evidence `.omo/evidence/task-7-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(metadata): post platform updates off control loop`

- [x] 8. Phase 4: implement scanner cache capacity/checkpoint policy and hide adapter internals
  What to do / Must NOT do: Add scanner cache tests for soft/hard database size policy hooks, passive checkpoint scheduling, and cleanup decisions; add public-header boundary test or compile test ensuring scanner stable contracts do not expose SQLite cache DTOs through TagReader adapter. Move or narrow `tag_reader_metadata_adapter` so public `inc/seriona/scanner/...` stable surface does not force consumers to include cache internals. Do not leak TagReader/SQLite/watcher implementation details into `scanner_contracts.h` or `file_scanner_service.h`.
  Allowed file domain: `inc/seriona/scanner/tag_reader_metadata_adapter.h`, `src/scanner/tag_reader_metadata_adapter.cpp`, `inc/seriona/scanner/cache/sqlite_scanner_cache.h`, `src/scanner/cache/sqlite_scanner_cache.cpp`, `src/scanner/file_scanner_orchestrator.cpp` only if adapter include changes require it, `tests/scanner/scanner_cache_tests.cpp`, `tests/scanner/scanner_tagreader_tests.cpp`, `tests/scanner/scanner_contract_tests.cpp`.
  Parallelization: Wave 4a | Blocked by: 3 | Blocks: 10 | Can parallelize with: 6 only
  References: `DESIGN.md:562`, `DESIGN.md:565`, `VIBE_CODING_BUG_REPORT.md:242`, `VIBE_CODING_BUG_REPORT.md:278`, `VIBE_CODING_BUG_REPORT.md:280`, `tests/CMakeLists.txt:176`, `tests/CMakeLists.txt:181`, `tests/CMakeLists.txt:125`.
  Acceptance criteria: cache policy tests verify checkpoint/cleanup decisions without requiring huge real files; scanner contract compile tests do not include SQLite DTOs through stable headers; existing scanner behavior remains intact.
  QA scenarios: happy = `cmake --build build --target seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests && ctest --test-dir build -R 'seriona_scanner_(cache|tagreader|contract)' --output-on-failure`; failure = old headers expose cache DTO or policy no-op assertion fails. Evidence `.omo/evidence/task-8-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(scanner): enforce cache policy and public boundaries`

- [x] 9. Phase 4: make snapshot subscriptions value-safe and nonblocking
  What to do / Must NOT do: Add tests proving subscriber callbacks cannot retain dangling references and a slow/reentrant subscriber cannot block control state progression indefinitely. Change subscription callback semantics to by-value delivery or controlled post/copy queue; initial snapshot and publish must use the same safe delivery semantics. Do not let arbitrary subscriber code execute under control state locks.
  Allowed file domain: `inc/seriona/control/control_contracts.h`, `src/control/subscription_store.cpp`, `src/control/media_controller.cpp` only if publish integration changes, `tests/control/control_contract_tests.cpp`, `tests/control/media_controller_tests.cpp`.
  Parallelization: Wave 4c | Blocked by: 4,7 | Blocks: 10 | Can parallelize with: none
  References: `DESIGN.md:160`, `DESIGN.md:187`, `VIBE_CODING_BUG_REPORT.md:198`, `VIBE_CODING_BUG_REPORT.md:200`, `VIBE_CODING_BUG_REPORT.md:202`, `tests/CMakeLists.txt:149`, `tests/CMakeLists.txt:153`.
  Acceptance criteria: subscriber receives an owned snapshot value or queued copy; slow subscriber test does not starve control event processing beyond bounded threshold; API contract documents value semantics in type/signature or tests.
  QA scenarios: happy = `cmake --build build --target seriona_control_contract_tests seriona_media_controller_tests && ctest --test-dir build -R 'seriona_(control_contract|media_controller)' --output-on-failure`; failure = old const-reference callback allows retained reference misuse test or blocking subscriber timeout. Evidence `.omo/evidence/task-9-seriona-vibe-bug-refactor.md`.
  Commit: Y | `fix(control): deliver snapshots with safe value semantics`

- [x] 10. Final integration: verify full architecture contract and no scope drift
  What to do / Must NOT do: Run full build/test, inspect diffs for forbidden real-time operations and boundary leaks, and ensure every prior task has evidence and commit. Do not implement new fixes here except minimal corrections required by full-test failures; if a failure maps to a previous task domain, return to that task loop and commit there.
  Allowed file domain: `.omo/evidence/task-10-seriona-vibe-bug-refactor.md` only, unless fixing a failed prior task by returning to that task's allowed domain.
  Parallelization: Wave 5 | Blocked by: 5,6,7,8,9 | Blocks: final
  References: `DESIGN.md:157`, `DESIGN.md:214`, `DESIGN.md:405`, `VIBE_CODING_BUG_REPORT.md:302`, `CMakeLists.txt:201`.
  Acceptance criteria: full `cmake --build build` and `ctest --test-dir build --output-on-failure` pass; read-only search confirms `renderCallback` path has no forbidden calls; `git status --short` contains only expected clean or documented plan/evidence artifacts after commits.
  QA scenarios: happy = `cmake --build build && ctest --test-dir build --output-on-failure`; failure = any failing test sends executor back to owning task and prevents final completion. Evidence `.omo/evidence/task-10-seriona-vibe-bug-refactor.md`.
  Commit: N | final verification only unless previous-task fix required.

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [x] F1. Plan compliance audit: read `.omo/plans/seriona-vibe-bug-refactor.md`, all `.omo/evidence/task-*`, and `git log --oneline -10`; verify every completed code task has a matching evidence file, passing task-specific tests, and one atomic commit using only allowed Git write commands.
- [x] F2. Code quality review: inspect changed `src/`/`inc/` files for RAII, no source-level data races, no callback under lock, no hidden blocking work in control event loop, no public leakage of platform/private dependency types.
- [x] F3. Real runtime QA: run `cmake --build build` and `ctest --test-dir build --output-on-failure`; if audio hardware-independent tests are available only through fake backend, do not require physical audio hardware.
- [x] F4. Scope fidelity: verify W8/`DESIGN.md` was not modified by this execution, Qt/QML was not introduced, realtime callback forbidden-operation search is clean, and parallel tasks did not touch overlapping file domains except through declared dependencies.

## Commit strategy
- Pre-task rule: run `git status --short` before edits; if unrelated dirty files exist, do not touch or stage them.
- Allowed Git reads: `git status`, `git diff`, `git log`, `git show`, `git blame` as needed.
- Allowed Git writes only: `git add <explicit target file...>` and `git commit -m "<message>"`.
- Forbidden Git writes: `git add .`, `git reset`, `git restore`, `git checkout`, `git stash`, `git rebase`, `git merge`, `git push`, force operations, interactive operations, global config changes.
- Atomicity: after each task's tests pass, stage only that task's modified files plus its evidence file if the execution convention commits evidence; commit with the exact message listed in the task.
- Parallel safety: when tasks run concurrently, code edits may happen in isolated file domains, but all `git add`/`git commit` operations must acquire a single executor-level Git lock and run sequentially.
- Failure policy: if commit fails due to hooks/tests, fix within the same task domain and create a new normal commit after passing; do not amend unless the user explicitly authorizes.

## Success criteria
- All 11 todos including preflight and final integration are completed or explicitly deferred by user; no task is skipped silently.
- Each Bug/report item is covered: C1, C2, C3, C4, C5, W1, W2, W3, W4, W5, W6, W7, A2.1; W8 remains excluded as already fixed.
- Every implementation task has a failing-test-first note, passing task-specific ctest transcript, and an atomic commit.
- Full build and full ctest pass: `cmake --build build` and `ctest --test-dir build --output-on-failure`.
- `AudioOutputDevice::renderCallback()` remains RT-safe: no FFmpeg, logs, heap allocation, blocking locks, sink dispatch, or device lifecycle operations.
- `mediaController` event loop only performs lightweight command reduction/state merge/scheduling; scanner, audio, metadata heavy work belongs to their own workers/adapters.
- Public contracts remain platform- and implementation-neutral; no Qt/QML or forbidden private dependency type leaks.
