# mediaController 模块实现计划

## TL;DR
> **Summary**: 实现纯 C++23 `mediaController` 控制层：它是唯一业务状态归并点，串行消费音频/扫描/元数据事件，处理高层控制命令，并向 UI/OS 代理发布不可变快照和领域通知。
> **Deliverables**:
> - 扩展 `inc/seriona/control/control_contracts.h`，补齐 library state、domain notification、controller facade/service API、命令结果与错误模型。
> - 新增 `seriona_control` 模块库，源码位于 `src/control/...`。
> - 新增 fake-driven doctest/CTest 覆盖命令语义、事件归并、订阅/注销、shutdown 和边界静态检查。
> - 更新 CMake/test 注册，让 `ctest --test-dir build -R seriona.control --output-on-failure` 成为控制层回归入口。
> **Effort**: Large
> **Parallel**: YES - 7 waves
> **Critical Path**: Task 1 → Task 4 → Task 3 → Task 6 → Task 7 → Final Verification

## Context
### Original Request
用户要求读取 `DESIGN.md`，确认 `mediaController` 模块具体要求，并通过网络搜索调研，基于模块要求和资料编写 `mediaController` 模块的代码编写/修改计划书。

### Interview Summary
- 用户确认：文件扫描模块、音频播放模块、元数据共享模块已完成；只剩 `mediaController` 未开始。
- 本轮只产出计划，不编写或修改 C++ 源码。
- 语言与文档：新增面向用户/项目的内容使用中文；计划文件保存在 `.omo/plans/`。

### Metis Review (gaps addressed)
- 不假设存在独立 `seriona_audio` 库目标；控制层只使用音频公共契约和当前 app/test 源码布局。
- 计划必须补齐 `LibraryStateSnapshot`、扫描状态、领域通知、`MediaController` facade/API、命令返回/错误模型。
- 音频 `BackendEventSink` 与扫描 `ScannerEventSink` 是不同契约；控制层内部适配为私有队列事件，不把它们写成已统一的公共事件。
- 串行执行器必须规定启动/停止、队列 drain、析构取消订阅、回调异常和重入/注销行为。
- 明确命令语义：无当前曲目的 `Play`、`SelectTrack` 校验、`SkipNext/Previous`、repeat/shuffle、`SeekBy` clamp、volume clamp、metadata 命令回流。

## Work Objectives
### Core Objective
实现一个纯 C++23 控制层模块，使 `mediaController` 成为扫描、音频和元数据共享模块之间唯一的业务编排点与权威状态源。

