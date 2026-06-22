# metadata-sharing-module-implementation - Work Plan

## TL;DR (For humans)
**What you'll get:** 一个跨平台一致 API 的系统媒体元数据共享模块：Linux 侧优先可用并可验证，Windows 侧先保留官方 SMTC 编码路径与编译隔离，不阻塞当前开发。

**Why this approach:** 元数据共享模块必须位于控制层快照上方，不能直接接音频/扫描；系统实际协议只能接收有限字段，所以先做统一内部快照到平台 DTO 的筛选映射，再分别接 Linux MPRIS 和 Windows SMTC。

**What it will NOT do:** 不实现完整 mediaController、播放队列或 UI；不让平台 API 泄漏到公共头；不在 Windows 无窗口/无验证环境时强行启用真实 SMTC。

**Effort:** Large
**Risk:** High - Linux MPRIS 必须通过系统 `sdbus-c++` 依赖落地并可验证；Windows 桌面 SMTC 需要 HWND/WinRT interop，且当前不做 Windows 运行测试。
**Decisions to sanity-check:** Linux 优先、Windows 先编码不测、统一 API 不随平台改变、进度 1 秒更新、功能级 commit。

Your next move: 运行 `$start-work` 执行，或先要求 review/修改计划。Full execution detail follows below.

---

> TL;DR (machine): Large/high-risk plan for a C++23 metadata sharing module with unified control API, Linux MPRIS priority, Windows SMTC compile-isolated coding, Linux-only test gates, and feature-level commits.

