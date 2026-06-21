# Task 6 Evidence

- Implemented `src/metadata/metadata_synchronizer.h` and `src/metadata/metadata_synchronizer.cpp` to split static metadata dirty detection from timeline cadence detection.
- Wired `seriona_metadata` to build the synchronizer via `CMakeLists.txt` and exposed it through `metadataServiceSynchronize(...)` in `src/metadata/metadata_service.cpp`.
- Added failing-first service tests covering 500ms playback ticks, 1000ms timeline cadence, metadata-only changes, immediate timeline boundary updates, and stale snapshot rejection.
- Verified with `cmake --build build --target seriona_metadata_service_tests`.
- Verified with `ctest --test-dir build -R seriona.metadata_service --output-on-failure`.
- Notes: `version` / `sampledAt` are used only for freshness gating; static metadata updates do not resend title/artwork/capabilities on cadence ticks.
