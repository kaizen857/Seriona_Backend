# Task 14 Evidence

- Fixed the lifetime bug in metadata command unregistration by removing raw-`this` captures from `src/metadata/metadata_service_backend.cpp`, `src/metadata/metadata_service_recording.cpp`, and `src/metadata/metadata_mpris_linux.cpp`.
- Moved command-sink cleanup onto shared state so `SubscriptionHandle::unsubscribe` stays safe and idempotent even after the backend/service object is destroyed.
- Added regression coverage in `tests/metadata/metadata_service_recording_tests.cpp` and `tests/metadata/metadata_mpris_tests.cpp` that exercises unsubscribe after backend destruction and calls it twice.
- Verified with `cmake --build build --target seriona_metadata_service_tests seriona_metadata_service_recording_tests seriona_metadata_mpris_tests`.
- Verified with `ctest --test-dir build -R 'seriona.metadata_(service|service_recording|mpris)' --output-on-failure`.
