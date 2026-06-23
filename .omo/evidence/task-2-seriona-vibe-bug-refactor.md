# Task 2 Evidence - seriona-vibe-bug-refactor

## Scope
- Task redo: `2. Phase 2: make AudioEventDispatcher sink thread-safe and lock-outside-call`
- Edited only allowed task files:
  - `inc/seriona/audio/events/audio_event_dispatcher.h`
  - `src/audio/events/audio_event_dispatcher.cpp`
  - `src/audio/audio_playback_service.cpp`
  - `tests/audio/audio_event_dispatcher_tests.cpp`
  - `tests/audio/audio_event_dispatcher_shutdown_tests.cpp`
  - `tests/audio/audio_shutdown_lifecycle_tests.cpp`
  - `.omo/evidence/task-2-seriona-vibe-bug-refactor.md`
  - `.omo/notepads/seriona-vibe-bug-refactor/learnings.md`

## Current-state inspection
- `codegraph_explore` inspected current dispatcher, service destructor, and targeted tests before editing.
- Existing partial work already had mutex-protected dispatcher state, sink copies invoked outside the lock, progress worker shutdown before sink clearing, and regression tests.
- Evidence was stale from the interrupted pass, so this redo re-established a fresh red/green proof from the current working tree.

## Failing-first proof
- To prove the existing tests still catch the bug, I temporarily restored the old production behavior in allowed production files only:
  - removed dispatcher synchronization around `sink_` and `eventVersion_`
  - invoked `sink_` directly from `dispatch()`
  - moved service destruction back to clearing sinks before `stopProgressWorker()`
- No test code was weakened for this red run.

Command:
```sh
cmake --build build --target seriona_audio_event_dispatcher_tests seriona_audio_event_dispatcher_shutdown_tests seriona_audio_shutdown_lifecycle_tests
```

Result before final implementation:
```text
[1/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_shutdown_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[2/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[3/9] Building CXX object tests/CMakeFiles/seriona_audio_shutdown_lifecycle_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[4/9] Building CXX object tests/CMakeFiles/seriona_audio_shutdown_lifecycle_tests.dir/__/src/audio/audio_playback_service.cpp.o
[5/9] Linking CXX executable tests/seriona_audio_shutdown_lifecycle_tests
[6/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_shutdown_tests.dir/audio/audio_event_dispatcher_shutdown_tests.cpp.o
[7/9] Linking CXX executable tests/seriona_audio_event_dispatcher_shutdown_tests
[8/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_tests.dir/audio/audio_event_dispatcher_tests.cpp.o
[9/9] Linking CXX executable tests/seriona_audio_event_dispatcher_tests
```

Command:
```sh
ctest --test-dir build -R 'seriona\.(audio_event_dispatcher|audio_event_dispatcher_shutdown|audio_shutdown_lifecycle)' --output-on-failure
```

Result before final implementation:
```text
Test #16: seriona.audio_event_dispatcher_shutdown ...Subprocess aborted
terminate called after throwing an instance of 'std::bad_function_call'
  what():  bad_function_call
TEST CASE: audio_event_dispatcher supports concurrent set clear and dispatch
FATAL ERROR: test case CRASHED: SIGABRT

33% tests passed, 2 tests failed out of 3
The following tests FAILED:
  15 - seriona.audio_event_dispatcher (Failed)
  16 - seriona.audio_event_dispatcher_shutdown (Subprocess aborted)
```

## Implementation notes
- `AudioEventDispatcher` protects both `sink_` and `eventVersion_` with one mutex.
- `dispatch()` copies the sink and prepares the versioned event while synchronized, then releases the mutex before invoking the callback.
- `nextVersion()` and `hasEventSink()` read the synchronized dispatcher state under the same mutex.
- `src/audio/audio_playback_service.cpp` is limited to lifecycle ordering: `~SingleTrackAudioPlaybackService()` stops the progress worker before clearing dispatcher and state-machine sinks.
- `src/audio/events/audio_event_dispatcher.cpp` directly includes `<mutex>` so the implementation owns its `std::lock_guard` dependency.

## Final verification
Command:
```sh
cmake --build build --target seriona_audio_event_dispatcher_tests seriona_audio_event_dispatcher_shutdown_tests seriona_audio_shutdown_lifecycle_tests
```

Result:
```text
[1/9] Building CXX object tests/CMakeFiles/seriona_audio_shutdown_lifecycle_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[2/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_shutdown_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[3/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_tests.dir/__/src/audio/events/audio_event_dispatcher.cpp.o
[4/9] Building CXX object tests/CMakeFiles/seriona_audio_shutdown_lifecycle_tests.dir/__/src/audio/audio_playback_service.cpp.o
[5/9] Linking CXX executable tests/seriona_audio_shutdown_lifecycle_tests
[6/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_shutdown_tests.dir/audio/audio_event_dispatcher_shutdown_tests.cpp.o
[7/9] Linking CXX executable tests/seriona_audio_event_dispatcher_shutdown_tests
[8/9] Building CXX object tests/CMakeFiles/seriona_audio_event_dispatcher_tests.dir/audio/audio_event_dispatcher_tests.cpp.o
[9/9] Linking CXX executable tests/seriona_audio_event_dispatcher_tests
```

Command:
```sh
ctest --test-dir build -R 'seriona\.(audio_event_dispatcher|audio_event_dispatcher_shutdown|audio_shutdown_lifecycle)' --output-on-failure
```

Result:
```text
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 15: seriona.audio_event_dispatcher
1/3 Test #15: seriona.audio_event_dispatcher ............   Passed    0.02 sec
    Start 16: seriona.audio_event_dispatcher_shutdown
2/3 Test #16: seriona.audio_event_dispatcher_shutdown ...   Passed    0.00 sec
    Start 25: seriona.audio_shutdown_lifecycle
3/3 Test #25: seriona.audio_shutdown_lifecycle ..........   Passed    0.20 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.22 sec
```

## Diagnostics
- Dedicated `lsp_diagnostics` tool was not available in this toolset.
- `apply_patch` diagnostics caught the missing direct `<mutex>` include in `src/audio/events/audio_event_dispatcher.cpp`; adding the include cleared that reported issue before final build/test.
