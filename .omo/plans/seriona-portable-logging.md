# seriona-portable-logging - Work Plan

## TL;DR (For humans)
**What you'll get:** Seriona will write detailed debug-to-critical logs to both the terminal and a portable log file stored beside the app. The library database and exported artwork will also move beside the app so the player can be copied as one portable folder.

**Why this approach:** Use spdlog the way it is designed for a small C++ app: one default multi-sink logger, initialized once, then direct logging calls from the backend. Runtime data goes under a single module-neutral `SerionaData` folder to avoid temporary-system paths and avoid exposing internal module names.

**What it will NOT do:** It will not add a big logging framework, a config-file system, UI changes, or logs inside realtime audio callbacks.

**Effort:** Large
**Risk:** Medium - broad observability touches across modules plus runtime path relocation, but changes are additive and testable.
**Decisions to sanity-check:** `SerionaData` folder name; console level `info+`; file level `debug+`; rotating log file rather than unbounded basic file.

Your next move: run `$start-work .omo/plans/seriona-portable-logging.md`, or ask for a high-accuracy review first. Full execution detail follows below.

---

> TL;DR (machine): Large / medium-risk plan to add spdlog multi-sink logging and portable `SerionaData` runtime paths, with no realtime audio logging and no large logging framework.

## Scope
### Must have
- Add spdlog to the CMake build using the existing root-level dependency pattern. Prefer `find_package(spdlog CONFIG REQUIRED)` and link `spdlog::spdlog` privately to targets that include it from `.cpp` files.
- Add a small private logging bootstrap, e.g. `src/logging/logging.cpp` plus `src/logging/logging.h`, with an initialization function that accepts runtime paths and creates a default spdlog logger.
- Configure exactly two sinks: terminal output and file output. Use `stdout_color_sink_mt` for terminal and `rotating_file_sink_mt` for portable file logs unless implementation evidence shows the target package lacks rotating sink support.
- Set default logger runtime level to `debug`. File sink logs `debug+`; terminal sink logs `info+`; `error+` flushes immediately. Use a pattern that includes timestamp, level, thread id, logger name, and message.
- Add portable runtime path resolution from the app executable directory. Runtime data root must be `<executable directory>/SerionaData/`.
- Store logs at `<executable directory>/SerionaData/logs/seriona.log`.
- Store the scanner/library database at `<executable directory>/SerionaData/library.sqlite`.
- Store exported artwork at `<executable directory>/SerionaData/artwork/`.
- Thread runtime paths through production app/controller/scanner assembly without leaking scanner internals into public contracts.
- Add comprehensive but non-spammy logs across app, control, audio, scanner, and metadata boundaries: lifecycle, state transitions, accepted/rejected commands, backend selection, filesystem paths, scan summaries, cache maintenance, FFmpeg/TagReader/SQLite/MPRIS errors, fallback paths, and shutdown.
- Use `debug`, `info`, `warn`, `error`, and `critical` appropriately. Do not use `trace` for this request.
- Add tests covering logging initialization, portable path derivation, scanner path injection, and representative module logging/failure behavior.
- Execute final build, full CTest, and a live portable smoke that verifies log/database/artwork locations.

