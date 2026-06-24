# Task 8 Evidence - scanner cache policy and adapter boundary

## Scope

- Modified only Task 8 allowed source/test files plus this evidence file and the task notepad append.
- Left `scanner_contracts.h` and `file_scanner_service.h` unchanged.
- `src/scanner/file_scanner_orchestrator.cpp` was changed only to adapt the now cache-neutral TagReader adapter result back into internal cache DTOs.

## Failing-first regressions

1. Added cache policy regressions in `tests/scanner/scanner_cache_tests.cpp`:
   - `sqlite scanner cache reports capacity and checkpoint policy decisions without large fixtures`
   - `sqlite scanner cache maintenance checkpoints and prunes roots by policy`
2. Added boundary regression in `tests/scanner/scanner_contract_tests.cpp`:
   - `tag reader adapter public boundary does not expose sqlite cache dto types`

Initial LSP diagnostics immediately after test patch showed the expected red state:

```text
tests/scanner/scanner_cache_tests.cpp:
ERROR [72:91] Unknown type name 'CacheMaintenancePolicy'
ERROR [216:51] Use of undeclared identifier 'CacheMaintenancePolicy'
ERROR [234:44] Use of undeclared identifier 'CacheMaintenancePolicy'

tests/scanner/scanner_contract_tests.cpp:
ERROR [221:17] Static assertion failed due to requirement '!requires (seriona::scanner::MappedTagMetadata mapped) { mapped.cachedSong; }'
ERROR [222:60] No member named 'metadata' in 'seriona::scanner::MappedTagMetadata'
ERROR [223:41] No member named 'embeddedLyrics' in 'seriona::scanner::MappedTagMetadata'
```

The first target build after implementation found one test-expression issue, then rebuild passed after converting the negative member check to a template concept:

```text
$ cmake --build build --target seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests
FAILED: tests/CMakeFiles/seriona_scanner_contract_tests.dir/scanner/scanner_contract_tests.cpp.o
/home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/tests/scanner/scanner_contract_tests.cpp:221:62: error: ‘struct seriona::scanner::MappedTagMetadata’ has no member named ‘cachedSong’
```

## Implementation notes

- `inc/seriona/scanner/cache/sqlite_scanner_cache.h` now exposes `CacheMaintenancePolicy`, `CacheMaintenanceDecision`, and `CacheMaintenanceResult` plus `maintenanceDecision()` / `maintainCache()` hooks.
- `src/scanner/cache/sqlite_scanner_cache.cpp` computes actual database/WAL sizes, root count, checkpoint recommendation, cleanup recommendation, hard-limit vacuum recommendation, passive checkpoint execution, and oldest-root pruning.
- `inc/seriona/scanner/tag_reader_metadata_adapter.h` now includes only `scanner_contracts.h` and returns cache-neutral `MappedTagMetadata` fields (`SongMetadata`, lyrics vectors, `TagUserStats`).
- `src/scanner/file_scanner_orchestrator.cpp` converts `MappedTagMetadata` to `cache::CachedSong` internally, keeping cache DTOs inside scanner implementation/cache paths.

## Verification

```text
$ cmake --build build --target seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests
[1/5] Linking CXX static library libseriona_scanner.a
[2/5] Linking CXX executable tests/seriona_scanner_cache_tests
[3/5] Linking CXX executable tests/seriona_scanner_tagreader_tests
[4/5] Building CXX object tests/CMakeFiles/seriona_scanner_contract_tests.dir/scanner/scanner_contract_tests.cpp.o
[5/5] Linking CXX executable tests/seriona_scanner_contract_tests
```

```text
$ ctest --test-dir build -R 'seriona\.(scanner_cache|scanner_tagreader|scanner_contract)' --output-on-failure
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 27: seriona.scanner_contract
1/3 Test #27: seriona.scanner_contract .........   Passed    0.03 sec
    Start 40: seriona.scanner_cache
2/3 Test #40: seriona.scanner_cache ............   Passed    0.04 sec
    Start 41: seriona.scanner_tagreader
3/3 Test #41: seriona.scanner_tagreader ........   Passed    0.04 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.12 sec
```

Exact requested combined command:

```text
$ cmake --build build --target seriona_scanner_cache_tests seriona_scanner_tagreader_tests seriona_scanner_contract_tests && ctest --test-dir build -R 'seriona\.(scanner_cache|scanner_tagreader|scanner_contract)' --output-on-failure
ninja: no work to do.
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 27: seriona.scanner_contract
1/3 Test #27: seriona.scanner_contract .........   Passed    0.04 sec
    Start 40: seriona.scanner_cache
2/3 Test #40: seriona.scanner_cache ............   Passed    0.04 sec
    Start 41: seriona.scanner_tagreader
3/3 Test #41: seriona.scanner_tagreader ........   Passed    0.04 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.12 sec
```

## Boundary checks

```text
$ grep equivalent: inc/seriona/scanner/tag_reader_metadata_adapter.h for sqlite_scanner_cache|cache::|CachedSong|CachedUserStats
No matches found
```

`inc/seriona/scanner` still contains cache DTOs only under `inc/seriona/scanner/cache/sqlite_scanner_cache.h`; stable `scanner_contracts.h` and `file_scanner_service.h` were not modified.

## LSP diagnostics

- Dedicated `lsp_diagnostics` tool was not available in the current tool list.
- `apply_patch` emitted inline LSP diagnostics during the red phase and after one stale-index pass; final CMake build was used as the authoritative compiler check.

## Status

- Targeted build: PASS
- Targeted CTest: PASS
- Public-boundary leakage regression: PASS