## Scope
### Must have
- `inc/seriona/control/control_contracts.h` platform-neutral control snapshot/command contracts with `PlayerStateSnapshot`, `PlaybackCapabilities`, `MediaControlCommand`, subscription/command callback seams, and no MPRIS/SMTC/Qt/QML types.
- `inc/seriona/metadata/metadata_contracts.h` platform-neutral metadata sharing service contracts and factory options that expose one top-level API across Linux, Windows, and Noop backends.
- `src/metadata/...` core mapper/synchronizer that filters full internal snapshots into platform-supported DTOs and never forwards internal `version`, `sampledAt`, output format, or error summary to OS metadata payloads.
- Linux MPRIS implementation path using D-Bus semantics for `org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player`, with field mapping for playback status, metadata, position, duration, volume, loop, shuffle, and capability flags.
- Linux builds must require the system `sdbus-c++` dependency for the real MPRIS adapter, using the old-project-compatible include form `<sdbus-c++/sdbus-c++.h>` only in private Linux implementation files; missing `sdbus-c++` must fail configuration with a clear dependency message instead of silently producing a model-only implementation.
- Linux command bridge from MPRIS `Play`, `Pause`, `PlayPause`, `Stop`, `Next`, `Previous`, `Seek`, `SetPosition`, `LoopStatus`, `Shuffle`, and `Volume` changes into unified `MediaControlCommand` callbacks.
- Windows SMTC implementation files must be coded behind compile guards, using the Microsoft official `SystemMediaTransportControlsDisplayUpdater`/`MusicProperties`/`Thumbnail`/`Update()` flow and `ISystemMediaTransportControlsInterop::GetForWindow(...)` for real desktop integration, but no Windows runtime test is required now.
- Noop and fake/recording backends for default builds and Linux tests without requiring a real D-Bus session or Windows shell media controls.
- `seriona_metadata` CMake target, metadata CTest targets, app link-only integration, Chinese docs, and evidence files.
- `AGENTS.md` project-boundary update before product implementation: keep the backend pure C++23/no Qt-QML-UI rule, explicitly allow MPRIS/SMTC/sdbus-c++/WinRT only inside metadata platform adapter implementation files, and explicitly allow this plan's committed evidence under `.omo/evidence/metadata-sharing-module-implementation/`.
- Every Linux-facing feature must pass agent-executed tests before dependent work proceeds; failed tests must be fixed and rerun before continuing.
- Every completed feature must be committed with `git add` and `git commit`; git read operations are unrestricted, but git write operations other than `git add`/`git commit` are forbidden.
- User decision: `AGENTS.md` must be updated by the worker before metadata product implementation so future sessions no longer see a conflict. MPRIS/SMTC dependencies are allowed only inside metadata platform adapter implementation files and must never leak into common public headers, audio, scanner, Qt/QML/UI, or real-time audio paths. `.omo/evidence/metadata-sharing-module-implementation/` is explicitly allowed as committed execution evidence for this plan.
### Must NOT have (guardrails, anti-slop, scope boundaries)
- Do not implement a full `mediaController`, playback queue, UI/QML bridge, Qt object, hidden window, or Windows message loop inside the backend module.
- Do not change top-level public API per operating system; platform differences must be hidden behind factory options, backend capabilities, and runtime degradation.
- Do not let metadata sharing directly depend on or call audio/scanner service instances; all state comes from control snapshots and all OS commands return through command callbacks.
- Do not put MPRIS, SMTC, sdbus-c++, WinRT, HWND, Qt/QML, or platform D-Bus/COM types in platform-neutral public headers.
- Do not forward internal snapshot fields such as `version`, `sampledAt`, output format, or error summary into MPRIS metadata maps or SMTC display/timeline payloads.
- Do not make Windows tests a blocker in this phase; Windows code must compile-isolate on Linux and is not runtime-validated now.
- Do not proceed past a failed Linux feature test; fix forward and retest.
- Do not use `git reset`, `git revert`, `git checkout`, `git restore`, `git clean`, rebase, amend, or any git write operation other than `git add` and `git commit`.

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD with doctest + CTest for contracts, mapper, synchronizer, fake backend, and Linux MPRIS fake-D-Bus adapter seams.
- Evidence: `.omo/evidence/metadata-sharing-module-implementation/task-<N>.md` for each task. Do not place evidence under `build/` because `build/` is ignored and evidence must be committed with each completed feature. Todo 1 must update `AGENTS.md` so this evidence directory is allowed for this plan.
- Linux mandatory gate: each Linux feature todo must run its listed focused CTest command; if it fails, stop the dependent chain, delegate bug fix, rerun the same command, and only then continue.
- Windows current-phase gate: Windows files must be compile-guarded and structurally reviewed; Linux must prove they are excluded from Linux builds and public headers. Do not require a Windows machine, Windows shell media controls, or Windows CTest execution.
- Final verification: configure, build, all metadata-focused tests, and full CTest on the current Linux environment.
- Instruction verification: after Todo 1, `AGENTS.md` must explicitly permit metadata platform adapters to use MPRIS/SMTC/sdbus-c++/WinRT, explicitly allow `.omo/evidence/metadata-sharing-module-implementation/` as committed plan evidence, and still ban Qt/QML/UI plus platform leakage outside adapter implementation files.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.
- Wave 1 sequential-only: create the minimal metadata CMake/test target first, then public contracts and expanded test scaffolding define all downstream API.
- Wave 2 partially parallel: mapper/synchronizer/fake backend can split after contracts, but commits touching the same `src/metadata` core files must be serialized.
- Wave 3 sequential Linux path: Linux MPRIS DTO/object/command behavior depends on core mapper and fake backend.
- Wave 4 parallel-safe with care: Windows compile-isolated files and docs can proceed after public API stabilizes, but any CMake/app integration conflict must be resolved forward before commit.
- Wave 5 sequential integration: app link-only, docs, evidence, full verification, and final review.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2,3,4,5,6,7,8,9,10,11,12,13 | none |
| 2 | 1 | 3,4,5,6,7,8,9,10,11,12,13 | none |
| 3 | 2 | 4,5,6,7,8,9,10,11,12,13 | none |
| 4 | 3 | 5,6,7,8,9,10,11,12,13 | none |
| 5 | 4 | 6,7,8,10,11,12,13 | 9 if no shared files |
| 6 | 5 | 7,8,10,11,12,13 | 9 if no shared files |
| 7 | 6 | 8,10,11,12,13 | 9 if no shared files |
| 8 | 7 | 10,11,12,13 | 9 if no shared files |
| 9 | 4 | 11,12,13 | 6,7,8 if no CMake/shared header conflict |
| 10 | 8 | 11,12,13 | none |
| 11 | 9,10 | 12,13 | none |
| 12 | 11 | 13 | none |
| 13 | 12 | final wave | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
- [x] 1. Update AGENTS metadata exception and add minimal metadata test scaffold
  What to do / Must NOT do: First update `AGENTS.md` so the project boundary says the backend remains pure C++23 and still forbids Qt/QML/UI, but metadata platform adapter implementation files may use MPRIS/SMTC/sdbus-c++/WinRT when kept out of public/common headers, audio, scanner, and real-time audio paths. The same `AGENTS.md` update must explicitly allow committed evidence for this plan under `.omo/evidence/metadata-sharing-module-implementation/`. Then add the initial `seriona_metadata` static library target, `tests/metadata/` directory, `seriona_metadata_contract_tests` executable, and `seriona.metadata_contract` CTest registration before any contract QA depends on them. The initial library may contain only a minimal private translation unit if needed. Must NOT implement product behavior beyond scaffolding, and must NOT require real D-Bus, Windows SDK, or platform media controls for this first scaffold.
  Parallelization: Wave 1 | Blocked by: none | Blocks: all downstream work | Mode: sequential-only
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:63`, `tests/CMakeLists.txt:421`, `.omo/drafts/metadata-sharing-module-implementation.md:54`, `.omo/drafts/metadata-sharing-module-implementation.md:106`, `.gitignore:41`
  Acceptance criteria (agent-executable): `AGENTS.md` contains the metadata adapter exception, the `.omo/evidence/metadata-sharing-module-implementation/` exception, and still bans Qt/QML/UI; `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` configures; `seriona_metadata_contract_tests` builds; `ctest -R seriona.metadata_contract` runs at least one non-placeholder doctest assertion.
  QA scenarios (name the exact tool + invocation): happy `rg "metadata.*(MPRIS|SMTC)" AGENTS.md && rg "sdbus-c\+\+" AGENTS.md && rg "WinRT" AGENTS.md && rg "\.omo/evidence/metadata-sharing-module-implementation" AGENTS.md && rg "Qt/QML/UI" AGENTS.md && cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build --target seriona_metadata_contract_tests && ctest --test-dir build -R seriona.metadata_contract --output-on-failure`; failure test asserts the CTest output contains a real metadata contract test case name, not only a placeholder. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-1.md`
  Commit: Y | `build(metadata): add metadata scaffold and instructions` | Stage only `AGENTS.md`, root/tests CMake, minimal metadata scaffold files, initial contract test, and evidence.

