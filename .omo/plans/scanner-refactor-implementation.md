# scanner-refactor-implementation - Work Plan

## TL;DR (For humans)
<!-- Fill this LAST, after the detailed plan below is written, so it summarizes the REAL plan. -->
<!-- Plain English for a non-engineer: NO file paths, NO todo numbers, NO wave/agent/tool names. -->

**What you'll get:** 音乐库扫描速度提升 8-47 倍（热扫描从 4 分钟降到 5 秒内），文件移动后播放统计不再丢失，支持智能增量扫描（只处理变化的文件）。数据库自动升级，无需手动操作。

**Why this approach:** 用文件属性（路径、大小、修改时间）代替读取整个文件来判断变化，节省 99% 的磁盘读取。使用 32 个后台线程并行处理，但通过限流保护外部库安全（最多 4 个同时读取标签）。给每首歌分配稳定 ID 和位置 ID，移动文件时只更新位置，播放次数等统计保留在稳定 ID 上。

**What it will NOT do:** 不会改变现有的应用接口，不会破坏其他模块（音频、控制、元数据），不会引入 Qt/界面依赖，不会要求用户重新配置任何东西。

**Effort:** Large (9 天，30 个任务，4 个阶段)
**Risk:** Medium - 数据库自动迁移需谨慎测试，外部标签读取库的线程安全需验证
**Decisions I made for you:** 
- 使用成熟的第三方线程池库（BS::thread_pool）而非手写
- 全 CPU 线程数 worker（32 个）但用限流保护标签库（最多 4 并发）
- 完全抛弃文件内容 hash，改用元数据（路径+大小+时间）
- 数据库从 V2 自动升级到 V3，有自动备份和回滚
- 默认开启增量扫描，用户可强制全量

Your next move: 批准后开始执行，按 Phase 1→2→3→4 顺序进行，每个 Phase 完成后可查看阶段性成果。全部完成后会进行最终验证（代码质量、性能测试、真实场景）并等待你的确认。

---

> TL;DR (machine): Large effort, Medium risk. Delivers 8-47x scanner performance via metadata hash + concurrent worker pool + incremental scan. Schema V2→V3 auto-migration, dual-ID system preserves user stats on file move.

## Scope
### Must have
- SQLite Schema V3（content + locations 双表，支持双 ID 系统）
- 双 ID 计算：`computeContentId()`（稳定）和 `computeLocationId()`（易变）
- Metadata hash 替代全文件 hash（path + size + mtime）
- Schema 迁移脚本（v2 → v3，自动备份和回滚）
- BS::thread_pool 集成（CMake FetchContent）
- `ScannerWorkerPool` 实现（全 CPU worker + TagReader semaphore 限制）
- 增量扫描三阶段（检测删除、新增、变化）
- Directory tree hash 计算
- 性能统计（Phase breakdown + cumulative CPU time）
- 单元测试覆盖所有核心功能
- ThreadSanitizer 验证无数据竞争
- 实际音乐库测试（1000/5000/10000 首）

### Must NOT have (guardrails, anti-slop, scope boundaries)
- 不得破坏现有公共 API 契约（`scanner_contracts.h`、`file_scanner_service.h`）
- 不得将 TagReader、SQLite、BS::thread_pool 类型泄漏到公共头文件
- 不得在 worker 线程中直接调用 `publishEvent()` 或写 SQLite
- 不得让事件发布顺序乱序（必须保持确定性）
- 不得在未验证 TagReader 线程安全前放开并发限制
- 不得引入 C++23 coroutine 或 async I/O runtime
- 不得实现完全并行目录遍历（ripgrep 风格 work-stealing）
- 不得修改音频、控制、metadata 模块代码
- 不得添加 Qt/QML/UI 依赖

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- Test decision: **tests-after** + doctest（项目现有框架）
- 每个任务完成后立即编写单元测试，验证后再进入下一任务
- Phase 2 完成后运行 ThreadSanitizer 验证无数据竞争
- Phase 4 运行实际音乐库性能测试（冷/热/增量扫描）
- Evidence: .omo/evidence/task-<N>-scanner-refactor-implementation.<ext>
  - 单元测试输出
  - 性能统计报告
  - ThreadSanitizer 报告
  - Schema 迁移验证日志

## Execution strategy
### Parallel execution waves
> Target 5-8 todos per wave. Fewer than 3 (except the final) means you under-split.

**Wave 1: Phase 1.1 - Schema 基础（3 个任务）** - 并行
- 任务 1-3：双 ID 计算函数、SQLite Schema V3 建表、xxHash 依赖

**Wave 2: Phase 1.2 - Cache 接口（4 个任务）** - 需要 Wave 1
- 任务 4-7：SQLiteCacheV3 类实现、Content/Location CRUD、索引

**Wave 3: Phase 1.3 - 迁移脚本（3 个任务）** - 需要 Wave 2
- 任务 8-10：Schema 迁移、备份回滚、单元测试

**Wave 4: Phase 2.1 - BS::thread_pool 集成（2 个任务）** - 并行（独立于 Wave 3）
- 任务 11-12：CMake 配置、编译验证

**Wave 5: Phase 2.2 - Worker Pool 核心（5 个任务）** - 需要 Wave 4
- 任务 13-17：ScannerWorkerPool 类、任务提交、结果收集、统计

**Wave 6: Phase 2.3 - Orchestrator 集成（4 个任务）** - 需要 Wave 2, 5
- 任务 18-21：reconcileRoot 改造、并发处理、事件顺序、ThreadSanitizer

**Wave 7: Phase 3.1 - 增量扫描（5 个任务）** - 需要 Wave 6
- 任务 22-26：扫描模式决策、三阶段检测、directory hash、集成

**Wave 8: Phase 4 - 性能调优（4 个任务）** - 需要 Wave 7
- 任务 27-30：实际测试、并发压测、性能报告、配置调优

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1-3 | - | 4-7 | Each other |
| 4-7 | 1-3 | 8-10, 18-21 | Each other |
| 8-10 | 4-7 | - | Each other |
| 11-12 | - | 13-17 | 1-10 |
| 13-17 | 11-12 | 18-21 | Each other |
| 18-21 | 4-7, 13-17 | 22-26 | Each other |
| 22-26 | 18-21 | 27-30 | Each other |
| 27-30 | 22-26 | - | Each other |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->

---
### **Phase 1: 数据库重构（3 天，任务 1-10）**
---

