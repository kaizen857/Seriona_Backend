# file-scanner-module-implementation - Work Plan

## TL;DR (For humans)
**What you'll get:** 一个独立的高性能文件扫描模块：能并行扫描音乐目录、构建文件浏览器式播放列表树、把完整标签、嵌入歌词、外部 `.lrc` 歌词和当前生效歌词写入可变 SQLite 缓存，并在运行期监听文件夹变化做增量更新。

**Why this approach:** 用 hash-first 缓存避免重复 TagReader 解析，用扫描层处理 sidecar `.lrc` 以避免扩大 TagReader 职责，用单 writer SQLite 保证并发安全，用 vendored/pinned watcher 把 Linux 递归监听复杂度隔离在第三方库而不是写进业务代码。

**What it will NOT do:** 首版不解析 CUE、不接 UI/Qt/QML、不做播放队列/联网歌词/联网封面、不修改 TagReader 去查找 sidecar `.lrc`，也不在扫描模块里直接做 FFmpeg 播放或音频硬件访问。

**Effort:** XL
**Risk:** High - 涉及新模块、按用户决策把当前 Seriona 根 CMake 从 C++20 升到 C++23、外部 TagReader/SQLite/watcher/hash/sidecar `.lrc` 集成和多线程缓存一致性。
**Decisions to sanity-check:** scanner 目标名固定为 `seriona_scanner`；默认 watcher 用 vendored/pinned `wtr/watcher` 单头文件；SQLite/xxHash 用系统库；TagReader 从本地路径私有链接；测试默认全部 fake 化，真实 watcher 只做可选 smoke。

Your next move: 如果要开始实现，请显式发 `$start-work` 或“开始执行这个计划”；如果想先做高精度计划审查，请要求运行 Momus review。Full execution detail follows below.

---

> TL;DR (machine): XL/high-risk architecture implementation; upgrade current Seriona root CMake from C++20 to C++23 per user decision and deliver `seriona::scanner` C++23 module with parallel hash-first scan, full SQLite cache including embedded/external LRC lyrics, TagReader adapter, wtr watcher adapter, and doctest/CTest coverage.