- [x] 2. Define platform-neutral control snapshot contracts
  What to do / Must NOT do: Add `inc/seriona/control/control_contracts.h` with value-semantic `TrackIdentity`, `DisplayMetadata`, `ArtworkRef`, `PlaybackStatus`, `RepeatMode`, `PlaybackCapabilities`, `PlaybackTimeline`, `PlayerStateSnapshot`, `MediaControlCommand`, `SubscriptionHandle`/callback aliases, and command sink types. Include fields from `DESIGN.md` internal snapshot, but keep all platform types out. Define `version`/`sampledAt` as internal freshness fields, not OS payload fields. Must NOT include MPRIS/SMTC/sdbus/WinRT/HWND/Qt/QML headers or names.
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 3-13 | Mode: sequential-only
  References: `DESIGN.md:189`, `DESIGN.md:201`, `DESIGN.md:620`, `DESIGN.md:646`, `DESIGN.md:658`, `.omo/drafts/metadata-sharing-module-implementation.md:24`, `.omo/drafts/metadata-sharing-module-implementation.md:32`, `.omo/drafts/metadata-sharing-module-implementation.md:33`, `.omo/drafts/metadata-sharing-module-implementation.md:37`, `inc/seriona/audio/audio_contracts.h:100`, `inc/seriona/scanner/scanner_contracts.h:87`
  Acceptance criteria (agent-executable): Header compiles in the existing metadata contract test target; public API is identical across platforms and does not require platform macros for consumers.
  QA scenarios: happy `cmake --build build --target seriona_metadata_contract_tests && ctest --test-dir build -R seriona.metadata_contract --output-on-failure`; failure test asserts no platform header/name appears in public control contract and default snapshot state maps to no current track. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-2.md`
  Commit: Y | `feat(metadata): define control snapshot contracts` | Stage only `inc/seriona/control/control_contracts.h`, focused contract tests, and evidence.

- [x] 3. Define metadata service contracts and factory seams
  What to do / Must NOT do: Add `inc/seriona/metadata/metadata_contracts.h` exposing a stable `MetadataSharingService` interface, `MetadataSharingOptions`, `MetadataBackendKind`, `MetadataBackendCapabilities`, platform-neutral `PlatformMediaState`, `MetadataSyncResult`, command callback registration, start/stop/update APIs, and `makeMetadataSharingService(...)`. Public API must not differ per OS; platform selection is via options/capabilities. Must NOT expose platform types or require Windows HWND in the top-level API; represent any future host handle as an opaque platform extension object in options without changing core methods.
  Parallelization: Wave 1 | Blocked by: 2 | Blocks: 4-13 | Mode: sequential-only
  References: `.omo/drafts/metadata-sharing-module-implementation.md:14`, `.omo/drafts/metadata-sharing-module-implementation.md:24`, `.omo/drafts/metadata-sharing-module-implementation.md:30`, `.omo/drafts/metadata-sharing-module-implementation.md:33`, `.omo/drafts/metadata-sharing-module-implementation.md:91`, `DESIGN.md:113`, `DESIGN.md:620`, `CMakeLists.txt:63`
  Acceptance criteria (agent-executable): New metadata contract tests instantiate options for Linux/Windows/Noop without changing method signatures; platform headers absent from public header include graph.
  QA scenarios: happy `cmake --build build --target seriona_metadata_contract_tests && ctest --test-dir build -R seriona.metadata_contract --output-on-failure`; failure test verifies Windows-specific host absence degrades through options/capabilities instead of changing public API. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-3.md`
  Commit: Y | `feat(metadata): define service contracts` | Stage only metadata public header, focused tests/CMake, and evidence.