### Deliverables
- `inc/seriona/control/control_contracts.h`：扩展公共控制契约。
- `inc/seriona/control/media_controller.h`：新增控制层 facade/service 公共入口。
- `src/control/media_controller.cpp`、`src/control/control_state_reducer.*`、`src/control/control_event_loop.*`、`src/control/media_controller_module.*`：控制层实现与内部组件。
- `tests/control/...`：契约、命令语义、事件归并、订阅、metadata 回流、shutdown 测试。
- `CMakeLists.txt`、`tests/CMakeLists.txt`：新增 `seriona_control` 和 `seriona.control*` 测试注册。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` 配置成功且不新增外部依赖。
- `cmake --build build --target seriona_control` 构建成功。
- `cmake --build build --target seriona_control_contract_tests seriona_media_controller_tests` 构建成功。
- `ctest --test-dir build -R seriona.control --output-on-failure` 全部通过。
- `rg -n "Qt|QML|sdbus|dbus|windows.h|MPRIS|SMTC|TagReader|SQLite|miniaudio" inc/seriona/control src/control tests/control` 只允许测试说明字符串命中；生产控制层无命中。

### Must Have
- 所有底层事件 sink 只 post 到控制层队列；不在底层线程同步执行业务逻辑。
- 控制层状态只在单一串行执行器/事件消费点修改。
- `MediaController` 控制类不要过度拆分；控制流程、命令处理、模块适配和状态发布尽量集中在 `MediaController` 类及 `src/control/media_controller.cpp` 中，只有通用底层机制如事件循环/订阅存储可独立文件化。
- 初次订阅立即收到当前 `PlayerStateSnapshot` 和 library snapshot。
- 订阅句柄注销后不再回调；shutdown 后 sink 投递不崩溃。
- 旧版本底层事件不得覆盖新命令导致的更新状态。
- 元数据共享模块的系统媒体命令回调必须投递到同一条控制命令路径。
- 生产默认事件循环在 `MediaController::start()` 启动单 worker thread；测试通过 `MediaControllerOptions{.runInlineForTests = true}` 禁止 worker，并由 `drainForTests()` 显式推进。
- 初始 library state 固定为 version `0`、scan status `Idle`、无 root snapshot、空 progress、无错误。
- `MediaControllerCommandResult` 固定包含 `accepted`、`MediaControllerErrorCode code`、`std::string message`；成功 code 为 `None`，拒绝 code 至少包含 `ControllerStopped`、`NoPlayableTrack`、`TrackNotInLibrary`、`InvalidCommand`。

### Must NOT Have (guardrails, AI slop patterns, scope boundaries)
- 不引入 Qt/QML/UI 类型。
- 不引入 MPRIS/SMTC/Windows/DBus 平台 API 到 `inc/seriona/control` 或 `src/control`。
- 不依赖真实音频硬件、真实媒体目录、真实 watcher、真实 DBus/SMTC。
- 不新增播放列表编辑、收藏、配置文件、日志系统实现或虚拟歌单。
- 不重构音频为独立库目标；首期只创建 `seriona_control` 并链接现有 scanner/metadata 目标及公共契约。
- 不让 scanner、audio、metadata 彼此直接调用或订阅。
- 不把 `MediaController` 的业务方法过度拆到多个小文件、小类或过度抽象层；禁止为每个命令单独创建 handler 文件/类。

### Git Constraints
- 每完成一个“功能”必须执行一次 `git commit`；功能粒度由实现者按用户可理解的能力边界判断，不等同于本计划的 Task 编号。
- 允许无限制使用 git 读取操作，例如 `git status`、`git diff`、`git log`、`git show`、`git blame`。
- git 写入操作只允许 `git add` 和 `git commit`；禁止 `git reset`、`git checkout`、`git restore`、`git branch`、`git merge`、`git rebase`、`git tag`、`git push`、`git stash` 等写入操作。
- 每次 commit 前必须用 git 读取操作确认仅暂存本功能相关文件；不得提交 `.omo/evidence/`，除非用户另行明确要求。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after + doctest/CTest，沿用现有 fake backend/test harness 风格。
- QA policy: 每个任务包含 agent-executed happy path 与 failure/edge case 场景。
- Evidence: `.omo/evidence/task-{N}-{slug}.{ext}`。

## Execution Strategy
### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = under-splitting.
> Extract shared dependencies as Wave-1 tasks for max parallelism.

Wave 1: Task 1 contract extension.
Wave 2: Task 2 fake test harness, Task 4 event loop/subscriptions.
Wave 3: Task 3 CMake scaffolding for files that now exist.
Wave 4: Task 5 state reducer/command semantics.
Wave 5: Task 6 module integration adapters.
Wave 6: Task 7 metadata callback + shutdown lifecycle.
Wave 7: Task 8 end-to-end controller tests/static guardrails.

### Dependency Matrix (full, all tasks)
- Task 1: blocks Tasks 2, 3, 4, 5, 6, 7, 8.
- Task 2: blocks Tasks 3, 5, 6, 7, 8.
- Task 3: blocks build/test execution for Tasks 5-8.
- Task 4: blocks Tasks 3, 5, 6, 7, 8.
- Task 5: blocks Tasks 6, 7, 8 and requires Task 3 to append its new source file to CMake.
- Task 6: blocks Tasks 7, 8 and requires Task 3's CMake pattern to be extended with facade sources.
- Task 7: blocks Task 8.
- Task 8: final implementation verification before F1-F4.

### Agent Dispatch Summary (wave → task count → categories)
- Wave 1 → 1 task → quick.
- Wave 2 → 2 tasks → quick, deep.
- Wave 3 → 1 task → quick.
- Wave 4 → 1 task → deep.
- Wave 5 → 1 task → unspecified-high.
- Wave 6 → 1 task → unspecified-high.
- Wave 7 → 1 task → deep.

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.
> Commit statements below are feature-boundary guidance only. Do not commit merely because a task ended; commit when a user-visible/plan-visible feature is complete and verified.

- [x] 1. Extend control public contracts

  **What to do**: Modify `inc/seriona/control/control_contracts.h` only. Add `LibraryScanStatus`, `LibraryStateSnapshot`, `ControlDomainNotificationKind`, `ControlDomainNotification`, `MediaControllerErrorCode`, `MediaControllerCommandResult`, `MediaControllerOptions`, `MediaControllerDependencies`, and callback/factory aliases for library snapshot and domain notification subscriptions. `MediaControllerErrorCode` values are exactly `None`, `ControllerStopped`, `NoPlayableTrack`, `TrackNotInLibrary`, `InvalidCommand`, `BackendRejected`. `MediaControllerCommandResult` fields are exactly `bool accepted`, `MediaControllerErrorCode code`, `std::string message`. `MediaControllerOptions` fields are exactly `bool runInlineForTests{false}` and `std::uint64_t shuffleSeed{0}`. `MediaControllerDependencies` fields are exactly `std::shared_ptr<audio::AudioPlaybackService> audio`, `std::shared_ptr<scanner::FileScannerService> scanner`, `std::unique_ptr<metadata::MetadataSharingService> metadata`. Keep existing `PlayerStateSnapshot`, `MediaControlCommand`, and `SubscriptionHandle` source-compatible. Use only standard library includes plus public audio/scanner/metadata/control headers; prefer value types and `std::optional`/`std::variant` over inheritance.
  **Must NOT do**: Do not include Qt/QML/platform headers, audio implementation headers, SQLite, TagReader, miniaudio, or metadata private headers.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: single public header extension with no implementation complexity.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`frontend-design`, `playwright`] - no UI/browser work.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: 2,3,4,5,6,7,8 | Blocked By: none

  **References** (executor has NO interview context - be exhaustive):
  - Requirement: `DESIGN.md:34` - `mediaController` maintains authoritative state and stable control interfaces.
  - Requirement: `DESIGN.md:156` - down commands from `mediaController`, up events to it.
  - Requirement: `DESIGN.md:656` - snapshots include track, playback, capabilities, library tree version, scan state, and recent errors.
  - Pattern: `inc/seriona/control/control_contracts.h:94` - existing `PlayerStateSnapshot` shape to extend without breakage.
  - Pattern: `inc/seriona/scanner/scanner_contracts.h:124` - `PlaylistTreeSnapshot` version/tree fields for library snapshot embedding/reference.
  - Pattern: `inc/seriona/metadata/metadata_contracts.h:28` - metadata already consumes `control::PlayerStateSnapshot`.

  **Acceptance Criteria** (agent-executable only):
  - [ ] `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` configures after header changes.
  - [ ] `cmake --build build --target seriona_metadata_contract_tests seriona_scanner_contract_tests` reaches compile for contracts or fails only on pre-existing doctest include issue documented separately.
  - [ ] `rg -n "Qt|QML|sdbus|dbus|windows.h|MPRIS|SMTC|TagReader|SQLite|miniaudio" inc/seriona/control` has no production contract matches.

  **QA Scenarios** (MANDATORY - task incomplete without these):
  ```
  Scenario: Contract compilation smoke
    Tool: Bash
    Steps: Run `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`.
    Expected: Configure exits 0 and reports no new dependency lookup beyond existing CMake dependencies.
    Evidence: .omo/evidence/task-1-control-contract-config.txt

  Scenario: Boundary grep
    Tool: Bash
    Steps: Run `rg -n "Qt|QML|sdbus|dbus|windows.h|MPRIS|SMTC|TagReader|SQLite|miniaudio" inc/seriona/control`.
    Expected: No output for production control headers.
    Evidence: .omo/evidence/task-1-control-contract-boundary.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `feat(control): define media controller contracts` | Files: [`inc/seriona/control/control_contracts.h`] | Rule: commit only when the complete public contract feature is implemented and verified.

