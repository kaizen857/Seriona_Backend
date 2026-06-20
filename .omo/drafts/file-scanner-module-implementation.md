---
slug: file-scanner-module-implementation
status: drafting
intent: clear
pending-action: write .omo/plans/file-scanner-module-implementation.md
approach: 依据已批准的 file-scanner-module 草稿，生成一个只规划、不实现的 C++23 文件扫描模块执行计划；默认使用 SQLite3 + libxxhash + pinned/vendored wtr/watcher + 本地 TagReaderCore。
---

# Draft: file-scanner-module-implementation

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->

| id | outcome | status | evidence path |
| --- | --- | --- | --- |
| C1 | CMake/依赖：Seriona 升级 C++23，新增 scanner 库目标并接入 SQLite3/libxxhash/wtr/TagReaderCore | active | `CMakeLists.txt:8`, `app/CMakeLists.txt:1`, `tests/CMakeLists.txt:1`, `TagReader/CMakeLists.txt:7` |
| C2 | 公共契约：`inc/seriona/scanner/...` 暴露纯 C++ 服务、事件、树节点、配置和错误模型 | active | `DESIGN.md:49`, `DESIGN.md:490`, `inc/seriona/audio/audio_contracts.h:12` |
| C3 | 纯逻辑：扩展名过滤、路径规范化、hash/Merkle、树 builder、线程池、取消/进度 | active | `.omo/drafts/file-scanner-module.md:69`, `.omo/drafts/file-scanner-module.md:80` |
| C4 | SQLite cache：完整 MusicTag/完整 Lyrics/错误/root/目录 hash 持久化，WAL + 单 writer + 迁移 | active | `.omo/drafts/file-scanner-module.md:73`, `.omo/drafts/file-scanner-module.md:96`, SQLite WAL docs |
| C5 | TagReader adapter：仅调用 `TagReader::Read(path, coverExportDir)`，隔离异常和封面副作用 | active | `TagReader/include/TagReader.hpp:7`, `TagReader/include/Tag.hpp:11`, `TagReader/AGENTS.md:25` |
| C6 | Watcher adapter：wtr/watcher 事件转 dirty hint，fake watcher 覆盖业务逻辑 | active | `.omo/drafts/file-scanner-module.md:84`, wtr/watcher header/source facts |
| C7 | Scanner service：全量/启动增量/运行时增量/取消/事件发布统一编排 | active | `.omo/drafts/file-scanner-module.md:104`, `FILE_SCANNER_ANALYSIS.md:53` |
| C8 | 测试与验收：doctest + CTest，fake TagReader/fake watcher/fake clock/临时 SQLite | active | `tests/CMakeLists.txt:295`, Metis review |

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->

| assumption | adopted default | rationale | reversible? |
| --- | --- | --- | --- |
| scanner 编译组织 | 新增 `src/CMakeLists.txt` 和 `seriona_scanner` 静态库，app/tests 链接库 | 当前 app/tests 显式列源码，scanner 源码若重复列会难维护 | 是 |
| wtr/watcher 获取 | 首选 vendored single header 到 `third_party/watcher/include/wtr/watcher.hpp`，并记录 MIT license/source commit | 不要求 pacman/yay；减少 Linux 递归 watcher 代码 | 是，可改 `FetchContent` pinned commit |
| SQLite schema 版本 | 用 `schema_meta(user_version/schema_version)` + `PRAGMA user_version` 双记录 | 方便迁移与调试；缓存可重建但仍需兼容升级 | 是 |
| TagReader 并发 | 默认可在多个 worker 并发调用，但 scanner 必须通过 adapter 接口，测试用 fake；若实测不安全，adapter 可加限流 mutex | 公开 API 无状态静态函数，但有封面导出副作用；需显式 cover dir | 是 |
| CUE | `.cue` 首版忽略并记录 unsupported/ignored，不创建歌曲节点 | 用户明确当前只处理单曲 | 是 |

## Findings (cited - path:lines)