- [x] 4. Expand metadata build targets and Linux MPRIS dependency gate
  What to do / Must NOT do: Expand `seriona_metadata` and metadata tests by adding mapper/service/MPRIS test executables and CTest names: `seriona.metadata_mapper`, `seriona.metadata_service`, `seriona.metadata_mpris`, and smoke-ready `seriona.metadata_mpris_smoke`. Add a Linux-only required `sdbus-c++` dependency gate for the real MPRIS adapter using the system header `<sdbus-c++/sdbus-c++.h>` in private implementation only; on Linux, configuration must fail with a clear message if `sdbus-c++` is missing. Add `SERIONA_METADATA_SIMULATE_MISSING_SDBUS` mirroring the scanner dependency simulation style so the missing-dependency path is agent-testable without uninstalling packages. Ensure Windows SDK is not required on Linux. Must NOT touch unrelated audio/scanner behavior.
  Parallelization: Wave 1 | Blocked by: 3 | Blocks: 5-13 | Mode: sequential-only
  References: `CMakeLists.txt:63`, `tests/CMakeLists.txt:421`, `.omo/drafts/metadata-sharing-module-implementation.md:54`, `.omo/drafts/metadata-sharing-module-implementation.md:106`, `.omo/drafts/metadata-sharing-module-implementation.md:107`
  Acceptance criteria (agent-executable): Linux configure detects `sdbus-c++`; metadata contract/mapper/service/MPRIS test targets build; platform headers still do not appear in public headers.
  QA scenarios: happy `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build --target seriona_metadata_contract_tests seriona_metadata_mapper_tests seriona_metadata_service_tests seriona_metadata_mpris_tests && ctest --test-dir build -R seriona.metadata --output-on-failure`; failure `rm -rf build-missing-sdbus && set +e; cmake -S . -B build-missing-sdbus -DSERIONA_BUILD_TESTS=ON -DSERIONA_METADATA_SIMULATE_MISSING_SDBUS=ON 2>&1 | tee .omo/evidence/metadata-sharing-module-implementation/task-4-missing-sdbus.log; exit_code=${pipestatus[1]}; set -e; test ${exit_code} -ne 0 && rg "seriona_metadata dependency gate: sdbus-c\+\+ is required for Linux MPRIS metadata sharing" .omo/evidence/metadata-sharing-module-implementation/task-4-missing-sdbus.log` must pass. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-4.md`
  Commit: Y | `build(metadata): add metadata module targets` | Stage CMake/test harness/build scaffolding and evidence.

- [x] 5. Implement snapshot field filtering and platform DTO mapper
  What to do / Must NOT do: Implement mapper logic that converts `PlayerStateSnapshot` into platform-neutral DTOs for MPRIS/SMTC-supported fields only. Include exact support matrix: track ID, file URI/path, title/artist/album/album artist/genre/track number, art ref, status, timeline, duration, volume where supported, repeat, shuffle, capabilities. Explicitly exclude `version`, `sampledAt`, output format, and error summary from OS payload DTOs. Must generate legal MPRIS object paths for `mpris:trackid` that do not use `/org/mpris...` reserved prefix except NoTrack sentinel.
  Parallelization: Wave 2 | Blocked by: 4 | Blocks: 6-13 | Mode: sequential-only for mapper files
  References: `.omo/drafts/metadata-sharing-module-implementation.md:48`, `.omo/drafts/metadata-sharing-module-implementation.md:52`, `.omo/drafts/metadata-sharing-module-implementation.md:58`, `.omo/drafts/metadata-sharing-module-implementation.md:69`, `.omo/drafts/metadata-sharing-module-implementation.md:75`, `DESIGN.md:624`
  Acceptance criteria (agent-executable): Mapper tests prove supported fields map correctly, unsupported fields never appear, MPRIS time uses microseconds, Windows DTO does not claim volume/mute support, and generated MPRIS track IDs are valid object paths outside reserved prefixes.
  QA scenarios: happy `cmake --build build --target seriona_metadata_mapper_tests && ctest --test-dir build -R seriona.metadata_mapper --output-on-failure`; failure scenarios include snapshots with error summaries/output formats and invalid raw track IDs. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-5.md`
  Commit: Y | `feat(metadata): map snapshots to platform fields` | Stage mapper source/tests and evidence.