- [x] 2. Add control fake harness and contract tests

  **What to do**: Create `tests/control/control_test_harness.h/.cpp` with fake `AudioPlaybackService`, fake `FileScannerService`, fake `MetadataSharingService`, snapshot collectors, command collectors, and deterministic clock helpers. Create `tests/control/control_contract_tests.cpp` validating default-constructibility, value semantics, subscription handle unsubscribe behavior, and no platform-only types in public contracts. Fakes must expose counters for calls like `loadTrack`, `play`, `pause`, `scan`, `startWatching`, `update`, and captured callbacks.
  **Must NOT do**: Do not use real FFmpeg/miniaudio/audio hardware, real filesystem scanning, real SQLite, real DBus/SMTC, or sleeps for synchronization.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: test harness and contract tests can be implemented from existing patterns.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`debugging`] - not investigating runtime crash.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 3,5,6,7,8 | Blocked By: 1

  **References**:
  - Pattern: `tests/scanner/scanner_test_harness.h:20` - fake scanner harness style.
  - Pattern: `tests/scanner/scanner_test_harness.cpp:41` - deterministic fake implementations.
  - Pattern: `tests/audio/audio_output_device_tests.cpp:31` - fake backend seam with counters.
  - Pattern: `tests/metadata/metadata_service_tests.cpp:18` - recording hooks and controlled service tests.
  - Test registration: `tests/CMakeLists.txt:1` and `tests/CMakeLists.txt:495` - one executable per concern plus `add_test(NAME seriona.<topic> ...)`.

  **Acceptance Criteria**:
  - [ ] `cmake --build build --target seriona_control_contract_tests` builds once Task 3 registers the target.
  - [ ] `ctest --test-dir build -R seriona.control_contract --output-on-failure` passes once Task 3 registers the test.
  - [ ] Tests use fakes only; `rg -n "generated_audio_fixtures|/tmp|DBus|SMTC|MPRIS" tests/control` has no dependency on real platform services.

  **QA Scenarios**:
  ```
  Scenario: Contract tests pass
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_contract --output-on-failure` after target registration.
    Expected: All control contract assertions pass.
    Evidence: .omo/evidence/task-2-control-contract-test.txt

  Scenario: Fake-only harness guard
    Tool: Bash
    Steps: Run `rg -n "DBus|SMTC|MPRIS|miniaudio|SQLite|TagReader" tests/control`.
    Expected: No matches except explicit boundary assertion strings.
    Evidence: .omo/evidence/task-2-control-fake-boundary.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `test(control): add media controller fake harness` | Files: [`tests/control/control_test_harness.h`, `tests/control/control_test_harness.cpp`, `tests/control/control_contract_tests.cpp`] | Rule: commit only when the complete fake harness/contract-test feature is implemented and verified.