## Scope
### Must have
- 全仓 C++23：按用户已确认决策把当前 `CMakeLists.txt:8` 的 C++20 升级到 C++23，保证现有音频模块和新增 scanner 一起构建测试；如果升级暴露现有音频编译问题，只修与标准升级直接相关的问题，不借机重构 audio。
- 模块边界：新增 `seriona::scanner`，公共头在 `inc/seriona/scanner/...`，实现放 `src/scanner/...`，不依赖 Qt/QML/UI/音频硬件。
- 构建目标：新增固定名称 `seriona_scanner` 静态库目标，app/tests 链接该目标，不让 scanner 源码在多个可执行目标中重复列出；本计划不重构现有 audio 源码为 `seriona_core`。
- 公共契约：提供扫描配置、节点/树快照、完整歌曲 payload、事件、错误码、服务接口和工厂函数。
- 并行扫描：全量扫描必须使用有界线程池，分阶段执行遍历、hash、TagReader 解析、树/DB 归并，并支持取消。
- Hash-first 缓存：用 `XXH3_128bits` streaming hash 保存文件内容 hash，用排序 Merkle 聚合目录 hash；root 路径匹配时优先 hash 比对，命中直接读缓存。
- 完整 SQLite 缓存：保存 root、目录、歌曲、完整 `MusicTag` 字段、TagReader 嵌入歌词行、外部 `.lrc` 歌词行、当前生效歌词来源/行、外部 `.lrc` 文件路径/hash/mtime、扫描错误、hash、播放统计、封面路径；缓存可变并随增量实时更新；播放统计字段在文件重扫时必须保留用户值，不能被 TagReader 默认值无条件覆盖。
- 外部 `.lrc` sidecar：扫描模块负责查找、解析、缓存和监听同目录同 basename 的 `.lrc`；默认优先级为 `ExternalLrc` > 非空 TagReader `MusicTag::lyrics()` 作为 `EmbeddedTag` > `None`，不再试图区分 TagReader 输出内部的“同步歌词/纯文本歌词”来源，因为当前 TagReader 公共类型只暴露统一 `Lyrics` 行；`.lrc` 创建/修改/删除只刷新歌词缓存和歌曲快照，不重跑 TagReader。
- TagReader 适配：只调用 `TagReader::Read(path, coverExportDir)`，逐文件捕获异常，显式传 Seriona 管理的 cover export 目录。
- 文件监听：默认使用 pinned/vendored `wtr/watcher`，事件只作为 dirty hint，去抖后重新 stat/hash/解析；watcher warning/error/overflow 触发目录/root 重扫。
- 树输出：构建真实文件系统层级的 root/目录/歌曲树，目录优先稳定排序，聚合歌曲数/时长/封面，发布版本化快照或增量事件。
- 测试：doctest + CTest，使用 fake TagReader、fake watcher、fake clock、临时目录、临时 SQLite；覆盖 happy/failure 和取消/错误路径。
### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不实现 CUE 解析；`.cue` 首版只忽略或记录 unsupported，不创建可播放分轨节点。
- 不引入 Qt/QML、UI 模型、MPRIS/SMTC、播放队列、歌词下载、联网封面抓取、媒体库 UI 业务。
- 不在 scanner 里直接做 FFmpeg 解码/播放或打开真实音频设备；标签/技术信息只走 TagReader adapter。
- 不要求 TagReader 修改 `MusicTag` 来保存外部 `.lrc` 路径；sidecar `.lrc` 是 scanner/media-library 层文件系统关系，不是音频文件内部标签。
- 不让多 worker 直接写 SQLite；SQLite 写入必须单 writer/批量事务。
- 不依赖真实音频硬件或用户媒体库做默认测试；真实 watcher 只允许可选 smoke，不作为核心验证唯一依据。
- 不运行 `pacman`/`yay`；缺系统依赖时只提示用户外部安装 `sqlite`/`xxhash` 等。
- 不覆盖或提交 `.omo/run-continuation/...` 运行时脏文件。

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: TDD where practical for pure scanner pieces; tests-after for CMake/dependency glue and watcher smoke. Framework: existing doctest + CTest.
- Evidence: each todo writes command output, generated temp fixture paths, and relevant assertions to `build/seriona-evidence/task-<N>-file-scanner-module-implementation.md`; do not write execution evidence under `.omo/`.
- Specific first: run new scanner CTest targets by `ctest --test-dir build -R 'seriona.scanner' --output-on-failure` after each scanner wave.
- Broad after confidence: run `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`, `cmake --build build`, and `ctest --test-dir build --output-on-failure`.
- Dependency probes: use CMake configure output and/or `pkg-config --modversion sqlite3 libxxhash`; if missing, stop and report external install command, do not run package managers.
- Runtime QA: all scanner tests create temp roots and temp DB files, generate text/binary fixture files, sidecar `.lrc` fixtures, and fake TagReader responses; malformed `.lrc` fixtures must cover bad timestamp, oversized file, duplicate timestamps, metadata-only file, and mixed plain/timed lines; no real audio device or user library.

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.

