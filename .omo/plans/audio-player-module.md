# AudioPlayer 音频播放模块实现计划

## TL;DR
> **Summary**: 从空 C++ 后端基线开始，建立 CMake/CTest/依赖基础设施，并按 `AudioPlayer` 门面 + `AudioPlaybackService` 内部分层实现音频播放模块。实现范围覆盖 FFmpeg 解封装/解码/filter graph、固定容量 PCM 队列、播放时钟、miniaudio 低层设备、状态机、事件投递和混音模式同目标格式无缝切歌。
> **Deliverables**:
> - CMake + CTest + 轻量 C++ 测试框架基线
> - FFmpeg/miniaudio 依赖接入与探测
> - `AudioPlayer` public facade 与 `AudioPlaybackService` 内部服务接口
> - FFmpeg RAII 解码/filter graph 层
> - PCM 队列、播放时钟、状态机、事件投递
> - miniaudio 设备层与单曲播放串接
> - 输出模式回退、预加载和混音模式无缝切歌
> **Effort**: XL
> **Parallel**: YES - 5 waves
> **Critical Path**: T1 → T2 → T3 → T5/T6 → T8 → T10 → T12

## Context
### Original Request
用户要求读取并根据 `DESIGN.md`，先进行资料收集，然后根据资料与设计目标规划详细的音频播放模块代码编写任务。音频播放模块拟定名字为 `AudioPlayer`，也允许推荐更合适命名。用户明确要求当前不要分配 plan agent。

### Interview Summary
- 计划范围包含构建系统、FFmpeg/miniaudio 依赖接入和测试框架引入。
- 用户已明确批准本计划包含新增 CMake/CTest、FFmpeg/miniaudio 依赖接入和测试框架引入；这是对 `AGENTS.md` 中新增基础设施前需确认意图的满足。
- 命名采用分层命名：用户可见/顶层门面为 `AudioPlayer`，内部服务/接口语义使用 `AudioPlaybackService`。
- 首期以 Arch Linux/KDE/Wayland 本地验证为主；Windows 保持接口和 CMake 条件分支可扩展，不把 Windows 真实设备验证作为首轮完成条件。
- 默认 C++ 标准为 C++20。
- 测试策略为 CTest + `doctest`；`doctest` 以单头 `third_party/doctest/doctest.h` 或系统包二选一接入，优先单头 vendored 以降低空项目启动成本。真实设备和听感验证只能作为补充，不能作为唯一验收。
- miniaudio 获取方式固定为单头接入：`third_party/miniaudio/miniaudio.h`，由实施任务显式放置或下载源码快照；不得隐藏下载二进制。
- SPSC/PCM 队列固定为项目内自实现固定容量 ring buffer，不引入额外并发队列依赖；满队列写入返回失败并递增 dropped/overflow 计数，空队列读取补零并递增 underrun 计数。
- `BackendEventSink` 最小占位接口固定为 `using BackendEventSink = std::function<void(BackendEvent)>;`，`BackendEvent` 至少包含事件类型、来源模块、单调版本号、时间戳和音频 payload；后续 `mediaController` 可替换 sink 实现但不改变音频模块只投递值事件的边界。

### Metis Review (gaps addressed)
- 已加入平台范围、依赖获取、C++ 标准、测试素材、无设备 CI 策略和 `BackendEventSink` 生命周期护栏。
- 已禁止首期 UI/QML、MPRIS/SMTC、SQLite、播放队列策略、DSP/均衡器、cubeb 泛化和多后端插件层。
- 已把无缝切歌范围收窄为混音模式、同一目标格式、自然下一首衔接。
- 已要求每个任务有 agent-executable 验收，真实设备/听感只作为平台原型补充。

## Work Objectives
### Core Objective
实现可测试、可扩展、实时回调安全的 C++ 音频播放模块基础，保证后续 `mediaController` 能通过 `AudioPlayer` 下发命令，并通过 `BackendEventSink` 接收值语义播放事件。

### Deliverables
- `CMakeLists.txt`、测试入口、依赖探测和基本开发命令。
- `inc/seriona/audio/` 下的 public audio API 与 `src/audio/` 下的实现分层。
- `tests/audio/` 下覆盖状态机、FFmpeg wrapper、filter graph、PCM queue、clock、fake device 和事件投递的测试。
- `tools/` 或 `tests/fixtures/` 下可生成的无版权短音频 fixture。
- `.omo/evidence/` 下每个任务的命令输出、测试日志或平台原型记录。

