# Task 8 Evidence

- Implemented `src/metadata/metadata_mpris_linux.cpp` as the Linux backend entry behind private `sdbus-c++` usage.
- Added `src/metadata/metadata_mpris_private.h` for the fake-bus seam used by deterministic tests.
- Replaced the smoke-only `tests/metadata/metadata_mpris_tests.cpp` with fake-bus coverage for object model, object path, `file://` artwork, `SetPosition` rejection, and `CanControl` gating.
- Wired `seriona_metadata_mpris_tests` in `tests/CMakeLists.txt` and kept `seriona.metadata_mpris_smoke` as a fixed CTest entry.
- Switched Linux service factory selection in `src/metadata/metadata_service_backend.cpp` to the real metadata backend.
- Verification: `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build --target seriona_metadata_mpris_tests`, and `ctest --test-dir build -R 'seriona.metadata_mpris|seriona.metadata_mpris_smoke' --output-on-failure` passed.
