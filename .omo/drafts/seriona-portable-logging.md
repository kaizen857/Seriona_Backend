---
slug: seriona-portable-logging
status: plan-written
intent: clear
pending-action: start work or request high-accuracy review
approach: Add one thin spdlog bootstrap and one portable runtime-data directory, then thread those concerns through the existing backend boundaries without creating a large logging framework.
---

# Draft: seriona-portable-logging

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->
- C1 | Build dependency and minimal logging bootstrap are available to every backend target | active | .omo/evidence/task-1-seriona-portable-logging.md
- C2 | App derives a portable runtime data root beside the executable | active | .omo/evidence/task-2-seriona-portable-logging.md
- C3 | Scanner database and artwork export paths move from system temp to portable runtime data | active | .omo/evidence/task-3-seriona-portable-logging.md
- C4 | Control/app/metadata/audio/scanner emit useful logs through the one default logger | active | .omo/evidence/task-4-seriona-portable-logging.md .. task-9
- C5 | Realtime audio callback and PCM queue hot paths remain log-free | active | .omo/evidence/task-5-seriona-portable-logging.md
- C6 | Full build, tests, and live portable smoke validate paths and logs | active | .omo/evidence/task-10-seriona-portable-logging.md

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->
- Runtime data root | `<executable directory>/SerionaData/` | User rejected hyphenated/internal-module names and accepted a subdirectory; `SerionaData` is portable, stable, and module-neutral | Yes, one path helper constant.
- Log file path | `<executable directory>/SerionaData/logs/seriona.log` | Keeps logs portable and grouped without exposing module names | Yes.
- Database path | `<executable directory>/SerionaData/library.sqlite` | Avoids `scanner` wording and describes user-visible library data | Yes.
- Artwork export path | `<executable directory>/SerionaData/artwork/` | Avoids `scanner-covers` and keeps exported artwork portable | Yes.
- spdlog shape | One default multi-sink logger, initialized once by app startup | Official spdlog supports default logger + multiple sinks; avoids over-designed framework | Yes.
- Sink choice | Console color sink + rotating file sink | Official multi-sink and rotating-file examples cover this; rotation prevents unbounded portable log growth | Yes.
- Levels | Runtime logger level `debug`; file sink `debug+`; console sink `info+`; `flush_on(error)` | User requested `debug` through `critical`; console should not be noisy | Yes.
- Subagent execution | Implementation prompts should request lightweight workers / gpt-5.4-mini where available | User requested subagents and preferred gpt-5.4-mini; current tool cannot force model IDs, so encode as execution instruction | Yes.

## Findings (cited - path:lines)
- Root CMake centralizes dependencies and core targets in `CMakeLists.txt:15`, `CMakeLists.txt:35`, `CMakeLists.txt:111`, `CMakeLists.txt:123`, `CMakeLists.txt:132`, with target links at `CMakeLists.txt:52`, `CMakeLists.txt:175`, `CMakeLists.txt:183`, `CMakeLists.txt:195`.
- `spdlog` is not currently referenced in repo CMake or source; system `pkg-config --modversion spdlog` returned `1.17.0` during planning.
- Official spdlog docs support a logger with multiple sinks (`stdout_color_sink_mt` + `basic_file_sink_mt`/`rotating_file_sink_mt`), `spdlog::set_default_logger`, runtime `set_level`, sink-level filters, patterns, `flush_on`, and `flush_every`.
- Production runtime storage defaults are hardcoded to system temp in `src/scanner/file_scanner_orchestrator.cpp:30` and `src/scanner/file_scanner_orchestrator.cpp:34`.
- Scanner injection already supports `databasePath` and `coverExportDir` through `src/scanner/file_scanner_service_internal.h:53`; the implementation consumes them in `src/scanner/file_scanner_orchestrator.cpp:244`, `src/scanner/file_scanner_orchestrator.cpp:316`, and `src/scanner/file_scanner_orchestrator.cpp:513`.
- SQLite cache path contract is `inc/seriona/scanner/cache/sqlite_scanner_cache.h:15`; cache creates parent directories and opens SQLite in `src/scanner/cache/sqlite_scanner_cache.cpp:556`.
- App startup currently has no executable-directory discovery; `app/main.cpp:14` only uses `argv[0]` for usage text, and `app/terminal_controller.cpp:181` calls `makeProductionMediaController()` without runtime paths.
- Public production assembly is in `src/control/media_controller_module.cpp:51` and should receive runtime path options without leaking scanner internals.
- Realtime hard constraint: `AGENTS.md` forbids logs/dynamic allocation/locks in `AudioOutputDevice::renderCallback()`; source anchor is `src/audio/device/audio_output_device.cpp:253`.
- Logging surfaces identified: app startup `app/main.cpp:14`, terminal flow `app/terminal_controller.cpp:181`, control reducer `src/control/control_state_reducer.cpp:187`, control loop `src/control/control_event_loop.cpp:20`, audio service `src/audio/audio_playback_service.cpp:104`, FFmpeg source `src/audio/ffmpeg/ffmpeg_audio_source.cpp:167`, scanner orchestrator `src/scanner/file_scanner_orchestrator.cpp:242`, SQLite cache `src/scanner/cache/sqlite_scanner_cache.cpp:556`, TagReader bridge `src/scanner/tag_reader_metadata_adapter.cpp:105`, metadata backend `src/metadata/metadata_service_backend.cpp:40`, and Linux MPRIS `src/metadata/metadata_mpris_linux.cpp:173`.

## Decisions (with rationale)
- Use `SerionaData` as the runtime data directory name. It has no hyphen, does not expose internal module names, and remains understandable to users copying the app folder.
- Add a thin logging module rather than a framework. It should expose only initialization/shutdown helpers and rely on spdlog's default logger/global functions elsewhere.
- Do not create per-module logger classes. Use message text/module prefixes or logger pattern metadata as needed; the backend is one cohesive product.
- Link `spdlog::spdlog` privately to backend targets that include spdlog in `.cpp` files. Do not expose spdlog types in public contract headers.
- Keep logs out of realtime audio and PCM queue hot paths. Emit summarized underrun/fallback logs from service/control paths instead.
- Use tests-after plus live smoke. Existing behavior is broad and logging is observability-focused; targeted regressions should assert path selection, logger sinks, and no realtime logger references.

## Scope IN
- Add CMake discovery/linking for spdlog.
- Add minimal `seriona::logging` bootstrap in private source/header files.
- Configure one default spdlog logger with console and rotating file sinks.
- Add portable runtime path helper deriving paths from the executable directory.
- Move production scanner database and artwork output to `SerionaData`.
- Add logs across app/control/audio/scanner/metadata lifecycle, state transitions, recoverable fallback, and failures.
- Add targeted tests and live smoke verification for logging and portable paths.

## Scope OUT (Must NOT have)
- No large logging framework, service locator, or per-module logger factory hierarchy.
- No spdlog types in public API headers under `inc/seriona/...` unless unavoidable and explicitly justified.
- No logging in `AudioOutputDevice::renderCallback()`, miniaudio data callback, PCM queue read/write hot paths, or other realtime paths.
- No JSON/YAML/TOML config system in this plan.
- No Qt/QML/UI changes.
- No unrelated behavior rewrites.

## Open questions
- None. User selected the portable subdirectory strategy and requested a better non-hyphenated, module-neutral name; this draft adopts `SerionaData`.

## Approval gate
status: approved-for-plan-file
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