### Definition of Done (verifiable conditions with commands)
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` 成功配置。
- `cmake --build build` 成功构建 audio 模块和测试目标。
- `ctest --test-dir build --output-on-failure` 全部通过。
- `rg -n "QML|MPRIS|SMTC|SQLite" inc src tests` 不在音频模块源码中出现直接耦合；仅允许文档或测试说明出现。
- `rg -n "ma_device_(start|stop|uninit|init)|BackendEventSink|spdlog|av_read_frame|avcodec" src/audio` 的匹配经代码审查确认不出现在 miniaudio realtime callback 函数体内。

### Must Have
- C++20、CMake、CTest 基线。
- FFmpeg 使用 send/receive API、drain/flush/seek/filter graph 正确封装。
- miniaudio realtime callback 只从 PCM 队列读、补零、更新无锁计数。
- `AudioPlayer` 只作为门面，不变成巨型类；资源用 RAII 分层管理。
- 所有上行事件通过 `BackendEventSink` 异步投递，事件对象值语义。

### Must NOT Have
- 不引入 Qt/QML 类型或依赖。
- 不在音频模块访问 MPRIS、SMTC、SQLite 或 UI。
- 不在 realtime callback 中调用 FFmpeg、日志、分配、阻塞锁、设备 start/stop/uninit/init 或 `BackendEventSink`。
- 不提前实现播放队列、随机/循环策略、收藏、歌词、封面、均衡器、响度标准化或系统媒体集成。
- 不把 cubeb 作为首期抽象层落地；只保留后续评估入口。

## Verification Strategy
> ZERO HUMAN INTERVENTION - all verification is agent-executed.
- Test decision: tests-after for infrastructure and each implementation slice, using CTest + lightweight C++ test framework.
- QA policy: Every task has agent-executed scenarios; real-device/听感验证作为补充证据，不作为唯一 acceptance criteria。
- Evidence: `.omo/evidence/task-{N}-{slug}.{ext}`；每个任务执行命令前必须先运行 `mkdir -p .omo/evidence`。
- Gate policy: T0 依赖预检是全局阻塞门禁；任一必需依赖缺失时，执行者必须停止全部实现任务，不得进入 T1，不得写源码，只向用户报告缺失项和建议安装方式。
- Per-task policy: 每个 task 完成后必须立即运行该 task 的 Acceptance Criteria 与 QA Scenarios；只有所有测试和证据文件通过，才能放行依赖它的后续 task。失败时必须回到同一 task 修复代码并重跑该 task 测试，直到通过；不得用后续 task 掩盖前一 task 失败。

## Preflight Gate (MANDATORY BEFORE T1)

- [x] 0. Check required system dependencies

  **What to do**: Before any implementation work, verify system tools and development libraries needed by `AudioPlayer`: C++20 compiler, CMake, CTest, pkg-config, FFmpeg development packages (`libavformat`, `libavcodec`, `libavutil`, `libavfilter`, `libswresample`), standard build toolchain, and Git repository status. Also verify that `third_party/` can be created later for `doctest` and `miniaudio` single headers, but do not download anything in T0.
  **Must NOT do**: Do not create CMake files, source files, third-party headers, or tests. Do not continue to T1 if any required tool/library is missing.

  **Dependency check command**:
  ```bash
  mkdir -p .omo/evidence && {
    set -e
    c++ --version
    cmake --version
    ctest --version
    pkg-config --version
    git rev-parse --is-inside-work-tree
    pkg-config --exists libavformat libavcodec libavutil libavfilter libswresample
    pkg-config --modversion libavformat libavcodec libavutil libavfilter libswresample
  } 2>&1 | tee .omo/evidence/task-0-dependency-preflight.txt
  ```

  **Pass condition**: command exits 0 and `.omo/evidence/task-0-dependency-preflight.txt` records versions for compiler, CMake/CTest, pkg-config, Git repository status, and all required FFmpeg libraries.

  **Fail condition**: command exits nonzero, Git repository status is invalid, or any required tool/library is absent. Stop all work immediately, report missing compiler/CMake/CTest/pkg-config/Git/FFmpeg dependency names to the user, and do not run T1 or any implementation task. Because git write operations are restricted to `git add` and `git commit`, do not run `git init`; ask the user to initialize or repair the Git repository if this check fails.

## Execution Strategy
### Global Execution Gates
- Gate 0: T0 dependency preflight must pass before T1. If T0 fails, stop all work and report missing dependencies; do not create or modify implementation files.
- Gate per task: a task is not complete until its implementation, Acceptance Criteria, QA Scenarios, and evidence files all pass. If any check fails, redo that same task and rerun its checks before unlocking dependent tasks.
- Gate per wave: a wave is not complete until every task in that wave that is eligible to run has passed its own QA evidence. Do not begin a blocked downstream task merely because it is listed in the next wave.
- Gate final: final verification wave runs only after T1-T15 all pass and required commits have been created.

### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = acceptable here because dependencies are heavy and repo starts empty.
> Extract shared dependencies as Wave-1 tasks for max parallelism.
> Parallelism rule: tasks in the same wave may run in parallel only when each task's `Blocked By` list is fully satisfied and there is no direct dependency between them. If a task in the same wave blocks another task, execute the blocking task first and do not parallelize that pair.
> Wave label rule: a Wave is a scheduling batch, not a blanket permission to run every task in that Wave at once. Always obey each task's `Blocked By` field over the Wave label.

Wave 0: T0 dependency preflight; must run alone and pass before all other tasks.
Wave 1: T1 infrastructure first; after T1 passes, T2 shared contracts and T3 fixture generation may run in parallel.
Wave 2: T4 state machine and T7 PCM queue/clock may run in parallel after their blockers pass; T5 FFmpeg source waits for T3; T6 filter graph waits for T5, so T6 runs only after T5 passes and must not run in parallel with T5.
Wave 3: T8 miniaudio device layer and T9 event dispatcher may run in parallel after their blockers; T10 facade integration waits for T2-T9 and must run after T8/T9/T6/T7/T5/T4 pass.
Wave 4: T11 output fallback and T12 preload/seamless may run in parallel after T10; T13 hardening waits for T11 and T12 and must run only after both pass.
Wave 5: T14 docs may run after T1 but should incorporate latest implementation evidence; T15 platform prototype waits for T8 and T10. T14 and T15 may run in parallel after their blockers pass.

### Dependency Matrix (full, all tasks)
- T0 blocks T1-T15 and final verification.
- T1 blocks T2-T15.
- T2 blocks T4, T9, T10, T11, T12, T13.
- T3 blocks T5, T6, T10, T12.
- T4 blocks T10, T11, T13.
- T5 blocks T6, T10, T12.
- T6 blocks T10, T11, T12.
- T7 blocks T8, T10, T12.
- T8 blocks T10, T11, T13, T15.
- T9 blocks T10, T11, T13.
- T10 blocks T11, T12, T13.
- T11 blocks T13.
- T12 blocks T13.
- T13 blocks final verification.
- T14 can run after T1, but should include references from T5-T8 when available.
- T15 runs after T8 and T10.

### Agent Dispatch Summary (wave → task count → categories)
- Wave 0 → 1 task → unspecified-low (preflight only, no commit)
- Wave 1 → 3 tasks → quick, quick, unspecified-low
- Wave 2 → 4 tasks → deep, deep, deep, deep
- Wave 3 → 3 tasks → deep, deep, deep
- Wave 4 → 3 tasks → deep, deep, unspecified-high
- Wave 5 → 2 tasks → writing, unspecified-high

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

- [x] 1. Establish CMake/CTest/dependency baseline

  **What to do**: Create `CMakeLists.txt`, `cmake/` helper modules if needed, `tests/CMakeLists.txt`, and minimal test executable wiring. Configure C++20, warnings, `SERIONA_BUILD_TESTS`, FFmpeg discovery, and `doctest` test framework. Prefer system FFmpeg discovery through CMake. Add explicit third-party header locations: `third_party/doctest/doctest.h` and `third_party/miniaudio/miniaudio.h`; if the implementation downloads them, it must fetch source headers only and record source/version in docs or CMake comments.
  **Must NOT do**: Do not implement audio playback logic in this task. Do not add Qt/QML. Do not hide dependency downloads inside opaque scripts.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: infrastructure setup is concrete but foundational.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`qt-qml`] - backend must not depend on QML.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: T2-T15 | Blocked By: T0

  **References**:
  - Existing constraint: `AGENTS.md:1` - Chinese collaboration and confirm-before-new-infra constraints.
  - Design: `DESIGN.md:7` - project starts without CMake/build/test infra.
  - Design: `DESIGN.md:11` - pure C++ backend.
  - Design: `DESIGN.md:13` - backend must not introduce Qt dependencies.
  - External: `https://ffmpeg.org/doxygen/8.0/group__lavf__decoding.html` - FFmpeg libraries to link.
  - External: `https://miniaud.io/docs/manual/` - miniaudio low-level API integration.

  **Acceptance Criteria**:
  - [ ] `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` exits 0.
  - [ ] `cmake --build build` exits 0.
  - [ ] `ctest --test-dir build --output-on-failure` exits 0 with at least one doctest placeholder test.
  - [ ] `.omo/evidence/task-1-cmake-baseline.txt` contains configure/build/ctest output.

  **QA Scenarios**:
  ```
  Scenario: Configure and run empty baseline tests
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && { cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure; } 2>&1 | tee .omo/evidence/task-1-cmake-baseline.txt`
    Expected: All commands exit 0 and at least one test is discovered.
    Evidence: .omo/evidence/task-1-cmake-baseline.txt

  Scenario: Qt remains absent
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && (rg -n "Qt|QML|find_package\(Qt" CMakeLists.txt cmake tests inc src || true) | tee .omo/evidence/task-1-no-qt.txt`
    Expected: No Qt dependency is introduced.
    Evidence: .omo/evidence/task-1-no-qt.txt
  ```

  **Commit**: YES | Message: `build: add cmake test baseline` | Files: `CMakeLists.txt`, `cmake/**`, `tests/**`, `.omo/evidence/task-1-*`