- Wave 1 foundation: C++23/CMake targets, public scanner contracts, pure test fixtures/fakes.
- Wave 2 pure logic: hashing, tree builder, thread pool/scheduler primitives; can run mostly without SQLite/TagReader.
- Wave 3 persistence/integration: SQLite cache and TagReader adapter, then hash-first cache reconciliation.
- Wave 4 orchestration: scanner service full/incremental/cancel/progress and watcher adapter/debounce.
- Wave 5 end-to-end hardening: app/test CMake linkage, optional real watcher smoke, full build/test sweep, docs notes if needed.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 | none |
| 2 | 1 | 3, 4, 5, 8, 9, 10, 11, 12 | 3 |
| 3 | 1 | 4, 5, 6, 7, 8, 9, 10, 11, 12 | 2 |
| 4 | 2, 3 | 8, 9, 10, 11, 12 | 5, 6, 7 |
| 5 | 2, 3 | 8, 9, 10, 11, 12 | 4, 6, 7 |
| 6 | 1, 3 | 8, 9, 10, 11, 12 | 4, 5, 7 |
| 7 | 1, 3 | 9, 10, 11, 12 | 4, 5, 6 |
| 8 | 4, 5, 6 | 9, 10, 11, 12 | none |
| 9 | 7, 8 | 10, 11, 12 | none |
| 10 | 8, 9 | 11, 12 | none |
| 11 | 10 | 12 | none |
| 12 | 11 | final verification | none |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->
- [x] 1. CMake foundation and dependency gates
  What to do / Must NOT do: Upgrade Seriona root CMake from its current C++20 to C++23 per user decision; create a fixed `seriona_scanner` static library target for scanner sources only; keep existing app/audio source listing unless a later separate plan refactors audio; add `find_package(SQLite3 REQUIRED)` and `pkg_check_modules(SERIONA_XXHASH REQUIRED IMPORTED_TARGET libxxhash)` inside a scanner dependency block after the existing FFmpeg pkg-config block and before `seriona_scanner` target creation; add developer-only configure probes `SERIONA_SCANNER_SIMULATE_MISSING_SQLITE` and `SERIONA_SCANNER_SIMULATE_MISSING_XXHASH` defaulting OFF, and when either is ON, short-circuit only the matching scanner dependency in that scanner dependency block with a deterministic scanner-specific `message(FATAL_ERROR ...)` after FFmpeg has already been found; vendor `wtr/watcher` as a pinned single header under `third_party/watcher/include/wtr/watcher.hpp` plus license/source note under `third_party/watcher/`; integrate local TagReader exactly by `add_subdirectory("/home/kaizen857/cppProject(app_and_lib)/TagReader" "${PROJECT_BINARY_DIR}/tagreader" EXCLUDE_FROM_ALL)` when `TagReaderCore` is not already a target, then link `seriona_scanner` to `TagReaderCore` privately for the production adapter only; do not expose TagReader headers from scanner public includes. Must NOT run package managers; if SQLite/xxHash missing, stop with the exact user command to run externally.
  Parallelization: Wave 1 | Blocked by: none | Blocks: all implementation todos.
  References (executor has NO interview context - be exhaustive): `CMakeLists.txt:1`, `CMakeLists.txt:8`, `CMakeLists.txt:21`, `CMakeLists.txt:35`, `app/CMakeLists.txt:1`, `tests/CMakeLists.txt:1`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:7`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/CMakeLists.txt:71`, `.omo/drafts/file-scanner-module.md:87`.
  Acceptance criteria (agent-executable): `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` configures with `CMAKE_CXX_STANDARD 23` and creates/link-checks `seriona_scanner`; configure output or CMake target inspection confirms SQLite3/libxxhash imported targets, vendored wtr include target, and private `TagReaderCore` linkage; `cmake --build build` still builds existing targets; `ctest --test-dir build --output-on-failure` keeps existing tests passing before scanner behavior lands.
  QA scenarios (name the exact tool + invocation): happy: run configure/build/ctest and write outputs to `build/seriona-evidence/task-1-file-scanner-module-implementation.md`; failure: configure throwaway build dirs with `cmake -S . -B build-missing-sqlite -DSERIONA_BUILD_TESTS=ON -DSERIONA_SCANNER_SIMULATE_MISSING_SQLITE=ON` and `cmake -S . -B build-missing-xxhash -DSERIONA_BUILD_TESTS=ON -DSERIONA_SCANNER_SIMULATE_MISSING_XXHASH=ON`, verify each configure log first finds/gets past FFmpeg, then fails in the scanner dependency block, names only the scanner dependency, tells the user to install externally, and does not attempt `pacman`/`yay`.
  Commit group suggestion only | build(scanner): add C++23 scanner dependency foundation

