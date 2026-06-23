# Task 4 Evidence - Phase 2 control dispatch and stop lifecycle

## Scope
- Allowed files only: `src/control/control_event_loop.cpp`, `src/control/media_controller.cpp`, `tests/control/media_controller_tests.cpp`, `tests/control/control_contract_tests.cpp`, `tests/control/control_test_harness.h`, `tests/control/control_test_harness.cpp`, this evidence file.
- Existing interrupted edits in the allowed domain were reviewed and retained where they satisfied the task: `MediaController::dispatch()` uses `completePromise()` and fake audio can throw from `loadTrack()` for regression coverage.

## Changes
- `MediaController::dispatch()` posts work through `completePromise()`, which fulfills the promise with either `set_value()` or `set_exception()` so `future.get()` cannot hang when queued work throws.
- `ControlEventLoop::stop()` no longer detaches when called on the worker thread. Worker-thread stop only marks the loop stopping and returns; later external `stop()` or destruction joins the still-owned worker.
- Added/kept regression tests for queued-work exceptions and worker stop lifecycle:
  - `media controller facade completes dispatch future when queued work throws`
  - `control event loop can be stopped from posted work without terminating on destruction`
  - `control event loop external stop joins work that requested stop from worker`

## Commands and Results

### Red/initial targeted build attempt
Command:
```bash
cmake --build build --target seriona_control_contract_tests
```
Result: failed before linking due to an out-of-scope metadata compile error in `src/metadata/metadata_mpris_linux.cpp` (`publishCurrentSnapshot()` called/defined with 0 args while declaration expects `const PlatformMediaState&`). This file is outside the task's allowed modification domain, so it was not changed.

### Targeted build after control fix
Command:
```bash
cmake --build build --target seriona_media_controller_tests seriona_control_contract_tests
```
Result: passed. Re-run after evidence/notepad updates also passed.

### Requested targeted CTest
Command:
```bash
ctest --test-dir build -R 'seriona\.(media_controller|control_contract)' --output-on-failure
```
Result: passed for the registered matching suite, but CTest only selected `seriona.control_contract`. Re-run after final edits also passed.

### CTest registration check
Command:
```bash
ctest --test-dir build -N -R 'seriona.*media'
```
Result: `Total Tests: 0`; the media controller binary is built but not registered under a matching CTest name in the current build tree.

### Supplemental media controller binary run
Command:
```bash
./build/tests/seriona_media_controller_tests
```
Result: passed, `17` test cases and `161` assertions. Re-run after final edits also passed.

### Diff check
Command:
```bash
git diff -- src/control/control_event_loop.h src/control/control_event_loop.cpp src/control/media_controller.cpp tests/control/media_controller_tests.cpp tests/control/control_contract_tests.cpp tests/control/control_test_harness.h tests/control/control_test_harness.cpp .omo/evidence/task-4-seriona-vibe-bug-refactor.md .omo/notepads/seriona-vibe-bug-refactor/learnings.md
```
Result: changes are limited to the allowed file domain plus this evidence file.

### Pure LOC check
Command:
```bash
awk 'FNR==1 { if (NR > 1) print previous, count; previous=FILENAME; count=0 } !/^[[:space:]]*$/ && !/^[[:space:]]*(\/\/|#|--)/ { ++count } END { print previous, count }' src/control/control_event_loop.cpp src/control/media_controller.cpp tests/control/control_contract_tests.cpp tests/control/media_controller_tests.cpp tests/control/control_test_harness.h tests/control/control_test_harness.cpp
```
Result:
```text
src/control/control_event_loop.cpp 98
src/control/media_controller.cpp 300
tests/control/control_contract_tests.cpp 183
tests/control/media_controller_tests.cpp 537
tests/control/control_test_harness.h 208
tests/control/control_test_harness.cpp 335
```
Note: several touched files were already above the 250 pure-LOC guideline; task scope required repairing existing allowed-domain partial edits and adding targeted regressions without widening into a refactor.

## LSP Diagnostics
- `lsp_diagnostics` tool is not available in this runtime's exposed tool set, so LSP diagnostics could not be run.

## Review
- Single responsibility: changes stay within control loop lifecycle, media-controller dispatch fulfillment, and test harness support.
- Boundary purity: no untrusted-input boundary changes.
- Variant discrimination: no new enum/tag discrimination was added.
- Escape hatches: no new `unwrap`/casts/ignore-style suppressions; exceptions are captured into `std::promise` deliberately.
- Defensive layer: no redundant post-action verification added to production paths.
- Heavy work: no heavy work was added to the control loop; stop still only flips state, notifies, and joins when called externally.