### Must NOT have (guardrails, anti-slop, scope boundaries)
- Must not create a large logging framework, per-module logger factory hierarchy, service locator, or config-file subsystem.
- Must not expose `spdlog` types in public API headers under `inc/seriona/...` unless the executor records a specific compile-time reason in evidence.
- Must not log from `AudioOutputDevice::renderCallback()`, miniaudio data callback bridge, `PcmBufferQueue::readIfGeneration()`, `PcmBufferQueue::write()`, or any direct realtime audio path.
- Must not allocate, lock, perform disk I/O, or call spdlog from realtime audio callback paths.
- Must not add Qt/QML/UI or system-media UI changes.
- Must not rename product targets or restructure modules.
- Must not log sensitive full payloads unnecessarily. File paths are acceptable for this local music backend; raw lyrics contents should not be logged.
- Must not split the backend into conceptually independent logging subsystems. Treat it as one backend product with one shared logger.

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: tests-after with doctest/CTest plus targeted live smoke. Use TDD only when a task changes existing observable behavior such as runtime path selection.
- Evidence: `.omo/evidence/task-<N>-seriona-portable-logging.md` per todo, containing commands, outputs, and any live smoke artifacts. Final evidence path: `.omo/evidence/task-final-seriona-portable-logging.md`.
- Required focused commands include `cmake --build build`, `ctest --test-dir build -R <focused-regex> --output-on-failure`, `ctest --test-dir build --output-on-failure`, and a live `./build/seriona <music path>` smoke that confirms files under `build/SerionaData/`.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 1: foundation and path plumbing. Prefer lightweight execution workers (`gpt-5.4-mini` where available): Todos 1-4.
- Wave 2: module logging coverage. Prefer lightweight execution workers (`gpt-5.4-mini` where available): Todos 5-9, with Todo 5 guarding realtime constraints for audio.
- Wave 3: integration tests, live smoke, documentation/evidence: Todos 10-12.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2, 5-9 | none |
| 2 | none | 3, 4, 10, 12 | 1 |
| 3 | 2 | 4, 10, 12 | 1 |
| 4 | 2, 3 | 10, 12 | 1 |
| 5 | 1 | 10, 11 | 6, 7, 8, 9 |
| 6 | 1, 2 | 10, 11 | 5, 7, 8, 9 |
| 7 | 1, 2, 3 | 10, 11, 12 | 5, 6, 8, 9 |
| 8 | 1 | 10, 11 | 5, 6, 7, 9 |
| 9 | 1, 2 | 10, 11 | 5, 6, 7, 8 |
| 10 | 1-9 | 11, 12 | none |
| 11 | 10 | 12 | none |
| 12 | 10, 11 | final wave | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 1. Add spdlog dependency and minimal logger bootstrap
  What to do / Must NOT do: Add `find_package(spdlog CONFIG REQUIRED)` near existing dependency discovery. Add private logging files under `src/logging/` (or another private source directory if worker finds a better local convention) with a tiny API: initialize default logger from a log path, expose shutdown/flush if needed, and do not introduce a framework. Link spdlog privately to backend targets and tests that directly compile logging implementation. Must not put spdlog includes in public contract headers.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 2, 5, 6, 7, 8, 9, 10
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:15`, `CMakeLists.txt:35`, `CMakeLists.txt:52`, `CMakeLists.txt:111`, `CMakeLists.txt:123`, `CMakeLists.txt:132`, `CMakeLists.txt:175`, `CMakeLists.txt:183`, `CMakeLists.txt:195`; spdlog official docs: multi-sink logger, default logger, `stdout_color_sink_mt`, `rotating_file_sink_mt`, `set_level`, sink levels, `flush_on`.
  Acceptance criteria (agent-executable): A focused logging test constructs the logger with a temporary log path, emits `debug/info/warn/error/critical`, flushes, and proves the file contains all five messages while the code compiles with no public spdlog exposure.
  QA scenarios (name the exact tool + invocation): Happy: `cmake --build build --target <logging-test-target>` and `ctest --test-dir build -R 'seriona.logging' --output-on-failure`. Failure: initialize with an unwritable path in a test and assert graceful error reporting or fallback documented by the implementation. Evidence `.omo/evidence/task-1-seriona-portable-logging.md`.
  Commit: Y | `feat(logging): add spdlog bootstrap`

- [x] 2. Add portable runtime path resolution from executable directory
  What to do / Must NOT do: Add a small runtime path helper that derives the executable directory from app startup and returns `SerionaData`, `SerionaData/logs/seriona.log`, `SerionaData/library.sqlite`, and `SerionaData/artwork`. On Linux prefer robust executable path discovery such as `/proc/self/exe`; use `argv[0]` fallback only if necessary and record fallback behavior. Must not add a config-file system.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 3, 4, 10, 12
  References (executor has NO interview context - be exhaustive): `app/main.cpp:14`, `app/terminal_controller.cpp:181`, `app/CMakeLists.txt:1`, `tools/miniaudio_platform_probe.cpp:31`; user decision: data root name is `SerionaData`, no hyphen, no internal module names.
  Acceptance criteria (agent-executable): Tests assert executable directory `/tmp/example/bin/seriona` maps to `/tmp/example/bin/SerionaData`, log file `SerionaData/logs/seriona.log`, database `SerionaData/library.sqlite`, artwork `SerionaData/artwork`.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.runtime_paths' --output-on-failure`. Failure: relative/empty executable path fallback test produces a deterministic current-directory based data root or a documented error path. Evidence `.omo/evidence/task-2-seriona-portable-logging.md`.
  Commit: Y | `feat(app): add portable runtime paths`