- [x] 2. Public scanner contracts and service facade
  What to do / Must NOT do: Add `inc/seriona/scanner/scanner_contracts.h` and `inc/seriona/scanner/file_scanner_service.h` defining pure C++ types: `ScannerConfig`, `ScannerRoot`, `ScanMode`, `ScanProgress`, `ScannerErrorCode`, `ScannerError`, `ScannerEventType`, `ScannerEvent`, `ScannerEventSink`, `PlaylistNodeKind`, `LyricsSource`, `SongMetadata`, `LyricLine`, `PlaylistNode`, `PlaylistTreeSnapshot`, `FileScannerService`, and `makeFileScannerService`. `LyricsSource` must distinguish at least `None`, `EmbeddedTag`, and `ExternalLrc`; public `SongMetadata::lyrics` is the current effective lyrics selected by that source, and `SongMetadata` must include optional external lyrics path/hash/mtime fields separately from lyric lines. Include fields for `sourceFilePath`, `offset`, `duration`, `logicalTrackId` for future CUE but do not implement CUE parsing. Define all timestamp/duration units explicitly as `std::chrono` types, not raw integers, at the public boundary. Must NOT include TagReader headers, SQLite headers, watcher headers, Qt/QML, or audio device headers in public scanner headers.
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 4, 5, 8, 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `inc/seriona/audio/audio_contracts.h:12`, `inc/seriona/audio/audio_contracts.h:160`, `inc/seriona/audio/audio_contracts.h:170`, `DESIGN.md:49`, `DESIGN.md:490`, `.omo/drafts/file-scanner-module.md:77`, `.omo/drafts/file-scanner-module.md:81`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Tag.hpp:11`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Lyrics.hpp:11`.
  Acceptance criteria (agent-executable): a new `seriona_scanner_contract_tests` target compiles public headers alone and asserts no TagReader include is needed; contract tests verify default config values, node kind distinction, future CUE fields default empty/zero, lyrics source defaults to `None`, external lyrics path/hash/mtime are optional, and event variant/type consistency.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_contract --output-on-failure`; failure: add a positive compile-only CTest target that includes scanner contracts without TagReader include directories and passes only when contracts compile without TagReader types; if a future leak appears, that target fails during build/CTest setup.
  Commit group suggestion only | feat(scanner): define public scanner contracts

- [x] 3. Scanner test harness, fakes, and temp fixtures
  What to do / Must NOT do: Add `tests/scanner/` helpers for temp directory/DB management, fake clock, fake event sink, fake TagReader adapter, fake watcher, deterministic fixture file writer, deterministic `.lrc` fixture writer, and scanner assertion helpers. Keep helpers scanner-specific unless genuinely reusable. Must NOT use real audio hardware, real user media folders, or network.
  Parallelization: Wave 1 | Blocked by: 1 | Blocks: 4, 5, 6, 7, 8, 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `tests/CMakeLists.txt:122`, `tests/CMakeLists.txt:295`, `third_party/doctest/doctest.h`, `.omo/drafts/file-scanner-module.md:93`, `AGENTS.md:11` for fake-backend/no-real-hardware testing policy.
  Acceptance criteria (agent-executable): helper-only test target proves temp roots clean up, fake TagReader can return success/throw/slow result, fake watcher can emit create/modify/destroy/rename/watcher-warning events for audio and `.lrc` paths, fake clock advances deterministically, and `.lrc` fixture helper writes valid/invalid timed lyric files.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_test_harness --output-on-failure`; failure: fake TagReader configured to throw must be observable by tests without terminating the process.
  Commit group suggestion only | test(scanner): add scanner fakes and fixtures

- [x] 4. Path classification, sidecar LRC parsing, audio candidate filtering, and symlink policy
  What to do / Must NOT do: Implement scanner-local path utilities: root canonicalization, relative UTF-8 serialization, stable display names, extension whitelist from `DESIGN.md:517-525` as first pass, explicit exclusion of video/common containers listed in `DESIGN.md:525`, ignored `.cue` status, `.lrc` sidecar candidate detection, unsupported/non-regular/permission-denied error records, and default no-follow-symlink traversal. Implement scanner-local `.lrc` parser for UTF-8 text files: ignore metadata tags like `[ar:]`/`[ti:]`, accept one or more `[mm:ss.xx]` timestamps per lyric line, normalize CRLF/LF, trim lyric text, sort by timestamp then text, deduplicate exact timestamp+text pairs, cap file bytes and line count using scanner config defaults, and return structured parse errors without throwing through scan orchestration. TagReader success remains the final audio confirmation; because current TagReader does not expose “no video stream”, define an internal `AudioCandidateVerifier` seam whose first implementation is `TagReaderOnlyAudioCandidateVerifier` and records video-stream exclusion as future hardening, but do not implement a second FFmpeg probe in this plan. Must NOT open FFmpeg directly or treat extension whitelist as sufficient for入库.
  Parallelization: Wave 2 | Blocked by: 2, 3 | Blocks: 8, 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `DESIGN.md:517`, `DESIGN.md:527`, `.omo/drafts/file-scanner-module.md:78`, `.omo/drafts/file-scanner-module.md:79`, `.omo/drafts/file-scanner-module.md:99`.
  Acceptance criteria (agent-executable): scanner path/LRC tests cover directory root, single-file root, supported extensions, uppercase extensions, `.cue` ignored/unsupported, `.lrc` recognized as sidecar candidate but not song node, symlink not followed, permission/stat failures recorded, stable relative path ordering, multi-timestamp `.lrc` lines expanding into multiple `LyricLine`s, metadata-only `.lrc` resolving to empty lyrics, duplicate timestamp+text deduplication, malformed timestamps producing a recoverable lyrics parse error, and oversized `.lrc` producing a bounded error.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_paths --output-on-failure`; failure: create an unreadable or vanished fixture path during traversal and assert a `ScannerError` is emitted while the scan continues; create malformed/oversized `.lrc` fixtures and assert parse errors are recorded without aborting the scan.
  Commit group suggestion only | feat(scanner): add path filtering and traversal policy

- [x] 5. XXH3 file hash and directory Merkle hash
  What to do / Must NOT do: Implement `src/scanner/hash/...` using `XXH3_128bits` streaming for file content; store hashes in canonical portable form; implement deterministic directory Merkle hash over sorted child tuples containing relative name, node type, size, mtime normalization, and child hash. The same file-hash utility must be usable for external `.lrc` content hash, but `.lrc` hash changes must invalidate only lyrics metadata, not audio file identity. Add bounded I/O chunk size and cancellation checks. Must NOT use cryptographic hash or mtime-only shortcut for cache identity.
  Parallelization: Wave 2 | Blocked by: 2, 3 | Blocks: 8, 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `.omo/drafts/file-scanner-module.md:75`, `.omo/drafts/file-scanner-module.md:95`, installed `xxhash.h` API facts verified by system header/pkg-config during implementation: `XXH3_createState`, `XXH3_128bits_reset`, `XXH3_128bits_update`, `XXH3_128bits_digest`, `XXH128_canonicalFromHash`.
  Acceptance criteria (agent-executable): hash tests prove identical content has identical hash despite mtime changes, content mutation changes hash, `.lrc` content mutation changes sidecar hash without changing paired audio hash, directory child rename/add/delete changes Merkle hash, sorted traversal is deterministic, cancellation stops long hash without corrupt result.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_hash --output-on-failure`; failure: simulate file disappearing mid-hash and assert a recoverable scanner error, not process abort.
  Commit group suggestion only | feat(scanner): add XXH3 hash and Merkle hashing

