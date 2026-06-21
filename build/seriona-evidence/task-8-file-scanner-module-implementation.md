# task 8 evidence - sqlite scanner cache

## Commands
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`
- `cmake --build build`
- `ctest --test-dir build -R 'seriona.scanner_cache|seriona.scanner' --output-on-failure`

## Result
- Configure: pass
- Build: pass
- CTest: pass, 7/7 tests passed including `seriona.scanner_cache`

## Observables
- SQLite cache round-trips full metadata, embedded lyrics, external lyrics, directory rows, and errors.
- External lyrics override/clear path preserves embedded lyrics across refresh.
- User stats survive refresh after explicit update.
- Prune and passive checkpoint behave deterministically.
- Busy writer conflict surfaces as `std::runtime_error` through a second connection.

## Adversarial classes
- `stale_state`: probed by reload-after-refresh and prune-after-save; passed.
- `dirty_worktree`: not applicable to runtime behavior; verified separately by staging only task-8 files before commit.
- `misleading_success_output`: not applicable; commands were executed for real and CTest output showed passing cases.
- `long_commands`: probed; configure/build/CTest completed within bounded time.
- `interruptions`: probed by resuming the interrupted session from boulder state; passed.

## Cleanup
- Temporary DBs and fixtures were created under the scanner test harness temp directories and removed with process exit.