- [x] 3. Thread portable paths through production controller/scanner assembly
  What to do / Must NOT do: Extend production app/controller assembly so `runTerminalController()` or the app entrypoint passes runtime paths into `makeProductionMediaController()` and scanner dependencies. Use existing scanner dependency fields `databasePath` and `coverExportDir`. Remove production reliance on temp defaults for the app path, but keep safe defaults for unit tests or explicit dependency-free construction. Must not expose scanner-specific path names in public user-facing data directory names.
  Parallelization: Wave 1 | Blocked by: 2 | Blocks: 4, 7, 10, 12
  References (executor has NO interview context - be exhaustive): `src/scanner/file_scanner_service_internal.h:53`, `src/scanner/file_scanner_orchestrator.cpp:30`, `src/scanner/file_scanner_orchestrator.cpp:34`, `src/scanner/file_scanner_orchestrator.cpp:244`, `src/scanner/file_scanner_orchestrator.cpp:316`, `src/scanner/file_scanner_orchestrator.cpp:513`, `src/control/media_controller_module.cpp:51`, `src/control/media_controller_module.h`, `app/terminal_controller.cpp:190`.
  Acceptance criteria (agent-executable): A production assembly test or contract test proves scanner dependencies receive `SerionaData/library.sqlite` and `SerionaData/artwork` when runtime paths are supplied.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.(control_contract|scanner_service)' --output-on-failure`. Failure: empty supplied paths still use safe defaults and do not crash tests. Evidence `.omo/evidence/task-3-seriona-portable-logging.md`.
  Commit: Y | `fix(scanner): use portable runtime storage`

- [x] 4. Initialize logging at app startup and shut it down cleanly
  What to do / Must NOT do: Initialize logging before controller construction in app startup using the portable log file path. Log startup args, resolved data root, log file path, database path, artwork path, scan root, and shutdown. If logging initialization fails, print a concise terminal error and continue with console-only fallback or return a clear startup error; choose the least surprising implementation and document it in evidence. Must not let logger setup throw uncaught exceptions out of `main`.
  Parallelization: Wave 1 | Blocked by: 1, 2, 3 | Blocks: 10, 12
  References (executor has NO interview context - be exhaustive): `app/main.cpp:14`, `app/terminal_controller.cpp:181`, `app/terminal_controller.cpp:201`, `app/terminal_controller.cpp:203`, `app/terminal_controller.cpp:220`, spdlog docs for `spdlog::set_default_logger`, `flush_on`, and `flush_every`.
  Acceptance criteria (agent-executable): Running `./build/seriona <valid music dir>` creates `build/SerionaData/logs/seriona.log` before or during startup and includes startup/path lines.
  QA scenarios (name the exact tool + invocation): Happy: run `./build/seriona '/home/kaizen857/Music/CloudMusic(for MP4)/R・I・O・T/'` in tmux, wait 2 seconds, verify log file exists and contains startup/data-root entries, then kill session. Failure: simulate invalid music path and verify error log is emitted. Evidence `.omo/evidence/task-4-seriona-portable-logging.md`.
  Commit: Y | `feat(app): initialize portable logging`

- [ ] 5. Add audio logs outside realtime paths
  What to do / Must NOT do: Add logs around audio service lifecycle, load/play/pause/resume/seek/stop commands, output format negotiation, device init/start/stop/rebind failures, FFmpeg open/read/seek/filter errors, underrun summary events, and fallback decisions. Must not log inside `AudioOutputDevice::renderCallback()`, miniaudio data callback, PCM queue read/write, or tight clock hot paths.
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 10, 11
  References (executor has NO interview context - be exhaustive): `AGENTS.md` realtime constraints; `src/audio/audio_playback_service.cpp:104`, `src/audio/audio_playback_service.cpp:935`, `src/audio/audio_playback_service.cpp:941`, `src/audio/audio_playback_service.cpp:957`, `src/audio/device/audio_output_device.cpp:133`, `src/audio/device/audio_output_device.cpp:253`, `src/audio/device/miniaudio_output_device_backend.cpp:45`, `src/audio/ffmpeg/ffmpeg_audio_source.cpp:167`, `src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp:182`, `src/audio/buffer/pcm_buffer_queue.cpp:13`.
  Acceptance criteria (agent-executable): Focused audio tests pass; static grep proves no `spdlog` token appears in `renderCallback`, miniaudio callback bridge body, or PCM queue source unless evidence explains a non-runtime test-only include.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.(audio_output_device|audio_player_single_track|audio_player_small_buffer|audio_error_matrix|ffmpeg_audio_source|ffmpeg_filter_pipeline)' --output-on-failure`. Failure: run an existing missing/corrupt fixture test and verify an `error` log line is emitted to the test log file if the logging harness captures it. Evidence `.omo/evidence/task-5-seriona-portable-logging.md`.
  Commit: Y | `feat(audio): log playback lifecycle`