- [x] 6. Implement synchronizer with split metadata/timeline dirty paths
  What to do / Must NOT do: Implement core synchronizer that accepts snapshots, deduplicates by track/status/capability/timeline state, emits static metadata updates only on actual track/metadata/artwork/capability change, and emits timeline updates every 1 second during playback plus immediately on pause/seek/resume/track change/stop. Use `version`/`sampledAt` only for stale detection/freshness, not OS payload. Must NOT resend artwork/title every 1-second position tick.
  Parallelization: Wave 2 | Blocked by: 5 | Blocks: 7,8,10,11,12,13 | Mode: sequential-only for core synchronizer files
  References: `.omo/drafts/metadata-sharing-module-implementation.md:26`, `.omo/drafts/metadata-sharing-module-implementation.md:56`, `.omo/drafts/metadata-sharing-module-implementation.md:97`, `.omo/drafts/metadata-sharing-module-implementation.md:117`, `DESIGN.md:201`, `DESIGN.md:667`
  Acceptance criteria (agent-executable): Fake clock tests show timeline update at 1-second cadence while playing, immediate update on non-continuous changes, no duplicate metadata update on position-only ticks, stale snapshot ignored.
  QA scenarios: happy `cmake --build build --target seriona_metadata_service_tests && ctest --test-dir build -R seriona.metadata_service --output-on-failure`; failure scenarios include 500ms tick no emit, 1000ms tick emits timeline only, title change emits metadata once. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-6.md`
  Commit: Y | `feat(metadata): synchronize timeline and metadata updates` | Stage synchronizer source/tests and evidence.

- [x] 7. Implement Noop and recording backend service lifecycle
  What to do / Must NOT do: Implement `MetadataSharingService` Noop backend and test-only recording backend for explicit Noop selection, Windows-without-host runtime degradation, and non-Linux builds. Support start/stop idempotency, update-after-stop no-op/failure result, command callback registration, backend failure reporting, and capability reporting. On Linux, missing `sdbus-c++` is a configuration failure for the metadata module, not a Noop fallback. Must NOT block on platform API or require a live D-Bus session/SMTC shell for automated tests.
  Parallelization: Wave 2 | Blocked by: 6 | Blocks: 8,10,11,12,13 | Mode: sequential-only if touching core service files
  References: `.omo/drafts/metadata-sharing-module-implementation.md:16`, `.omo/drafts/metadata-sharing-module-implementation.md:92`, `.omo/drafts/metadata-sharing-module-implementation.md:97`, `DESIGN.md:113`, `DESIGN.md:115`, `DESIGN.md:671`
  Acceptance criteria (agent-executable): Service lifecycle tests pass; fake backend records expected static/timeline updates; failure paths report sync failure without throwing across public API.
  QA scenarios: happy `cmake --build build --target seriona_metadata_service_tests && ctest --test-dir build -R seriona.metadata_service --output-on-failure`; failure scenarios include backend start failure and callback after stop. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-7.md`
  Commit: Y | `feat(metadata): add noop service lifecycle` | Stage service/fake backend tests and evidence.