- 原批准草稿要求：全量多线程并行、SQLite 完整缓存含完整歌词、hash-first root 缓存、运行时实时更新、CUE 暂不实现、TagReader 只做标签/技术信息读取；见 `.omo/drafts/file-scanner-module.md:9`、`.omo/drafts/file-scanner-module.md:67`。
- 当前 Seriona 根 CMake 是 C++20，只有 `app`、`tests`、可选 `tools` 子目录；见 `CMakeLists.txt:5`、`CMakeLists.txt:8`、`CMakeLists.txt:35`。
- 当前 app/tests 显式列出源码；scanner 应以库目标接入，避免重复维护；见 `app/CMakeLists.txt:1`、`tests/CMakeLists.txt:8`。
- 现有测试使用 doctest + CTest，测试名注册在 `tests/CMakeLists.txt`；见 `tests/CMakeLists.txt:122`、`tests/CMakeLists.txt:295`。
- 旧项目输出播放列表树而非扁平列表，目录节点持有 children/parent/聚合统计；见 `FILE_SCANNER_ANALYSIS.md:7`、`FILE_SCANNER_ANALYSIS.md:27`、`FILE_SCANNER_ANALYSIS.md:94`。
- TagReader 公开入口只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`；见 `TagReader/AGENTS.md:11`、`TagReader/include/TagReader.hpp:7`。
- `MusicTag` 包含完整元数据、文件信息、歌词、播放统计；见 `TagReader/include/Tag.hpp:11`、`TagReader/include/Tag.hpp:24`、`TagReader/include/Tag.hpp:38`。
- `Lyrics` 是 `std::vector<Lyric>`，每行有 microseconds timestamp 与 text；见 `TagReader/include/Lyrics.hpp:11`、`TagReader/include/Lyrics.hpp:34`。
- TagReader 是 C++23，目标名 `TagReaderCore`，依赖 FFmpeg/libswscale/Iconv；见 `TagReader/CMakeLists.txt:7`、`TagReader/CMakeLists.txt:71`、`TagReader/CMakeLists.txt:109`。
- TagReader `Read(path)` 也会导出封面，显式 cover dir 会创建并拒绝 symlink；见 `TagReader/AGENTS.md:25`。
- 本机只读探测：`sqlite3=3.53.2`、`libxxhash=0.8.3`、`libuv=1.52.1`；`efsw`/`libfswatch` pkg-config 不存在。
- SQLite WAL 官方文档：读写可并发但同一时刻只有一个 writer；默认约 1000 pages 自动 checkpoint；长读事务会造成 checkpoint 饥饿。
- SQLite `busy_timeout` 是每连接 busy handler 设置；计划需每个连接设置。
- SQLite `sqlite3_wal_checkpoint_v2` 支持 PASSIVE/FULL/RESTART/TRUNCATE；PASSIVE 不等待读写者，TRUNCATE 会截断 WAL。
- xxHash 官方文档：`XXH3_128bits` 提供 128-bit 非加密高速 hash；streaming API 使用 `XXH3_createState`、`XXH3_128bits_reset`、`XXH3_128bits_update`、`XXH3_128bits_digest`，持久化建议用 canonical 表示。
- wtr/watcher C++ API 是 header-only `include/wtr/watcher.hpp`，典型 `auto watcher = watch(path, callback)`，`watch.close()` 关闭；事件字段含 `path_name`、`path_type`、`effect_type`、`effect_time`、`associated`。
- wtr/watcher 事件 `path_type::watcher` 会携带 watcher 自身 live/die/warning/error 消息；Linux 头文件中有 `w_sys_q_overflow`、`w_self_q_overflow` 等 warning 字符串，业务层必须把 watcher warning/error 当作重扫信号。

## Decisions (with rationale)

- 计划只生成实现任务，不改产品代码；实现需等用户后续 `$start-work` 或明确开始。
- 全仓提升 C++23 是第一步，原因是 TagReader 已是 C++23 且用户已批准，避免 target 标准割裂。
- Scanner 公共接口放 `inc/seriona/scanner/...`，实现放 `src/scanner/...`，不触碰音频实时路径。
- Scanner 以 `seriona_scanner` 静态库接入，tests 新增 scanner 专项 CTest，app 只链接库，不在 app CMake 重复列 scanner 源码。
- SQLite 使用 C API RAII 薄封装，WAL + busy_timeout + 单 writer + 批量事务，不引入 ORM。
- Hash 用 `XXH3_128bits` streaming + canonical hex/text 持久化；目录 hash 是排序 Merkle 聚合。
- Watcher 默认 vendored/pinned wtr/watcher，业务测试全部走 fake watcher；真实 watcher 只做可选 smoke。
- TagReader 通过 adapter 接口隔离；公共 scanner header 不暴露 TagReader 内部 include。

## Scope IN

- C++23/CMake 依赖接入与 scanner 库目标。
- Scanner 公共契约、事件、配置、错误、树节点、版本化快照。
- 扩展名初筛、路径规范化、不跟随 symlink、权限/错误记录。
- 文件 XXH3 128-bit hash、目录 Merkle hash、hash-first 缓存命中/失效。
- SQLite schema、迁移、完整 MusicTag/完整 Lyrics/错误缓存、WAL 策略。
- TagReader adapter 和 fake TagReader 测试替身。
- 线程池、有界任务队列、取消、进度节流、单 writer 归并。
- wtr/watcher adapter、事件去抖、warning/error/overflow 重扫策略、fake watcher 测试。
- 全量扫描、启动增量扫描、运行期增量扫描、手动 refresh、cancel。
- doctest/CTest 覆盖 happy/failure；所有验证由代理运行。

## Scope OUT (Must NOT have)

- 不实现 CUE 解析，不把 `.cue` 变成可播放节点；只保留未来字段/ignored 状态。
- 不引入 Qt/QML、UI、MPRIS/SMTC、SQLite 媒体库业务 UI、播放队列、歌词联网下载、封面联网抓取。
- 不在 scanner 中直接做 FFmpeg 解码/播放；标签和技术信息读取只走 TagReader adapter。
- 不依赖真实音频硬件；测试默认使用 fake TagReader/fake watcher/fake clock/临时目录/临时 SQLite。
- 不运行 `pacman`/`yay`；缺依赖只提示用户外部安装。
- 不让 worker 线程直接写 SQLite；不在实时音频回调路径执行任何 scanner 工作。

## Open questions

- 无阻塞问题。用户已确认原草稿内容无误；计划采用原草稿默认依赖策略。

## Approval gate
status: approved-for-plan
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->

用户已在 2026-06-20 明确确认 `.omo/drafts/file-scanner-module.md` 内容无误；本文件只记录正式执行计划生成过程。下一动作：写 `.omo/plans/file-scanner-module-implementation.md`，不实现代码。
