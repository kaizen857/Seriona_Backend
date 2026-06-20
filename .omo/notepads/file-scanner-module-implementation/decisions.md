## 2026-06-20 task 1 scanner foundation

- Use root `CMAKE_CXX_STANDARD 23` for the whole project per approved plan.
- Use `find_package(SQLite3 REQUIRED)` for the SQLite3 scanner gate and `pkg_check_modules(SERIONA_XXHASH REQUIRED IMPORTED_TARGET libxxhash)` for the required `PkgConfig::SERIONA_XXHASH` target.
- Keep `TagReaderCore` private to `seriona_scanner`; no TagReader include directories or types are exposed through scanner public headers.
- Vendor `wtr/watcher` as the upstream `e-dant/watcher` release branch single header at `third_party/watcher/include/wtr/watcher.hpp`, with `SOURCE.md` and MIT license note in `third_party/watcher/`.
- Clear tests registered by the external TagReader child directory so Seriona top-level `ctest` remains scoped to Seriona tests while still allowing `TagReaderCore` to build for scanner linkage.