- [x] 8. Implement real Linux MPRIS adapter with fake D-Bus tests
  What to do / Must NOT do: Implement the Linux MPRIS platform adapter behind Linux compile guards using the required system `sdbus-c++` dependency and private include `<sdbus-c++/sdbus-c++.h>`, plus a fake D-Bus adapter seam for deterministic tests. It must expose/register the real MPRIS object model for `/org/mpris/MediaPlayer2`, `org.mpris.MediaPlayer2`, and `org.mpris.MediaPlayer2.Player`, and support `PropertiesChanged`/method/property behavior through the adapter seam. Map `mpris:trackid`, `mpris:length`, `mpris:artUrl`, `xesam:title`, `xesam:artist`, `xesam:album`, `xesam:url`, `PlaybackStatus`, `LoopStatus`, `Shuffle`, `Volume`, `Position`, and capability properties. Local art on Linux uses `file://` URI. Must NOT reduce this to a model-only implementation, require a live user D-Bus session for automated tests, or expose sdbus-c++ in public headers.
  Parallelization: Wave 3 | Blocked by: 7 | Blocks: 10,11,12,13 | Mode: sequential-only for Linux MPRIS path
  References: `.omo/drafts/metadata-sharing-module-implementation.md:17`, `.omo/drafts/metadata-sharing-module-implementation.md:51`, `.omo/drafts/metadata-sharing-module-implementation.md:57`, `.omo/drafts/metadata-sharing-module-implementation.md:73`, `DESIGN.md:624`, `DESIGN.md:703`
  Acceptance criteria (agent-executable): Linux configure/build proves `sdbus-c++` is available; fake D-Bus tests verify real adapter registration model, all listed MPRIS properties/methods/signals, file art URL formatting, no reserved track path, and no platform types leak in public API; `seriona.metadata_mpris_smoke` exists as a fixed CTest smoke entry.
  QA scenarios: happy `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build --target seriona_metadata_mpris_tests && ctest --test-dir build -R "seriona.metadata_mpris|seriona.metadata_mpris_smoke" --output-on-failure`; failure scenarios include invalid art path, stale SetPosition track id, CanControl=false, and a compile assertion that `<sdbus-c++/sdbus-c++.h>` is included only from private Linux implementation/test files. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-8.md`
  Commit: Y | `feat(metadata): implement linux mpris adapter` | Stage Linux MPRIS source/tests and evidence.

- [x] 9. Add Windows SMTC compile-isolated implementation skeleton
  What to do / Must NOT do: Add Windows-specific files behind `#ifdef _WIN32`/CMake guards that preserve the same top-level API. Code the official SMTC flow shape: host `HWND` injection object or opaque host handle, `ISystemMediaTransportControlsInterop::GetForWindow(...)` acquisition, `DisplayUpdater.Type = Music`, `MusicProperties`, `Thumbnail = RandomAccessStreamReference`, `Update()`, local artwork via `StorageFile::GetFileFromPathAsync(absPath)` + `CreateFromFile(file)`, URI artwork via `CreateFromUri` only for supported app/http(s) schemes. Mark timeline/repeat/shuffle/playback-rate as extension path requiring `ISystemMediaTransportControls2`/C++/WinRT verification. Must NOT add Windows runtime tests, require Windows SDK on Linux builds, or change public API.
  Parallelization: Wave 4 | Blocked by: 4 | Blocks: 11,12,13 | Mode: parallel-safe with 6-8 only if no shared CMake/header conflict; otherwise serialize commit
  References: `.omo/drafts/metadata-sharing-module-implementation.md:18`, `.omo/drafts/metadata-sharing-module-implementation.md:29`, `.omo/drafts/metadata-sharing-module-implementation.md:30`, `.omo/drafts/metadata-sharing-module-implementation.md:31`, `.omo/drafts/metadata-sharing-module-implementation.md:63`, `.omo/drafts/metadata-sharing-module-implementation.md:64`, `.omo/drafts/metadata-sharing-module-implementation.md:99`, `.omo/drafts/metadata-sharing-module-implementation.md:100`
  Acceptance criteria (agent-executable): Linux build proves Windows implementation is excluded; public API remains identical; static/source review confirms no Windows test requirement and no `file://` plan for Windows local artwork.
  QA scenarios: happy `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON && cmake --build build --target seriona_metadata_contract_tests`; structural checks `rg "#ifdef _WIN32|ISystemMediaTransportControls|RandomAccessStreamReference|winrt|HWND" src/metadata tests/metadata CMakeLists.txt tests/CMakeLists.txt` must find Windows-guarded implementation evidence, and `! rg "ISystemMediaTransportControls|RandomAccessStreamReference|winrt|HWND|sdbus" inc/seriona` must pass to prove no public-header platform leaks. No Windows CTest is required. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-9.md`
  Commit: Y | `feat(metadata): scaffold windows smtc adapter` | Stage Windows-specific guarded files, any CMake guard changes, structural tests/evidence only.

- [x] 10. Wire Linux MPRIS commands into unified control commands
  What to do / Must NOT do: Implement command translation from Linux MPRIS methods/property writes into `MediaControlCommand`: play, pause, play-pause, stop, next, previous, seek relative, set position with stale track guard, set volume, set repeat, set shuffle. Ensure callbacks are asynchronous/value-semantic and do not call audio/scanner. Must NOT execute media control directly in MPRIS adapter.
  Parallelization: Wave 3 | Blocked by: 8 | Blocks: 11,12,13 | Mode: sequential-only
  References: `.omo/drafts/metadata-sharing-module-implementation.md:56`, `.omo/drafts/metadata-sharing-module-implementation.md:70`, `DESIGN.md:620`, `DESIGN.md:654`, `DESIGN.md:669`
  Acceptance criteria (agent-executable): Fake MPRIS command tests record expected `MediaControlCommand` values; stale `SetPosition` ignored; `CanControl=false` prevents commands where specified.
  QA scenarios: happy `cmake --build build --target seriona_metadata_mpris_tests && ctest --test-dir build -R seriona.metadata_mpris --output-on-failure`; failure scenarios include stale SetPosition, invalid negative absolute seek, disabled capability. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-10.md`
  Commit: Y | `feat(metadata): translate mpris controls` | Stage Linux command bridge/tests/evidence.