- [x] 2. Define audio contracts and naming boundary

  **What to do**: Add public headers under `inc/seriona/audio/` for `AudioPlayer`, `AudioPlaybackService`, `TrackPlaybackRequest`, `AudioOutputConfig`, `AudioDeviceFormat`, `PlaybackClockSnapshot`, `PlaybackEvent`, `PlaybackState`, `PlaybackErrorCode`, and the minimal backend event contract. Define `BackendEventSink` as `std::function<void(BackendEvent)>`; define `BackendEvent` with event type, source module, monotonic version, timestamp, and audio payload variant/value object sufficient for audio events. `AudioPlayer` is the public facade; `AudioPlaybackService` is the internal service interface/implementation boundary.
  **Must NOT do**: Do not implement FFmpeg/miniaudio logic. Do not expose FFmpeg raw pointers, device handles, QML, MPRIS, SMTC, or SQLite types.

  **Recommended Agent Profile**:
  - Category: `quick` - Reason: type/interface task with precise design references.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`qt-qml`] - not QML code.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: T4, T9, T10, T11, T12, T13 | Blocked By: T0, T1

  **References**:
  - Design: `DESIGN.md:153` - all cross-module communication goes through `mediaController`.
  - Design: `DESIGN.md:177` - audio module publishes to `BackendEventSink`.
  - Design: `DESIGN.md:281` - `AudioPlaybackService` or equivalent interface.
  - Design: `DESIGN.md:297` - required core data objects.
  - Plan context: `.omo/plans/audio-player-module.md:21` - confirmed naming and scope decisions.

  **Acceptance Criteria**:
  - [ ] Public headers compile through a contract-only test target.
  - [ ] `AudioPlayer` is a facade type; `AudioPlaybackService` remains internal or implementation-facing.
  - [ ] `BackendEventSink` compiles as `std::function<void(BackendEvent)>` and event payload tests prove value semantics.
  - [ ] `rg -n "AVFormatContext|AVCodecContext|ma_device|QML|MPRIS|SMTC|SQLite" inc/seriona/audio` returns no matches.
  - [ ] `.omo/evidence/task-2-contracts.txt` records build/test and grep results.

  **QA Scenarios**:
  ```
  Scenario: Compile public contract headers
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && { cmake --build build --target seriona_audio_contract_tests && ctest --test-dir build --output-on-failure -R audio_contract; } 2>&1 | tee .omo/evidence/task-2-contracts.txt`
    Expected: Contract test compiles and passes.
    Evidence: .omo/evidence/task-2-contracts.txt

  Scenario: No foreign raw dependencies in public headers
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && (rg -n "AVFormatContext|AVCodecContext|ma_device|QML|MPRIS|SMTC|SQLite" inc/seriona/audio || true) | tee .omo/evidence/task-2-public-boundary.txt`
    Expected: No matches.
    Evidence: .omo/evidence/task-2-public-boundary.txt
  ```

  **Commit**: YES | Message: `feat(audio): define playback contracts` | Files: `inc/seriona/audio/**`, `tests/audio/**`, `.omo/evidence/task-2-*`

