# seriona-log-cleanup - Work Plan

## TL;DR (For humans)
<!-- Fill this LAST, after the detailed plan below is written, so it summarizes the REAL plan. -->
<!-- Plain English for a non-engineer: NO file paths, NO todo numbers, NO wave/agent/tool names. -->

**What you'll get:** Seriona will create a new timestamped log file every time it starts, and automatically delete the oldest logs when the total log folder exceeds 50 MB so the folder never grows unbounded.

**Why this approach:** spdlog alone cannot manage by folder total size or create per-session files; a thin pre-startup cleanup function using `<filesystem>` handles both, then spdlog's standard rotating_file_sink_mt manages intra-session rotation.

**What it will NOT do:** It will not add a custom spdlog sink, not change the log format or levels, not affect any module beyond logging bootstrap and app startup.

**Effort:** Short
**Risk:** Low - additive filesystem scan and cleanup, no runtime path changes
**Decisions to sanity-check:** 50 MB total log folder limit; timestamp format `YYYYMMDDHHmmss`

Your next move: run `$start-work .omo/plans/seriona-log-cleanup.md`, or ask for a high-accuracy review first. Full execution detail follows below.

---

> TL;DR (machine): Short / low-risk plan to add per-session timestamped log files and 50MB folder-level cleanup using <filesystem>, without custom spdlog sinks.

## Scope
### Must have
- Add `seriona::logging::prepareLogFile(logDir, maxTotalMB)` to scan the log directory, delete oldest files if total exceeds limit, then return a timestamped log file path.
- Generate per-session log filenames: `seriona-YYYYMMDDHHmmss.log` (e.g. `seriona-20260624192801.log`).
- Use `<filesystem>` for directory iteration, file size, and deletion; no external dependencies.
- Retain spdlog's `rotating_file_sink_mt` for intra-session rotation (5 MB max, 3 rotated files).
- Integrate `prepareLogFile()` into `app/terminal_controller.cpp` before `logging::initialize()`.
- Default total log folder limit: 50 MB.
- Handle empty directories, permission errors, rotated file suffixes (`.log.1`, `.log.2`, `.log.3`), and non-log files gracefully.
- Add tests covering: cleanup when over limit (deletes oldest), cleanup when under limit (deletes none), empty directory, rotated file cleanup, and timestamped filename generation.

### Must NOT have (guardrails, anti-slop, scope boundaries)
- Must not create a custom spdlog sink class.
- Must not change the log format, levels, or flush behavior.
- Must not change the `RuntimePaths` struct or `resolveRuntimePaths()` — log directory stays `SerionaData/logs/`, only the filename generation moves to the logging module.
- Must not affect any module beyond `src/logging/` and `app/terminal_controller.cpp`.
- Must not add external dependencies beyond `<filesystem>` (already available in C++17).
- Must not log during the cleanup step itself (no spdlog available yet at that point); use stderr for errors.

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: tests-after with doctest/CTest. TDD for `prepareLogFile` since it's new behavior; tests-after for integration.
- Evidence: `.omo/evidence/task-<N>-seriona-log-cleanup.md` per todo. Final evidence: `.omo/evidence/task-final-seriona-log-cleanup.md`.
- Required commands: `cmake --build build`, `ctest --test-dir build -R 'seriona.logging' --output-on-failure`, `ctest --test-dir build --output-on-failure`, live smoke verifying old logs deleted and new timestamped file created.

## Execution strategy
### Parallel execution waves
- Wave 1: Implementation of `prepareLogFile()` + tests (Todos 1-2, serial — test depends on implementation)
- Wave 2: Integration into app startup + live smoke (Todo 3, blocked by 1-2)

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2, 3 | none |
| 2 | 1 | 3 | none |
| 3 | 1, 2 | final wave | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 1. Add prepareLogFile() to logging bootstrap
  What to do / Must NOT do: Add `seriona::logging::prepareLogFile(logDir, maxTotalMB)` declaration to `src/logging/logging.h` and implementation to `src/logging/logging.cpp`. It must: (1) iterate `logDir` for files with `.log` extension (including rotated `.log.1`, `.log.2`, `.log.3`), (2) collect each file's path + mtime + size, (3) sort by mtime ascending (oldest first), (4) calculate total folder size, (5) while total > maxTotalMB, delete the oldest file, subtract its size, repeat, (6) compute a timestamped filename `seriona-YYYYMMDDHHmmss.log` and return the full path `logDir / filename`. Handle: empty dir (return timestamped path, no delete), permission errors (log to stderr, skip file, no crash), only `.log*` files counted/deleted. Must NOT call spdlog inside this function (no logger available yet). Must NOT change `RuntimePaths`.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 2, 3
  References (executor has NO interview context - be exhaustive): `src/logging/logging.h:1`, `src/logging/logging.cpp:1-48`. Use `<filesystem>`: `directory_iterator`, `file_size`, `last_write_time`, `remove`. Use `<chrono>` for timestamp: `system_clock::now()`, `time_t`, `localtime`, `strftime`. Default limit: 50 MB (50 * 1024 * 1024). Timestamp format: `%Y%m%d%H%M%S`.
  Acceptance criteria (agent-executable): Function compiles and returns a path ending in `seriona-YYYYMMDDHHmmss.log` under the given logDir.
  QA scenarios (name the exact tool + invocation): Happy: create temp dir with 3 log files totaling 60MB, call prepareLogFile(logDir, 50MB), verify oldest file deleted and returned path is timestamped. Failure: unwritable directory → stderr message, no crash. Empty directory → returns timestamped path, no error. Evidence `.omo/evidence/task-1-seriona-log-cleanup.md`.
  Commit: Y | `feat(logging): add per-session log file with folder cleanup`