- [x] **1. 实现双 ID 计算函数（`computeContentId` 和 `computeLocationId`）**
  
  **What to do**: 创建 `inc/seriona/scanner/song_identity.h` 和 `src/scanner/song_identity.cpp`，实现两个核心 ID 计算函数：
  - `computeContentId(duration_ms, title, artist)`: 使用 xxHash64 计算稳定内容 ID
  - `computeLocationId(path, fileSize, mtime)`: 使用 xxHash64 计算易变位置 ID
  - `normalizeForId(text)`: 文本归一化（lowercase + trim）
  
  **Must NOT do**: 
  - 不得在此引入 SQLite 或 TagReader 依赖
  - 不得在公共头文件中包含 xxhash.h（使用前向声明或实现文件中包含）
  
  **Parallelization**: Wave 1 | Blocked by: - | Blocks: 4-7
  
  **References**:
  - `docs/optimal-scanner-architecture.md:1043-1078` - 双 ID 计算接口定义
  - `docs/metadata-based-content-hash.md` - Metadata hash 方案完整说明
  - `inc/seriona/scanner/hash_utils.h` - 现有 hash 工具参考
  
  **Acceptance criteria**:
  ```bash
  # 编译成功
  cmake --build build --target seriona_scanner
  
  # 单元测试通过
  ctest --test-dir build -R 'seriona.scanner.song_identity' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 使用 doctest 测试 `computeContentId("Song", "Artist", 180000)` 返回一致的 64 字符十六进制字符串
  - Happy: 相同输入多次调用返回相同 ID
  - Happy: `computeLocationId("/path/to/song.mp3", 5242880, <mtime>)` 返回有效 ID
  - Failure: 空路径、零大小、无效 mtime 返回空字符串或抛出异常
  - Failure: `normalizeForId("")` 返回空字符串
  - Evidence: `.omo/evidence/task-1-song-identity-test.txt`
  
  **Commit**: Y | `feat(scanner): add dual ID computation (content_id + location_id)`

---

- [x] **2. 创建 SQLite Schema V3 建表 SQL 和版本管理**
  
  **What to do**: 在 `src/scanner/cache/` 中创建 `schema_v3.sql`（或内嵌到 C++ 字符串），定义 5 个表：
  - `content` 表（content_id PK，音频元数据 + 用户统计）
  - `locations` 表（location_id PK，file_path UNIQUE，外键 content_id）
  - `lyrics` 表（location_id FK，kind，line_index，timestamp_ms，text）
  - `scan_roots` 表（root_path PK，directory_tree_hash，扫描状态）
  - `scan_errors` 表（id AUTOINCREMENT，root_path FK，错误信息）
  - 所有必要的索引（idx_content_album, idx_content_artist, idx_locations_content 等）
  - Schema version 设置为 3（PRAGMA user_version=3）
  
  **Must NOT do**:
  - 不得修改现有 Schema V2 的表结构（保持向后兼容）
  - 不得在此任务中实现迁移逻辑
  
  **Parallelization**: Wave 1 | Blocked by: - | Blocks: 4-7
  
  **References**:
  - `docs/optimal-scanner-architecture.md:88-231` - Schema V3 完整定义
  - `inc/seriona/scanner/cache/sqlite_scanner_cache.h` - 现有 cache 接口
  - `src/scanner/cache/sqlite_scanner_cache.cpp` - 现有实现参考
  
  **Acceptance criteria**:
  ```bash
  # SQL 语法验证（使用 sqlite3 命令行）
  sqlite3 /tmp/test_schema_v3.db < src/scanner/cache/schema_v3.sql
  sqlite3 /tmp/test_schema_v3.db "PRAGMA user_version;" # 输出 3
  sqlite3 /tmp/test_schema_v3.db ".schema" | grep -E "(content|locations|lyrics|scan_roots|scan_errors)"
  ```
  
  **QA scenarios**:
  - Happy: 使用 sqlite3 执行建表 SQL，所有表和索引创建成功
  - Happy: 验证外键约束存在（`PRAGMA foreign_keys=ON; INSERT INTO locations ...` 无效 content_id 失败）
  - Happy: 验证 UNIQUE 约束（locations.file_path 重复插入失败）
  - Failure: 缺少必要列时建表失败
  - Evidence: `.omo/evidence/task-2-schema-v3-test.txt`
  
  **Commit**: Y | `feat(scanner): define SQLite Schema V3 with dual-ID tables`

---

- [x] **3. 添加 xxHash 依赖到 CMake 构建系统**
  
  **What to do**: 修改 `CMakeLists.txt`，使用 `pkg_check_modules` 或 `find_package` 添加 libxxhash 依赖：
  - 在 scanner 库相关的 CMake 配置中添加 xxhash 查找
  - 链接 `seriona_scanner` 到 xxhash
  - 添加编译时检查（SERIONA_SCANNER_SIMULATE_MISSING_XXHASH 选项已存在，确保正确处理）
  
  **Must NOT do**:
  - 不得破坏现有构建配置
  - 不得在非 scanner 模块引入 xxhash 依赖
  
  **Parallelization**: Wave 1 | Blocked by: - | Blocks: 4-7
  
  **References**:
  - `CMakeLists.txt:1-50` - 现有依赖配置示例
  - `CMakeLists.txt:8` - SERIONA_SCANNER_SIMULATE_MISSING_XXHASH 选项
  
  **Acceptance criteria**:
  ```bash
  # 配置成功（假设系统已安装 libxxhash）
  cmake -S . -B build -DSERIONA_BUILD_TESTS=ON
  
  # 编译成功
  cmake --build build --target seriona_scanner
  ```
  
  **QA scenarios**:
  - Happy: 系统安装 libxxhash 时配置和编译成功
  - Failure: `cmake -DSERIONA_SCANNER_SIMULATE_MISSING_XXHASH=ON` 时配置失败并给出清晰错误提示
  - Evidence: `.omo/evidence/task-3-xxhash-cmake.txt`
  
  **Commit**: Y | `build(scanner): add libxxhash dependency for metadata hash`

---

- [x] **4. 实现 `SQLiteCacheV3` 类基础结构和连接管理**
  
  **What to do**: 创建 `inc/seriona/scanner/cache/sqlite_cache_v3.h` 和 `src/scanner/cache/sqlite_cache_v3.cpp`：
  - 定义 `SQLiteCacheV3` 类（类似现有 `SQLiteScannerCache`）
  - 构造函数：接受 `ScannerCacheConfig`，打开数据库，初始化 Schema V3
  - 检查 `PRAGMA user_version`，如果为 0 则建表，如果为 3 则直接使用
  - 实现 `schemaVersion()` 和 `journalMode()` 查询方法
  - Writer transaction 机制（复用现有 `WriterTransaction` 设计）
  
  **Must NOT do**:
  - 不得在此任务中实现 CRUD 操作（留给任务 5-7）
  - 不得实现迁移逻辑（留给任务 8）
  
  **Parallelization**: Wave 2 | Blocked by: 1-3 | Blocks: 8-10, 18-21
  
  **References**:
  - `inc/seriona/scanner/cache/sqlite_scanner_cache.h:78-120` - 现有 SQLiteScannerCache 接口
  - `docs/optimal-scanner-architecture.md:1071-1119` - SQLiteCacheV3 接口定义
  
  **Acceptance criteria**:
  ```bash
  cmake --build build --target seriona_scanner
  # 单元测试验证数据库创建
  ctest --test-dir build -R 'seriona.scanner.cache_v3_basic' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 构造函数创建新数据库，schema version = 3
  - Happy: 打开现有 V3 数据库，不重建表
  - Failure: 打开 V2 数据库（user_version=2）时不自动迁移（抛出错误或返回状态）
  - Evidence: `.omo/evidence/task-4-cache-v3-basic.txt`
  
  **Commit**: Y | `feat(scanner): implement SQLiteCacheV3 base structure`