- [x] 6. Playlist tree builder and snapshot publisher
  What to do / Must NOT do: Implement mutable internal builder nodes and immutable/versioned public `PlaylistTreeSnapshot`; support directory/song nodes, weak parent or stable node IDs without shared_ptr cycles, directory-first stable sort, aggregation of song count/duration, cover fallback, external `.lrc` lyrics metadata attached to the paired song node, and empty-directory pruning policy matching design. `.lrc` files must never appear as tree nodes. Must NOT expose mutable internal tree across threads.
  Parallelization: Wave 2 | Blocked by: 1, 3 | Blocks: 8, 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `FILE_SCANNER_ANALYSIS.md:7`, `FILE_SCANNER_ANALYSIS.md:27`, `FILE_SCANNER_ANALYSIS.md:94`, `.omo/drafts/file-scanner-module.md:81`, `.omo/drafts/file-scanner-module.md:82`.
  Acceptance criteria (agent-executable): tree tests build nested directories/songs, attach external `.lrc` metadata to the matching song, verify `.lrc` files do not appear as nodes, verify parent access does not create ownership cycles, directory-first sorting is stable, aggregate totals match child metadata, published snapshot remains unchanged after builder mutation.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_tree --output-on-failure`; failure: mutate builder after publishing and assert old snapshot data is unchanged.
  Commit group suggestion only | feat(scanner): build playlist tree snapshots

- [x] 7. Thread pool, bounded queues, cancellation, and progress throttling
  What to do / Must NOT do: Implement scanner-local `ThreadPool`/task scheduler with configurable worker counts, bounded queues/backpressure, stop token/cancel checks, exception capture, result fan-in, and progress event throttling by time/count. Must NOT use this pool in the miniaudio realtime callback or audio playback path.
  Parallelization: Wave 2 | Blocked by: 1, 3 | Blocks: 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `.omo/drafts/file-scanner-module.md:71`, `.omo/drafts/file-scanner-module.md:80`, `AGENTS.md` realtime audio constraints, `inc/seriona/audio/audio_contracts.h:168` event sink pattern.
  Acceptance criteria (agent-executable): scheduler tests prove all tasks complete, exceptions are reported, cancellation prevents queued tasks from starting, progress events are throttled, and queue limit blocks/rejects predictably without busy spin.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_scheduler --output-on-failure`; failure: one worker task throws and scan continues collecting the error without deadlock.
  Commit group suggestion only | feat(scanner): add bounded scan scheduler