- [x] 2. Add cleanup and timestamp tests
  What to do / Must NOT do: Add test cases to `tests/logging/logging_tests.cpp` covering: (A) directory over 50MB → oldest file deleted, total now under limit; (B) directory under 50MB → no files deleted; (C) empty directory → no error, timestamped path returned; (D) rotated files (`.log.1`, `.log.2`) included in size calculation and deleted; (E) non-log files ignored in size/deletion; (F) timestamped filename matches expected pattern. Must NOT test spdlog logger creation or log content. Must NOT create a new test file — extend existing `logging_tests.cpp`.
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 3
  References (executor has NO interview context - be exhaustive): `tests/logging/logging_tests.cpp:1-66`, `tests/logging/CMakeLists.txt` (test target `seriona_logging_tests`). Use doctest fixtures with temp directories via `<filesystem>`.
  Acceptance criteria (agent-executable): `ctest --test-dir build -R 'seriona.logging' --output-on-failure` passes all new cases.
  QA scenarios (name the exact tool + invocation): Happy: all test cases above pass. Failure: test creates 80MB total, limit 50MB, asserts 30MB+ deleted and only newest file remains. Evidence `.omo/evidence/task-2-seriona-log-cleanup.md`.
  Commit: Y | `test(logging): cover per-session log cleanup`

- [x] 3. Integrate prepareLogFile into app startup
  What to do / Must NOT do: In `app/terminal_controller.cpp`, replace the hardcoded `runtimePaths.logFile` usage with a call to `prepareLogFile(runtimePaths.dataRoot / "logs", 50)` to get a timestamped log path, then pass that to `seriona::logging::initialize()`. The `ensureDirectoriesExist()` call already creates the logs dir — ensure it runs before `prepareLogFile`. Must NOT change any other file. Must NOT change the shutdown sequence.
  Parallelization: Wave 2 | Blocked by: 1, 2 | Blocks: final wave
  References (executor has NO interview context - be exhaustive): `app/terminal_controller.cpp:190-200` (startup section with `logging::initialize` call). `prepareLogFile` signature: `std::filesystem::path prepareLogFile(const std::filesystem::path& logDir, std::uintmax_t maxTotalBytes)`. Default: 50 MB = 52428800.
  Acceptance criteria (agent-executable): `cmake --build build` passes, 45/45 `ctest` passes. Running `./build/seriona <music dir>` creates `SerionaData/logs/seriona-YYYYMMDDHHmmss.log`. Running again creates a second timestamped file. After creating enough log data to exceed 50MB, the oldest file is deleted.
  QA scenarios (name the exact tool + invocation): Happy: live tmux smoke — run app, verify `SerionaData/logs/seriona-*.log` exists with timestamp. Run again, verify second file. Manually create old large log files, run again, verify oldest deleted. Failure: unwritable log dir — app starts with console-only fallback (existing behavior). Evidence `.omo/evidence/task-3-seriona-log-cleanup.md`.
  Commit: Y | `feat(app): use timestamped session log file`

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE.
- [ ] F1. Plan compliance audit
  Verify all 3 todos have evidence, matching commits, and accepted QA.
- [ ] F2. Code quality review
  Read implementation for exception safety, filesystem edge cases, no spdlog dependency in cleanup, no memory leaks.
- [ ] F3. Real manual QA
  Live smoke with two app launches, verify timestamped files and old log deletion.
- [ ] F4. Scope fidelity
  Confirm no custom spdlog sink, no RuntimePaths changes, no module changes beyond logging/app startup.

## Commit strategy
- One commit per todo (3 total).
- Use semantic English style: `feat(logging): ...`, `test(logging): ...`, `feat(app): ...`.

## Success criteria
- Every app startup creates `SerionaData/logs/seriona-YYYYMMDDHHmmss.log`.
- Total log folder stays ≤50 MB.
- Oldest logs deleted first.
- Rotated files (`*.log.1`, `*.log.2`, `*.log.3`) included in cleanup.
- Non-log files in the directory are ignored.
- 45/45 tests pass, live smoke confirmed.