- [x] 3. Register seriona_control build and tests

  **What to do**: Update root `CMakeLists.txt` to add static library `seriona_control` using only files that already exist after Tasks 2 and 4: `src/control/control_event_loop.cpp` and `src/control/subscription_store.cpp`. Include `inc` publicly and `src` privately. Link `seriona_control` to `seriona_scanner` and `seriona_metadata`; do not link implementation-only audio objects because audio currently has no library target. Update `tests/CMakeLists.txt` with `seriona_control_contract_tests` and the initial `seriona_media_controller_tests` executable using only existing test files from Task 2 and any Task 4 event-loop tests, includes, definitions, links, and `add_test(NAME seriona.control_contract ...)`, `add_test(NAME seriona.control_controller ...)`. Later Tasks 5 and 6 must append their new source files to `seriona_control` and test target in the same task that creates those files.
  **Must NOT do**: Do not create or refactor an audio static library target; do not remove existing tests; do not change TagReader lookup.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: CMake registration only.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`git-master`] - no commit requested during implementation unless executor chooses commit step.

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: build/test execution for 5,6,7,8 | Blocked By: 1,2,4

  **References**:
  - Pattern: `CMakeLists.txt:71` - `seriona_scanner` target registration.
  - Pattern: `CMakeLists.txt:83` - `seriona_metadata` target registration.
  - Pattern: `CMakeLists.txt:120` - link libraries for module targets.
  - Pattern: `tests/CMakeLists.txt:1` - add executable test target style.
  - Pattern: `tests/CMakeLists.txt:495` - CTest name style.
  - Guardrail: Metis - no assumption of existing `seriona_audio` target.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` configures.
  - [ ] `cmake --build build --target seriona_control` builds immediately after Task 3 because every referenced source file already exists.
  - [ ] `ctest -N --test-dir build | rg "seriona.control"` lists the new control tests.

  **QA Scenarios**:
  ```
  Scenario: CTest registration visible
    Tool: Bash
    Steps: Run `ctest -N --test-dir build | rg "seriona.control"`.
    Expected: Output includes `seriona.control_contract` and `seriona.control_controller`.
    Evidence: .omo/evidence/task-3-control-ctest-registration.txt

  Scenario: No audio target refactor
    Tool: Bash
    Steps: Run `git diff -- CMakeLists.txt app/CMakeLists.txt tests/CMakeLists.txt`.
    Expected: Diff adds `seriona_control` and tests using only existing Task 2/4 files; no missing future file references, no new `seriona_audio` target, and no removal of existing audio source wiring.
    Evidence: .omo/evidence/task-3-control-cmake-diff.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `build(control): register media controller targets` | Files: [`CMakeLists.txt`, `tests/CMakeLists.txt`, `app/CMakeLists.txt` if required] | Rule: commit only when the complete build/CTest registration feature is implemented and verified.