- [x] 11. Integrate metadata module into build and app link-only path
  What to do / Must NOT do: Link `seriona_metadata` into `app/CMakeLists.txt` through `target_link_libraries(seriona PRIVATE seriona_metadata)` only, without constructing or starting `MetadataSharingService` from `app/main.cpp`. Ensure app does not start system media service until a future controller owns lifecycle. Keep `seriona` backend pure C++23 and no Qt/QML/UI. Resolve any CMake conflicts forward; do not use git rollback operations.
  Parallelization: Wave 5 | Blocked by: 9,10 | Blocks: 12,13 | Mode: sequential-only due CMake/app conflicts
  References: `app/CMakeLists.txt:1`, `CMakeLists.txt:63`, `.omo/drafts/metadata-sharing-module-implementation.md:100`, `.omo/drafts/metadata-sharing-module-implementation.md:114`, `AGENTS.md:4`
  Acceptance criteria (agent-executable): App target builds; running metadata tests remains independent; no behavior change to CLI main.
  QA scenarios: happy `cmake --build build --target seriona seriona_metadata_mpris_tests && ctest --test-dir build -R seriona.metadata --output-on-failure`; failure scenario checks `! rg "makeMetadataSharingService|MetadataSharingService|MPRIS|SMTC|sdbus|winrt" app/main.cpp` passes, proving no behavior-starting integration. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-11.md`
  Commit: Y | `build(metadata): link metadata module` | Stage app/root CMake integration and evidence.

- [x] 12. Write Chinese metadata sharing documentation
  What to do / Must NOT do: Add `docs/metadata-sharing.md` in Chinese explaining module boundaries, unified API, Linux MPRIS field mapping, Windows SMTC official flow, 1-second timeline policy, static metadata dirty policy, Windows HWND requirement, Windows current-phase no-test status, Linux test gates, and unsupported/internal fields. Must NOT present Windows runtime support as already verified.
  Parallelization: Wave 5 | Blocked by: 11 | Blocks: 13 | Mode: sequential-only after implementation details settle
  References: `.omo/drafts/metadata-sharing-module-implementation.md:32`, `.omo/drafts/metadata-sharing-module-implementation.md:33`, `.omo/drafts/metadata-sharing-module-implementation.md:34`, `.omo/drafts/metadata-sharing-module-implementation.md:35`, `.omo/drafts/metadata-sharing-module-implementation.md:36`, `.omo/drafts/metadata-sharing-module-implementation.md:110`, `AGENTS.md:11`
  Acceptance criteria (agent-executable): Doc exists, is Chinese, contains explicit Linux-first/Windows-not-tested caveat and field matrix.
  QA scenarios: happy `cmake --build build --target seriona_metadata_mpris_tests && ctest --test-dir build -R seriona.metadata --output-on-failure`; doc check `rg "Linux|Windows|1 秒|sdbus-c\+\+|MPRIS|ISystemMediaTransportControlsInterop|file://|RandomAccessStreamReference" docs/metadata-sharing.md` and `! rg "Windows.*(已验证|已测试|runtime-tested|运行测试通过)" docs/metadata-sharing.md` must pass. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-12.md`
  Commit: Y | `docs(metadata): document metadata sharing module` | Stage docs and evidence.

