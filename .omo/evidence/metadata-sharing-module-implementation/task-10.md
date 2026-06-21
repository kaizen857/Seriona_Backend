# Task 10 Evidence

- Added Linux MPRIS command handlers in `src/metadata/metadata_mpris_linux.cpp` so `Play`, `Pause`, `PlayPause`, `Stop`, `Next`, `Previous`, `Seek`, `SetPosition`, `SetVolume`, `SetRepeatMode`, and `SetShuffle` all translate into `MediaControlCommand` values.
- Kept command translation value-semantic and asynchronous: the adapter only registers callbacks and emits command objects through `MediaControlCommandSink`; it does not call audio or scanner services.
- Preserved stale-track rejection for `SetPosition` and added negative seek rejection plus capability gating for command dispatch.
- Expanded `tests/metadata/metadata_mpris_tests.cpp` with fake-bus/fake-object coverage for every translated command path and the disabled-capability negative path.
- Verified: `cmake --build build --target seriona_metadata_mpris_tests` passed, and `ctest --test-dir build -R seriona.metadata_mpris --output-on-failure` passed.
