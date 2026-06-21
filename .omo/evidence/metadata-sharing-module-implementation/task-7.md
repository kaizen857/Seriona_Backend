# Task 7 Evidence

- Implemented explicit Noop lifecycle support in `src/metadata/metadata_service_backend.cpp` with idempotent `start()`/`stop()` and rejected `update()` after stop.
- Added recording-oriented test hooks in `src/metadata/metadata_service_testing.h` / `src/metadata/metadata_service_testing.cpp` and wired them into `tests/metadata/metadata_service_tests.cpp`.
- Covered explicit Noop selection, Windows-without-host capability degradation, idempotent start/stop, update-after-stop rejection, command callback registration/unregistration, backend start failure reporting, and recording backend structured metadata/timeline evidence via `MetadataServiceRecord` entries.
- Kept Linux `sdbus-c++` failure as configure-time only; no live D-Bus or SMTC dependency is required by the tests.
- Verified with `cmake --build build --target seriona_metadata_service_tests seriona_metadata_service_recording_tests`.
- Verified with `ctest --test-dir build -R 'seriona.metadata_service|seriona.metadata_service_recording' --output-on-failure`.
- Pure LOC check: `src/metadata/metadata_service_backend.cpp` 139, `src/metadata/metadata_service_recording.cpp` 115, `tests/metadata/metadata_service_tests.cpp` 248, `tests/metadata/metadata_service_recording_tests.cpp` 43.