---

- [x] **5. 实现 `SQLiteCacheV3` 的 Content 表 CRUD 操作**
  
  **What to do**: 在 `SQLiteCacheV3` 类中实现 content 表操作：
  - `upsertContent(contentId, metadata)`: INSERT OR REPLACE content 记录
  - `loadContent(contentId)`: 查询单个 content
  - `updateUserStats(contentId, userStats)`: 更新 play_count、rating、last_played
  - SQL prepared statements 准备和复用
  
  **Must NOT do**:
  - 不得在此实现 locations 表操作
  - 不得破坏 writer transaction 机制
  
  **Parallelization**: Wave 2 | Blocked by: 1-3 | Blocks: 8-10, 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:90-125` - content 表定义
  - `src/scanner/cache/sqlite_scanner_cache.cpp` - 现有 SQL 操作示例
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.cache_v3_content' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: upsertContent 插入新记录，loadContent 返回正确数据
  - Happy: upsertContent 更新现有记录（相同 content_id）
  - Happy: updateUserStats 正确累加 play_count
  - Failure: loadContent 不存在的 content_id 返回 nullopt
  - Evidence: `.omo/evidence/task-5-cache-v3-content.txt`
  
  **Commit**: Y | `feat(scanner): implement Content table CRUD in SQLiteCacheV3`

---

- [x] **6. 实现 `SQLiteCacheV3` 的 Locations 表 CRUD 操作**
  
  **What to do**: 在 `SQLiteCacheV3` 类中实现 locations 表操作：
  - `upsertLocation(location)`: INSERT OR REPLACE location 记录
  - `loadLocation(locationId)`: 查询单个 location
  - `loadLocationsByRoot(rootPath)`: 查询某 root 下所有 locations
  - `pruneDeletedLocations(rootPath, retainedLocationIds)`: 删除不在保留列表中的 locations
  - 处理外键级联删除（删除 content 时自动删除关联 locations）
  
  **Must NOT do**:
  - 不得在此实现 lyrics、scan_roots、scan_errors 表操作
  
  **Parallelization**: Wave 2 | Blocked by: 1-3 | Blocks: 8-10, 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:133-173` - locations 表定义
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.cache_v3_locations' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: upsertLocation 插入新记录，外键 content_id 有效
  - Happy: loadLocationsByRoot 返回该 root 下所有 locations
  - Happy: pruneDeletedLocations 删除不在保留列表中的记录
  - Failure: upsertLocation 使用无效 content_id，外键约束失败
  - Failure: file_path UNIQUE 约束冲突
  - Evidence: `.omo/evidence/task-6-cache-v3-locations.txt`
  
  **Commit**: Y | `feat(scanner): implement Locations table CRUD in SQLiteCacheV3`

---

- [x] **7. 实现 `SQLiteCacheV3` 的 Lyrics、ScanRoots、ScanErrors 表操作**
  
  **What to do**: 在 `SQLiteCacheV3` 类中实现剩余三个表的操作：
  - Lyrics: `replaceLyrics(locationId, lyrics)`, `loadLyrics(locationId, kind)`
  - ScanRoots: `updateScanRoot(rootPath, treeHash, totalFiles)`, `loadScanRoot(rootPath)`
  - ScanErrors: `saveErrors(rootPath, errors)`, `loadErrors(rootPath)`, `clearErrors(rootPath)`
  
  **Must NOT do**:
  - 不得破坏外键级联关系
  
  **Parallelization**: Wave 2 | Blocked by: 1-3 | Blocks: 8-10, 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:177-230` - lyrics, scan_roots, scan_errors 表定义
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.cache_v3_auxiliary' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: replaceLyrics 替换某 location 的歌词行
  - Happy: updateScanRoot 更新扫描状态
  - Happy: saveErrors 保存扫描错误，loadErrors 返回正确列表
  - Failure: replaceLyrics 使用无效 location_id，外键失败
  - Evidence: `.omo/evidence/task-7-cache-v3-auxiliary.txt`
  
  **Commit**: Y | `feat(scanner): implement Lyrics/ScanRoots/ScanErrors in SQLiteCacheV3`

---

- [x] **8. 实现 Schema V2 → V3 自动迁移脚本**
  
  **What to do**: 在 `SQLiteCacheV3` 构造函数中添加迁移逻辑：
  - 检测 `PRAGMA user_version`，如果为 2 则触发迁移
  - 从 V2 的 `songs` 表读取所有记录
  - 计算每条记录的 `content_id` 和 `location_id`
  - 插入到 V3 的 `content` 和 `locations` 表（去重 content_id）
  - 迁移用户统计（play_count, rating, last_played）- 相同 content_id 取 MAX
  - 更新 `PRAGMA user_version=3`
  - 删除旧表（`songs`, `directories`, `roots`）
  
  **Must NOT do**:
  - 不得在未备份前删除旧数据
  - 不得破坏正在使用的数据库文件
  
  **Parallelization**: Wave 3 | Blocked by: 4-7 | Blocks: -
  
  **References**:
  - `docs/optimal-scanner-architecture.md:905-953` - 迁移策略完整定义
  - `docs/sqlite-cache-design.md` - 现有 V2 schema 分析
  
  **Acceptance criteria**:
  ```bash
  # 创建 V2 测试数据库
  # 运行迁移
  ctest --test-dir build -R 'seriona.scanner.migration_v2_to_v3' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: V2 数据库自动迁移到 V3，user_version = 3
  - Happy: 迁移后 play_count 保留（验证特定歌曲）
  - Happy: 相同内容的多个文件去重为同一 content_id
  - Failure: 迁移中途失败（模拟磁盘满），数据库回滚到 V2
  - Evidence: `.omo/evidence/task-8-migration-test.txt`
  
  **Commit**: Y | `feat(scanner): implement automatic Schema V2→V3 migration`

---

