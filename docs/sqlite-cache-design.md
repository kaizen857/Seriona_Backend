# Seriona SQLite Cache 设计分析

## 架构概览

Seriona 使用 SQLite 作为持久化缓存，避免每次启动都重新扫描音频库。核心设计是 **root-based caching**：以扫描根目录为单位缓存所有元数据。

---

## 数据库结构

### Schema Version: 2

**配置**：
- Journal Mode: **WAL**（Write-Ahead Logging，支持并发读）
- Foreign Keys: **ON**（级联删除）
- Busy Timeout: 500ms（默认）

### Schema V3

Phase 1 已落地一个独立的 `SQLiteCacheV3` 桥接实现；现有生产扫描器入口 `SQLiteScannerCache` 仍是 V2 runtime code，后续阶段不要把它误认为已经切到 V3。V3 的核心变化是把原 V2 `songs` 行拆成两个稳定边界：

- `content` 表以 `content_id` 为主键，保存标题、艺术家、专辑、时长、音频技术参数以及 `play_count`、`rating`、`last_played_ms` 等用户统计。
- `locations` 表以 `location_id` 为主键，保存 `root_path`、`file_path`、`file_size_bytes`、`file_mtime_ns`、CUE 偏移、封面路径和外部歌词路径，并通过 `content_id` 外键指向 `content`。
- `lyrics` 表绑定到 `location_id`，因为同一内容在不同文件位置上可能有不同外部歌词文件。
- `scan_roots` 和 `scan_errors` 替代 V2 的 `roots` / `errors`，继续按 root 记录目录树状态和扫描错误。

ID 语义如下：`content_id` 由 `duration + title + artist` 计算，用来在文件移动或重命名后保留用户统计；`location_id` 由 `path + size + mtime` 计算，用来判断同一文件位置是否发生变化。因此文件移动时，预期行为是 `location_id` 改变，`content_id` 不变，用户统计仍留在 `content` 行。

`SQLiteCacheV3` 打开数据库时会读取 `PRAGMA user_version`：新库初始化为 V3；V2 库会先复制 `<database>.bak`，再在事务中迁移到 V3。迁移会把 V2 `roots/songs` 写入 V3 `scan_roots/content/locations`，重复内容的用户统计按当前实现合并为较大的播放次数/评分和较新的最后播放时间。迁移成功后会删除 V2 表并设置 `PRAGMA user_version=3`，但备份文件会被有意保留；迁移失败会从备份恢复原 V2 数据库，`rollbackToBackup()` 也可手动回滚到该备份。

### 表结构

#### 1. `roots` 表（扫描根目录）

```sql
CREATE TABLE roots(
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,           -- 根目录路径（如 /home/user/Music）
  directory_hash TEXT NOT NULL,        -- 目录树 Merkle hash
  updated_at_ms INTEGER NOT NULL       -- 最后更新时间（毫秒）
);
```

**说明**：
- `path` 是唯一键：同一根目录只有一个缓存条目
- `directory_hash`：用于快速检测目录结构变化（Merkle tree hash）
- `updated_at_ms`：用于 LRU 淘汰策略（保留最近扫描的 8 个根目录）

---

#### 2. `songs` 表（音频文件元数据）

```sql
CREATE TABLE songs(
  id INTEGER PRIMARY KEY,
  root_id INTEGER NOT NULL,            -- 外键 → roots.id
  track_id TEXT NOT NULL,              -- 轨道 ID（当前为文件路径）
  file_path TEXT NOT NULL,             -- 文件绝对路径
  
  -- 音频元数据
  title TEXT NOT NULL,
  artist TEXT NOT NULL,
  album TEXT NOT NULL,
  album_artist TEXT NOT NULL,
  genre TEXT NOT NULL,
  track_number INTEGER,
  disc_number INTEGER,
  year INTEGER,
  
  -- 技术参数
  sample_rate INTEGER,
  bit_depth INTEGER,
  channels INTEGER,
  duration_ms INTEGER,
  
  -- 文件属性
  file_size_bytes INTEGER,
  file_mtime_ns INTEGER,               -- 文件修改时间（纳秒）
  content_hash TEXT NOT NULL,          -- 🔑 内容 hash（当前为全文件 xxHash）
  
  -- 歌词相关
  lyrics_source TEXT NOT NULL,         -- 'embedded' | 'external' | 'none'
  external_lrc_path TEXT,              -- 外部 .lrc 路径
  external_lrc_hash TEXT,              -- 外部 .lrc 内容 hash
  external_lrc_mtime_ns INTEGER,       -- 外部 .lrc 修改时间
  
  -- CUE 分轨支持
  source_file_path TEXT NOT NULL,      -- 原始文件路径（CUE 情况下不同于 file_path）
  offset_ms INTEGER,                   -- CUE 分轨偏移
  logical_track_id TEXT NOT NULL,      -- 逻辑轨道 ID
  
  -- 封面
  artwork_path TEXT,                   -- 封面图片路径
  
  -- 用户统计
  play_count INTEGER NOT NULL DEFAULT 0,
  rating INTEGER,                      -- 评分（可选）
  last_played_ms INTEGER,              -- 最后播放时间
  
  UNIQUE(root_id, track_id),
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);

CREATE INDEX idx_songs_root_file ON songs(root_id, file_path);
```