- [x] 8. SQLite cache layer with full metadata and migration
  What to do / Must NOT do: Implement `src/scanner/cache/...` RAII SQLite C API wrapper, schema migration via `PRAGMA user_version` and a `schema_meta` row, prepared statements, `PRAGMA journal_mode=WAL`, `PRAGMA busy_timeout=<configured>`, `PRAGMA foreign_keys=ON`, root/directories/songs/lyrics/errors tables, indices, full `MusicTag` field persistence, separate embedded lyric row persistence, separate external `.lrc` lyric row persistence, effective lyric source persistence, external `.lrc` path/hash/mtime persistence, file hash and directory hash, single writer transaction API, startup load, delete pruning, and checkpoint helper using `sqlite3_wal_checkpoint_v2`. Store `playCount`, `rating`, and `lastPlayed` as user data columns that are preserved across metadata refresh unless an explicit user-data update API changes them. Must NOT put complete lyrics in an omitted summary-only field; store lyrics in a separate `lyrics` table keyed by song id, lyric kind (`embedded`/`external`), and ordered line index so directory browsing can avoid loading large lyrics.
  Parallelization: Wave 3 | Blocked by: 4, 5, 6 | Blocks: 9, 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `.omo/drafts/file-scanner-module.md:73`, `.omo/drafts/file-scanner-module.md:96`, `.omo/drafts/file-scanner-module.md:98`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Tag.hpp:46`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Lyrics.hpp:45`, SQLite WAL docs, SQLite `busy_timeout` docs, SQLite `sqlite3_wal_checkpoint_v2` docs.
  Acceptance criteria (agent-executable): cache tests create a temp DB, migrate schema, save/load a root tree with complete metadata, embedded lyric lines, external `.lrc` lyric lines, and effective lyric source, persist and reload `lyrics_source`, `lyrics_file_path`, `lyrics_file_hash`, and `lyrics_file_mtime`, prove an external `.lrc` override does not delete cached embedded lyrics, prove deleting `.lrc` can restore embedded lyrics from cache without TagReader, update changed song hash, update changed `.lrc` hash without changing audio hash, delete missing subtree, preserve scan errors, preserve user play stats across metadata refresh, run passive checkpoint, and verify no worker API writes without writer transaction.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_cache --output-on-failure`; failure: open a second connection/transaction to force busy handling and assert configured timeout/error mapping is deterministic.
  Commit group suggestion only | feat(scanner): persist full scanner cache in SQLite

- [x] 9. TagReader adapter and metadata mapping
  What to do / Must NOT do: Implement an internal adapter interface plus production `TagReaderMetadataReader` that calls only `TagReader::Read(path, coverExportDir)`; map every available `MusicTag` field and every embedded `Lyrics` line into scanner internal metadata as embedded lyrics, setting `lyrics_source=EmbeddedTag` only when the embedded lyrics vector is non-empty and no service-level external `.lrc` override has been applied; map TagReader `playCount`/`rating`/`lastPlayed` only for first import or when no cache user-data row exists, because SQLite cache preservation rules own later user-data values; convert TagReader microsecond integer durations/offsets to `std::chrono::microseconds`, preserve `std::filesystem::file_time_type` and `std::chrono::system_clock::time_point` semantics deterministically, and document conversions in tests; capture exceptions into `ScannerError`; ensure cover export dir is configured and created under Seriona cache ownership. Must NOT call TagReader internals, swallow cover cache failures silently, leak TagReader includes into public scanner headers, or modify TagReader/MusicTag to store external `.lrc` paths.
  Parallelization: Wave 3 | Blocked by: 7, 8 | Blocks: 10, 11, 12.
  References (executor has NO interview context - be exhaustive): `/home/kaizen857/cppProject(app_and_lib)/TagReader/AGENTS.md:11`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/AGENTS.md:25`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/TagReader.hpp:7`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Tag.hpp:11`, `/home/kaizen857/cppProject(app_and_lib)/TagReader/include/Lyrics.hpp:11`, `.omo/drafts/file-scanner-module.md:47`, `.omo/drafts/file-scanner-module.md:48`.
  Acceptance criteria (agent-executable): adapter tests with fake TagReader map title/genre/artist/album/albumArtist/composer/year/track/disc/embedded lyrics/path/cover/duration/offset/lastModified/sampleRate/bitDepth/bitRate/channels/format and initial playCount/rating/lastPlayed import; tests prove cached user play stats override TagReader defaults on refresh; embedded lyrics map to embedded lyric storage and effective `EmbeddedTag` only when no external `.lrc` override is applied by scanner service; throwing fake reader produces per-file error and scan continues; compile test proves public scanner contract has no TagReader include dependency.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_tagreader --output-on-failure`; failure: fake reader throws on one file among many and final scan includes other songs plus one error.
  Commit group suggestion only | feat(scanner): adapt TagReader metadata safely

- [ ] 10. Scanner service full scan and hash-first startup reconciliation
  What to do / Must NOT do: Implement `FileScannerService` orchestration for `scanRoot`, `refresh`, `cancel`, and `querySnapshot`; support directory and single-file roots; run traversal -> audio hash + `.lrc` sidecar hash -> cache compare -> metadata read for new/changed/missing audio -> sidecar `.lrc` parse for new/changed/missing lyrics -> effective lyrics selection -> single-writer cache update -> tree publish -> events. On root path match, unchanged audio hashes must load complete metadata and embedded lyrics from cache without calling TagReader; unchanged `.lrc` hashes must load external lyrics from cache; changed/new audio files call TagReader and update metadata/cache; changed/new/deleted `.lrc` files update only external lyrics rows, effective lyrics source, and song snapshot; deleted `.lrc` files must restore cached embedded lyrics or `None` without TagReader; deleted audio files are removed. Must NOT block forever on cancellation, emit unbounded progress events, or treat cache as authority when filesystem contradicts it.
  Parallelization: Wave 4 | Blocked by: 8, 9 | Blocks: 11, 12.
  References (executor has NO interview context - be exhaustive): `.omo/drafts/file-scanner-module.md:76`, `.omo/drafts/file-scanner-module.md:80`, `.omo/drafts/file-scanner-module.md:82`, `.omo/drafts/file-scanner-module.md:83`, `FILE_SCANNER_ANALYSIS.md:53`, `DESIGN.md:541`, `DESIGN.md:550`.
  Acceptance criteria (agent-executable): service tests prove first scan reads all supported audio files, parses matching `.lrc`, and writes cache; second scan with unchanged audio and `.lrc` hashes does not invoke fake TagReader or reparse `.lrc`; modified audio file triggers one TagReader re-read; modified `.lrc` triggers only lyrics reparse and no TagReader call; new `.lrc` overrides embedded lyrics while preserving cached embedded lyrics; deleted `.lrc` falls back to cached embedded lyrics or `None` without TagReader; new audio file inserts; deleted audio file prunes; single-file root works and can discover same-basename `.lrc` next to the file; cancellation emits canceled state and leaves DB consistent.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_service --output-on-failure`; failure: fake TagReader fails for one changed audio file and service records error while preserving previous cached/other successful songs; malformed `.lrc` records a lyrics error and falls back to embedded lyrics without failing the whole scan.
  Commit group suggestion only | feat(scanner): orchestrate hash-first file scans