- [x] 3. Add generated test audio fixtures

  **What to do**: Add a tiny fixture generation path that creates deterministic short WAV/PCM audio for tests, with no copyrighted assets. The generator can be C++ test helper or script invoked by CMake/CTest. Fixtures should cover silence, sine wave, short duration, and seek-friendly known sample counts.
  **Must NOT do**: Do not commit copyrighted music. Do not require network access. Do not require real audio device.

  **Recommended Agent Profile**:
  - Category: `unspecified-low` - Reason: fixture tooling and tests.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`xlsx`] - not spreadsheet work.

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: T5, T6, T10, T12 | Blocked By: T0, T1

  **References**:
  - Design: `DESIGN.md:461` - prototype verification requires common formats.
  - Metis finding: generated/minimal fixtures avoid copyright risk.

  **Acceptance Criteria**:
  - [ ] Fixture generation runs in CTest without network.
  - [ ] Generated files are small and deterministic.
  - [ ] Tests verify expected sample count/duration metadata for generated WAV/PCM.
  - [ ] `.omo/evidence/task-3-fixtures.txt` records fixture test output.

  **QA Scenarios**:
  ```
  Scenario: Generate deterministic fixtures
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_fixture 2>&1 | tee .omo/evidence/task-3-fixtures.txt`
    Expected: Fixture tests pass and report expected duration/sample counts.
    Evidence: .omo/evidence/task-3-fixtures.txt

  Scenario: No copyrighted binary fixtures
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && (rg -n "copyright|artist|album|mp3|flac" tests/fixtures tools tests/audio || true) 2>&1 | tee .omo/evidence/task-3-no-copyright-fixtures.txt`
    Expected: No committed copyrighted music metadata or large binary music assets are present.
    Evidence: .omo/evidence/task-3-no-copyright-fixtures.txt
  ```

  **Commit**: YES | Message: `test(audio): add generated fixtures` | Files: `tests/audio/**`, `tools/**`, `tests/fixtures/**`, `.omo/evidence/task-3-*`

- [x] 4. Implement playback state machine

  **What to do**: Implement `PlaybackStateMachine` with states `Idle`, `Loading`, `Ready`, `Playing`, `Paused`, `Draining`, `Stopped`, `Error`, command serialization, cancellation semantics, and event emission into a fake sink. Cover `loadTrack`, `play`, `pause`, `resume`, `stop`, `seek`, natural end, and error recovery.
  **Must NOT do**: Do not call FFmpeg/miniaudio. Do not spawn unbounded detached threads. Do not decide next-track policy.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: state transitions and cancellation edge cases.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`debugging`] - this is planned implementation, not runtime debugging.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: T10, T11, T13 | Blocked By: T0, T1, T2

  **References**:
  - Design: `DESIGN.md:372` - state meanings and command sequence.
  - Design: `DESIGN.md:395` - commands are ordered and cancel in-flight work.
  - Design: `DESIGN.md:448` - first-phase audio events.

  **Acceptance Criteria**:
  - [ ] State machine tests cover all legal transitions and illegal transition errors.
  - [ ] `seek -> pause -> stop` serial cancellation test passes.
  - [ ] Clearing sink prevents further event delivery after shutdown.
  - [ ] `.omo/evidence/task-4-state-machine.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Legal playback transitions
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R playback_state_machine 2>&1 | tee .omo/evidence/task-4-state-machine.txt`
    Expected: Tests pass for load/play/pause/resume/seek/stop/end/error transitions.
    Evidence: .omo/evidence/task-4-state-machine.txt

  Scenario: Cancellation order is deterministic
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R playback_state_machine_cancellation 2>&1 | tee .omo/evidence/task-4-cancellation.txt`
    Expected: `seek -> pause -> stop` produces final `Stopped` and no stale seek event after stop.
    Evidence: .omo/evidence/task-4-cancellation.txt
  ```

  **Commit**: YES | Message: `feat(audio): add playback state machine` | Files: `src/audio/**`, `inc/seriona/audio/**`, `tests/audio/**`, `.omo/evidence/task-4-*`

- [x] 5. Implement FFmpeg audio source RAII layer

  **What to do**: Implement `FfmpegAudioSource` for file open, stream selection, decoder init, packet read, `avcodec_send_packet`, looped `avcodec_receive_frame`, decoder drain with NULL packet, seek with decoder flush, and error conversion. Use RAII wrappers for format context, codec context, packets, and frames.
  **Must NOT do**: Do not implement filter graph here. Do not expose FFmpeg raw pointers in public headers. Do not access output device.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: FFmpeg lifecycle and error handling.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`qt-qml`] - no QML.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: T6, T10, T12 | Blocked By: T0, T1, T3

  **References**:
  - Design: `DESIGN.md:269` - FFmpeg is responsible for demux/decode.
  - Design: `DESIGN.md:338` - file open phase.
  - External: `https://ffmpeg.org/doxygen/8.0/group__lavf__decoding.html` - `av_read_frame` and demuxing.
  - External: `https://ffmpeg.org/doxygen/8.0/group__lavc__encdec.html` - send/receive and draining.

  **Acceptance Criteria**:
  - [ ] Tests open generated WAV fixture and decode expected nonzero frame count.
  - [ ] Open missing file returns `OpenFailed`.
  - [ ] No-audio input returns `UnsupportedFormat` if such generated fixture exists; otherwise test uses invalid file.
  - [ ] Seek flush test confirms post-seek frames do not report stale pre-seek position.
  - [ ] `.omo/evidence/task-5-ffmpeg-source.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Decode generated fixture
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R ffmpeg_audio_source 2>&1 | tee .omo/evidence/task-5-ffmpeg-source.txt`
    Expected: Fixture opens, decodes frames, drains cleanly, and reports expected duration tolerance.
    Evidence: .omo/evidence/task-5-ffmpeg-source.txt

  Scenario: Missing file maps to OpenFailed
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R ffmpeg_audio_source_errors 2>&1 | tee .omo/evidence/task-5-errors.txt`
    Expected: Missing path test returns `OpenFailed` without crash or leaked context.
    Evidence: .omo/evidence/task-5-errors.txt
  ```

  **Commit**: YES | Message: `feat(audio): add ffmpeg source wrapper` | Files: `src/audio/ffmpeg/**`, `tests/audio/**`, `CMakeLists.txt`, `.omo/evidence/task-5-*`

