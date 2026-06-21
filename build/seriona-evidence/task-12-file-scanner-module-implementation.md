# Task 12 file-scanner-module-implementation evidence

## Scope

- Task: `12. Integration hardening, optional smoke, docs, and full verification`.
- Integration: app target links `seriona_scanner`; `app/main.cpp` runtime behavior remains unchanged.
- Scanner CTest grouping: existing CTest names use `seriona.scanner*` and are exercised with `ctest -R 'seriona.scanner'`.
- Optional real watcher smoke: not added; default verification remains fake-watcher and hardware/media independent.

## Verification log

## Manual QA artifact

- This file is the manual-QA artifact for task 12. It contains the complete configure/build/scanner-CTest/full-CTest outputs captured from the working tree after app scanner link integration.
- Optional real watcher smoke was not added. Default verification remains fake-watcher based and does not require real watcher support, user media folders, or audio hardware.

## Adversarial probes

- Stale state: forced `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` before build; configure regenerated build files successfully.
- Dirty worktree: commit preparation uses `GIT_MASTER=1 git status`, full diff inspection, and path-specific staging to avoid unrelated dirty files.
- Hung/long commands: configure/build/CTest were run sequentially with bounded shell timeouts; no command hung.
- Flaky tests: scanner CTest and full CTest each passed once in sequence after the relink; no retry was needed.
- Misleading success output: each logged command records its explicit shell exit code after the tool output.
- Repeated interruptions: verification was split into independent commands, so each phase has a durable evidence section.
- Malformed input / prompt injection / cancel-resume: not directly applicable to task 12 because no parser, prompt surface, or cancellation runtime behavior changed in this integration-only step; those paths remain covered by existing scanner tests.

## Cleanup receipts

- No package manager, formatter setup, linter setup, or unrelated product fix was run.
- No `.omo/run-continuation` file, root `.omo/boulder.json`, or unrelated task file is intentionally staged for task 12.
- App runtime behavior was preserved by changing only link libraries, not `app/main.cpp`.

### cmake configure
```text
CMake Warning (dev) at /usr/share/cmake/Modules/FetchContent.cmake:1386 (message):
  The DOWNLOAD_EXTRACT_TIMESTAMP option was not given and policy CMP0135 is
  not set.  The policy's OLD behavior will be used.  When using a URL
  download, the timestamps of extracted files should preferably be that of
  the time of extraction, otherwise code that depends on the extracted
  contents might not be rebuilt if the URL changes.  The OLD behavior
  preserves the timestamps from the archive instead, but this is usually not
  what you want.  Update your project to the NEW behavior or specify the
  DOWNLOAD_EXTRACT_TIMESTAMP option with a value of true to avoid this
  robustness issue.
Call Stack (most recent call first):
  /home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:63 (FetchContent_Declare)
This warning is for project developers.  Use -Wno-dev to suppress it.

-- Configuring done (0.1s)
-- Generating done (0.1s)
-- Build files have been written to: /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
```
configure_exit=0

### cmake build
```text
[1/1] Linking CXX executable seriona
```
build_exit=0

### scanner ctest
```text
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
      Start 26: seriona.scanner_test_harness
 1/10 Test #26: seriona.scanner_test_harness .....   Passed    0.01 sec
      Start 27: seriona.scanner_contract
 2/10 Test #27: seriona.scanner_contract .........   Passed    0.03 sec
      Start 28: seriona.scanner_paths
 3/10 Test #28: seriona.scanner_paths ............   Passed    0.00 sec
      Start 29: seriona.scanner_hash
 4/10 Test #29: seriona.scanner_hash .............   Passed    0.00 sec
      Start 30: seriona.scanner_tree
 5/10 Test #30: seriona.scanner_tree .............   Passed    0.00 sec
      Start 31: seriona.scanner_scheduler
 6/10 Test #31: seriona.scanner_scheduler ........   Passed    0.02 sec
      Start 32: seriona.scanner_cache
 7/10 Test #32: seriona.scanner_cache ............   Passed    0.03 sec
      Start 33: seriona.scanner_tagreader
 8/10 Test #33: seriona.scanner_tagreader ........   Passed    0.03 sec
      Start 34: seriona.scanner_service
 9/10 Test #34: seriona.scanner_service ..........   Passed    0.05 sec
      Start 35: seriona.scanner_watcher
10/10 Test #35: seriona.scanner_watcher ..........   Passed    0.16 sec

100% tests passed, 0 tests failed out of 10

Total Test time (real) =   0.34 sec
```
scanner_ctest_exit=0