- [x] **9. 实现数据库备份和回滚机制**
  
  **What to do**: 在迁移前自动创建备份：
  - 在 `SQLiteCacheV3` 迁移逻辑中，先复制数据库文件到 `.bak` 后缀
  - 迁移失败时自动恢复备份
  - 提供手动回滚 API：`rollbackToBackup()`
  - 迁移成功后可选删除备份
  
  **Must NOT do**:
  - 不得在备份期间阻塞主线程过久（对大数据库使用异步或进度通知）
  
  **Parallelization**: Wave 3 | Blocked by: 4-7 | Blocks: -
  
  **References**:
  - `docs/optimal-scanner-architecture.md:948-953` - 备份和回滚提示
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.backup_rollback' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 迁移前创建 .bak 文件
  - Happy: 迁移失败，自动恢复 .bak
  - Happy: rollbackToBackup() 手动恢复
  - Failure: 备份失败（磁盘满），迁移拒绝启动
  - Evidence: `.omo/evidence/task-9-backup-rollback.txt`
  
  **Commit**: Y | `feat(scanner): add database backup and rollback for migration`

---

- [x] **10. Phase 1 集成测试和文档更新**
  
  **What to do**: 
  - 编写端到端测试：创建 V2 数据库 → 自动迁移 → 验证 V3 数据完整性
  - 验证所有 CRUD 操作在真实数据库上正确工作
  - 更新 `docs/sqlite-cache-design.md`，添加 Schema V3 章节
  - 更新 `AGENTS.md`，记录 Schema V3 已实施
  
  **Must NOT do**:
  - 不得修改公共 API 契约
  
  **Parallelization**: Wave 3 | Blocked by: 4-7 | Blocks: -
  
  **References**:
  - `docs/sqlite-cache-design.md` - 需要更新的文档
  - `docs/AGENTS.md` - 项目指南
  
  **Acceptance criteria**:
  ```bash
  # 运行所有 Phase 1 测试
  ctest --test-dir build -R 'seriona.scanner.(song_identity|cache_v3|migration)' --output-on-failure
  
  # 验证文档已更新
  grep -q "Schema V3" docs/sqlite-cache-design.md
  ```
  
  **QA scenarios**:
  - Happy: 端到端测试通过，V2 → V3 迁移无数据丢失
  - Happy: 文件移动场景：location_id 变化，content_id 不变，play_count 保留
  - Evidence: `.omo/evidence/task-10-phase1-integration.txt`
  
  **Commit**: Y | `docs(scanner): update docs for Schema V3 implementation`

---
### **Phase 2: 并发 Worker Pool（2 天，任务 11-21）**
---

- [x] **11. 使用 CMake FetchContent 集成 BS::thread_pool**
  
  **What to do**: 修改 `CMakeLists.txt`，添加 BS::thread_pool 依赖：
  - 使用 `FetchContent_Declare` 声明 `https://github.com/bshoshany/thread-pool.git`，版本 `v4.1.0`
  - 设置 `GIT_SHALLOW TRUE` 加速下载
  - `FetchContent_MakeAvailable(thread_pool)`
  - 链接 `seriona_scanner` 到 `BS::thread_pool`
  
  **Must NOT do**:
  - 不得在非 scanner 模块引入此依赖
  - 不得修改 BS::thread_pool 源码
  
  **Parallelization**: Wave 4 | Blocked by: - | Blocks: 13-17
  
  **References**:
  - `docs/optimal-scanner-architecture.md:526-543` - CMake 集成示例
  - BS::thread_pool 仓库: https://github.com/bshoshany/thread-pool
  
  **Acceptance criteria**:
  ```bash
  # 配置成功，自动下载 BS::thread_pool
  cmake -S . -B build -DSERIONA_BUILD_TESTS=ON
  
  # 验证 BS::thread_pool 头文件存在
  ls build/_deps/thread_pool-src/BS_thread_pool.hpp
  ```
  
  **QA scenarios**:
  - Happy: 首次配置自动下载 v4.1.0
  - Happy: 重新配置使用缓存，不重复下载
  - Failure: 网络不可用时配置失败并给出清晰提示
  - Evidence: `.omo/evidence/task-11-thread-pool-cmake.txt`
  
  **Commit**: Y | `build(scanner): integrate BS::thread_pool via FetchContent`

---

- [x] **12. 验证 BS::thread_pool 编译和基础功能**
  
  **What to do**: 创建简单的单元测试验证 BS::thread_pool 可用：
  - 创建 `tests/scanner/test_thread_pool_integration.cpp`
  - 测试创建 `BS::thread_pool` 实例
  - 测试提交简单任务并等待结果
  - 测试异常传播（任务抛出异常，future.get() 重新抛出）
  
  **Must NOT do**:
  - 不得在此实现实际的 Scanner worker pool
  
  **Parallelization**: Wave 4 | Blocked by: - | Blocks: 13-17
  
  **References**:
  - `docs/optimal-scanner-architecture.md:713-769` - BS::thread_pool 使用示例
  
  **Acceptance criteria**:
  ```bash
  cmake --build build --target seriona_scanner_tests
  ctest --test-dir build -R 'seriona.scanner.thread_pool_basic' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 创建 4 线程 pool，提交 10 个任务，全部完成
  - Happy: 任务返回值正确传递到 future
  - Failure: 任务抛出异常，future.get() 重新抛出
  - Evidence: `.omo/evidence/task-12-thread-pool-test.txt`
  
  **Commit**: Y | `test(scanner): verify BS::thread_pool integration`

---

- [x] **13. 实现 `ScannerWorkerPool` 类基础结构**
  
  **What to do**: 创建 `inc/seriona/scanner/worker_pool.h` 和 `src/scanner/worker_pool.cpp`：
  - 定义 `WorkerTask` 结构（候选文件 + 缓存 location）
  - 定义 `WorkerResult` 结构（content_id, location_id, metadata, lyrics, wasCacheHit）
  - 定义 `ScannerWorkerPool` 类：
    - 构造函数：接受 `Config`（workerCount, tagReaderConcurrentLimit）
    - 持有 `BS::thread_pool` 实例
    - 持有 `std::counting_semaphore<>` 用于 TagReader 限流
    - 持有 `std::vector<std::future<WorkerResult>>` 存储待完成任务
  - 实现 `submitBatch(tasks)` 和 `waitAll()` 方法框架
  
  **Must NOT do**:
  - 不得在此实现实际的文件处理逻辑（留给任务 14）
  - 不得暴露 BS::thread_pool 类型到公共头文件
  
  **Parallelization**: Wave 5 | Blocked by: 11-12 | Blocks: 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:548-710` - ScannerWorkerPool 完整实现
  
  **Acceptance criteria**:
  ```bash
  cmake --build build --target seriona_scanner
  ```
  
  **QA scenarios**:
  - Happy: 构造 ScannerWorkerPool，指定 4 worker + 2 TagReader 并发
  - Happy: submitBatch 提交空任务列表，waitAll 返回空结果
  - Evidence: `.omo/evidence/task-13-worker-pool-basic.txt`
  
  **Commit**: Y | `feat(scanner): implement ScannerWorkerPool base structure`