- [x] 6. Implement FFmpeg filter graph pipeline

  **What to do**: Implement `FfmpegFilterPipeline` that converts decoded frames to target PCM format for both direct/mixed target decisions. Use graph alloc/parse/config, buffersrc frame input, buffersink frame output, EOF NULL handling, and clear/rebuild on seek or format change.
  **Must NOT do**: Do not perform device output. Do not embed output mode policy beyond target format conversion.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: FFmpeg filter graph lifecycle is error-prone.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`security-research`] - not a security audit.

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: T10, T11, T12 | Blocked By: T0, T1, T3, T5 | Reason: must run after T5 FFmpeg source passes.

  **References**:
  - Design: `DESIGN.md:269` - FFmpeg filter graph handles resampling/format/channel conversion/gain.
  - Design: `DESIGN.md:334` - audio output library does not resample or mix.
  - External: `https://ffmpeg.org/doxygen/7.1/group__lavfi.html` - libavfilter API.
  - External: `https://ffmpeg.org/doxygen/7.1/group__lavfi__buffersrc.html` - buffersrc usage.
  - External: `https://ffmpeg.org/doxygen/7.1/group__lavfi__buffersink.html` - buffersink usage.

  **Acceptance Criteria**:
  - [ ] Tests convert generated fixture to configured sample rate/format/channel count.
  - [ ] EOF/drain test passes without frame loss or infinite loop.
  - [ ] Invalid filter target maps to `UnsupportedFormat` or `FormatNegotiationFailed`.
  - [ ] `.omo/evidence/task-6-filter-pipeline.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Convert source fixture to mixed target PCM
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R ffmpeg_filter_pipeline 2>&1 | tee .omo/evidence/task-6-filter-pipeline.txt`
    Expected: Output frames match target sample rate, format, and channel count.
    Evidence: .omo/evidence/task-6-filter-pipeline.txt

  Scenario: Filter pipeline rejects impossible target
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R ffmpeg_filter_pipeline_errors 2>&1 | tee .omo/evidence/task-6-filter-errors.txt`
    Expected: Invalid target configuration returns typed error and releases graph resources.
    Evidence: .omo/evidence/task-6-filter-errors.txt
  ```

  **Commit**: YES | Message: `feat(audio): add ffmpeg filter pipeline` | Files: `src/audio/ffmpeg/**`, `tests/audio/**`, `.omo/evidence/task-6-*`

- [x] 7. Implement PCM buffer queue and playback clock

  **What to do**: Implement project-owned fixed-capacity `PcmBufferQueue` ring buffer with SPSC semantics, plus `PlaybackClock` based on submitted/consumed frame counts, seek base, pause freeze, resume rebase, and underrun counters. Full queue write returns `false` and increments overflow/dropped counter; empty queue read writes silence to destination and increments underrun counter.
  **Must NOT do**: Do not allocate in consumer/read path. Do not block in consumer/read path. Do not use a third-party queue dependency in this task. Do not destroy queue while producer/consumer are active.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: concurrency and realtime safety.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`debugging`] - no runtime bug yet.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: T8, T10, T12 | Blocked By: T0, T1

  **References**:
  - Design: `DESIGN.md:313` - PCM queue must avoid realtime allocation.
  - Design: `DESIGN.md:397` - realtime callback model.
  - External: `https://github.com/boostorg/lockfree/blob/develop/include/boost/lockfree/spsc_queue.hpp` - SPSC queue semantics reference only, not a dependency.
  - External: `https://github.com/cameron314/readerwriterqueue` - single producer/single consumer constraints reference only.
  - External: `https://github.com/rigtorp/SPSCQueue/blob/master/README.md` - fixed-capacity SPSC patterns reference only.

  **Acceptance Criteria**:
  - [ ] Empty queue read returns silence buffer and increments underrun count.
  - [ ] Full queue write returns `false` and increments overflow/dropped counter without blocking.
  - [ ] Seek clear removes stale PCM.
  - [ ] Clock freezes on pause and rebases on resume.
  - [ ] `.omo/evidence/task-7-pcm-clock.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Queue handles empty and full without blocking
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R pcm_buffer_queue 2>&1 | tee .omo/evidence/task-7-pcm-clock.txt`
    Expected: Empty reads produce zeros; full writes return deterministic status; no deadlock.
    Evidence: .omo/evidence/task-7-pcm-clock.txt

  Scenario: Clock pause/resume/seek behavior
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R playback_clock 2>&1 | tee .omo/evidence/task-7-clock.txt`
    Expected: Pause freezes position, resume does not count paused wall time, seek rebases position.
    Evidence: .omo/evidence/task-7-clock.txt
  ```

  **Commit**: YES | Message: `feat(audio): add pcm queue and playback clock` | Files: `src/audio/buffer/**`, `src/audio/clock/**`, `tests/audio/**`, `.omo/evidence/task-7-*`

- [x] 8. Implement miniaudio device layer with fakeable boundary

  **What to do**: Implement `AudioOutputDevice` around miniaudio low-level API: device enumeration, format config, init, start, stop, uninit, callback `pUserData`, and callback-to-queue read path. Add a fake device boundary for tests that do not require real hardware.
  **Must NOT do**: Do not call `ma_device_start`, `ma_device_stop`, `ma_device_uninit`, `ma_device_init`, FFmpeg, logging, allocation, locks, or `BackendEventSink` inside the callback.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: device lifecycle and callback safety.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`playwright`] - no browser.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: T10, T11, T13, T15 | Blocked By: T0, T1, T7

  **References**:
  - Design: `DESIGN.md:350` - miniaudio low-level device API is preferred.
  - Design: `DESIGN.md:405` - realtime callback constraints.
  - External: `https://miniaud.io/docs/manual/` - miniaudio lifecycle and callback restrictions.
  - External: `https://miniaud.io/docs/examples/simple_playback.html` - simple playback and `pUserData`.
  - External: `https://portaudio.com/docs/v19-doxydocs-dev/writing_a_callback.html` - realtime callback restrictions.
  - External: `https://github.com/google/oboe/blob/main/docs/FullGuide.md` - callback forbidden operations.

  **Acceptance Criteria**:
  - [ ] Fake device tests verify callback reads PCM and fills silence on underrun.
  - [ ] Static grep/review evidence shows forbidden calls are absent from callback body.
  - [ ] Device lifecycle tests call start/stop/uninit outside callback through fake or guarded boundary.
  - [ ] `.omo/evidence/task-8-miniaudio-device.txt` records tests and grep evidence.

  **QA Scenarios**:
  ```
  Scenario: Fake device callback consumes queue safely
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_output_device 2>&1 | tee .omo/evidence/task-8-miniaudio-device.txt`
    Expected: Fake callback copies queued PCM, fills silence on underrun, and updates non-blocking counters.
    Evidence: .omo/evidence/task-8-miniaudio-device.txt

  Scenario: Callback forbidden calls absent
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && (rg -n "ma_device_(start|stop|uninit|init)|BackendEventSink|spdlog|av_read_frame|avcodec|new |malloc|std::mutex" src/audio || true) | tee .omo/evidence/task-8-callback-guardrails.txt`
    Expected: No forbidden calls appear inside realtime callback implementation; if matches exist elsewhere, evidence notes callback function is clean.
    Evidence: .omo/evidence/task-8-callback-guardrails.txt
  ```

  **Commit**: YES | Message: `feat(audio): add miniaudio device layer` | Files: `src/audio/device/**`, `tests/audio/**`, `CMakeLists.txt`, `.omo/evidence/task-8-*`