### full ctest
```text
Test project /home/kaizen857/cppProject(app_and_lib)/Seriona_Backend/build
      Start  1: seriona.doctest.placeholder
 1/35 Test  #1: seriona.doctest.placeholder ...................   Passed    0.03 sec
      Start  2: seriona.audio_contract
 2/35 Test  #2: seriona.audio_contract ........................   Passed    0.03 sec
      Start  3: seriona.audio_fixture
 3/35 Test  #3: seriona.audio_fixture .........................   Passed    0.03 sec
      Start  4: seriona.ffmpeg_audio_source
 4/35 Test  #4: seriona.ffmpeg_audio_source ...................   Passed    0.03 sec
      Start  5: seriona.ffmpeg_audio_source_errors
 5/35 Test  #5: seriona.ffmpeg_audio_source_errors ............   Passed    0.03 sec
      Start  6: seriona.ffmpeg_audio_source_seek
 6/35 Test  #6: seriona.ffmpeg_audio_source_seek ..............   Passed    0.05 sec
      Start  7: seriona.ffmpeg_filter_pipeline
 7/35 Test  #7: seriona.ffmpeg_filter_pipeline ................   Passed    0.03 sec
      Start  8: seriona.ffmpeg_filter_pipeline_drain
 8/35 Test  #8: seriona.ffmpeg_filter_pipeline_drain ..........   Passed    0.03 sec
      Start  9: seriona.ffmpeg_filter_pipeline_errors
 9/35 Test  #9: seriona.ffmpeg_filter_pipeline_errors .........   Passed    0.03 sec
      Start 10: seriona.pcm_buffer_queue
10/35 Test #10: seriona.pcm_buffer_queue ......................   Passed    0.00 sec
      Start 11: seriona.audio_output_device
11/35 Test #11: seriona.audio_output_device ...................   Passed    0.00 sec
      Start 12: seriona.playback_clock
12/35 Test #12: seriona.playback_clock ........................   Passed    0.00 sec
      Start 13: seriona.playback_state_machine
13/35 Test #13: seriona.playback_state_machine ................   Passed    0.00 sec
      Start 14: seriona.playback_state_machine_cancellation
14/35 Test #14: seriona.playback_state_machine_cancellation ...   Passed    0.00 sec
      Start 15: seriona.audio_event_dispatcher
15/35 Test #15: seriona.audio_event_dispatcher ................   Passed    0.00 sec
      Start 16: seriona.audio_event_dispatcher_shutdown
16/35 Test #16: seriona.audio_event_dispatcher_shutdown .......   Passed    0.00 sec
      Start 17: seriona.audio_player_single_track
17/35 Test #17: seriona.audio_player_single_track .............   Passed    0.04 sec
      Start 18: seriona.audio_player_small_buffer
18/35 Test #18: seriona.audio_player_small_buffer .............   Passed    0.06 sec
      Start 19: seriona.output_format_negotiation
19/35 Test #19: seriona.output_format_negotiation .............   Passed    0.03 sec
      Start 20: seriona.output_format_negotiation_failure
20/35 Test #20: seriona.output_format_negotiation_failure .....   Passed    0.03 sec
      Start 21: seriona.audio_player_seamless_mix
21/35 Test #21: seriona.audio_player_seamless_mix .............   Passed    0.04 sec
      Start 22: seriona.audio_player_direct_preload
22/35 Test #22: seriona.audio_player_direct_preload ...........   Passed    0.03 sec
      Start 23: seriona.audio_player_preload_failure
23/35 Test #23: seriona.audio_player_preload_failure ..........   Passed    0.03 sec
      Start 24: seriona.audio_error_matrix
24/35 Test #24: seriona.audio_error_matrix ....................   Passed    0.03 sec
      Start 25: seriona.audio_shutdown_lifecycle
25/35 Test #25: seriona.audio_shutdown_lifecycle ..............   Passed    0.04 sec
      Start 26: seriona.scanner_test_harness
26/35 Test #26: seriona.scanner_test_harness ..................   Passed    0.01 sec
      Start 27: seriona.scanner_contract
27/35 Test #27: seriona.scanner_contract ......................   Passed    0.02 sec
      Start 28: seriona.scanner_paths
28/35 Test #28: seriona.scanner_paths .........................   Passed    0.00 sec
      Start 29: seriona.scanner_hash
29/35 Test #29: seriona.scanner_hash ..........................   Passed    0.00 sec
      Start 30: seriona.scanner_tree
30/35 Test #30: seriona.scanner_tree ..........................   Passed    0.00 sec
      Start 31: seriona.scanner_scheduler
31/35 Test #31: seriona.scanner_scheduler .....................   Passed    0.02 sec
      Start 32: seriona.scanner_cache
32/35 Test #32: seriona.scanner_cache .........................   Passed    0.03 sec
      Start 33: seriona.scanner_tagreader
33/35 Test #33: seriona.scanner_tagreader .....................   Passed    0.02 sec
      Start 34: seriona.scanner_service
34/35 Test #34: seriona.scanner_service .......................   Passed    0.04 sec
      Start 35: seriona.scanner_watcher
35/35 Test #35: seriona.scanner_watcher .......................   Passed    0.15 sec

100% tests passed, 0 tests failed out of 35

Total Test time (real) =   0.95 sec
```
full_ctest_exit=0