- [ ] 6. Add control and app command logs
  What to do / Must NOT do: Log command acceptance/rejection, visible playback state changes, seek suppression decisions, subscription lifecycle, event-loop start/stop, queued task failures, and domain notifications. Keep logs at boundaries and meaningful decisions; do not log every position tick or every subscriber callback invocation.
  Parallelization: Wave 2 | Blocked by: 1, 2 | Blocks: 10, 11
  References (executor has NO interview context - be exhaustive): `src/control/media_controller.cpp:32`, `src/control/control_event_loop.cpp:20`, `src/control/control_state_reducer.cpp:187`, `src/control/control_state_reducer.cpp:326`, `src/control/control_state_reducer.cpp:412`, `src/control/media_controller_module.cpp:51`, `app/terminal_controller.cpp:98`, `app/terminal_controller.cpp:181`.
  Acceptance criteria (agent-executable): Control tests pass and a targeted test or smoke log shows command accepted/rejected and seek-visible-state suppression without position-tick spam.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.(control_contract|control_controller)' --output-on-failure`. Failure: submit invalid command in an existing control test path and assert/log evidence contains `warn` rejection. Evidence `.omo/evidence/task-6-seriona-portable-logging.md`.
  Commit: Y | `feat(control): log command decisions`

- [ ] 7. Add scanner and cache logs with portable path coverage
  What to do / Must NOT do: Log scanner configuration, roots, scan start/finish summaries, cache open/migration/maintenance/checkpoint, cache unavailable, path classification warnings, hash traversal summaries, TagReader per-file failures, LRC parse failures, watcher start/stop/debounce, and artwork export path. Avoid logging raw lyrics text or excessive per-file debug unless guarded by `debug` and summarized by default.
  Parallelization: Wave 2 | Blocked by: 1, 2, 3 | Blocks: 10, 11, 12
  References (executor has NO interview context - be exhaustive): `src/scanner/file_scanner_service.cpp:9`, `src/scanner/file_scanner_orchestrator.cpp:242`, `src/scanner/file_scanner_orchestrator.cpp:293`, `src/scanner/file_scanner_orchestrator.cpp:618`, `src/scanner/cache/sqlite_scanner_cache.cpp:556`, `src/scanner/cache/sqlite_scanner_cache.cpp:668`, `src/scanner/path_utils.cpp:145`, `src/scanner/hash_utils.cpp:136`, `src/scanner/lrc_parser.cpp:98`, `src/scanner/tag_reader_metadata_adapter.cpp:105`, `src/scanner/scan_scheduler.cpp:29`, `tests/scanner/scanner_test_harness.cpp`, `tests/scanner/scanner_service_tests.cpp`.
  Acceptance criteria (agent-executable): Scanner tests pass; a scan smoke writes log entries showing `SerionaData/library.sqlite` and `SerionaData/artwork`, plus scan completed summary.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.scanner' --output-on-failure`. Failure: scanner test with broken metadata logs a `warn`/`error` without failing unrelated successful files. Evidence `.omo/evidence/task-7-seriona-portable-logging.md`.
  Commit: Y | `feat(scanner): log scan and cache flow`