- [x] 9. Implement event dispatcher and sink lifecycle

  **What to do**: Implement `AudioEventDispatcher` that converts internal audio events to value-semantic `BackendEvent` or audio event payloads, handles sink cleared/shutdown behavior, version/timestamp assignment, and fake sink tests.
  **Must NOT do**: Do not call sink from realtime callback. Do not keep references to caller-owned event payloads. Do not block audio state machine on sink delivery.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: lifecycle and async boundary correctness.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`frontend-design`] - not UI.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: T10, T11, T13 | Blocked By: T0, T1, T2

  **References**:
  - Design: `DESIGN.md:158` - events only to `BackendEventSink`.
  - Design: `DESIGN.md:179` - playback events are value semantic and versioned/timestamped.
  - Design: `DESIGN.md:461` - stable backend event entry.
  - Design: `DESIGN.md:653` - events carry type/source/time/version/payload.

  **Acceptance Criteria**:
  - [ ] Events are copied/moved by value and do not expose mutable internals.
  - [ ] Clearing sink prevents delivery and does not crash.
  - [ ] Dispatcher assigns monotonic version or timestamp.
  - [ ] `.omo/evidence/task-9-event-dispatcher.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Fake sink receives ordered value events
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_event_dispatcher 2>&1 | tee .omo/evidence/task-9-event-dispatcher.txt`
    Expected: Fake sink receives ordered events with version/timestamp and copied payloads.
    Evidence: .omo/evidence/task-9-event-dispatcher.txt

  Scenario: Sink cleared during shutdown
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_event_dispatcher_shutdown 2>&1 | tee .omo/evidence/task-9-shutdown.txt`
    Expected: No event is delivered after sink clear; no crash.
    Evidence: .omo/evidence/task-9-shutdown.txt
  ```

  **Commit**: YES | Message: `feat(audio): add event dispatcher` | Files: `src/audio/events/**`, `tests/audio/**`, `.omo/evidence/task-9-*`

- [x] 10. Integrate `AudioPlayer` single-track playback path

  **What to do**: Wire `AudioPlayer` facade, `AudioPlaybackService`, state machine, FFmpeg source, filter pipeline, PCM queue/clock, device layer, and event dispatcher for single-track playback. Support `loadTrack -> play -> pause -> resume -> seek -> stop` with fake device and generated fixtures.
  **Must NOT do**: Do not implement playback queue policy. Do not access UI/OS/SQLite. Do not require real device for tests.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: integration across all core layers.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`qt-qml`] - backend module only.

  **Parallelization**: Can Parallel: NO | Wave 3 | Blocks: T11, T12, T13, T15 | Blocked By: T0, T2, T3, T4, T5, T6, T7, T8, T9

  **References**:
  - Design: `DESIGN.md:262` - audio module responsibility.
  - Design: `DESIGN.md:321` - playback chain.
  - Design: `DESIGN.md:385` - command sequences.
  - Design: `DESIGN.md:448` - emitted events.

  **Acceptance Criteria**:
  - [ ] Integration test uses generated fixture and fake device to play/pause/resume/seek/stop.
  - [ ] Events include `TrackChanged`, `PlaybackStateChanged`, `PlaybackPositionUpdated`, `PositionDiscontinuity`.
  - [ ] No direct UI/OS/SQLite references in audio sources.
  - [ ] `.omo/evidence/task-10-single-track.txt` records tests and grep evidence.

  **QA Scenarios**:
  ```
  Scenario: Single-track command path
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_player_single_track 2>&1 | tee .omo/evidence/task-10-single-track.txt`
    Expected: Load/play/pause/resume/seek/stop succeeds with fake device and ordered events.
    Evidence: .omo/evidence/task-10-single-track.txt

  Scenario: Audio module has no forbidden coupling
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && (rg -n "QML|MPRIS|SMTC|SQLite" inc/seriona/audio src/audio tests/audio || true) | tee .omo/evidence/task-10-no-coupling.txt`
    Expected: No production audio module coupling to UI, OS metadata, or SQLite.
    Evidence: .omo/evidence/task-10-no-coupling.txt
  ```

  **Commit**: YES | Message: `feat(audio): integrate audio player playback path` | Files: `inc/seriona/audio/**`, `src/audio/**`, `tests/audio/**`, `.omo/evidence/task-10-*`

- [x] 11. Implement output mode negotiation and fallback

  **What to do**: Implement direct/mix mode decision logic, device format negotiation result object, direct-to-mix fallback, mix format downgrade, `OutputFormatChanged`, `OutputModeFallback`, and `FormatNegotiationFailed` paths.
  **Must NOT do**: Do not implement cubeb backend. Do not expose negotiation details directly to UI; emit events only through sink.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: policy and error path matrix.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`material-3`] - no UI design.

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: T13 | Blocked By: T0, T4, T6, T8, T9, T10

  **References**:
  - Design: `DESIGN.md:352` - direct and mix modes.
  - Design: `DESIGN.md:359` - format negotiation steps.
  - Design: `DESIGN.md:368` - stable result object.
  - Design: `DESIGN.md:370` - fallback rules.

  **Acceptance Criteria**:
  - [ ] Fake device matrix tests cover direct success, direct fallback to mix, mix downgrade, total failure.
  - [ ] Fallback events contain actual format and reason.
  - [ ] `FormatNegotiationFailed` maps to `PlaybackError` only when no usable format remains.
  - [ ] `.omo/evidence/task-11-output-fallback.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Direct mode falls back to mix
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R output_format_negotiation 2>&1 | tee .omo/evidence/task-11-output-fallback.txt`
    Expected: Fake unsupported direct source falls back to mix and emits fallback + format changed events.
    Evidence: .omo/evidence/task-11-output-fallback.txt

  Scenario: Total negotiation failure reports playback error
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R output_format_negotiation_failure 2>&1 | tee .omo/evidence/task-11-negotiation-failure.txt`
    Expected: No usable fake format produces typed `FormatNegotiationFailed` playback error.
    Evidence: .omo/evidence/task-11-negotiation-failure.txt
  ```

  **Commit**: YES | Message: `feat(audio): add output format fallback` | Files: `src/audio/**`, `tests/audio/**`, `.omo/evidence/task-11-*`