- [x] 4. Implement serial event loop and subscription store

  **What to do**: Add `src/control/control_event_loop.h/.cpp` and `src/control/subscription_store.h/.cpp`. Event loop behavior is fixed: when `MediaControllerOptions::runInlineForTests == false`, `start()` creates one worker thread that serially drains posted work; when `runInlineForTests == true`, no worker thread is created and tests must call `drainForTests()` to execute queued work. It supports `bool post(std::function<void()>)`, `void drainForTests()`, `void stop()`, and destructor-safe shutdown. `post` after stop returns `false` and does not throw. Subscription store supports immediate initial snapshot delivery, monotonic subscription IDs, explicit unsubscribe, safe unsubscribe during callback, and exception containment: catch subscriber exceptions and convert to domain notification/error counter without breaking later subscribers.
  **Must NOT do**: Do not expose event loop in public API; do not use scanner/audio worker pools as control state executor; do not block audio/scanner callback threads.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: lifecycle, concurrency, and reentrancy semantics require care.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`debugging`] - implementation task, not live bug investigation.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 3,5,6,7,8 | Blocked By: 1

  **References**:
  - Requirement: `DESIGN.md:159` - sink implementation only posts into control serial executor.
  - Requirement: `DESIGN.md:232` - callback shape but async post semantics.
  - Requirement: `DESIGN.md:254` - event executor and heavy worker pools are separate.
  - Requirement: `DESIGN.md:658` - subscriptions return uninstallable handle and initial snapshot.
  - Requirement: `DESIGN.md:671` - unsubscribe avoids callback after subscriber destruction.
  - External: `https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/core/strands.html` - serialized handler model reference.
  - External: `https://fuchsia.dev/fuchsia-src/development/languages/c-cpp/thread-safe-async` - ordered async dispatcher behavior reference.

  **Acceptance Criteria**:
  - [ ] `cmake --build build --target seriona_control` builds.
  - [ ] Tests prove `post` does not execute inline by checking a fake sink only mutates state after `drainForTests()`.
  - [ ] Tests prove unsubscribe during callback does not invalidate iteration or call removed subscriber again.
  - [ ] Tests prove `post` after `stop()` does not throw or mutate state.

  **QA Scenarios**:
  ```
  Scenario: Post-only sink behavior
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure --test-output-size-passed 2000`.
    Expected: Test case `control event loop posts without inline execution` passes.
    Evidence: .omo/evidence/task-4-control-post-only.txt

  Scenario: Shutdown safety
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test case `control event loop drops posts after stop without crash` passes.
    Evidence: .omo/evidence/task-4-control-shutdown.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `feat(control): add serial event loop and subscriptions` | Files: [`src/control/control_event_loop.h`, `src/control/control_event_loop.cpp`, `src/control/subscription_store.h`, `src/control/subscription_store.cpp`, `tests/control/media_controller_tests.cpp`] | Rule: commit only when the complete event-loop/subscription feature is implemented and verified.