- [ ] 8. Add metadata and MPRIS logs
  What to do / Must NOT do: Log backend selection, service start/update/stop, no-op/failure backend paths, synchronizer suppression/emission decisions at debug level without logging every position-only tick, Linux MPRIS bus/object export, property publish summaries, command dispatch/rejection, SetPosition validation, and stop/shutdown. Keep platform-specific details inside `src/metadata/` only.
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 10, 11
  References (executor has NO interview context - be exhaustive): `src/metadata/metadata_service.cpp:8`, `src/metadata/metadata_service_backend.cpp:40`, `src/metadata/metadata_synchronizer.cpp:136`, `src/metadata/metadata_mpris_linux.cpp:173`, `src/metadata/metadata_mpris_linux.cpp:417`, `src/metadata/metadata_mpris_linux.cpp:437`, `src/metadata/metadata_windows_private.cpp:25`, `AGENTS.md` metadata private boundary.
  Acceptance criteria (agent-executable): Metadata tests pass; MPRIS tests prove command rejection and SetPosition paths still behave while logs are emitted at appropriate levels in a captured file.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.(metadata_mapper|metadata_service|metadata_mpris|metadata_contract)' --output-on-failure`. Failure: invalid MPRIS track id/unsupported backend logs warning/error without changing return semantics. Evidence `.omo/evidence/task-8-seriona-portable-logging.md`.
  Commit: Y | `feat(metadata): log platform sync flow`

- [ ] 9. Add critical-level coverage for process-level unrecoverable failures only
  What to do / Must NOT do: Ensure at least one legitimate `critical` path exists for process-level startup/build/runtime unrecoverable failures, such as logging bootstrap impossible to recover if implementation chooses fatal startup behavior, or a top-level uncaught exception guard. Do not downgrade normal backend failures into `critical`; most playback/scan errors are `error` or `warn`.
  Parallelization: Wave 2 | Blocked by: 1, 2 | Blocks: 10, 11
  References (executor has NO interview context - be exhaustive): `app/main.cpp:14`, `app/terminal_controller.cpp:181`, spdlog docs for `critical`; user requested levels `debug` through `critical`.
  Acceptance criteria (agent-executable): A focused test or controlled startup failure emits a `critical` log line; normal invalid scan path does not use `critical`.
  QA scenarios (name the exact tool + invocation): Happy: logging level test validates all five levels including critical. Failure: inject impossible logger path or top-level exception path and verify critical/fallback behavior exactly as designed. Evidence `.omo/evidence/task-9-seriona-portable-logging.md`.
  Commit: Y | `feat(logging): cover critical failures`

- [ ] 10. Update test/CMake integration for logging and portable storage
  What to do / Must NOT do: Add/adjust test targets for logging bootstrap/runtime paths, and update existing tests for new portable naming if needed. Keep tests deterministic by using temporary directories, not the real build output except live smoke. Ensure direct test executables link spdlog only when they compile logging code directly.
  Parallelization: Wave 3 | Blocked by: 1-9 | Blocks: 11, 12
  References (executor has NO interview context - be exhaustive): `tests/CMakeLists.txt:1`, `tests/CMakeLists.txt:121`, `tests/CMakeLists.txt:131`, `tests/CMakeLists.txt:144`, `tests/CMakeLists.txt:149`, `tests/CMakeLists.txt:153`, `tests/scanner/scanner_test_harness.h`, `tests/scanner/scanner_test_harness.cpp`, `tests/scanner/scanner_cache_tests.cpp`, `tests/scanner/scanner_service_tests.cpp`, `tests/scanner/scanner_watcher_tests.cpp`, `tests/scanner/scanner_tagreader_tests.cpp`, `tests/metadata/metadata_mpris_tests.cpp`, `tests/control/media_controller_tests.cpp`.
  Acceptance criteria (agent-executable): All new tests are registered in CTest and fail if runtime data paths regress to system temp or if logger init omits a sink.
  QA scenarios (name the exact tool + invocation): Happy: `ctest --test-dir build -R 'seriona.(logging|runtime_paths|scanner|metadata|control)' --output-on-failure`. Failure: grep/assertion in tests rejects `/tmp/seriona/scanner-cache.sqlite` as a production default. Evidence `.omo/evidence/task-10-seriona-portable-logging.md`.
  Commit: Y | `test(logging): cover portable observability`

- [ ] 11. Run full validation and static guardrails
  What to do / Must NOT do: Build and test the complete project, then run static searches to enforce realtime no-log constraints and module-neutral runtime names. Do not fix unrelated failures except those introduced by this plan; document existing flaky failures separately if they recur.
  Parallelization: Wave 3 | Blocked by: 10 | Blocks: 12
  References (executor has NO interview context - be exhaustive): `AGENTS.md` build commands; realtime constraints in `AGENTS.md`; no-log anchors `src/audio/device/audio_output_device.cpp:253`, `src/audio/buffer/pcm_buffer_queue.cpp`, `src/audio/device/miniaudio_output_device_backend.cpp`.
  Acceptance criteria (agent-executable): `cmake --build build` passes; `ctest --test-dir build --output-on-failure` passes; `git diff --check` passes; grep guard shows no spdlog calls in realtime forbidden paths; grep guard shows no production `scanner-cache.sqlite`, `scanner-covers`, or `seriona-data` naming remains except test references if intentionally preserved.
  QA scenarios (name the exact tool + invocation): Happy: commands above. Failure: intentionally list and fail if forbidden files contain `spdlog::` or `SPDLOG_`. Evidence `.omo/evidence/task-11-seriona-portable-logging.md`.
  Commit: Y | `chore(logging): verify observability guardrails`

