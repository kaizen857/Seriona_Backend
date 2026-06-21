# Task 11 evidence - wtr/watcher adapter, debounce, and runtime incremental updates

## Scope delivered
- Added private watcher runtime seam: `FolderWatcher`, `FolderWatcherFactory`, `WatchEvent`, `WtrFolderWatcher`, and `WtrFolderWatcherFactory` wrap vendored `wtr::watch(path, callback)` and call RAII `close()` on `stopWatching` / destruction.
- Extended service/facade with `startWatching` and `stopWatching`; watcher events enqueue dirty hints and the debounce loop re-runs existing hash-first reconciliation rather than trusting event payload state.
- Mapped `create`, `modify`, `destroy`, `rename`, `owner`, `other`, watcher path kinds, associated rename events, and warning/error/overflow-style watcher messages.
- `.lrc` create/modify/destroy/rename events go through the existing external lyrics reconciliation path; fake-reader counts prove `.lrc`-only changes do not call TagReader.

## Manual QA artifact
- Command: `ctest --test-dir build -R 'seriona.scanner_watcher|seriona.scanner_service|seriona.scanner' --output-on-failure`
- Observable result: 10/10 scanner tests passed in 0.33s, including `seriona.scanner_watcher`.
- Fake-reader observable counts: watcher `.lrc` modify/delete test keeps fake TagReader read count at 1 while snapshot lyrics switch ExternalLrc -> EmbeddedTag; watcher warning test increments fake reader from 1 to 2 only after root reconciliation discovers the new audio file.

## Verification commands and results
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` passed; only existing TagReader FetchContent CMP0135 developer warning appeared.
- `cmake --build build` passed after explicitly initializing the new watcher dependency field in service tests.
- `ctest --test-dir build -R 'seriona.scanner_watcher|seriona.scanner_service|seriona.scanner' --output-on-failure` passed: 10 tests, 0 failures.

## Adversarial probes
- Malformed input: watcher warning/overflow-style message `w_sys_q_overflow` triggers root reconciliation and records a scanner error event instead of trusting partial events.
- Cancel/resume or long-running flow: `stopWatching()` sets the stop flag, closes watchers, joins debounce thread, and later fake callbacks are ignored by closed fake watcher state.
- Stale state: all watcher events rerun existing filesystem/hash/cache reconciliation, so deleted audio is pruned and renamed/new files are discovered from disk state.
- Dirty worktree: final staging is restricted to task-11 scanner/runtime/test/evidence/notepad/plan files; unrelated dirty files remain unstaged.
- Hung/long commands: configure, build, and CTest all completed under the default tool timeouts; no watcher test sleeps wait unboundedly.
- Flaky tests: fake watcher and 5ms debounce are deterministic enough for CTest; wait helpers use bounded retries and fail loudly.
- Misleading success output: build was run before CTest sequentially, avoiding the prior executable-not-found race noted in task-10 learnings.
- Repeated interruptions: `stopWatching()` is idempotent and also called by service destruction, so watcher resources close on explicit stop or teardown.

## Cleanup receipts
- No real watcher smoke was required for default tests; fake watcher drives all acceptance behavior.
- LSP diagnostics were stale for generated include paths/private headers during edits; CMake build and CTest are the source of truth for this C++ task.
- Pure LOC after edits: `src/scanner/file_scanner_orchestrator.cpp` 501, `tests/scanner/scanner_watcher_tests.cpp` 231, `inc/seriona/scanner/scanner_contracts.h` 124, `src/scanner/file_scanner_service_internal.h` 42. The orchestrator was already a task-10 large orchestration unit; task 11 used the existing service seam instead of a broad refactor to preserve committed behavior.

## Residual risks
- Runtime watcher reconciliation currently debounces to root-level hash-first reconciliation for correctness; path-scoped partial reconciliation can be optimized later without changing semantics.
- Production `wtr` live/die messages are mapped but only warning/error/overflow-like messages publish root-reconciliation errors.
