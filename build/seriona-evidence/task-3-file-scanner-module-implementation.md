# Task 3 scanner test harness evidence

## Baseline before edits

- Configured with `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` successfully; only an upstream TagReader FetchContent developer warning was emitted.
- Built existing targets with `cmake --build build` successfully before adding scanner harness files.
- Ran focused baseline `ctest --test-dir build -R seriona\.audio_contract --output-on-failure`; 1/1 passed.

## Harness coverage added

- `tests/scanner/scanner_test_harness.*` provides temp scanner root and DB marker cleanup helpers, deterministic audio and `.lrc` fixture writers, fake clock, fake event sink, fake TagReader adapter, fake watcher events, and assertion helpers.
- Fake TagReader models success, exception, and deterministic block/release behavior without real TagReader, hardware, user media folders, or network.
- Fake watcher models audio and `.lrc` create/modify/destroy/rename events, rename old/new path associations, and watcher warning events.
- `seriona.scanner_test_harness` CTest target is helper-only and does not link production scanner logic.

## Verification notes

- First targeted scanner harness CTest run failed on incorrect expected byte counts for deterministic `.lrc` fixtures; the failure proved the helper-only target was executing and was fixed by matching the exact written fixture lengths.
- Follow-up verification closed the watcher-warning edge: `FakeWatcher` now exposes explicit `audioWarning(...)` and `lrcWarning(...)` helpers, and the helper-only test asserts both warning events preserve their `PathKind` and path payload.