---

- [x] **14. 实现 `ScannerWorkerPool::processTask()` 文件处理逻辑**
  
  **What to do**: 在 `ScannerWorkerPool` 中实现 `processTask()` 私有方法：
  - 检查缓存：如果 `task.cachedLocation` 的 `location_id` 匹配，返回缓存数据
  - 缓存未命中：获取 TagReader semaphore，调用 `tagReader.read()`
  - 计算 `content_id` 使用任务 1 实现的函数
  - 统计计时（tagReaderTime，使用原子变量累加）
  - 使用 RAII guard 自动释放 semaphore
  
  **Must NOT do**:
  - 不得在 worker 中调用 `publishEvent()` 或写 SQLite
  - 不得在实时路径中使用日志（避免竞争）
  
  **Parallelization**: Wave 5 | Blocked by: 11-12 | Blocks: 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:655-709` - processTask 实现
  - `src/scanner/file_scanner_orchestrator.cpp:100-200` - 现有 reconcileAudio 逻辑参考
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.worker_pool_process' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 缓存命中路径，< 1μs 返回
  - Happy: 缓存未命中，调用 TagReader，返回正确 metadata
  - Happy: 多个 worker 并发处理，semaphore 限制 TagReader 并发为 2
  - Failure: TagReader 抛出异常，异常传播到 future
  - Evidence: `.omo/evidence/task-14-worker-pool-process.txt`
  
  **Commit**: Y | `feat(scanner): implement processTask with cache check and TagReader`

---

- [x] **15. 实现批量任务提交和结果收集**
  
  **What to do**: 在 `ScannerWorkerPool` 中完善 `submitBatch()` 和 `waitAll()`：
  - `submitBatch()`：循环任务，使用 `pool_.submit_task()` 提交 lambda，存储 future
  - 批量提交优化：每次提交 64 个任务（减少调度开销）
  - `waitAll()`：循环所有 futures，调用 `.get()` 收集结果
  - 清空 futures 列表，准备下一批
  
  **Must NOT do**:
  - 不得阻塞在单个 future 上（按顺序 get 可能导致不必要等待）
  
  **Parallelization**: Wave 5 | Blocked by: 11-12 | Blocks: 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:629-653` - submitBatch 和 waitAll 实现
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.worker_pool_batch' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 提交 100 个任务（64 + 36），waitAll 返回 100 个结果
  - Happy: 结果顺序不保证与提交顺序一致（验证并发）
  - Failure: 某个任务失败，waitAll 抛出异常但不影响其他任务
  - Evidence: `.omo/evidence/task-15-worker-pool-batch.txt`
  
  **Commit**: Y | `feat(scanner): implement batch task submission and result collection`

---

- [x] **16. 实现性能统计（原子累加和 Phase breakdown）**
  
  **What to do**: 在 `ScannerWorkerPool` 中添加性能统计：
  - 定义 `ScannerPerfStats` 结构：
    - `std::atomic<uint64_t> cacheHits`
    - `std::atomic<uint64_t> filesScanned`
    - `std::atomic<uint64_t> totalTagReaderTimeUs`
    - `std::atomic<uint64_t> totalHashTimeUs`（如果仍需 hash）
  - 在 `processTask()` 中使用 `fetch_add()` 累加统计
  - 提供 `stats()` 方法返回当前统计快照
  - 实现 Phase breakdown 格式化输出（参考旧 MusicPlayer 格式）
  
  **Must NOT do**:
  - 不得使用非原子变量（导致数据竞争）
  - 不得在热路径中使用锁
  
  **Parallelization**: Wave 5 | Blocked by: 11-12 | Blocks: 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:760-768` - 性能统计示例
  - `docs/scanner-architecture-comparison.md` - 旧项目性能报告格式
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.worker_pool_stats' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 100 个任务，80% 缓存命中，stats.cacheHits = 80
  - Happy: totalTagReaderTimeUs 正确累加所有 worker 时间
  - Happy: 多线程并发更新统计，无数据竞争（ThreadSanitizer 验证）
  - Evidence: `.omo/evidence/task-16-worker-pool-stats.txt`
  
  **Commit**: Y | `feat(scanner): add atomic performance statistics to worker pool`

---

- [x] **17. 实现配置调优和错误处理**
  
  **What to do**: 完善 `ScannerWorkerPool::Config`：
  - 添加 `getOptimalWorkerCount()` 自动选择函数（默认 `hardware_concurrency()`）
  - 添加 `getOptimalTagReaderLimit()` 函数（默认 4，可通过环境变量或配置覆盖）
  - 实现 `cancel()` 方法：暂停 pool，等待当前任务完成，清空 futures
  - 实现异常收集：单个任务失败不影响其他，但记录到错误列表
  
  **Must NOT do**:
  - 不得强制取消正在运行的任务（可能破坏 TagReader 状态）
  
  **Parallelization**: Wave 5 | Blocked by: 11-12 | Blocks: 18-21
  
  **References**:
  - `docs/optimal-scanner-architecture.md:789-841` - 配置调优和错误处理
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.worker_pool_config' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: getOptimalWorkerCount() 在 32 核系统返回 32
  - Happy: cancel() 后 pool 停止接受新任务
  - Happy: 10 个任务中 2 个失败，waitAll 返回 8 个结果 + 2 个错误
  - Evidence: `.omo/evidence/task-17-worker-pool-config.txt`
  
  **Commit**: Y | `feat(scanner): add config tuning and error handling to worker pool`

---

- [x] **18. 重构 `reconcileRoot()` 以使用 `ScannerWorkerPool`**
  
  **What to do**: 修改 `src/scanner/file_scanner_orchestrator.cpp` 中的 `reconcileRoot()`：
  - 保留串行目录枚举（`discoverScannerPaths()`）
  - 构建 cached song map（从 O(n) 查找优化为 O(1)）
  - 创建 `ScannerWorkerPool` 实例
  - 将音频文件候选转换为 `WorkerTask` 列表，批量提交
  - 调用 `waitAll()` 收集结果
  - 保持原有事件发布顺序（按发现顺序发布 `FileScanned`）
  
  **Must NOT do**:
  - 不得破坏现有公共 API
  - 不得改变事件发布顺序
  - 不得在此任务中实现增量扫描（留给 Phase 3）
  
  **Parallelization**: Wave 6 | Blocked by: 4-7, 13-17 | Blocks: 22-26
  
  **References**:
  - `src/scanner/file_scanner_orchestrator.cpp:100-400` - 现有 reconcileRoot 实现
  - `docs/optimal-scanner-architecture.md:714-769` - 集成示例
  
  **Acceptance criteria**:
  ```bash
  # 编译成功
  cmake --build build --target seriona_scanner
  
  # 现有测试通过（验证向后兼容）
  ctest --test-dir build -R 'seriona.scanner' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 扫描 100 首歌，事件按文件路径顺序发布
  - Happy: 性能提升：冷扫描时间减少（对比 Phase 1 baseline）
  - Failure: TagReader 失败，错误正确记录和发布
  - Evidence: `.omo/evidence/task-18-reconcile-root-refactor.txt`
  
  **Commit**: Y | `refactor(scanner): integrate ScannerWorkerPool into reconcileRoot`

