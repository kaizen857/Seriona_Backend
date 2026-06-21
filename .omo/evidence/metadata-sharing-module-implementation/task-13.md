# Task 13 Evidence

- Revalidated on current `HEAD` after `c8ffe67 fix(metadata): record validated metadata cleanup`.
- `git log --oneline --grep metadata -10` shows the current metadata chain ending at `c8ffe67`, with the earlier feature commits still intact.
- `git status --short` at verification time showed unrelated `.omo/` workspace files, but no product code changes were made for this evidence refresh.
- `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` — passed.
- Configure note: CMake still prints the existing `FetchContent.cmake` `CMP0135` / `DOWNLOAD_EXTRACT_TIMESTAMP` developer warning, but configuration completes successfully.
- `cmake --build build` — passed on the current verification run.
- `ctest --test-dir build -R seriona.metadata --output-on-failure` — passed (`6/6` tests).
- `ctest --test-dir build -R seriona.metadata_mpris_smoke --output-on-failure` — passed (`1/1` test).
- `ctest --test-dir build --output-on-failure` — passed (`41/41` tests).
- Historical note: the earlier full-verify blocker was the missing `src/metadata/metadata_mpris_linux.cpp` link in `seriona_metadata`; that issue is already fixed, and this refresh confirms the final-state evidence on current `HEAD`.
- Result: Linux metadata sharing evidence now reflects the post-`c8ffe67` repository state and closes the stale task-13 gap.