- [x] 12. Implement preload and mix-mode seamless next-track handoff

  **What to do**: Implement `prepareNext(...)`, `PreloadSlot`, background pre-open/pre-decode path, and mix-mode same-target-format natural next-track handoff into the PCM queue. Scope is only natural end of current track to prepared next track in mix mode.
  **Must NOT do**: Do not promise seamless direct mode. Do not handle device switch seamlessness. Do not implement queue policy; `mediaController` supplies next candidate.

  **Recommended Agent Profile**:
  - Category: `deep` - Reason: timing and buffer handoff complexity.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`frontend-design`] - no UI.

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: T13 | Blocked By: T0, T3, T5, T6, T7, T10

  **References**:
  - Design: `DESIGN.md:357` - `mediaController` provides next path.
  - Design: `DESIGN.md:415` - buffering, preload and seamless switching.
  - Design: `DESIGN.md:426` - seamless only for mix mode same target format first.

  **Acceptance Criteria**:
  - [ ] Fake-device integration test verifies prepared next track begins after current drain without device reopen.
  - [ ] Direct mode preload test reduces to preparation only and does not claim seamless.
  - [ ] Preload failure emits error but leaves next policy to controller.
  - [ ] `.omo/evidence/task-12-seamless.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Mix-mode seamless natural handoff
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_player_seamless_mix 2>&1 | tee .omo/evidence/task-12-seamless.txt`
    Expected: Two generated fixtures play back-to-back through fake device with one continuous output format/device session.
    Evidence: .omo/evidence/task-12-seamless.txt

  Scenario: Preload failure does not decide queue policy
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_player_preload_failure 2>&1 | tee .omo/evidence/task-12-preload-failure.txt`
    Expected: Error event is emitted; no automatic skip/random/retry policy is executed inside audio module.
    Evidence: .omo/evidence/task-12-preload-failure.txt
  ```

  **Commit**: YES | Message: `feat(audio): add preload seamless handoff` | Files: `src/audio/**`, `tests/audio/**`, `.omo/evidence/task-12-*`

- [x] 13. Harden error recovery, shutdown, and device-change behavior

  **What to do**: Cover shutdown ordering, queue destruction lifecycle, sink clearing, decode task cancellation, device unavailable, decode failure, seek failure, underrun threshold reporting, and stop-before-destroy safety.
  **Must NOT do**: Do not add global thread pool framework. Do not make real device/manual listening the only proof.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: cross-cutting reliability hardening.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`security-research`] - not requested.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: final verification | Blocked By: T0, T8, T9, T10, T11, T12 | Reason: hardening depends on both T11 and T12 passing.

  **References**:
  - Design: `DESIGN.md:434` - error categories.
  - Design: `DESIGN.md:446` - error payload requirements.
  - Design: `DESIGN.md:476` - thread boundary and callback isolation.
  - External: `https://miniaud.io/docs/manual/` - device lifecycle restrictions.

  **Acceptance Criteria**:
  - [ ] Tests cover `OpenFailed`, `UnsupportedFormat`, `DeviceUnavailable`, `FormatNegotiationFailed`, `DecodeFailed`, `BufferUnderrun`, `SeekFailed`.
  - [ ] Shutdown test stops producer/consumer before queue destruction.
  - [ ] No event is delivered after sink clear.
  - [ ] `.omo/evidence/task-13-hardening.txt` records tests.

  **QA Scenarios**:
  ```
  Scenario: Error matrix produces typed events
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_error_matrix 2>&1 | tee .omo/evidence/task-13-hardening.txt`
    Expected: Every planned error category maps to typed event with stage/category/summary/debug context.
    Evidence: .omo/evidence/task-13-hardening.txt

  Scenario: Shutdown lifecycle is safe
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && ctest --test-dir build --output-on-failure -R audio_shutdown_lifecycle 2>&1 | tee .omo/evidence/task-13-shutdown.txt`
    Expected: Stop-before-destroy, sink-clear, and queue destruction tests pass without deadlock/crash.
    Evidence: .omo/evidence/task-13-shutdown.txt
  ```

  **Commit**: YES | Message: `fix(audio): harden playback lifecycle` | Files: `src/audio/**`, `tests/audio/**`, `.omo/evidence/task-13-*`