- [ ] 12. Live portable smoke and documentation note
  What to do / Must NOT do: Run the built app from `build/` against a real music folder, verify `build/SerionaData/logs/seriona.log`, `build/SerionaData/library.sqlite`, and `build/SerionaData/artwork/` are created/used, and verify logs include startup, scan, play, MPRIS, seek, and shutdown. Add a concise user-facing note to project docs only if a suitable doc exists (`DESIGN.md` is acceptable if it already documents runtime architecture; do not inflate README if it is intentionally just a title unless the executor judges a short section is useful and records why).
  Parallelization: Wave 3 | Blocked by: 10, 11 | Blocks: final wave
  References (executor has NO interview context - be exhaustive): `app/CMakeLists.txt:7`, `app/terminal_controller.cpp:181`, real smoke folders from prior QA: `/home/kaizen857/Music/CloudMusic(for MP4)/R・I・O・T/` and `/home/kaizen857/Music/CloudMusic(for MP4)/[M3-44] ARForest - The Unfinished [FLAC]`; `DESIGN.md` logging/runtime sections.
  Acceptance criteria (agent-executable): Live smoke leaves no `seriona` process running, creates portable data files under `build/SerionaData/`, and log file contains at least one line at `debug`, `info`, `warn` or documented reason if no warning occurred, plus an `error`/`critical` line from a controlled failure test rather than normal run.
  QA scenarios (name the exact tool + invocation): Happy: tmux-run `./build/seriona '<music folder>'`, D-Bus Play/SetPosition smoke, inspect `build/SerionaData/`. Failure: run invalid path or controlled failure to validate error/critical log output. Evidence `.omo/evidence/task-12-seriona-portable-logging.md`.
  Commit: Y | `docs(logging): document portable runtime data`

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [ ] F1. Plan compliance audit
  Verify every todo has an evidence file, matching commit, accepted QA, and no skipped acceptance criteria. Review `.omo/plans/seriona-portable-logging.md`, `.omo/evidence/task-*seriona-portable-logging.md`, `git log --oneline -20`, and `git status`.
- [ ] F2. Code quality review
  Read implementation diffs for over-design, public API leakage, excessive logging, hot-path violations, exception safety, and CMake target hygiene. Must specifically inspect logging bootstrap, app startup, scanner path plumbing, and audio realtime files.
- [ ] F3. Real manual QA
  Re-run live smoke from `build/` with a real music folder, inspect `build/SerionaData/`, validate log contents, validate MPRIS still works, and ensure no process remains.
- [ ] F4. Scope fidelity
  Confirm no Qt/QML/UI/config-framework work was added, no internal module names appear in portable data root names, and the implementation treats the backend as one product with one logger.

## Commit strategy
- Prefer one commit per todo. Keep implementation and direct tests together.
- Use repository semantic English style: `feat(logging): ...`, `fix(scanner): ...`, `test(logging): ...`, `docs(logging): ...`.
- If execution agents must split further, preserve dependency order: build/logging foundation -> runtime paths -> scanner plumbing -> module logs -> tests/smoke/docs.
- Do not commit generated runtime data under `SerionaData/` or live smoke logs/databases/artwork.

## Success criteria
- `spdlog` is linked cleanly through CMake and no product code includes it through public API headers.
- Running the app creates and uses `<exe>/SerionaData/logs/seriona.log`, `<exe>/SerionaData/library.sqlite`, and `<exe>/SerionaData/artwork/`.
- Log file records debug-to-critical capable output through one default multi-sink logger; terminal output remains concise.
- Logs cover app/control/audio/scanner/metadata lifecycle, decisions, warnings, and failures without per-tick spam.
- Static guard confirms no logging in realtime audio callback or PCM queue hot paths.
- Focused tests, full `ctest`, live smoke, and final review wave pass.