**关键字段**：
- **`content_hash`**：当前存储全文件 xxHash，用于判断文件内容是否变化
- **`file_mtime_ns`**：文件修改时间，辅助判断
- **`track_id`**：当前实现为文件路径，未来可能改为 content_hash

---

#### 3. `lyrics` 表（歌词行）

```sql
CREATE TABLE lyrics(
  song_id INTEGER NOT NULL,            -- 外键 → songs.id
  kind TEXT NOT NULL,                  -- 'embedded' | 'external'
  line_index INTEGER NOT NULL,         -- 行索引
  timestamp_ms INTEGER NOT NULL,       -- 时间戳（毫秒）
  text TEXT NOT NULL,                  -- 歌词文本
  
  PRIMARY KEY(song_id, kind, line_index),
  FOREIGN KEY(song_id) REFERENCES songs(id) ON DELETE CASCADE
);

CREATE INDEX idx_lyrics_song_kind ON lyrics(song_id, kind);
```

**说明**：
- 分离存储 `embedded`（内嵌）和 `external`（外部 .lrc）歌词
- `line_index` 保证顺序
- 级联删除：删除 song 时自动删除关联歌词

---

#### 4. `directories` 表（目录 hash）

```sql
CREATE TABLE directories(
  root_id INTEGER NOT NULL,            -- 外键 → roots.id
  relative_path TEXT NOT NULL,         -- 相对路径（相对于 root）
  directory_hash TEXT NOT NULL,        -- 目录内容 hash
  mtime_ns INTEGER,                    -- 目录修改时间
  
  PRIMARY KEY(root_id, relative_path),
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);
```

**说明**：
- 用于快速检测目录级别的变化
- Merkle tree 的叶子节点

---

#### 5. `errors` 表（扫描错误记录）

```sql
CREATE TABLE errors(
  root_id INTEGER NOT NULL,            -- 外键 → roots.id
  error_index INTEGER NOT NULL,        -- 错误索引
  code TEXT NOT NULL,                  -- 错误码（如 'MetadataReadFailed'）
  message TEXT NOT NULL,               -- 错误消息
  detail TEXT NOT NULL,                -- 详细信息
  path TEXT,                           -- 出错文件路径（可选）
  
  PRIMARY KEY(root_id, error_index),
  FOREIGN KEY(root_id) REFERENCES roots(id) ON DELETE CASCADE
);
```

**说明**：
- 记录扫描过程中的错误（如 TagReader 失败）
- 用户可查看哪些文件扫描失败

---

#### 6. `schema_meta` 表（元数据）

```sql
CREATE TABLE schema_meta(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

-- 初始数据
INSERT INTO schema_meta(key, value) VALUES('schema_version', '2');
```

---

## 核心操作

### 1. 加载 Root Cache

```cpp
std::optional<CachedRoot> SQLiteScannerCache::loadRoot(const std::filesystem::path& rootPath) const {
    // 1. 查询 roots 表
    Statement select{db, "SELECT id, directory_hash FROM roots WHERE path=?1;"};
    select.bind(1, rootPath);
    if (!select.stepRow()) {
        return std::nullopt;  // Cache miss
    }
    
    CachedRoot root;
    root.rootPath = rootPath;
    root.directoryHash = select.textColumn(1);
    const auto rootId = select.int64Column(0);
    
    // 2. 加载关联数据
    loadDirectories(db, rootId, root);  // directories 表
    loadSongs(db, rootId, root);        // songs + lyrics 表
    loadErrors(db, rootId, root);       // errors 表
    
    return root;
}
```

