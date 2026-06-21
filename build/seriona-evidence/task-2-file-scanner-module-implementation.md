# Task 2 — Public scanner contracts and service facade

## Baseline before edits

- `GIT_MASTER=1 git status --short` showed pre-existing unrelated changes in `.omo/boulder.json` and `.omo/plans/file-scanner-module-implementation.md`; these were not edited or staged for this task.
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` completed successfully.
- `cmake --build build --target seriona_scanner` completed successfully before scanner contract edits.
- `ctest --test-dir build -R seriona.audio_contract --output-on-failure` passed: 1/1 tests passed.

## Red / compile-first proof

- Added `seriona_scanner_contract_tests` referencing `seriona/scanner/file_scanner_service.h` before the public header existed.
- After regenerating CMake, `cmake --build build --target seriona_scanner_contract_tests` failed for the expected reason: missing `seriona/scanner/file_scanner_service.h`.

## Implementation summary

- Added `inc/seriona/scanner/scanner_contracts.h` with pure C++ scanner value types, event sink, service interface, playlist snapshot types, lyrics fields, and future-CUE placeholders.
- Added `inc/seriona/scanner/file_scanner_service.h` with `FileScanner` facade declarations and `makeFileScannerService`.
- Added `src/scanner/file_scanner_service.cpp` as a minimal no-op facade/service stub for declarations and linking only; no scanning logic was implemented.
- Added scanner contract tests under `tests/scanner/` and registered `seriona.scanner_contract` in `tests/CMakeLists.txt`.

## Dependency boundary proof

- `seriona_scanner_contract_tests` includes only `${PROJECT_SOURCE_DIR}/inc` and `${PROJECT_SOURCE_DIR}/third_party/doctest`; it does not link `seriona_scanner`, `seriona_third_party_headers`, TagReader, watcher, SQLite, FFmpeg, or miniaudio include interfaces.
- Public scanner header grep for `TagReader|MusicTag|SQLite|sqlite|watcher|wtr/|Qt|QML|miniaudio|audio/device|AudioOutput` returned no matches.

## Verification after edits

- `cmake --build build --target seriona_scanner_contract_tests` passed.
- `ctest --test-dir build -R seriona.scanner_contract --output-on-failure` passed: 1/1 tests passed.
- `cmake --build build --target seriona_scanner` passed.

## Follow-up verification tightening

- Extended `tests/scanner/scanner_contract_tests.cpp` to explicitly assert `ScannerConfig` default values: 250 ms progress interval, empty extension list, no symlink following, embedded lyrics enabled, and external lyrics enabled.
- Added default `SongMetadata` assertions for `LyricsSource::None`, empty effective lyrics, unset external lyrics path/hash/mtime, empty `sourceFilePath`, unset `offset`/`duration`, and empty `logicalTrackId`.
- Added playlist node kind distinction assertions for root, directory, and track nodes, including parent relationships between structural and track-like nodes.
- Added scanner event type/payload consistency assertions for progress, playlist snapshot, song, and error events using `std::holds_alternative` and `std::get` on `ScannerEventPayload`.
- Follow-up targeted verification: `cmake --build build --target seriona_scanner_contract_tests` passed.
- Follow-up targeted verification: `ctest --test-dir build -R seriona.scanner_contract --output-on-failure` passed: 1/1 tests passed.
