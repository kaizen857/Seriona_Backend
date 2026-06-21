## 2026-06-20 task 1 scanner foundation

- Current Seriona root CMake previously used C++20 and only created audio/app/test targets; `seriona_scanner` did not exist.
- `sqlite3` and `libxxhash` are installed locally (`sqlite3` 3.53.2, `libxxhash` 0.8.3), so implementation could continue without package-manager commands.
- Adding local TagReader with `EXCLUDE_FROM_ALL` still lets its `test/CMakeLists.txt` register CTest tests unless the child test directory TESTS property is cleared after import.
- `TagReaderCore` builds as the scanner private dependency; scanner public header remains independent from `TagReader.hpp` and `MusicTag`.
- LSP diagnostics were unavailable in this session (`Connection closed` / `Not connected`), so CMake configure/build/CTest are the hard verification source.

## 2026-06-20 task 2 public scanner contracts

- Audio public contracts use a single `*_contracts.h` namespace block with enum/value types first, `std::variant` event payloads, `std::function` event sinks, then the pure virtual service and facade pattern.
- Scanner public header compile isolation can be tested without linking `seriona_scanner` and without inheriting `seriona_third_party_headers`; the contract target only needs `inc` and doctest.
- `src/scanner/scanner_module.cpp` remains the only current scanner source that includes watcher headers; the new public scanner headers do not expose watcher, SQLite, TagReader, Qt/QML, miniaudio, or audio device names.

## 2026-06-20 task 3 scanner test harness

- `tests/` had no scanner test directory before this task; existing tests use direct `add_executable`, per-target doctest include directories, `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`, and explicit `add_test` entries.
- The scanner production module is still a minimal link sentinel, so the harness was kept independent from production scanner internals and TagReader private types.
- A helper-only scanner CTest can validate fake TagReader exception behavior inside the test process with `CHECK_THROWS_WITH_AS`, proving exceptions do not terminate the process.
- Follow-up review found watcher warnings were hardcoded to audio path kind; warning-style fake events need the same audio/`.lrc` path-kind distinction as create/modify/destroy/rename events.

## 2026-06-20 task 2 follow-up verification

- Contract tests must assert default field values, not only assigned examples; scanner acceptance depends on `ScannerConfig`, lyrics defaults, and future-CUE placeholders being executable checks.
- `ScannerEventType` and `ScannerEventPayload` are a convention pair rather than a type-enforced discriminated union, so tests should construct each event category and assert the held `std::variant` alternative matches the declared type.

## 2026-06-20 task 4 scanner paths and LRC

- `generic_u8string()` returns `std::u8string` under C++23/libstdc++, so scanner relative UTF-8 serialization explicitly copies `char8_t` bytes into `std::string` for public contract storage.
- `std::filesystem::symlink_status` may report a vanished path through `std::error_code` instead of a non-existing status object; path classification maps that case to a recoverable `RootUnavailable` missing-path record.
- LRC metadata tags are restricted to alphabetic keys such as `[ar:]`/`[ti:]`; malformed numeric timestamp tags like `[00:61.00]` remain recoverable `InvalidTimestamp` errors instead of being ignored as metadata.
- Verified Task 4 with `cmake --build build --target seriona_scanner_paths_tests` and `ctest --test-dir build -R seriona.scanner_paths --output-on-failure`.

## 2026-06-20 task 5 scanner hash

- `hashFileContent()` streams file bytes through `XXH3_128bits` and encodes `XXH128_canonical_t` as 32 lowercase hex characters, so hashes are portable across host endianness.
- Directory Merkle hashing sorts children by relative UTF-8 path and includes child relative name, type, size, normalized mtime count, and child hash; tests fix mtimes when comparing two separately-created trees.
- External `.lrc` hashing reuses the same file-content utility as audio hashing, so lyric file changes alter only the sidecar hash while the paired audio hash stays unchanged.
- Verified Task 5 with `cmake --build build --target seriona_scanner_hash_tests` and `ctest --test-dir build -R seriona.scanner_hash --output-on-failure`.

## 2026-06-20 task 6 scanner tree

- `PlaylistTreeBuilder` publishes value-semantic `PlaylistTreeSnapshot` objects with stable node IDs instead of shared ownership links, so parent access is by ID and cannot form shared_ptr cycles.
- Tree tests verify directory-first child ordering, empty-directory pruning, root aggregate song count/duration, external `.lrc` metadata attached to the paired song, and no `.lrc` public nodes.
- Snapshot immutability is covered by mutating the builder after the first publish and asserting the prior snapshot's child list stays unchanged while the next version increments.
- Verified Task 6 with `cmake --build build --target seriona_scanner_tree_tests` and `ctest --test-dir build -R seriona.scanner_tree --output-on-failure`.