**性能**：
- 单次查询加载完整 root 数据
- 对于 5000 首歌，加载时间约 **100-500 ms**（取决于磁盘速度）

---

### 2. 保存 Root Cache

```cpp
void SQLiteScannerCache::saveRoot(const CachedRoot& root) {
    auto transaction = beginWriter();  // 开启事务
    
    // 1. Upsert root
    const auto rootId = ensureRoot(db, root);
    
    // 2. 替换 directories（删除旧数据，插入新数据）
    replaceDirectories(db, rootId, root.directories);
    
    // 3. Upsert songs
    for (const auto& song : root.songs) {
        const auto songId = upsertSong(db, rootId, song);
        replaceLyrics(db, songId, "embedded", song.embeddedLyrics);
        replaceLyrics(db, songId, "external", song.externalLyrics);
    }
    
    // 4. 删除不再存在的 songs
    pruneDeletedSongs(db, rootId, retainedTrackIds);
    
    // 5. 替换 errors
    replaceErrors(db, rootId, root.errors);
    
    transaction.commit();  // 提交事务
}
```

**特点**：
- **原子性**：整个 root 保存在单个事务中
- **替换策略**：先删除旧数据，再插入新数据（directories、lyrics、errors）
- **Upsert 策略**：songs 表使用 `ON CONFLICT DO UPDATE`

---

### 3. 增量更新（用户统计）

```cpp
void SQLiteScannerCache::updateUserStats(const std::filesystem::path& rootPath, 
                                         const std::string& trackId, 
                                         CachedUserStats stats) {
    auto transaction = beginWriter();
    
    Statement update{db, 
        "UPDATE songs SET play_count=?1, rating=?2, last_played_ms=?3 "
        "WHERE root_id=(SELECT id FROM roots WHERE path=?4) AND track_id=?5;"};
    
    update.bind(1, static_cast<int64_t>(stats.playCount));
    update.bindOptional(2, stats.rating);
    update.bindOptionalSystemTime(3, stats.lastPlayed);
    update.bind(4, rootPath);
    update.bind(5, trackId);
    update.stepDone();
    
    transaction.commit();
}
```

**用途**：
- 播放计数、评分、最后播放时间的快速更新
- 不需要重新扫描整个 root

---

### 4. Cache 维护

```cpp
struct CacheMaintenancePolicy {
    std::uintmax_t softDatabaseBytes = 256 MB;  // 软限制：开始清理
    std::uintmax_t hardDatabaseBytes = 512 MB;  // 硬限制：触发 VACUUM
    std::uintmax_t passiveCheckpointWalBytes = 4 MB;  // WAL 大小触发 checkpoint
    std::uint32_t maxCachedRoots = 8;           // 最多缓存 8 个根目录
};

CacheMaintenanceResult SQLiteScannerCache::maintainCache() {
    // 1. Checkpoint WAL（如果 > 4 MB）
    if (decision.checkpointRecommended) {
        result.checkpoint = checkpointPassive();
    }
    
    // 2. 清理旧 roots（如果 > 8 个或 > 256 MB）
    if (decision.cleanupRecommended) {
        result.rootsRemoved = pruneOldestRoots(maxCachedRoots);
    }
    
    // 3. VACUUM（如果 > 512 MB）
    if (decision.vacuumRecommended) {
        exec(db, "VACUUM;");
        result.vacuumed = true;
    }
    
    return result;
}
```

**LRU 淘汰策略**：
```sql
-- 删除最旧的 roots（基于 updated_at_ms）
SELECT id FROM roots 
ORDER BY updated_at_ms ASC, id ASC 
LIMIT ?1;
```

---

## 当前设计的优缺点

### ✅ 优点

1. **Root-based 隔离**：每个根目录独立缓存，易于管理
2. **级联删除**：删除 root 自动清理所有关联数据（songs、lyrics、errors）
3. **WAL 模式**：支持并发读（多个线程可同时读取）
4. **事务安全**：所有写操作在事务中，保证一致性
5. **LRU 淘汰**：自动清理最旧的缓存，避免无限增长
6. **用户统计分离**：play_count 等数据可独立更新，不影响扫描流程

### ⚠️ 缺点与改进空间

1. **`content_hash` 是全文件 hash**
   - **问题**：每次扫描都需要读取完整文件（慢）
   - **改进**：改为 metadata hash（path + size + mtime）
   - **影响**：需要理解 `content_hash` 的语义是"文件唯一标识"，不一定是"内容 hash"