---

- [x] **19. 实现结果排序以保持事件顺序**
  
  **What to do**: 在 `reconcileRoot()` 中添加结果排序逻辑：
  - `waitAll()` 返回的结果顺序不确定（取决于 worker 完成顺序）
  - 按原始文件发现顺序（或规范化路径）排序结果
  - 按排序后的顺序发布 `FileScanned` 事件
  - 验证 UI 快照更新的确定性
  
  **Must NOT do**:
  - 不得在排序中引入性能瓶颈（使用高效排序算法）
  
  **Parallelization**: Wave 6 | Blocked by: 4-7, 13-17 | Blocks: 22-26
  
  **References**:
  - `docs/scanner-concurrency-recommendation.md:121` - 事件顺序要求
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.event_ordering' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 提交乱序任务，事件按文件路径排序发布
  - Happy: 多次扫描相同目录，事件顺序一致
  - Evidence: `.omo/evidence/task-19-event-ordering.txt`
  
  **Commit**: Y | `fix(scanner): ensure deterministic event ordering in concurrent scan`

---

- [x] **20. 优化缓存查找（O(n) → O(1)）**
  
  **What to do**: 在 `reconcileRoot()` 中优化 `cachedSongByPath()`：
  - 在主线程构建 `std::unordered_map<std::string, CachedLocation>` 索引
  - Key 为规范化路径，Value 为 cached location
  - Worker 使用 map 查找而非线性扫描
  - 确保 map 是只读的（多线程安全）
  
  **Must NOT do**:
  - 不得在 worker 中修改 map（导致数据竞争）
  
  **Parallelization**: Wave 6 | Blocked by: 4-7, 13-17 | Blocks: 22-26
  
  **References**:
  - `docs/scanner-concurrency-recommendation.md:119` - cache lookup 优化
  - `src/scanner/file_scanner_orchestrator.cpp:82-95` - 现有 cachedSongByPath
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.cache_lookup' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 10000 首歌，缓存查找从 O(n) 降到 O(1)
  - Happy: 热扫描时间显著减少（缓存命中率 95%）
  - Evidence: `.omo/evidence/task-20-cache-lookup.txt`
  
  **Commit**: Y | `perf(scanner): optimize cache lookup from O(n) to O(1)`

---

- [x] **21. Phase 2 ThreadSanitizer 验证和集成测试**
  
  **What to do**: 
  - 使用 `-DCMAKE_CXX_FLAGS="-fsanitize=thread"` 重新编译
  - 运行所有 scanner 测试，验证无数据竞争
  - 编写压力测试：1000 首歌，4 worker，重复 10 次
  - 验证性能提升：对比 Phase 1 baseline（串行）和 Phase 2（并发）
  - 更新 `docs/optimal-scanner-architecture.md`，记录实际性能数据
  
  **Must NOT do**:
  - 不得忽略 ThreadSanitizer 报告的任何竞争
  
  **Parallelization**: Wave 6 | Blocked by: 4-7, 13-17 | Blocks: 22-26
  
  **References**:
  - `docs/optimal-scanner-architecture.md:996-1000` - ThreadSanitizer 验证
  
  **Acceptance criteria**:
  ```bash
  # ThreadSanitizer 构建
  cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DSERIONA_BUILD_TESTS=ON
  cmake --build build-tsan
  ctest --test-dir build-tsan -R 'seriona.scanner' --output-on-failure
  
  # 无 "WARNING: ThreadSanitizer" 输出
  ```
  
  **QA scenarios**:
  - Happy: 所有测试通过，无数据竞争报告
  - Happy: 性能测试显示冷扫描提升 1.5-3x
  - Evidence: `.omo/evidence/task-21-phase2-tsan.txt`
  
  **Commit**: Y | `test(scanner): verify thread safety with ThreadSanitizer`

---
### **Phase 3: 增量扫描（2 天，任务 22-26）**
---