- [x] 14. Document audio implementation research and constraints

  **What to do**: Add developer-facing documentation under `docs/` or `docs/audio/` summarizing FFmpeg/miniaudio/SPSC constraints, callback forbidden operations, architecture boundaries, and how to run tests/prototypes. Because project docs path is not currently established, implementer may create `docs/audio-player.md` as part of this task.
  **Must NOT do**: Do not contradict `DESIGN.md`. Do not document unverified commands.

  **Recommended Agent Profile**:
  - Category: `writing` - Reason: documentation synthesis.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`docx`] - not Word document.

  **Parallelization**: Can Parallel: YES | Wave 5 | Blocks: final verification | Blocked By: T0, T1

  **References**:
  - Design: `DESIGN.md:260` - audio module design.
  - Plan context: `.omo/plans/audio-player-module.md:28` - Metis review and research-derived guardrails.
  - External: `https://ffmpeg.org/doxygen/8.0/group__lavc__encdec.html` - FFmpeg send/receive.
  - External: `https://miniaud.io/docs/manual/` - miniaudio callback restrictions.
  - External: `https://portaudio.com/docs/v19-doxydocs-dev/writing_a_callback.html` - realtime callback rules.

  **Acceptance Criteria**:
  - [ ] Documentation lists exact commands that have been verified by prior tasks.
  - [ ] Documentation contains callback forbidden checklist.
  - [ ] Documentation states `AudioPlayer` facade / `AudioPlaybackService` internal naming.
  - [ ] `.omo/evidence/task-14-docs.txt` records documentation link check or grep checks.

  **QA Scenarios**:
  ```
  Scenario: Documentation includes required guardrails
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && rg -n "AudioPlayer|AudioPlaybackService|BackendEventSink|realtime|FFmpeg|miniaudio|SPSC|QML|MPRIS|SMTC" docs DESIGN.md | tee .omo/evidence/task-14-docs.txt`
    Expected: Docs mention naming, event boundary, callback constraints, and forbidden couplings.
    Evidence: .omo/evidence/task-14-docs.txt

  Scenario: Documented commands are verified
    Tool: Bash
    Steps: Run documented CMake/build/ctest commands from docs and tee output to `.omo/evidence/task-14-commands.txt`.
    Expected: Commands exit 0 and match prior evidence.
    Evidence: .omo/evidence/task-14-commands.txt
  ```

  **Commit**: YES | Message: `docs(audio): document playback implementation constraints` | Files: `docs/**`, `DESIGN.md` if cross-reference needed, `.omo/evidence/task-14-*`

- [x] 15. Add platform prototype harness for miniaudio local validation

  **What to do**: Add an optional, non-default prototype executable or test target that can exercise miniaudio on the current Linux desktop with generated fixture playback, pause stop/reopen behavior, and format negotiation logging. Mark it as optional hardware-dependent because real devices are not guaranteed in CI, but keep command output capturable as evidence.
  **Must NOT do**: Do not make this required for default `ctest`. Do not require human listening for implementation completion. Do not add UI.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` - Reason: local platform prototype and evidence capture.
  - Skills: [] - no specialized skill needed.
  - Omitted: [`playwright`] - no browser.

  **Parallelization**: Can Parallel: YES | Wave 5 | Blocks: final verification | Blocked By: T0, T8, T10

  **References**:
  - Design: `DESIGN.md:430` - pause device occupancy must be verified on KDE/PipeWire/PulseAudio.
  - Design: `DESIGN.md:461` - prototype verification checklist.
  - External: `https://miniaud.io/docs/manual/` - start/stop/uninit behavior.

  **Acceptance Criteria**:
  - [ ] Default build/test does not require real audio device.
  - [ ] Optional target builds when dependencies are available.
  - [ ] Harness can list devices and print negotiated format without starting playback.
  - [ ] `.omo/evidence/task-15-platform-prototype.txt` records optional harness output or skip reason.

  **QA Scenarios**:
  ```
  Scenario: Optional harness does not affect default tests
    Tool: Bash
    Steps: Run `mkdir -p .omo/evidence && { cmake --build build && ctest --test-dir build --output-on-failure; } 2>&1 | tee .omo/evidence/task-15-default-tests.txt`
    Expected: Default tests pass without real audio hardware.
    Evidence: .omo/evidence/task-15-default-tests.txt

  Scenario: Device listing prototype
    Tool: Bash
    Steps: Run optional harness command documented by the task with output teed to `.omo/evidence/task-15-platform-prototype.txt`, or write an explicit environment skip reason to that same file if audio device access is unavailable.
    Expected: Harness prints devices/format info or explicit environment skip; no crash.
    Evidence: .omo/evidence/task-15-platform-prototype.txt
  ```

  **Commit**: YES | Message: `test(audio): add optional miniaudio prototype harness` | Files: `tools/**`, `tests/audio/**`, `CMakeLists.txt`, `.omo/evidence/task-15-*`

## Final Verification Wave (MANDATORY — after ALL implementation tasks)
> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.
- [x] F1. Plan Compliance Audit — oracle
- [x] F2. Code Quality Review — unspecified-high
- [x] F3. Automated Runtime QA — unspecified-high (+ optional platform harness evidence, not UI, no human listening requirement)
- [x] F4. Scope Fidelity Check — deep

## Commit Strategy
- Commit cadence: create one commit after each implementation task T1-T15 passes its own Acceptance Criteria and QA Scenarios. T0 dependency preflight produces evidence only and must not be committed by itself unless it creates tracked documentation, which it should not.
- Commit frequency rationale: per-task commits give clear rollback/review boundaries for a large audio subsystem without mixing independent layers; this is safer than wave-level commits because failed later tasks do not contaminate already verified work.
- Commit prerequisites: before every commit, run `git status`, `git diff`, and the task-specific QA commands; inspect staged files so only intended files and evidence are staged.
- Allowed git write operations: only `git add` and `git commit` are permitted. All other git write operations are forbidden, including but not limited to `git reset`, `git checkout`, `git restore`, `git clean`, `git revert`, `git merge`, `git rebase`, `git stash`, branch creation/deletion, tag creation/deletion, and push.
- Allowed git read operations: unrestricted read-only commands such as `git status`, `git diff`, `git log`, `git show`, and `git blame` may be used.
- Commit message source: use each task's suggested `Commit` message unless implementation scope materially changes; if scope changes, use the same conventional style and mention the task number in the body.
- Failed task rule: if a task QA fails, do not commit partial work. Fix within the same task, rerun QA, then commit only after pass.
- Suggested order: T1 → T2/T3 as independent commits after T1 → eligible Wave 2 tasks as they pass → T10 integration → T11/T12 → T13 → T14/T15 → final verification summary if tracked docs/evidence changed.

## Success Criteria
- Implementation preserves `DESIGN.md` communication model: command down, event up, snapshot observed through controller.
- Default tests run without real audio device and without copyrighted fixtures.
- Realtime callback remains mechanically auditable and free of forbidden operations.
- `AudioPlayer` facade stays thin; FFmpeg, filter, device, queue, clock, state machine and event dispatch remain separate components.
- The module can play generated fixture through fake device in tests and can optionally enumerate/use real miniaudio device locally.