- [x] 11. wtr/watcher adapter, debounce, and runtime incremental updates
  What to do / Must NOT do: Add watcher abstraction and production `WtrFolderWatcher` around vendored `wtr::watch`; map create/modify/destroy/rename/owner/other and watcher live/die/warning/error messages; debounce events per path/root; treat all events as dirty hints and always re-check filesystem/hash; on `.lrc` create/modify/destroy/rename, schedule lyrics-only reconciliation for affected basename when possible and root/directory reconciliation when ambiguous; on warning/error/overflow-like watcher messages trigger directory/root rescan; integrate with service `startWatching`/`stopWatching`. Must NOT rely on watcher events as authoritative, parse external CLI output, require real watcher in unit tests, or call TagReader just because a `.lrc` changed.
  Parallelization: Wave 4 | Blocked by: 10 | Blocks: 12.
  References (executor has NO interview context - be exhaustive): `.omo/drafts/file-scanner-module.md:84`, `.omo/drafts/file-scanner-module.md:93`, `.omo/drafts/file-scanner-module.md:97`, wtr/watcher README/source facts: `watch(path, callback)`, `watch.close()`, `event::path_name`, `event::path_type::watcher`, `event::effect_type`, `event::associated`, Linux warning messages `w_sys_q_overflow`/`w_self_q_overflow`.
  Acceptance criteria (agent-executable): fake watcher tests prove create/modify/destroy/rename events schedule debounced rescans, associated rename marks both old/new paths dirty, `.lrc` modify schedules lyrics-only reconciliation and no TagReader call, `.lrc` delete falls back to embedded/none, watcher warning/error triggers root rescan, stopWatching closes watcher and no callbacks race after destruction.
  QA scenarios (name the exact tool + invocation): happy: `ctest --test-dir build -R seriona.scanner_watcher --output-on-failure`; failure: fake watcher emits overflow/watcher error and service performs root reconciliation rather than trusting partial events.
  Commit group suggestion only | feat(scanner): add folder watcher incremental scans