- [x] **22. 实现 directory tree hash 计算**
  
  **What to do**: 创建 `src/scanner/directory_tree_hash.cpp` 和对应头文件：
  - 实现 `computeDirectoryTreeHash(rootPath)` 函数
  - 使用 Merkle tree 风格：递归遍历目录，计算每个目录的 hash（路径 + 子目录 hash + 文件列表 hash）
  - 使用 xxHash64 快速计算
  - 返回根目录的总 hash（代表整个目录树结构）
  
  **Must NOT do**:
  - 不得读取文件内容（只用目录结构和文件名）
  - 不得在此实现缓存机制
  
  **Parallelization**: Wave 7 | Blocked by: 18-21 | Blocks: 27-30
  
  **References**:
  - `docs/optimal-scanner-architecture.md:431-451` - directory tree hash 计算
  
  **Acceptance criteria**:
  ```bash
  cmake --build build --target seriona_scanner
  ctest --test-dir build -R 'seriona.scanner.directory_tree_hash' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 相同目录结构多次计算返回相同 hash
  - Happy: 添加/删除文件后 hash 变化
  - Happy: 文件内容变化但目录结构不变，hash 不变
  - Failure: 无法访问的目录返回空 hash 或错误
  - Evidence: `.omo/evidence/task-22-directory-tree-hash.txt`
  
  **Commit**: Y | `feat(scanner): implement directory tree hash for incremental scan`

---

- [x] **23. 实现扫描模式决策（Full vs Incremental）**
  
  **What to do**: 在 `file_scanner_orchestrator.cpp` 中实现 `decideScanMode()`：
  - 从 `SQLiteCacheV3` 加载 `ScanRootInfo`（包含上次的 directory_tree_hash）
  - 如果 root 首次扫描，返回 `ScanMode::Full`
  - 计算当前 directory tree hash，对比缓存值
  - 如果 hash 不同，返回 `ScanMode::Full`
  - 如果 hash 相同，返回 `ScanMode::Incremental`
  
  **Must NOT do**:
  - 不得在用户明确指定 `ScanMode::Full` 时覆盖
  
  **Parallelization**: Wave 7 | Blocked by: 18-21 | Blocks: 27-30
  
  **References**:
  - `docs/optimal-scanner-architecture.md:426-451` - 扫描模式决策
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.scan_mode_decision' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 首次扫描返回 Full
  - Happy: 目录未变化返回 Incremental
  - Happy: 新增文件后返回 Full（目录结构变化）
  - Evidence: `.omo/evidence/task-23-scan-mode-decision.txt`
  
  **Commit**: Y | `feat(scanner): implement scan mode decision (Full vs Incremental)`

---

- [x] **24. 实现增量扫描三阶段检测（删除、新增、变化）**
  
  **What to do**: 在 `file_scanner_orchestrator.cpp` 中实现 `planIncrementalScan()`：
  - 输入：当前文件系统文件列表 + 缓存 locations
  - 阶段 1：识别删除的文件（缓存中存在但文件系统中不存在）
  - 阶段 2：识别新增的文件（文件系统中存在但缓存中不存在）
  - 阶段 3：识别变化的文件（location_id 不匹配，即 path/size/mtime 变化）
  - 返回 `IncrementalScanPlan` 结构（三个列表）
  
  **Must NOT do**:
  - 不得在此执行实际扫描（只做计划）
  
  **Parallelization**: Wave 7 | Blocked by: 18-21 | Blocks: 27-30
  
  **References**:
  - `docs/optimal-scanner-architecture.md:456-506` - 增量扫描三阶段
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.incremental_plan' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 100 首歌，95 首未变，3 首新增，2 首删除 → plan 正确分类
  - Happy: 修改 1 首歌的 mtime，识别为变化文件
  - Evidence: `.omo/evidence/task-24-incremental-plan.txt`
  
  **Commit**: Y | `feat(scanner): implement 3-phase incremental scan detection`

---

- [x] **25. 集成增量扫描到 `reconcileRoot()`**
  
  **What to do**: 修改 `reconcileRoot()` 支持增量扫描：
  - 调用 `decideScanMode()` 判断模式
  - Full 模式：使用现有全量扫描流程
  - Incremental 模式：
    - 调用 `planIncrementalScan()` 生成计划
    - 只对新增和变化文件提交 worker 任务
    - 删除的文件调用 `cache.pruneDeletedLocations()`
    - 未变化的文件直接使用缓存数据，发布 `FileScanned` 事件
  - 更新 `scan_roots` 表的 `directory_tree_hash`
  
  **Must NOT do**:
  - 不得破坏 Full 模式的现有功能
  - 不得跳过删除文件的清理
  
  **Parallelization**: Wave 7 | Blocked by: 18-21 | Blocks: 27-30
  
  **References**:
  - `docs/optimal-scanner-architecture.md:254-285` - reconcileRoot 整体架构
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.incremental_integration' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 首次扫描 1000 首（Full），第二次扫描未变化（Incremental，< 2 秒）
  - Happy: 新增 10 首歌，增量扫描只处理 10 首
  - Happy: 删除 5 首歌，cache 正确清理
  - Evidence: `.omo/evidence/task-25-incremental-integration.txt`
  
  **Commit**: Y | `feat(scanner): integrate incremental scan into reconcileRoot`

---

- [x] **26. Phase 3 端到端测试和性能验证**
  
  **What to do**: 
  - 编写端到端测试：模拟真实场景
    - 场景 1：首次扫描 5000 首（Full）
    - 场景 2：无变化重新扫描（Incremental，< 5 秒）
    - 场景 3：新增 50 首，修改 20 首（Incremental，约 10 秒）
    - 场景 4：删除 100 首（Incremental，验证清理）
  - 验证性能目标：热扫描 < 5 秒，温扫描 < 15 秒
  - 更新文档，记录增量扫描性能数据
  
  **Must NOT do**:
  - 不得使用真实音乐文件（使用 fixture 或模拟）
  
  **Parallelization**: Wave 7 | Blocked by: 18-21 | Blocks: 27-30
  
  **References**:
  - `docs/optimal-scanner-architecture.md:503-506` - 增量扫描性能目标
  
  **Acceptance criteria**:
  ```bash
  ctest --test-dir build -R 'seriona.scanner.incremental_e2e' --output-on-failure
  ```
  
  **QA scenarios**:
  - Happy: 场景 1-4 全部通过，性能达标
  - Happy: 增量扫描后 playlist 快照正确
  - Evidence: `.omo/evidence/task-26-phase3-e2e.txt`
  
  **Commit**: Y | `test(scanner): add end-to-end tests for incremental scan`

---
### **Phase 4: 性能调优与测试（2 天，任务 27-30）**
---

- [x] **27. 实际音乐库性能测试（1000/5000/10000 首）**
  
  **What to do**: 
  - 准备三个测试音乐库（或使用 fixture 生成）
  - 运行冷扫描、热扫描、增量扫描测试
  - 记录详细性能数据：
    - Phase breakdown（目录枚举、并发处理、结果聚合）
    - Cumulative CPU time（所有 worker 的总 CPU 时间）
    - Cache 命中率
    - TagReader 调用次数和总时间
  - 生成性能报告（Markdown 格式）
  
  **Must NOT do**:
  - 不得使用版权音乐文件（使用公共领域或自制 fixture）
  
  **Parallelization**: Wave 8 | Blocked by: 22-26 | Blocks: -
  
  **References**:
  - `docs/optimal-scanner-architecture.md:871-898` - 性能分析
  - `docs/scanner-architecture-comparison.md` - 性能对比格式
  
  **Acceptance criteria**:
  ```bash
  # 运行性能测试
  build/tests/seriona_scanner_perf_test --library-size=5000 --output=.omo/evidence/task-27-perf-5000.md
  
  # 验证性能目标
  # 冷扫描 < 30 秒
  # 热扫描 < 5 秒
  ```
  
  **QA scenarios**:
  - Happy: 5000 首冷扫描在 20-30 秒内完成
  - Happy: 5000 首热扫描在 3-5 秒内完成
  - Happy: 性能报告包含所有必要指标
  - Evidence: `.omo/evidence/task-27-perf-report.md`
  
  **Commit**: Y | `test(scanner): add real-world performance benchmarks`

---

- [x] **28. TagReader 并发压测和线程安全验证**
  
  **What to do**: 
  - 编写压力测试：模拟高并发 TagReader 调用
  - 测试不同 semaphore 限制（1, 2, 4, 8, 16）
  - 使用 ThreadSanitizer 验证每个配置
  - 使用 Valgrind (helgrind) 双重验证
  - 记录最优并发度：平衡性能和安全性
  - 如果发现线程不安全，降级到 semaphore=1
  
  **Must NOT do**:
  - 不得在未验证安全的情况下提高并发度
  
  **Parallelization**: Wave 8 | Blocked by: 22-26 | Blocks: -
  
  **References**:
  - `docs/scanner-concurrency-recommendation.md:100-101` - TagReader 线程安全考虑
  - `docs/optimal-scanner-architecture.md:804` - TagReader 并发限制
  
  **Acceptance criteria**:
  ```bash
  # ThreadSanitizer 压测
  build-tsan/tests/seriona_scanner_stress_test --tag-reader-concurrency=4 --iterations=100
  
  # Valgrind helgrind
  valgrind --tool=helgrind build/tests/seriona_scanner_stress_test
  ```
  
  **QA scenarios**:
  - Happy: semaphore=4，100 次迭代，无竞争报告
  - Happy: semaphore=8，性能提升但仍安全
  - Failure: semaphore=16，发现竞争 → 降级到 8
  - Evidence: `.omo/evidence/task-28-tagreader-stress.txt`
  
  **Commit**: Y | `test(scanner): verify TagReader thread safety under stress`

---

- [x] **29. 生成性能对比报告和文档更新**
  
  **What to do**: 
  - 汇总 Phase 1-4 的所有性能数据
  - 生成对比表格：
    - 当前实现 vs 优化后
    - 串行 vs 并发
    - Full vs Incremental
  - 更新 `docs/optimal-scanner-architecture.md`，填充实际性能数据
  - 更新 `README.md`（如果有），添加性能亮点
  - 创建 `docs/scanner-performance-report.md` 详细报告
  
  **Must NOT do**:
  - 不得夸大性能数字
  
  **Parallelization**: Wave 8 | Blocked by: 22-26 | Blocks: -
  
  **References**:
  - `docs/optimal-scanner-architecture.md:871-898` - 性能对比表格
  
  **Acceptance criteria**:
  ```bash
  # 验证文档存在并包含关键数据
  grep -q "热扫描.*< 5 秒" docs/scanner-performance-report.md
  grep -q "冷扫描.*< 30 秒" docs/scanner-performance-report.md
  ```
  
  **QA scenarios**:
  - Happy: 所有性能目标达成，报告清晰
  - Happy: 对比表格显示 8-47x 提升
  - Evidence: `.omo/evidence/task-29-performance-report.md`
  
  **Commit**: Y | `docs(scanner): add comprehensive performance report`

---

- [x] **30. 最终配置调优和用户可配置选项**
  
  **What to do**: 
  - 添加配置选项到 `ScannerConfig`：
    - `workerCount`（默认自动，可覆盖）
    - `tagReaderConcurrency`（默认 4，可覆盖）
    - `enableIncrementalScan`（默认 true）
    - `forceFull`（强制全量扫描）
  - 实现环境变量覆盖：
    - `SERIONA_SCANNER_WORKERS`
    - `SERIONA_SCANNER_TAGREADER_CONCURRENCY`
  - 添加降级开关：`SERIONA_SCANNER_DISABLE_CONCURRENCY`（回退串行）
  - 更新 `docs/file-scanner.md`，记录所有配置选项
  
  **Must NOT do**:
  - 不得破坏现有 API
  - 不得强制要求用户设置配置
  
  **Parallelization**: Wave 8 | Blocked by: 22-26 | Blocks: -
  
  **References**:
  - `inc/seriona/scanner/scanner_contracts.h` - ScannerConfig 定义
  - `docs/optimal-scanner-architecture.md:789-810` - 配置调优
  
  **Acceptance criteria**:
  ```bash
  # 验证配置生效
  SERIONA_SCANNER_WORKERS=8 build/app/seriona /path/to/music
  
  # 验证降级开关
  SERIONA_SCANNER_DISABLE_CONCURRENCY=1 build/app/seriona /path/to/music
  ```
  
  **QA scenarios**:
  - Happy: 设置 `workerCount=8`，worker pool 使用 8 线程
  - Happy: 设置 `SERIONA_SCANNER_DISABLE_CONCURRENCY=1`，回退串行
  - Happy: 配置文档清晰，用户可理解
  - Evidence: `.omo/evidence/task-30-config-tuning.txt`
  
  **Commit**: Y | `feat(scanner): add user-configurable concurrency options`

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.

- [~] **F1. Plan compliance audit**
  - Verify all 30 tasks completed
  - Verify all commits pushed
  - Verify all evidence files generated
  - Check no Must-NOT-have violations

- [x] **F2. Code quality review**
  - Run cppcheck on scanner module
  - Verify no compiler warnings (-Wall -Wextra -Wpedantic)
  - Check code follows project style (if clang-format exists)
  - Verify all public APIs documented

- [x] **F3. Real manual QA**
  - Run seriona app with real music library
  - Verify cold scan completes successfully
  - Verify hot scan < 5 seconds
  - Verify incremental scan works after file changes
  - Check UI updates correctly

- [x] **F4. Scope fidelity**
  - Verify all Must-have items delivered
  - Verify no scope creep beyond plan
  - Check performance targets met (8-47x improvement)
  - Verify backward compatibility preserved

## Commit strategy

**Atomic commits per task**: 每个任务完成后立即提交，包含实现 + 测试。

**Commit message format**: 
```
<type>(<scope>): <subject>

