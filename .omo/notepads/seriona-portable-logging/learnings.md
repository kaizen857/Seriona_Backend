# Seriona Portable Logging - Runtime Paths

## Task 2: Portable Runtime Path Resolution

**Status**: Complete
**Date**: 2026-06-24

## Files Created
- `inc/seriona/app/runtime_paths.h` — Public header with `RuntimePaths` struct and `resolveRuntimePaths()`
- `src/app/runtime_paths.cpp` — Implementation using `/proc/self/exe` on Linux
- `tests/app/runtime_paths_tests.cpp` — 4 test cases (all passing)

## Files Modified
- `app/CMakeLists.txt` — Added `runtime_paths.cpp` to seriona executable sources
- `tests/CMakeLists.txt` — Registered `seriona_runtime_paths_tests` target and CTest entry

## Design Decisions

### Priority order for executable directory resolution
1. If `executablePath` is absolute → use `parent_path()` (enables testing)
2. On Linux: `readlink("/proc/self/exe")` → `parent_path()`
3. Fallback: `std::filesystem::current_path()`

### Data root naming
- Data root: `<exe_dir>/SerionaData/` (no hyphen, no internal module names)
- Log: `SerionaData/logs/seriona.log`
- Database: `SerionaData/library.sqlite`
- Artwork: `SerionaData/artwork/`

### Namespace
- Uses `seriona::app` (matches existing `terminal_controller.h`)
- Struct `RuntimePaths` with aggregate initialization (no class hierarchy)

### Test results
- 4 test cases, all passing
- No regressions in existing 43 tests
- Pre-existing flaky test: `seriona.control_controller` (unrelated timing issue)
