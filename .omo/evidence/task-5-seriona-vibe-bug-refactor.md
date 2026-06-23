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