## 2026-06-20 task 7 scanner scheduler

- `ScanScheduler` is scanner-local infrastructure: bounded submission uses condition-variable waits for backpressure, `trySubmit()` reports full queues predictably, and cancellation drains queued tasks as cancelled results.
- Running tasks receive an atomic stop token; exceptions are captured as failed `ScanTaskResult` entries and do not prevent later task fan-in.
- `ProgressThrottle` publishes the first progress event, then gates by completed-count delta or elapsed interval so future scanner orchestration can avoid event spam deterministically.
- Verified Task 7 with `cmake --build build --target seriona_scanner_scheduler_tests` and `ctest --test-dir build -R seriona.scanner_scheduler --output-on-failure`.

## 2026-06-21 task 8 sqlite scanner cache

- `SQLiteScannerCache` persists root/directories/songs/errors plus embedded and external lyric rows, and reloads `LyricsSource` to reconstruct effective lyrics without TagReader.
- The cache keeps user stats (`playCount`, `rating`, `lastPlayed`) on refresh by reading the existing row before upserting refreshed metadata.
- WAL/busy-timeout behavior is exercised by a held writer transaction on a second connection, which reliably throws on conflicting writes.
- Verified Task 8 with `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build`, and `ctest --test-dir build -R 'seriona.scanner_cache|seriona.scanner' --output-on-failure`.

## 2026-06-21 task 9 tagreader adapter

- `TagReaderMetadataReader` only calls `TagReader::Read(path, coverExportDir)` and converts the returned `MusicTag` into scanner-internal raw metadata.
- Raw TagReader embedded lyrics are mapped into scanner embedded lyrics with millisecond conversion, while cached user stats override imported play stats on refresh.
- Per-file TagReader exceptions are captured into `ScannerError` entries so batch reads continue after a failure.
- Verified Task 9 with `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build`, and `ctest --test-dir build -R 'seriona.scanner_tagreader|seriona.scanner' --output-on-failure`.

## 2026-06-21 task 10 scanner service orchestration

- `scanner_contract_tests` can no longer compile `file_scanner_service.cpp` directly once the default service owns cache/hash/tree/tagreader dependencies; it links `seriona_scanner` instead.
- Hash-first service tests use fake TagReader read counts as the observable cache-hit proof: unchanged scans and `.lrc`-only changes keep the count stable, while audio byte changes increment it once.
- Deleted `.lrc` reconciliation must clear external lyric rows and reselect effective lyrics from cached embedded lyrics or `None` without calling TagReader.
- A stale FetchContent generator cache under `build/_deps/catch2-*` can block reconfigure even when the top-level `build` directory is valid; deleting only those generated subbuild dirs was sufficient.
- Forward fix: scanner service publication must pass root-relative paths into `PlaylistTreeBuilder`; passing only `filename()` flattens nested directory roots and hides directory nodes.
- Verification commands that build/link test executables and run CTest must run sequentially; parallel build + CTest can report a misleading "executable not found" while Ninja is still linking the target.


## 2026-06-21 task 11 scanner watcher runtime

- `wtr::watch` exposes an RAII `watch` class whose constructor starts async watching and whose `close()` joins/stops the watcher; production scanner wraps this behind a private `FolderWatcher` seam to keep public scanner headers watcher-free.
- Normal `s/self/live` and `s/self/die` watcher messages should not be treated as scanner errors; only `e/*`, `w_*`, warning/error, or overflow-like watcher messages force a root reconciliation error event.
- Fake watcher tests can prove `.lrc`-only runtime changes stay TagReader-free by checking fake metadata reader counts before and after watcher callbacks.
- `stopWatching()` tests must not keep raw pointers to watcher objects after service ownership releases them; store shared fake state instead to verify close and late-callback behavior safely.

## 2026-06-21 task 12 integration hardening

- `tests/CMakeLists.txt` already registers every scanner CTest under names beginning with `seriona.scanner`, so `ctest -R 'seriona.scanner'` is the complete scanner grouping without adding an aggregate target.
- App integration only needs `seriona` to link `seriona_scanner`; `app/main.cpp` remains audio-file playback only, preserving current CLI behavior while proving scanner is in the default build graph.
- Scanner docs belong in a focused `docs/file-scanner.md` rather than `docs/audio-player.md`, because the domains and runtime constraints are separate.
- No optional real-watcher smoke was added in task 12; default fake-watcher coverage plus full CTest keeps verification hardware/media independent.
