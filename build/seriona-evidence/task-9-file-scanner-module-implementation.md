# task 9 evidence - tagreader adapter and metadata mapping

## Commands
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`
- `cmake --build build`
- `ctest --test-dir build -R 'seriona.scanner_tagreader|seriona.scanner' --output-on-failure`

## Result
- Configure: pass
- Build: pass
- CTest: pass, 8/8 tests passed including `seriona.scanner_tagreader`

## Observables
- `TagReader::Read(path, coverExportDir)` is the only production read path used by the adapter.
- Raw TagReader metadata maps into scanner metadata with embedded lyrics, technical fields, and cached user-stat preservation.
- External `.lrc` override mode keeps effective lyrics empty while preserving cached embedded lyrics for later reuse.
- Fake reader failures are captured per file and do not stop later paths from being processed.

## Adversarial classes
- `stale_state`: probed by refresh/preserved-stats mapping; passed.
- `dirty_worktree`: not applicable to runtime behavior; verify by staging only task-9 files before commit.
- `misleading_success_output`: not applicable; the adapter tests were actually built and run.
- `long_commands`: probed; configure/build/CTest completed within bounded time.
- `interruptions`: probed by resuming the interrupted session from boulder state; passed.

## Cleanup
- Temporary TagReader fake fixtures and build artifacts were created under the build/test directories and removed with process exit.
