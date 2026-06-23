# Task 1 Evidence - audio callback queue lifetime generation-safe

## Scope
- Modified only allowed source/test files for task 1 plus this evidence file and task notepad.
- Product files changed: `inc/seriona/audio/buffer/pcm_buffer_queue.h`, `src/audio/buffer/pcm_buffer_queue.cpp`, `inc/seriona/audio/device/audio_output_device.h`, `src/audio/device/audio_output_device.cpp`.
- Test files changed: `tests/audio/pcm_buffer_queue_tests.cpp`, `tests/audio/audio_output_device_tests.cpp`.

## Failing-first regression
- Added `pcm_buffer_queue read discards pcm when seek generation changes during read` before production changes.
- Added `audio_output_device callback after stop observes inactive generation and fills silence` before production changes.
- Added `audio_output_device callback after uninitialize has no queue lifetime dependency` before production changes.

Command:
```text
cmake --build build --target seriona_pcm_buffer_queue_tests seriona_audio_output_device_tests
```

Result before implementation: failed as expected.
```text
tests/audio/pcm_buffer_queue_tests.cpp:131:33: error: 'class seriona::audio::PcmBufferQueue' has no member named 'generation'
tests/audio/pcm_buffer_queue_tests.cpp:134:29: error: 'class seriona::audio::PcmBufferQueue' has no member named 'readIfGeneration'
ninja: build stopped: subcommand failed.
```

## Implementation summary
- `PcmBufferQueue` now exposes a monotonic `PcmBufferQueueGeneration` and `readIfGeneration()`.
- `clearForSeek()` advances generation before clearing offsets/used bytes, so stale callback reads can be detected.
- `readIfGeneration()` rechecks generation around byte reservation and returns silence if a seek/reset crosses the callback read.
- `AudioOutputDevice` now publishes callback queue state through atomics (`AudioOutputDeviceCallbackState`) and deactivates it on `stop()`/`uninitialize()`.
- `renderCallback()` only reads atomic/shared callback state, reads PCM or fills silence, applies gain/mute, and updates counters; it does not call backend lifecycle, logging, FFmpeg, sink dispatch, heap allocation, or locks.

## Required command result
Command:
```text
cmake --build build --target seriona_pcm_buffer_queue_tests seriona_audio_output_device_tests
```

Result after implementation:
```text
[1/4] Building CXX object tests/CMakeFiles/seriona_pcm_buffer_queue_tests.dir/__/src/audio/buffer/pcm_buffer_queue.cpp.o
[2/4] Building CXX object tests/CMakeFiles/seriona_audio_output_device_tests.dir/__/src/audio/buffer/pcm_buffer_queue.cpp.o
[3/4] Linking CXX executable tests/seriona_pcm_buffer_queue_tests
[4/4] Linking CXX executable tests/seriona_audio_output_device_tests
```

Command:
```text
ctest --test-dir build -R 'seriona_(pcm_buffer_queue|audio_output_device)' --output-on-failure
```

Result: no tests matched because the existing CTest names use dots (`seriona.pcm_buffer_queue`, `seriona.audio_output_device`) rather than underscores.
```text
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
No tests were found!!!
```

Equivalent matching command:
```text
ctest --test-dir build -R 'seriona\.(pcm_buffer_queue|audio_output_device)' --output-on-failure
```

Result:
```text
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
    Start 10: seriona.pcm_buffer_queue
1/2 Test #10: seriona.pcm_buffer_queue .........   Passed    0.00 sec
    Start 11: seriona.audio_output_device
2/2 Test #11: seriona.audio_output_device ......   Passed    0.00 sec

100% tests passed, 0 tests failed out of 2

Total Test time (real) =   0.01 sec
```

## Additional checks
Command:
```text
git diff --check -- inc/seriona/audio/buffer/pcm_buffer_queue.h src/audio/buffer/pcm_buffer_queue.cpp inc/seriona/audio/device/audio_output_device.h src/audio/device/audio_output_device.cpp tests/audio/pcm_buffer_queue_tests.cpp tests/audio/audio_output_device_tests.cpp
```

Result: no output, no whitespace errors.

LSP diagnostics: attempted for all changed C++ files, but the MCP connection returned `Connection closed`. The targeted CMake build above completed with compiler warnings enabled for both affected test targets.

## Notes
- During verification, an intermediate implementation failed `seriona.pcm_buffer_queue` because normal `read()` did not decrement `usedBytes_` after introducing reserved reads. Fixed before final verification.
- `src/audio/device/audio_output_device.cpp` is 298 physical lines after this task and should be considered for a future responsibility split if further edits expand the file.
