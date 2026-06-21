# Task 10 - Scanner service full scan and hash-first startup reconciliation

## Scope delivered

- Replaced the default null `FileScannerService` with a real synchronous scanner orchestration over existing scanner pieces: path discovery, XXH3 hashing, TagReader metadata adapter, `.lrc` parser, SQLite cache, playlist snapshot builder, and scanner events.
- Preserved the public facade API: `FileScanner::scan(const std::vector<ScannerRoot>&, ScanMode)`, `stop()`, and `snapshot()`.
- Added an internal test-only construction seam in `src/scanner/file_scanner_service_internal.h`; public scanner headers remain unchanged.
- Added `seriona.scanner_service` coverage for hash-first cache hits, changed audio reread, changed/new/deleted `.lrc`, deleted audio pruning, single-file roots, malformed `.lrc`, TagReader failure, and cancellation/resume cache consistency.

## Manual QA artifact

This file is the manual-QA artifact for task 10. It records the exact commands and observable CTest evidence for cache-hit versus reread behavior:

- `scanner service scans hashes caches lyrics and skips unchanged rereads`: fake reader count remains `2` after the second unchanged full scan.
- `scanner service rereads changed audio and reparses only changed lrc`: fake reader count remains `1` after `.lrc` mutation, then becomes `2` after audio bytes change.
- `scanner service handles new and deleted lrc without tagreader and prunes deleted audio`: fake reader count remains `3` after new `.lrc`, deleted `.lrc`, and deleted audio reconciliation; deleted `.lrc` restores `EmbeddedTag` or `None`.

## Verification commands and results

```text
$ cmake -S . -B build -DSERIONA_BUILD_TESTS=ON
CMake Warning (dev) at /usr/share/cmake/Modules/FetchContent.cmake:1386 (message):
  The DOWNLOAD_EXTRACT_TIMESTAMP option was not given and policy CMP0135 is
  not set.
Call Stack (most recent call first):
  /home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:63 (FetchContent_Declare)
This warning is for project developers.  Use -Wno-dev to suppress it.

-- Configuring done (0.1s)
-- Generating done (0.1s)
-- Build files have been written to: /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
```

```text
$ cmake --build build
[0/1] Re-running CMake...
CMake Warning (dev) at /usr/share/cmake/Modules/FetchContent.cmake:1386 (message):
  The DOWNLOAD_EXTRACT_TIMESTAMP option was not given and policy CMP0135 is
  not set.
Call Stack (most recent call first):
  /home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:63 (FetchContent_Declare)
This warning is for project developers.  Use -Wno-dev to suppress it.

-- Configuring done (0.1s)
-- Generating done (0.1s)
-- Build files have been written to: /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
[1/6] Building CXX object CMakeFiles/seriona_scanner.dir/src/scanner/file_scanner_service.cpp.o
[2/6] Building CXX object CMakeFiles/seriona_scanner.dir/src/scanner/file_scanner_orchestrator.cpp.o
[3/6] Linking CXX static library libseriona_scanner.a
[4/6] Linking CXX executable tests/seriona_scanner_service_tests
[5/6] Linking CXX executable tests/seriona_scanner_contract_tests
```

```text
$ ctest --test-dir build -R 'seriona.scanner_service|seriona.scanner' --output-on-failure
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 26: seriona.scanner_test_harness
1/9 Test #26: seriona.scanner_test_harness .....   Passed    0.01 sec
    Start 27: seriona.scanner_contract
2/9 Test #27: seriona.scanner_contract .........   Passed    0.03 sec
    Start 28: seriona.scanner_paths
3/9 Test #28: seriona.scanner_paths ............   Passed    0.00 sec
    Start 29: seriona.scanner_hash
4/9 Test #29: seriona.scanner_hash .............   Passed    0.00 sec
    Start 30: seriona.scanner_tree
5/9 Test #30: seriona.scanner_tree .............   Passed    0.00 sec
    Start 31: seriona.scanner_scheduler
6/9 Test #31: seriona.scanner_scheduler ........   Passed    0.02 sec
    Start 32: seriona.scanner_cache
7/9 Test #32: seriona.scanner_cache ............   Passed    0.03 sec
    Start 33: seriona.scanner_tagreader
8/9 Test #33: seriona.scanner_tagreader ........   Passed    0.02 sec
    Start 34: seriona.scanner_service
9/9 Test #34: seriona.scanner_service ..........   Passed    0.04 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =   0.18 sec
```

## Adversarial probes