- [x] 5. Implement state reducer and command semantics

  **What to do**: Add `src/control/control_state_reducer.h/.cpp` and append `src/control/control_state_reducer.cpp` to `seriona_control` in `CMakeLists.txt` in this same task. Reducer owns `PlayerStateSnapshot`, `LibraryStateSnapshot`, repeat/shuffle/current-track selection state, last event versions per source, and recent domain notifications. Implement deterministic command semantics: `Play` without current track selects first playable track from current library if present else emits no-track notification and remains stopped; `SelectTrack` validates `TrackIdentity.trackId`/file path against current `PlaylistTreeSnapshot`; `SkipNext/Previous` use deterministic flattened tree order from current snapshot; repeat-one repeats current track; repeat-all wraps around flattened tree; shuffle uses injectable deterministic RNG seed for tests; `SeekBy` clamps to `[0,duration]` when duration known and never below zero; `SetVolume` clamps `[0.0F,1.0F]`; `SetMuted` updates snapshot and forwards audio command. Implement audio event reduction for `PlaybackStateChanged`, `TrackChanged`, `PlaybackPositionUpdated`, `PositionDiscontinuity`, `PlaybackEnded`, `OutputFormatChanged`, `OutputModeFallback`, `PlaybackError`. Implement scanner event reduction for scan started/progress/snapshot/completed/stopped/error.
  **Must NOT do**: Do not call audio/scanner services from reducer directly; reducer returns intents/commands to be executed by facade.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: central state machine and edge-case semantics.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`security-research`] - no security audit requested.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: 6,7,8 | Blocked By: 1,2,3,4

  **References**:
  - Requirement: `DESIGN.md:187` - control layer publishes complete snapshots, not raw playback events.
  - Requirement: `DESIGN.md:197` - domain notifications for one-time completion/error/fallback semantics.
  - Requirement: `DESIGN.md:205` - control caches true audio progress, does not invent authoritative progress.
  - Requirement: `DESIGN.md:492` - control owns current playlist tree snapshot.
  - Requirement: `FILE_SCANNER_ANALYSIS.md:144` - `mediaController` holds playlist tree root for UI/playback control.
  - Pattern: `inc/seriona/audio/audio_contracts.h:150` - playback event payload variants.
  - Pattern: `inc/seriona/scanner/scanner_contracts.h:131` - scanner event payload variants.

  **Acceptance Criteria**:
  - [ ] Tests cover no-library `Play`, first-track selection, invalid `SelectTrack`, valid `SelectTrack`, next/previous, repeat-one, repeat-all wrap, deterministic shuffle, seek clamp, volume clamp.
  - [ ] Tests cover old audio/scanner event versions being ignored.
  - [ ] Tests cover scanner snapshot updating library version and current library state.
  - [ ] Tests cover playback ended producing next-track intent or stopped state according to repeat/shuffle mode.

  **QA Scenarios**:
  ```
  Scenario: Command reducer happy path
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test cases for first track play, select valid track, skip next, repeat-all wrap, and deterministic shuffle pass.
    Evidence: .omo/evidence/task-5-control-command-happy.txt

  Scenario: Command reducer edge cases
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test cases for no-library play, invalid select, stale event ignored, seek clamp, and volume clamp pass.
    Evidence: .omo/evidence/task-5-control-command-edge.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `feat(control): reduce commands into authoritative state` | Files: [`src/control/control_state_reducer.h`, `src/control/control_state_reducer.cpp`, `tests/control/media_controller_tests.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` if test source list changes] | Rule: commit only when the complete command/state-reduction feature is implemented and verified.

- [x] 6. Implement MediaController facade and module adapters

  **What to do**: Add `inc/seriona/control/media_controller.h`, `src/control/media_controller.cpp`, and `src/control/media_controller_module.h/.cpp`, then append `src/control/media_controller.cpp` and `src/control/media_controller_module.cpp` to `seriona_control` in `CMakeLists.txt` in this same task. Public facade API is fixed: `class MediaController`; constructor `explicit MediaController(MediaControllerDependencies dependencies, MediaControllerOptions options = {})`; factory `std::unique_ptr<MediaController> makeMediaController(MediaControllerDependencies dependencies, MediaControllerOptions options = {})`; methods `void start()`, `void shutdown()`, `MediaControllerCommandResult submitCommand(const MediaControlCommand& command)`, `MediaControllerCommandResult scanLibrary(std::vector<scanner::ScannerRoot> roots, scanner::ScanMode mode)`, `SubscriptionHandle subscribePlayerState(PlayerStateSnapshotCallback callback)`, `SubscriptionHandle subscribeLibraryState(LibraryStateSnapshotCallback callback)`, `SubscriptionHandle subscribeDomainNotifications(ControlDomainNotificationCallback callback)`, `PlayerStateSnapshot playerStateSnapshot() const`, `LibraryStateSnapshot libraryStateSnapshot() const`, `audio::BackendEventSink audioEventSink()`, `scanner::ScannerEventSink scannerEventSink()`, and `void drainForTests()`. Facade must install audio/scanner sinks that only post private events, set metadata command callback to same command queue, and invoke metadata `start/update/stop` from committed `PlayerStateSnapshot` changes. Use reducer intents to call audio/scanner services outside reducer but on control executor.
  **Must NOT do**: Do not expose scanner/audio/metadata implementation details in public control API; do not call subscriber callbacks while holding reducer/state locks; do not synchronously call metadata platform API from bottom-module callback threads.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: integration across existing contracts and lifecycle boundaries.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`frontend-design`] - no UI work.

  **Parallelization**: Can Parallel: NO | Wave 5 | Blocks: 7,8 | Blocked By: 1,2,3,4,5

  **References**:
  - Requirement: `DESIGN.md:145` - command down, event up.
  - Requirement: `DESIGN.md:153` - all cross-module communication through `mediaController`.
  - Requirement: `DESIGN.md:654` - stable single event sink; true state updates only at control event consumer.
  - Requirement: `DESIGN.md:665` - adapters do their own thread switching; controller does not wait for UI/platform sync.
  - Pattern: `src/audio/audio_player.cpp:7` - facade delegates to service.
  - Pattern: `src/scanner/file_scanner_service.cpp:7` - facade/factory hides service implementation.
  - Pattern: `src/metadata/metadata_service_backend.cpp:180` - metadata factory/backend style.

  **Acceptance Criteria**:
  - [ ] `seriona_control` builds and exports public controller header.
  - [ ] Fake audio receives `loadTrack` then `play` for valid `Play`/`SelectTrack` paths.
  - [ ] Fake scanner receives `scan` for scan command and its snapshot updates library subscription.
  - [ ] Fake metadata receives `start` on controller start, `update` after snapshot change, and `stop` on shutdown.
  - [ ] Subscriber callbacks receive committed snapshots, not raw audio/scanner events.

  **QA Scenarios**:
  ```
  Scenario: Integrated fake playback path
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test case `media controller selects and plays a library track through fake audio` passes.
    Evidence: .omo/evidence/task-6-control-integrated-playback.txt

  Scenario: Snapshot-only subscriber path
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test case `media controller publishes committed snapshots without raw event passthrough` passes.
    Evidence: .omo/evidence/task-6-control-snapshot-only.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `feat(control): implement media controller facade` | Files: [`inc/seriona/control/media_controller.h`, `src/control/media_controller.cpp`, `src/control/media_controller_module.h`, `src/control/media_controller_module.cpp`, `tests/control/media_controller_tests.cpp`, `CMakeLists.txt`, `tests/CMakeLists.txt` if test source list changes] | Rule: commit only when the complete facade/module-adapter feature is implemented and verified.