<body - optional>

<footer - optional>
```

**Types**: 
- `feat`: 新功能
- `fix`: 错误修复
- `refactor`: 重构
- `perf`: 性能优化
- `test`: 测试
- `docs`: 文档
- `build`: 构建系统

**Scope**: `scanner` (所有任务)

**Branch strategy**:
- 创建 feature branch: `feature/scanner-refactor-optimal-architecture`
- Phase 1-4 按序提交
- 每个 Phase 完成后可选 merge checkpoint
- 最终通过 Final Verification 后 merge 到 main

**推送时机**:
- Phase 1 完成后推送（任务 1-10）
- Phase 2 完成后推送（任务 11-21）
- Phase 3 完成后推送（任务 22-26）
- Phase 4 完成后推送（任务 27-30）
- Final Verification 通过后最终推送

## Success criteria

**功能完整性**:
- ✅ SQLite Schema V3 实施并自动迁移
- ✅ 双 ID 系统工作正常（content_id + location_id）
- ✅ Metadata hash 替代全文件 hash
- ✅ BS::thread_pool 集成成功
- ✅ ScannerWorkerPool 实现并发扫描
- ✅ 增量扫描三阶段工作正常

**性能目标**:
- ✅ 热扫描（95% 未变）< 5 秒（目标 47x 提升）
- ✅ 温扫描（20% 变化）< 15 秒（目标 20-29x 提升）
- ✅ 冷扫描（首次）< 30 秒（目标 8-12x 提升）
- ✅ 文件移动后用户数据（play_count）保留

**质量保证**:
- ✅ 所有单元测试通过（ctest 100% pass）
- ✅ ThreadSanitizer 验证无数据竞争
- ✅ 端到端测试覆盖所有场景
- ✅ 现有公共 API 保持向后兼容
- ✅ 无编译警告（-Wall -Wextra）

**文档完整性**:
- ✅ 所有新 API 有注释
- ✅ 性能报告已生成
- ✅ 用户配置选项已文档化
- ✅ AGENTS.md 更新

**可维护性**:
- ✅ 代码风格一致
- ✅ 错误处理完善
- ✅ 日志输出清晰
- ✅ 配置降级机制可用