2. **`track_id` 是文件路径**
   - **问题**：移动/重命名文件会丢失 play_count 等用户数据
   - **改进**：改为 content_hash 作为稳定 ID
   - **影响**：需要修改 `UNIQUE(root_id, track_id)` 约束

3. **`cachedSongByPath()` 是 O(n) 线性查找**
   - **问题**：对于 5000 首歌，每次查找需遍历 5000 条记录
   - **改进**：构建 `unordered_map<path, CachedSong*>` 内存索引
   - **影响**：热扫描性能提升 10-50x

4. **没有 `file_size_bytes` 和 `file_mtime_ns` 的索引**
   - **问题**：metadata-based hash 需要频繁查询这两个字段
   - **改进**：添加复合索引 `idx_songs_metadata(root_id, file_path, file_size_bytes, file_mtime_ns)`
   - **影响**：加速增量扫描的变化检测

---

## Metadata Hash 集成方案

### 修改 1：`content_hash` 语义变更

**当前**：
```cpp
// reconcileAudio() 中
const auto hash = hashFileContent(audioPath, ...);  // 全文件读取
song.metadata.contentHash = *hash.hash;
```

**改为**：
```cpp
// reconcileAudio() 中
const auto metadataHash = computeMetadataHash(audioPath);  // 只 stat()
song.metadata.contentHash = metadataHash;
```

**SQL schema 不变**：
- `content_hash TEXT NOT NULL` 字段仍然存在
- 语义从"文件内容 hash"变为"文件身份 hash"
- 向后兼容：旧 cache 会因 hash 不匹配触发重新扫描（一次性迁移）

---

### 修改 2：增量扫描利用 metadata

**当前逻辑**（伪代码）：
```cpp
auto cachedSong = cachedSongByPath(cachedRoot, audioPath);
if (cachedSong && cachedSong->metadata.contentHash == fileContentHash(audioPath)) {
    return cachedSong;  // Cache 命中
}
// Cache miss：重新 TagReader
```

**优化后**：
```cpp
auto cachedSong = cachedSongByPath(cachedRoot, audioPath);
if (cachedSong) {
    auto currentMetadataHash = computeMetadataHash(audioPath);  // 极快
    if (cachedSong->metadata.contentHash == currentMetadataHash) {
        return cachedSong;  // Cache 命中（无文件读取）
    }
}
// Cache miss：重新 TagReader
```

**性能提升**：
- 热扫描：从 600ms → **< 100ms**（移除文件 hash 开销）
- 冷扫描：从 60-80秒 → **20-30秒**（只扫描变化文件）

---

### 修改 3：增量扫描三阶段（已设计）

见 `docs/metadata-based-content-hash.md` 的增量扫描策略。

---

## 实施建议

### Phase 1：Metadata Hash（2 天）

1. 实现 `computeMetadataHash(path)`
2. 替换 `hashFileContent()` 调用
3. 更新性能统计（移除 `totalHashTimeMs`）
4. 测试向后兼容性

### Phase 2：cachedSongByPath 优化（1 天）

```cpp
// 在 reconcileRoot() 开始时构建索引
std::unordered_map<std::string, const cache::CachedSong*> songIndex;
if (cachedRoot.has_value()) {
    for (const auto& song : cachedRoot->songs) {
        songIndex[pathKey(song.metadata.filePath)] = &song;
    }
}

// 替换 O(n) 查找
auto it = songIndex.find(pathKey(audioPath));
if (it != songIndex.end()) {
    cachedSong = *it->second;
}
```

### Phase 3：增量扫描（3 天）

1. 实现 `detectDeletedFiles()`
2. 实现 `detectNewFiles()`
3. 实现 `detectChangedFiles()`
4. 集成到 `runScan()` 路径选择

---

## 总结

**当前 SQLite 设计**：
- ✅ 成熟、可靠、支持事务和并发读
- ✅ Root-based 隔离，易于管理
- ⚠️ `content_hash` 是全文件 hash（性能瓶颈）
- ⚠️ `cachedSongByPath()` 是 O(n) 线性查找

**Metadata Hash 集成**：
- 零 schema 变更（只改 `content_hash` 语义）
- 向后兼容（旧 cache 一次性迁移）
- 性能提升：热扫描 6-10x，冷扫描 2-4x

**下一步**：
1. 实施 metadata hash（2 天）
2. 优化 cachedSongByPath（1 天）
3. 实施增量扫描（3 天）
4. 总计：**6 天**完整优化