- [x] 7. Complete metadata command callback and shutdown lifecycle

  **What to do**: Extend facade tests/implementation to cover metadata-to-controller command callbacks and full lifecycle. `MetadataSharingService::registerCommandCallback` must receive a sink that posts `MediaControlCommand` into the same command queue as UI commands and returns quickly. `shutdown()` order: mark stopping, clear audio/scanner sinks where supported by setting empty sinks, unregister metadata command callback via `SubscriptionHandle`, call metadata `stop()`, drain/drop queued callbacks safely, stop event loop, then leave final snapshots queryable. Ensure audio/scanner events posted after shutdown are dropped without crash. Ensure subscriber exceptions are contained and do not prevent metadata update or later subscribers.
  **Must NOT do**: Do not block indefinitely waiting for fake or real services; do not rely on destructor order alone for unsubscription.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: lifecycle and callback safety integration.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`debugging`] - planned lifecycle implementation, not post-failure debugging.

  **Parallelization**: Can Parallel: NO | Wave 6 | Blocks: 8 | Blocked By: 4,5,6

  **References**:
  - Requirement: `DESIGN.md:238` - subscriptions must support unsubscribe to avoid callbacks after destruction.
  - Requirement: `DESIGN.md:620` - OS media commands convert to controller commands.
  - Requirement: `DESIGN.md:671` - subscriber callbacks must not block event loop and must support explicit unsubscribe.
  - Pattern: `inc/seriona/metadata/metadata_contracts.h:54` - metadata command callback registration returns subscription handle.
  - Pattern: `tests/metadata/metadata_service_recording_tests.cpp:37` - recording backend callback unsubscribe safety.

  **Acceptance Criteria**:
  - [ ] Metadata fake command `Pause` updates controller command path exactly like direct `submitCommand(Pause)`.
  - [ ] Metadata callback after controller shutdown returns/drops safely without state mutation.
  - [ ] Unsubscribed player/library/domain subscribers are not called after unsubscribe.
  - [ ] Subscriber throwing exception does not stop later subscribers or crash tests.

  **QA Scenarios**:
  ```
  Scenario: Metadata command回流
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test case `metadata command callback posts into media controller command queue` passes.
    Evidence: .omo/evidence/task-7-control-metadata-command.txt

  Scenario: Shutdown callback safety
    Tool: Bash
    Steps: Run `ctest --test-dir build -R seriona.control_controller --output-on-failure`.
    Expected: Test cases for post-shutdown audio/scanner/metadata callbacks pass without crash or mutation.
    Evidence: .omo/evidence/task-7-control-shutdown-callbacks.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `feat(control): harden metadata callbacks and shutdown` | Files: [`src/control/media_controller.cpp`, `src/control/control_event_loop.cpp`, `tests/control/media_controller_tests.cpp`] | Rule: commit only when the complete metadata-callback/shutdown feature is implemented and verified.

- [x] 8. Finish end-to-end control verification and guardrails

  **What to do**: Add/finish `tests/control/media_controller_tests.cpp` cases so every critical behavior has coverage. Add a static boundary test or documented command check that greps `inc/seriona/control`, `src/control`, and `tests/control` for forbidden platform/backend implementation terms. Run focused and broad verification. If full `cmake --build build` fails on pre-existing doctest include issue in scanner contract tests, record it separately and still require `seriona_control`, `seriona_control_contract_tests`, and `seriona_media_controller_tests` to build and pass.
  **Must NOT do**: Do not fix unrelated scanner/audio doctest include failures unless they block control targets; do not weaken tests to pass.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: final coverage and guardrail verification across module.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`review-work`] - final verification wave already covers independent review.

  **Parallelization**: Can Parallel: NO | Wave 7 | Blocks: Final Verification | Blocked By: 1,2,3,4,5,6,7

  **References**:
  - Test pattern: `tests/CMakeLists.txt:495` - focused `ctest` names.
  - Guardrail: `AGENTS.md:5` - no Qt/QML/UI or platform system media in backend control path.
  - Guardrail: `AGENTS.md:15` - platform metadata details stay in `src/metadata` private implementation.
  - Requirement: `DESIGN.md:669` - all cross-module notification serialized through controller event queue.
  - External: `https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines` - immutable/value snapshot and RAII guidance.
  - External: `https://clang.llvm.org/docs/ThreadSafetyAnalysis.html` - optional reference for future lock annotations; do not require annotations in this task.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` exits 0.
  - [ ] `cmake --build build --target seriona_control seriona_control_contract_tests seriona_media_controller_tests` exits 0.
  - [ ] `ctest --test-dir build -R seriona.control --output-on-failure` exits 0.
  - [ ] Boundary grep has no production control matches for forbidden dependencies.
  - [ ] `git diff --check` exits 0.

  **QA Scenarios**:
  ```
  Scenario: Focused control regression
    Tool: Bash
    Steps: Run `cmake --build build --target seriona_control seriona_control_contract_tests seriona_media_controller_tests && ctest --test-dir build -R seriona.control --output-on-failure`.
    Expected: Build and focused tests exit 0.
    Evidence: .omo/evidence/task-8-control-focused-regression.txt

  Scenario: Static boundary and whitespace guard
    Tool: Bash
    Steps: Run `rg -n "Qt|QML|sdbus|dbus|windows.h|MPRIS|SMTC|TagReader|SQLite|miniaudio" inc/seriona/control src/control tests/control; git diff --check`.
    Expected: Forbidden grep has no production matches; `git diff --check` exits 0.
    Evidence: .omo/evidence/task-8-control-boundary-diffcheck.txt
  ```

  **Commit**: FEATURE-BOUNDARY | Suggested Message: `test(control): verify media controller integration` | Files: [`tests/control/media_controller_tests.cpp`, `tests/CMakeLists.txt`, any changed control files] | Rule: commit only when the complete final verification/guardrail feature is implemented and verified.

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE by agent-executed pass/fail criteria. Present consolidated results to user and get explicit "okay" before marking the work complete; this final user confirmation is a completion gate, not an implementation decision point.
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.
- [x] F1. Plan Compliance Audit — oracle
- [x] F2. Code Quality Review — unspecified-high
- [x] F3. Real Manual QA — unspecified-high
- [x] F4. Scope Fidelity Check — deep

