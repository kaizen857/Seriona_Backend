# Task 5 Evidence - seriona-vibe-bug-refactor

## Scope
- Implemented Phase 2/4 bridge for scanner work to leave `ControlEventLoop`.
- Modified only task-allowed source/test/evidence/notepad files; unrelated dirty files were present before/after and were not touched by this task.

## Red/Fix Notes
- Added scanner service regression proving `FileScannerService::scan()` returns before a fake slow TagReader read completes; old synchronous implementation would block until `release()`.
- Added media controller regression proving a blocked scanner `scan()` does not block `Pause` command dispatch or `PlaybackPositionUpdated` handling on the control executor.
- Refactored production `OrchestratedFileScannerService::scan()` to enqueue a value-semantic `ScanRequest` onto a scanner-owned worker; `runScan()` performs filesystem/TagReader/SQLite/tree synthesis outside the control event loop.

## Commands and Results
- `cmake --build build --target seriona_media_controller_tests`
  - First run after using `ScanScheduler` in `seriona_scanner` failed at link time because `src/scanner/scan_scheduler.cpp` is not part of the `seriona_scanner` target and task scope forbids editing `CMakeLists.txt`.
  - Resolution: replaced that integration with an internal scanner-owned worker in `src/scanner/file_scanner_orchestrator.cpp`.
- `cmake --build build --target seriona_media_controller_tests seriona_scanner_scheduler_tests seriona_scanner_service_tests`
  - Passed after implementation and test updates.
- `ctest --test-dir build -R 'seriona\.(media_controller|scanner_scheduler|scanner_service)' --output-on-failure`
  - Initial async test pass failed in `seriona.scanner_service` because existing scanner service tests assumed synchronous `scan()` completion.
  - Updated tests to wait for public snapshot/event outcomes instead of synchronous return.
  - Final result: passed, 2/2 tests (`seriona.scanner_scheduler`, `seriona.scanner_service`).
  - Note: current CTest registry names the media-controller test `seriona.control_controller`, so the requested regex does not select it.
- `./build/tests/seriona_media_controller_tests`
  - Passed: 18/18 test cases, 172/172 assertions.
- `git status --short`
  - Showed unrelated existing dirty files/artifacts (`DESIGN.md`, `.omo/boulder.json`, plan/report/run files) plus this task's allowed edits; no git write commands were used.
- `for f in ...; do awk ... "$f" | wc -l; done`
  - Pure LOC: `src/control/media_controller.cpp` 301, `src/scanner/file_scanner_orchestrator.cpp` 588, `tests/control/control_test_harness.cpp` 359, `tests/control/control_test_harness.h` 217, `tests/control/media_controller_tests.cpp` 584, `tests/scanner/scanner_service_tests.cpp` 354.
  - Several touched files are pre-existing large files; no scope-expanding split was done because task file domain forbids creating new implementation/test split files.

## Diagnostics
- No standalone `lsp_diagnostics` tool is available in this environment. `apply_patch` surfaced transient stale diagnostics for `tests/control/*` immediately after header edits; actual `cmake --build` compiled the declarations and definitions successfully.

## Acceptance
- `MediaController::scanLibrary()` no longer queues scanner work onto `ControlEventLoop`; it only validates running state and sends scanner start requests.
- Production scanner heavy work runs on scanner-owned worker state in `src/scanner/file_scanner_orchestrator.cpp`.
- Scanner events still cross back to control by value through `ScannerEventSink`.
## Task 10 scanner integration regression follow-up - 2026-06-24

Scope: only the Task 10 full-suite scanner-domain regressions in `seriona.scanner_service` and `seriona.scanner_watcher`.

Why this evidence belongs to Task 5: the observed failures came from Task 5's scanner-owned asynchronous `scan()` worker changing the test timing contract. The scanner production flow still publishes the same public playlist snapshots/events after worker completion; the broken assertions were reading internal fake TagReader counts or starting watcher reconciliation before the initial public snapshot was stable. This is not a Task 8 public-boundary/cache DTO regression.

