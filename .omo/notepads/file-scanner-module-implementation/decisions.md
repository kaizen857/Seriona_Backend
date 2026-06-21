## 2026-06-20 task 1 scanner foundation

- Use root `CMAKE_CXX_STANDARD 23` for the whole project per approved plan.
- Use `find_package(SQLite3 REQUIRED)` for the SQLite3 scanner gate and `pkg_check_modules(SERIONA_XXHASH REQUIRED IMPORTED_TARGET libxxhash)` for the required `PkgConfig::SERIONA_XXHASH` target.
- Keep `TagReaderCore` private to `seriona_scanner`; no TagReader include directories or types are exposed through scanner public headers.
- Vendor `wtr/watcher` as the upstream `e-dant/watcher` release branch single header at `third_party/watcher/include/wtr/watcher.hpp`, with `SOURCE.md` and MIT license note in `third_party/watcher/`.
- Clear tests registered by the external TagReader child directory so Seriona top-level `ctest` remains scoped to Seriona tests while still allowing `TagReaderCore` to build for scanner linkage.

## 2026-06-20 task 2 public scanner contracts

- Define scanner API in `inc/seriona/scanner/scanner_contracts.h` and expose the facade in `inc/seriona/scanner/file_scanner_service.h`, mirroring the audio contract/facade split while keeping scanner dependencies standard-library-only.
- Provide `FileScanner` as the public facade name and keep `FileScannerService` as the pure virtual service semantic name; `makeFileScannerService` returns `std::shared_ptr<FileScannerService>` to match the injected-facade ownership style.
- Keep the default factory implementation as a private no-op service stub for linkability only until later scanner implementation tasks add real scanning/cache/watcher behavior.

## 2026-06-20 task 3 scanner test harness

- Keep scanner test fakes and fixtures under `tests/scanner/` as a private test harness instead of exposing new production scanner seams before scanner logic exists.
- Model fake TagReader as a test adapter with success, throw, and block/release behavior so future cancellation tests can deterministically control slow metadata reads.
- Model fake watcher events with explicit audio versus `.lrc` path kind plus optional old path for rename events, preserving external lyric association cases without a real filesystem watcher.

## 2026-06-21 task 10 scanner service orchestration

- Keep the public scanner facade unchanged and add only a private `FileScannerServiceDependencies` construction seam for service tests and production factory wiring.
- Implement task-10 scan orchestration synchronously over existing scanner primitives; watcher debounce/runtime incremental updates remain task 11.
- Treat TagReader failure for a new audio file as an error-only outcome, not a placeholder song; if old cached metadata exists, preserve that cached song while recording the error.
- Use SQLite `saveRoot` as the single-writer reconciliation boundary after filesystem discovery/hash/metadata/LRC decisions are made in memory.
- Forward fix: carry `treeRelativePath` beside each reconciled cached song so cache identity remains absolute-path based while playlist publication remains root-relative and hierarchical.


## 2026-06-21 task 11 scanner watcher runtime

- Add `startWatching`/`stopWatching` to the scanner service/facade contract while keeping all watcher implementation types private to `src/scanner`.
- Treat watcher payloads only as dirty hints: debounce events and call the existing hash-first service reconciliation for watched roots, rather than applying create/delete/rename data directly.
- Use root-level reconciliation for warning/error/overflow and ambiguous events in task 11; this favors correctness and preserves task-10 cache semantics over path-scoped optimization.
- Inject `FolderWatcherFactory` only through the private `FileScannerServiceDependencies` test seam; production uses `WtrFolderWatcherFactory` automatically.
