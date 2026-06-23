# Task 3 Evidence - seriona-vibe-bug-refactor

## Scope
- Modified only task-allowed metadata files plus this required evidence file and allowed notepad append.
- Product changes stayed inside `src/metadata/metadata_mpris_private.h`, `src/metadata/metadata_mpris_linux.cpp`, and `src/metadata/metadata_mpris_backend.cpp`.
- Regression tests added in `tests/metadata/metadata_mpris_tests.cpp`.
- No public metadata/control/audio/scanner contracts were changed; no platform types were exposed outside metadata private files.

## Failing-first regression
- Baseline command from interrupted tree: `cmake --build build --target seriona_metadata_mpris_tests seriona_metadata_service_tests`
- Baseline result: failed before repair because `metadata_mpris_private.h` declared `publishCurrentSnapshot(const PlatformMediaState&)` while `metadata_mpris_linux.cpp` still called/defined `publishCurrentSnapshot()`.
- Failure excerpt: `error: no matching function for call to 'seriona::metadata::detail::LinuxMprisAdapter::publishCurrentSnapshot()'`.
- Prior failing-first proof retained from interrupted attempt: with the old unsynchronized optional sink, `ctest --test-dir build -R 'seriona.metadata_mpris' --output-on-failure` aborted in `std::optional::operator*()` during racing unsubscribe/update/dispatch.
- The retained tests cover two regressions: callback unsubscribe during dispatch, and concurrent command dispatch with subscription churn plus metadata update.

## Implementation summary
- `CommandSinkState` now owns a mutex-protected optional command sink with `set`, `clear`, and `snapshot` operations.
- MPRIS command handlers copy the sink via `snapshot()` before invocation, so synchronization is released before calling external code.
- `LinuxMprisAdapter` also snapshots `PlatformMediaState` under `stateMutex_` before command validation to avoid racing update/dispatch reads.
- Snapshot publishing now uses the accepted `PlatformMediaState` argument instead of reading mutable adapter state.
- MPRIS handlers still emit only `seriona::control::MediaControlCommand` values; no direct control/audio/scanner mutation was added.
- Linux backend unsubscribe now calls `CommandSinkState::clear()` instead of mutating optional storage directly.

## Verification
- Command: `cmake --build build --target seriona_metadata_mpris_tests seriona_metadata_service_tests`
- Result: passed.
- Command: `ctest --test-dir build -R 'seriona\.(metadata_mpris|metadata_service|metadata_service_recording|metadata_mpris_smoke)' --output-on-failure`
- Result: passed; 4/4 tests passed: `seriona.metadata_service`, `seriona.metadata_service_recording`, `seriona.metadata_mpris`, `seriona.metadata_mpris_smoke`.
- Search check: no remaining `commandSinkState_->sink`, `->sink`, `.sink`, or `sink.` direct access remained in `src/metadata/metadata_mpris*` after the fix.
- LSP diagnostics: unavailable in this Codex tool session; targeted CMake compilation completed with no errors.

## Review notes
- `src/metadata/metadata_mpris_linux.cpp` is a pre-existing large private Linux adapter (~289 pure LOC after this task); left in place to avoid widening this task into structural refactor work.
- Single responsibility remains the private MPRIS adapter; command handlers parse platform actions into `MediaControlCommand` values only.