Reproduction before fix:

```text
$ cmake --build build --target seriona_scanner_service_tests seriona_scanner_watcher_tests seriona_scanner_scheduler_tests seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests
ninja: no work to do.

$ ctest --test-dir build -R 'seriona\.(scanner_service|scanner_watcher|scanner_scheduler|scanner_cache|scanner_tagreader|scanner_contract)' --output-on-failure
seriona.scanner_contract ........ Passed
seriona.scanner_scheduler ....... Passed
seriona.scanner_cache ........... Passed
seriona.scanner_tagreader ....... Passed
seriona.scanner_service ......... Passed
seriona.scanner_watcher ......... Failed
```

Failing watcher symptoms:

- `scanner watcher debounces create modify destroy rename into hash-first rescans`: initial `reader->readCount() == 1U` observed `0`, then timed out waiting for fake TagReader read count.
- `scanner watcher updates lrc only without TagReader and handles delete fallback`: initial `reader->readCount() == 1U` observed `0`.
- `scanner watcher warning error and overflow messages force root reconciliation`: root-reconciliation warning event was checked before the async watcher-triggered scan path completed.
- `scanner watcher stop closes watcher and ignores later callbacks`: observed `readCount == 2` and two songs because the initial async scan raced with file creation after `stopWatching()`.
- `scanner watcher startup failure leaves no live callback into service`: initial `reader->readCount() == 1U` observed `0`.

Root cause:

- `FileScannerService::scan()` now enqueues work and returns immediately.
- Watcher tests called `scan()`, then immediately called `startWatching()` or mutated the watched directory while the initial scan was still queued/running.
- Service changed-audio coverage waited for TagReader `readCount == 2`, which can happen before `runScan()` publishes the updated playlist snapshot, leaving a stale title (`Before`) visible under full-suite timing.

Fix:

- `tests/scanner/scanner_watcher_tests.cpp`: wait for the initial public playlist snapshot to contain the expected song before starting watchers or exercising startup-failure/stop callback behavior.
- `tests/scanner/scanner_service_tests.cpp`: wait for the initial public snapshot instead of only fake TagReader count, and wait for the changed-audio public snapshot title to become `After` before asserting lyrics/cache behavior.
- Production scanner code was not changed; the fix keeps the real behavior assertions and removes only the invalid synchronous-test timing assumptions.

Validation after fix:

```text
$ cmake --build build --target seriona_scanner_service_tests seriona_scanner_watcher_tests
[1/4] Building CXX object tests/CMakeFiles/seriona_scanner_watcher_tests.dir/scanner/scanner_watcher_tests.cpp.o
[2/4] Linking CXX executable tests/seriona_scanner_watcher_tests
[3/4] Building CXX object tests/CMakeFiles/seriona_scanner_service_tests.dir/scanner/scanner_service_tests.cpp.o
[4/4] Linking CXX executable tests/seriona_scanner_service_tests

$ ctest --test-dir build -R 'seriona\.(scanner_service|scanner_watcher)' --output-on-failure
seriona.scanner_service ......... Passed
seriona.scanner_watcher ......... Passed
100% tests passed, 0 tests failed out of 2
```

Required full scanner regression after fix:

```text
$ cmake --build build --target seriona_scanner_service_tests seriona_scanner_watcher_tests seriona_scanner_scheduler_tests seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests
ninja: no work to do.

$ ctest --test-dir build -R 'seriona\.(scanner_service|scanner_watcher|scanner_scheduler|scanner_cache|scanner_tagreader|scanner_contract)' --output-on-failure
seriona.scanner_contract ......... Passed
seriona.scanner_scheduler ........ Passed
seriona.scanner_cache ............ Passed
seriona.scanner_tagreader ........ Passed
seriona.scanner_service .......... Passed
seriona.scanner_watcher .......... Passed
100% tests passed, 0 tests failed out of 6
```
