# Task 1 - File scanner CMake foundation evidence

## Scope
- Upgraded root CMake C++ standard from C++20 to C++23.
- Added fixed `seriona_scanner` static library target.
- Added scanner dependency gates for `SQLite3` and `PkgConfig::SERIONA_XXHASH`.
- Added local TagReader via `add_subdirectory("/home/kaizen857/cppProject(app_and_lib)/TagReader" "${PROJECT_BINARY_DIR}/tagreader" EXCLUDE_FROM_ALL)` when `TagReaderCore` is absent.
- Linked `TagReaderCore` privately to `seriona_scanner`; scanner public header does not include TagReader headers or expose TagReader types.
- Vendored pinned `wtr/watcher` header under `third_party/watcher/include/wtr/watcher.hpp` with source and license notes.

## Baseline proof before edits
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`: passed.
- `cmake --build build`: passed; build was already up to date before changes.

## Dependency availability
- `pkg-config --modversion sqlite3`: `3.53.2`.
- `pkg-config --modversion libxxhash`: `0.8.3`.

## Missing dependency probes
- `cmake -S . -B build-missing-sqlite -DSERIONA_BUILD_TESTS=ON -DSERIONA_SCANNER_SIMULATE_MISSING_SQLITE=ON`: failed after FFmpeg detection at `seriona_scanner dependency gate: SQLite3 is required for scanner caching`.
- `cmake -S . -B build-missing-xxhash -DSERIONA_BUILD_TESTS=ON -DSERIONA_SCANNER_SIMULATE_MISSING_XXHASH=ON`: failed after FFmpeg and SQLite3 detection at `seriona_scanner dependency gate: libxxhash is required for scanner hashing`.

## Final verification
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`: passed from a clean `build` directory.
- `cmake --build build`: passed after rerun with a longer timeout; produced `libseriona_scanner.a`, `TagReaderCore`, app, and tests.
- `ctest --test-dir build --output-on-failure`: passed, 25/25 Seriona tests.

## Diagnostics
- LSP diagnostics tool attempted for changed CMake/scanner/watcher files, but the tool reported `Connection closed` / `Not connected`; CMake configure/build/CTest are the executable verification evidence for this checkbox.

## Notes
- A TagReader/Catch2 `FetchContent` developer warning about `DOWNLOAD_EXTRACT_TIMESTAMP` appears during configure from the external TagReader project; it does not fail configure/build/tests.
- Existing audio contract test emits a pre-existing `-Wmissing-field-initializers` warning under C++23; build still succeeds and no audio source changes were needed.