- [x] 12. Integration hardening, optional smoke, docs, and full verification
  What to do / Must NOT do: Wire app target to scanner library without changing app behavior beyond linkability; add scanner CTest groups in `tests/CMakeLists.txt`; add concise Chinese docs if public usage/configuration needs explanation, including sidecar `.lrc` matching/priority/cache invalidation rules; add optional real watcher smoke test disabled by default or guarded by env/CMake option; run full configure/build/CTest and record evidence. Must NOT make optional smoke mandatory, add formatter/linter that does not exist, or fix unrelated product bugs.
  Parallelization: Wave 5 | Blocked by: 11 | Blocks: final verification.
  References (executor has NO interview context - be exhaustive): `app/CMakeLists.txt:1`, `tests/CMakeLists.txt:295`, `docs/audio-player.md`, `AGENTS.md` Chinese docs instruction, `.omo/drafts/file-scanner-module.md:100`.
  Acceptance criteria (agent-executable): `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` configures; `cmake --build build` succeeds; `ctest --test-dir build -R 'seriona.scanner' --output-on-failure` passes including external `.lrc` cases; `ctest --test-dir build --output-on-failure` passes or only unrelated pre-existing failures are documented with evidence; docs mention CUE deferred, dependency policy, and external `.lrc` handling if docs are added.
  QA scenarios (name the exact tool + invocation): happy: full configure/build/scanner CTest/all CTest with logs in `build/seriona-evidence/task-12-file-scanner-module-implementation.md`; failure: run optional smoke disabled by default and verify normal tests still pass without real watcher or media hardware.
  Commit group suggestion only | test(scanner): verify scanner integration end to end

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [ ] F1. Plan compliance audit: read implementation diff and confirm every todo acceptance criterion is evidenced; reject if any product code edits fall outside scanner/CMake/tests/docs scope or if `.omo/run-continuation` is included.
- [ ] F2. Code quality review: review C++23 code for ownership cycles, data races, SQLite statement lifetime, exception safety, cancellation, backpressure, sidecar `.lrc` parser bounds/error handling, and public header dependency leaks.
- [ ] F3. Real manual QA: run agent-executed full build/test commands and, if optional smoke is enabled and environment supports it, create a temp watched dir and verify create/modify/delete updates without user media/hardware.
- [ ] F4. Scope fidelity: verify no Qt/QML/UI/MPRIS/SMTC/play queue/CUE parsing/lyrics download/cover download/FFmpeg playback logic entered scanner, and verify external `.lrc` support did not require TagReader/MusicTag sidecar path changes.

## Commit strategy
- Commit lines in todos are grouping suggestions only, not permission to commit; prefer 6-8 atomic commits in this order if and only if the user explicitly requests commits: build foundation; public contracts/tests; pure scanner logic including `.lrc` sidecar detection; SQLite cache; TagReader adapter; service/watcher; integration verification/docs.
- Do not commit automatically unless user explicitly requests; if committing, inspect `git status`, `git diff`, and recent log first, and never stage `.omo/run-continuation` runtime files.
- Commit messages should use concise conventional style, e.g. `feat(scanner): define scanner contracts`.

## Success criteria
- `seriona::scanner` builds as a pure C++23 backend module with no Qt/QML/UI/audio-hardware dependency.
- Full scan uses bounded parallelism and supports cancellation/progress/error events.
- SQLite cache stores complete metadata, separate complete embedded lyrics, separate complete external `.lrc` lyrics, effective lyrics source, external `.lrc` path/hash/mtime, uses WAL/busy timeout/single writer, and supports hash-first startup reuse.
- Unchanged root scan loads cached data without TagReader calls; new/changed/deleted audio files update cache and tree correctly; new/changed/deleted `.lrc` files refresh only lyrics state and do not force TagReader calls.
- Runtime watcher events trigger debounced filesystem reconciliation and tolerate warning/error/overflow by rescan.
- CUE remains explicitly deferred/ignored, with future-compatible fields but no parser behavior.
- Scanner-specific CTests and full repo CTests pass with agent-recorded evidence.