## Commit Strategy
- Commit after each completed feature, not after each task. A feature may span one or more tasks if that is the smallest coherent user-visible capability.
- Suggested feature commits: public contracts, fake harness/tests, CMake registration, event loop/subscriptions, command/state reducer, facade/adapters, metadata callback/shutdown lifecycle, final integration guardrails.
- Allowed git writes are only `git add` and `git commit`; use unlimited git reads (`git status`, `git diff`, `git log`, `git show`, `git blame`) to inspect before committing.
- Before every commit: run relevant task QA, inspect `git status` and `git diff`, stage only files for that feature with `git add`, then run `git commit` with the suggested message or an equivalent feature-level message.
- Do not use git write operations other than `git add`/`git commit`; do not commit `.omo/evidence/` unless user explicitly asks to preserve execution evidence in git.

## Success Criteria
- `mediaController` is implemented as the only state aggregation and orchestration point for audio/scanner/metadata.
- UI/OS adapters can subscribe to control-layer snapshots and send platform-agnostic `MediaControlCommand` values.
- Audio/scanner callbacks only post; all state updates occur on control serial executor.
- Metadata command callbacks use the same command queue as direct UI commands.
- Control layer has focused doctest/CTest coverage for command semantics, stale event handling, subscriptions, metadata回流, and shutdown safety.
- Control layer production code has no forbidden UI/platform/backend implementation dependencies.