- [x] 13. Run full Linux verification and close implementation evidence
  What to do / Must NOT do: Run final Linux configure/build/focused/full tests, collect evidence, and ensure all Linux feature gates passed. Inspect git status/diff/log before final feature commit if evidence/status files changed; stage only intended files. Must NOT fix unrelated failures except forward fixes required by metadata module.
  Parallelization: Wave 5 | Blocked by: 12 | Blocks: final wave | Mode: sequential-only
  References: `.omo/drafts/metadata-sharing-module-implementation.md:118`, `.omo/drafts/metadata-sharing-module-implementation.md:119`, `.omo/drafts/metadata-sharing-module-implementation.md:120`, `AGENTS.md:15`
  Acceptance criteria (agent-executable): `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build`, `ctest --test-dir build -R seriona.metadata --output-on-failure`, and `ctest --test-dir build --output-on-failure` pass on Linux.
  QA scenarios: happy exact commands above; failure path records failing command, delegates fix, reruns focused then full tests. Evidence `.omo/evidence/metadata-sharing-module-implementation/task-13.md`
  Commit: Y | `test(metadata): verify metadata sharing module` | Stage final evidence and any verification-only docs/tests needed.

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [x] F1. Plan compliance audit: run `git status --short`, `git log --oneline --grep metadata`, inspect every `.omo/evidence/metadata-sharing-module-implementation/task-*.md`, and verify every todo acceptance criterion is evidenced, every Linux feature has a test pass before dependent work, every feature-level commit exists, and git write operations were limited to `git add`/`git commit`.
- [x] F2. Code quality review: run `! rg "ISystemMediaTransportControls|RandomAccessStreamReference|winrt|HWND|sdbus|sdbus-c\+\+|MPRIS|SMTC" inc/seriona` to prove no public-header platform leaks; review public API stability, ownership/lifetime, thread boundaries, callback unregistration, fake backend seams, mapper correctness, MPRIS object path validity, and Windows compile guards.
- [x] F3. Real manual QA: run `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build`, `ctest --test-dir build -R seriona.metadata --output-on-failure`, `ctest --test-dir build -R seriona.metadata_mpris_smoke --output-on-failure`, and `ctest --test-dir build --output-on-failure`; record all commands in `.omo/evidence/metadata-sharing-module-implementation/task-13.md`.
- [x] F4. Scope fidelity: run `rg "Qt|QML|mediaController queue|AudioPlaybackService|FileScannerService" src/metadata inc/seriona/metadata inc/seriona/control tests/metadata docs/metadata-sharing.md` and inspect matches; approve only if no Qt/QML/UI/full mediaController/play queue/audio realtime callback/scanner service coupling entered metadata sharing, Windows code is not claimed as runtime-tested, and top-level API is platform-identical.

## Commit strategy
- Commits are mandatory per completed feature, not per planning task if a task splits into multiple independently useful features.
- Before each commit, inspect `git status --short`, relevant `git diff`, and recent log with read-only git commands. Git writes are restricted to `git add` and `git commit`; do not use reset/revert/checkout/restore/clean/rebase/amend.
- Commit only intended feature files and evidence. If parallel workers produce overlapping edits, resolve by forward patching and then commit the resolved feature; do not use rollback operations.
- Suggested commit order:
  1. `build(metadata): add metadata scaffold and instructions`
  2. `feat(metadata): define control snapshot contracts`
  3. `feat(metadata): define service contracts`
  4. `build(metadata): add metadata module targets`
  5. `feat(metadata): map snapshots to platform fields`
  6. `feat(metadata): synchronize timeline and metadata updates`
  7. `feat(metadata): add noop service lifecycle`
  8. `feat(metadata): implement linux mpris adapter`
  9. `feat(metadata): scaffold windows smtc adapter`
  10. `feat(metadata): translate mpris controls`
  11. `build(metadata): link metadata module`
  12. `docs(metadata): document metadata sharing module`
  13. `test(metadata): verify metadata sharing module`

## Success criteria
- Linux metadata sharing path requires and links the system `sdbus-c++` dependency, builds the real MPRIS adapter, and passes focused metadata CTests plus full repo CTest.
- Public API is identical across Linux, Windows, and Noop backends.
- Linux MPRIS fake-D-Bus tests verify the real adapter seam, metadata/status/timeline/capability/command behavior, and object/property/method semantics without needing a live user D-Bus session.
- Windows SMTC code is isolated, follows Microsoft official DisplayUpdater/Thumbnail flow, requires host HWND for real desktop integration, and is not runtime-tested or overclaimed.
- 1-second timeline updates are implemented without resending static metadata every tick.
- Internal-only fields (`version`, `sampledAt`, output format, error summary) never appear in OS metadata/display/timeline payloads.
- Every Linux feature has passing evidence before dependent work proceeds.
- Every completed feature is committed with only `git add`/`git commit` as git write operations.