- Malformed input: malformed `.lrc` records a `MetadataReadFailed` scanner error and falls back to embedded lyrics without failing the scan.
- Cancel/resume: `stop()` before `scan()` emits a cancelled error/stopped event and leaves the prior cached snapshot intact.
- Stale state: deleted audio is pruned from the snapshot/cache; deleted `.lrc` clears external rows and restores embedded lyrics or `None`.
- Dirty worktree: inspected `GIT_MASTER=1 git status --short`; unrelated `.omo`, `AGENTS.md`, task-2/task-3 follow-up files were not edited for task 10.
- Hung/long commands: configure/build/CTest used bounded command timeouts and completed.
- Flaky tests: targeted scanner CTest was rerun after assertion fixes and passed.
- Misleading success output: full build initially exposed a `scanner_contract_tests` link failure even while older scanner CTest binaries passed; fixed CMake to link `seriona_scanner`, then rebuilt.
- Repeated interruptions: service scan is synchronous and cancellation is checked at scan start and per-root/hash operations; watcher/runtime interruptions are task 11 scope.

## Cleanup receipts

- LSP diagnostics were attempted on modified scanner files and failed with `Connection closed` / `Not connected`, matching prior project learnings; CMake build and CTest are the hard verification source.
- Pure LOC checks after splitting facade/orchestration: `src/scanner/file_scanner_service.cpp` = 34, `src/scanner/file_scanner_orchestrator.cpp` = 306, `src/scanner/file_scanner_service_internal.h` = 8, `tests/scanner/scanner_service_tests.cpp` = 209. The orchestrator is intentionally isolated; task 11 should split watcher/runtime logic before adding to it.
- `build/_deps/catch2-build` and `build/_deps/catch2-subbuild` were removed as regenerated build-cache cleanup only after a generator-cache conflict; no source files were discarded.

## Forward fix - preserve scanner snapshot hierarchy

Phase 1 review found that task-10 publication flattened every song into `song.metadata.filePath.filename()`, so directory roots lost real `root -> directory -> song` hierarchy. The forward fix carries each scanned song with its path relative to the matched scanner root and passes that relative path into `PlaylistTreeBuilder::addSong(...)`. `PlaylistTreeBuilder` then creates parent directory nodes such as `dir:artists` and `dir:artists/album` automatically. Single-file roots normalize the root-relative `.` path back to the filename, so they still publish `root -> track:single.flac` with no synthetic directory.

Additional service tests added:

- `scanner service publishes nested directory hierarchy for directory roots`: creates `artists/album/nested.flac` plus `nested.lrc`, then asserts `dir:artists -> dir:artists/album -> track:artists/album/nested.flac` parent links and verifies no `.lrc` node exists.
- `scanner service supports single file roots with same basename lrc`: additionally asserts the single-file snapshot contains only root + track and that the track parent is the root.

### Forward-fix verification

```text
$ cmake -S . -B build -DSERIONA_BUILD_TESTS=ON
CMake Warning (dev) at /usr/share/cmake/Modules/FetchContent.cmake:1386 (message):
  The DOWNLOAD_EXTRACT_TIMESTAMP option was not given and policy CMP0135 is
  not set.
Call Stack (most recent call first):
  /home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:63 (FetchContent_Declare)
This warning is for project developers.  Use -Wno-dev to suppress it.

-- Configuring done (0.1s)
-- Generating done (0.1s)
-- Build files have been written to: /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
```

```text
$ cmake --build build
ninja: no work to do.
```

```text
$ ctest --test-dir build -R 'seriona.scanner_service|seriona.scanner' --output-on-failure
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 26: seriona.scanner_test_harness
1/9 Test #26: seriona.scanner_test_harness .....   Passed    0.01 sec
    Start 27: seriona.scanner_contract
2/9 Test #27: seriona.scanner_contract .........   Passed    0.03 sec
    Start 28: seriona.scanner_paths
3/9 Test #28: seriona.scanner_paths ............   Passed    0.00 sec
    Start 29: seriona.scanner_hash
4/9 Test #29: seriona.scanner_hash .............   Passed    0.00 sec
    Start 30: seriona.scanner_tree
5/9 Test #30: seriona.scanner_tree .............   Passed    0.00 sec
    Start 31: seriona.scanner_scheduler
6/9 Test #31: seriona.scanner_scheduler ........   Passed    0.02 sec
    Start 32: seriona.scanner_cache
7/9 Test #32: seriona.scanner_cache ............   Passed    0.03 sec
    Start 33: seriona.scanner_tagreader
8/9 Test #33: seriona.scanner_tagreader ........   Passed    0.02 sec
    Start 34: seriona.scanner_service
9/9 Test #34: seriona.scanner_service ..........   Passed    0.04 sec

100% tests passed, 0 tests failed out of 9

Total Test time (real) =   0.18 sec
```

### Forward-fix adversarial probe

- Misleading success/failure output: running full build and CTest concurrently produced a transient `seriona.scanner_contract` "executable not found" failure while the test executable was still linking. Re-ran `cmake --build build` to completion first, then reran CTest sequentially; final targeted scanner CTest passed 9/9.
