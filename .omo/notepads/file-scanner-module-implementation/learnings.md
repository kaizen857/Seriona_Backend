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
