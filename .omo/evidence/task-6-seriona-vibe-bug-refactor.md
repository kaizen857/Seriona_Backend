# Task 6 Evidence - introduce audio command worker and transactional seek

## Scope
- Allowed domain observed for task-owned edits: `inc/seriona/audio/playback_state_machine.h`, `src/audio/audio_playback_service.cpp`, `src/audio/playback_state_machine.cpp`, `tests/audio/audio_player_single_track_tests.cpp`, `tests/audio/audio_player_small_buffer_tests.cpp`, `tests/audio/audio_error_matrix_tests.cpp`, `tests/audio/audio_shutdown_lifecycle_tests.cpp`, `tests/audio/playback_state_machine_tests.cpp`, and this evidence file.
- `inc/seriona/audio/audio_contracts.h` was not edited.
- `inc/seriona/audio/audio_playback_service.h` was inspected and left unchanged.
- Pre-existing dirty files outside task 6 were present in `git status --short` and were not edited by this task.

## Failing-first regressions
- Added `audio_player_public_commands_enqueue_without_waiting_for_device_start` to prove `pause()` and `stop()` return quickly while fake backend `start()` is blocked on the audio worker.
- Added playback state machine tests for `cancelSeek()` rollback and generation-checked stale seek completion.
- Red proof command:
  - `cmake --build build --target seriona_playback_state_machine_tests seriona_audio_player_single_track_tests`
  - Result: failed as expected before implementation because `PlaybackStateMachine` had no `cancelSeek` or `beginSeek` members.

## Implementation notes
- Public audio commands now enqueue lambdas onto a single `SingleTrackAudioPlaybackService` audio worker.
- The audio worker serializes mutations of FFmpeg source, filter pipeline, PCM queue, playback clock, output device, and playback state machine.
- The old progress thread path was collapsed into the same audio worker via `progressWorkerRunning_` wakeups; progress/refill work no longer mutates audio-owned state from a second thread.
- `seekOnWorker()` opens/seeks a fresh FFmpeg source, configures a fresh filter, and fills a fresh queue before publishing it. If preparation fails, `PlaybackStateMachine::cancelSeek()` restores the prior clock/state and emits a typed error.
- `PlaybackStateMachine::completeSeek(std::uint64_t)` ignores stale completions when a newer stop/fail/load has advanced generation.

## Verification
- `cmake --build build --target seriona_playback_state_machine_tests seriona_audio_player_single_track_tests seriona_audio_player_small_buffer_tests seriona_audio_error_matrix_tests seriona_audio_shutdown_lifecycle_tests`
  - Result: exit code 0, final output `ninja: no work to do.` after rebuilt objects were current.
- `ctest --test-dir build -R 'seriona\.(playback_state_machine|audio_player_single_track|audio_player_small_buffer|audio_error_matrix|audio_shutdown_lifecycle)' --output-on-failure`
  - Result: exit code 0; 6/6 tests passed.
- `git diff --name-only`
  - Result: task-owned audio files changed; unrelated pre-existing dirty files also listed (`DESIGN.md`, scanner files, `.omo/boulder.json`, task 8 evidence, etc.). Those unrelated files were not edited by task 6.
- LSP diagnostics: no `lsp_diagnostics` tool was available in this session. Compiler diagnostics from the targeted CMake build are clean.

## Post-write review
- Single responsibility: changed production files still own playback service orchestration and playback state transitions respectively.
- Boundary purity: no public `AudioPlaybackService`/`BackendEvent` contract changes; no private FFmpeg/miniaudio types leaked into public audio contracts.
- Variant discrimination: no new tagged variant branching beyond existing enum switches.
- Escape hatches: no `unwrap`/`expect` equivalents or warning suppressions added.
- Defensive layer: worker shutdown uses queue contract and join; no redundant post-action verification added in production code.
- Tests: new regressions fail before implementation and pass after worker/transactional seek changes.
