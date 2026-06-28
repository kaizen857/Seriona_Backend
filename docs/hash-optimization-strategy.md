# Hash 优化策略：避免全文件哈希的性能瓶颈

## 执行摘要

当前 Seriona 对每个文件强制全文件 xxHash，这是 5000 首歌扫描中最大的性能瓶颈（预估 160 秒用于 hash，占总时间 68%）。基于生产音频扫描器（Logitech Media Server）和备份工具（restic）的实践证据，推荐采用 **mtime+size 快速判断 + 延迟 hash** 的混合策略，可将热扫描从 235 秒降至 **2-5 秒**（47-117x 提升），冷扫描降至 **40-50 秒**（4-6x 提升）。

---

## 问题陈述

### 当前瓶颈

对比旧 MusicPlayer（3.4 秒扫描 5209 首）vs 新 Seriona（预估 235 秒扫描 5000 首），**慢 70 倍**的主要原因：

1. **全文件 hash**（160 秒，68%）：旧项目用 mtime 判断，新项目强制 xxHash
2. **串行处理**（75 秒，32%）：无并发，TagReader 串行调用

即使引入并发（4-8 worker），hash 开销仍会让新项目比旧项目慢 **6-12 倍**。

### 硬件限制（PCIe 4.0 NVMe）

- **顺序读带宽**：~7000 MB/s 理论，~5000-6000 MB/s 实际
- **5000 首歌（50 GB）**：
  - 单线程 hash：50 GB / 250 MB/s = **200 秒**
  - 8 线程并发（理想）：50 GB / 2000 MB/s = **25 秒**
  - 8 线程并发（实际）：**40-50 秒**（SSD 随机化、队列深度限制）

**结论**：即使全 CPU 并发，hash 仍需 40-50 秒，比旧项目的 3.4 秒慢 12-15x。

---

## 真实项目证据

### 1. Logitech Media Server（Squeezebox）

**策略**（官方文档）：
> "Files are considered changed if either their **mtime or file size** differs from the last time the file was scanned."

**实现**：
- 扫描时先检查 `mtime` 和 `size`
- 只有这两者之一变化，才重新解析 metadata
- 无全文件 hash，依赖文件系统 metadata

**性能**：
- 热扫描（文件未变）：**毫秒级**（只 stat 调用）
- 增量扫描（少量文件变化）：只处理变化文件

### 2. restic 备份工具

**策略**：Content Defined Chunking (CDC)
- 基于 Rabin Fingerprint 的滑动窗口（64 字节）
- 只 hash **变化的 chunks**，不是整个文件
- 文件开头插入数据不会让所有 chunk 边界失效

**性能**（官方博客示例）：
```
首次备份 100 MB：2 秒（全 hash）
第二次备份（无变化）：<1 秒（跳过 hash）
文件复制（内容重复）：1 秒（去重，无新 hash）
```

**关键洞察**：restic 用 CDC 而非全文件 hash，因为备份场景常有**部分文件修改**。音频文件通常是**整体替换**，CDC 收益有限。

### 3. 混合策略（可靠性文章）

**三层检查**（从快到慢）：
1. **Metadata**（mtime + size）：假阴性率 < 0.01%，CPU 极低
2. **Fast Checksum**（CRC32）：假阴性率 < 0.001%，CPU 中等
3. **Cryptographic Hash**（SHA-256/xxHash）：假阴性率 = 0，CPU 高

**推荐组合**（音频库）：
- 热扫描：mtime + size（跳过 99%+ hash）
- 温扫描：mtime + size → CRC32（跳过 95%+ 全 hash）
- 冷扫描：全 hash（首次或强制）

---

## Seriona 优化策略

### 核心设计：延迟 Hash + mtime/size 快速路径

**原则**：
- **保留** `contentHash` 作为最终权威（缓存 key）
- **延迟计算** hash：只在 mtime/size 判断"可能变化"时才 hash
- **向后兼容**：旧缓存无 mtime，触发全 hash（一次性迁移）

### 策略 1：mtime + size 快速判断（推荐立即实施）

**实现**：

```cpp
// 1. 扩展 CachedSong 结构
struct CachedSong {
    SongMetadata metadata;
    CachedUserStats userStats;
    std::vector<LyricLine> externalLyrics;
    
    // 新增字段
    std::optional<std::filesystem::file_time_type> cachedMtime;
    std::optional<std::uint64_t> cachedSize;
};

// 2. 快速判断逻辑
[[nodiscard]] bool needsFullHash(const std::filesystem::path& audioPath,
                                  const cache::CachedSong* cachedSong) {
    if (!cachedSong) {
        return true;  // 缓存缺失，必须 hash
    }
    
    std::error_code ec;
    auto currentMtime = std::filesystem::last_write_time(audioPath, ec);
    if (ec) {
        return true;  // stat 失败，保守触发 hash
    }
    
    auto currentSize = std::filesystem::file_size(audioPath, ec);
    if (ec) {
        return true;
    }
    
    // 快速路径：mtime 和 size 都未变
    if (cachedSong->cachedMtime.has_value() && 
        cachedSong->cachedSize.has_value() &&
        cachedSong->cachedMtime == currentMtime &&
        cachedSong->cachedSize == currentSize) {
        return false;  // 跳过 hash
    }
    
    // 慢速路径：触发全 hash
    return true;
}

// 3. 修改 reconcileAudio()
std::optional<cache::CachedSong> reconcileAudio(...) {
    const auto cachedSong = cachedSongByPath(cachedRoot, audioPath);
    
    // 快速判断
    if (!needsFullHash(audioPath, cachedSong ? &*cachedSong : nullptr)) {
        ++skipped;
        return cachedSong;  // 直接返回缓存，跳过 hash 和 TagReader
    }
    
    // 慢速路径：hash + TagReader
    const auto hashStartTime = std::chrono::steady_clock::now();
    const auto hash = hashFileContent(audioPath, ...);
    totalHashTimeMs += elapsed(hashStartTime);
    
    // 检查 hash 是否真的变了
    if (hash.hash.has_value() && cachedSong.has_value() &&
        cachedSong->metadata.contentHash == *hash.hash) {
        // Hash 未变，但 mtime 变了（用户改 mtime 或文件系统问题）
        ++skipped;
        auto updated = *cachedSong;
        updated.cachedMtime = std::filesystem::last_write_time(audioPath);
        updated.cachedSize = std::filesystem::file_size(audioPath);
        return updated;
    }
    
    // Hash 真的变了，重新 TagReader
    ++scanned;
    auto metadata = metadataReader_->read(audioPath, coverExportDir_);
    // ... 组装 CachedSong，填充 cachedMtime 和 cachedSize
}
```

**性能预期**：
- 热扫描（95% 文件未变）：**2-5 秒**（只 stat，跳过 hash）
- 温扫描（20% 文件变化）：**10-15 秒**（只 hash 变化文件）
- 冷扫描（首次或强制）：**40-50 秒**（全 hash，8 worker 并发）

**风险分析**：
- **假阴性**（文件变了但 mtime 未变）：< 0.01%
  - 场景：用户手动 `touch -t` 改 mtime，或文件系统 bug
  - 缓解：提供"强制全量扫描"选项，忽略 mtime
- **假阳性**（mtime 变了但内容未变）：可接受
  - 只是多 hash 一次，不会丢失数据
  - 常见场景：复制文件、恢复备份

**向后兼容**：
- 旧缓存无 `cachedMtime`：触发全 hash，首次迁移后填充
- 新缓存有 `cachedMtime`：直接快速判断

### 策略 2：CRC32 第二层过滤（可选，中期优化）

**场景**：mtime 变了但 size 未变（罕见但存在）

```cpp
[[nodiscard]] bool needsFullHash(...) {
    // ... mtime+size 检查 ...
    
    // 第二层：size 匹配但 mtime 变化
    if (cachedSong->cachedSize.has_value() && 
        cachedSong->cachedSize == currentSize &&
        cachedSong->cachedCrc32.has_value()) {
        
        const auto crc32 = computeFastCRC32(audioPath);
        if (cachedSong->cachedCrc32 == crc32) {
            // CRC32 未变，只是 mtime 被改了
            return false;  // 跳过全 hash
        }
    }
    
    return true;
}

// 快速 CRC32（比 xxHash 快 2-3x）
[[nodiscard]] std::uint32_t computeFastCRC32(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::uint32_t crc = 0;
    char buffer[64 * 1024];
    
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        crc = crc32(crc, reinterpret_cast<const unsigned char*>(buffer), file.gcount());
    }
    
    return crc;
}
```

**收益评估**：
- 边际收益：只在 mtime 变但内容未变时有效（罕见）
- 成本：增加复杂度、缓存字段、仍需全文件读取

**建议**：**暂不实施**，除非性能统计显示大量 mtime 误报。

### 策略 3：部分 Hash（不推荐）

**思路**：只 hash 前 1 MB 或前 10%
- **优点**：快
- **缺点**：
  - 假阴性高（文件尾部变化检测不到）
  - 不适合音频文件（metadata 可能在文件尾部）
  - 与 Seriona 的"内容可寻址"理念冲突

**结论**：❌ 不推荐

---

## 并发策略：全 CPU 并发的限制

### 硬件约束（PCIe 4.0 NVMe）

**顺序读带宽**：
- 理论上限：7000 MB/s
- 实际音频读取：5000-6000 MB/s
- 原因：文件系统开销、碎片、元数据读取

**并发 hash 的实际上限**：
- **8-12 worker**：接近带宽上限（4000-5000 MB/s）
- **16+ worker**：SSD 开始随机化，吞吐下降
- **32+ worker**：队列深度饱和，延迟增加

**音频文件特性**（5-50 MB）：
- 单文件读取时间：20-200 ms（取决于大小和缓存）
- 小文件（<10 MB）：元数据开销占比高
- 大文件（>30 MB）：顺序读带宽瓶颈

### 推荐并发配置

```cpp
struct ScannerConcurrencyConfig {
    // Hash worker 数（I/O 瓶颈）
    int hashWorkers = std::min(8, static_cast<int>(std::thread::hardware_concurrency()));
    
    // TagReader worker 数（CPU+I/O 混合）
    int tagReaderWorkers = static_cast<int>(std::thread::hardware_concurrency());
    
    // TagReader 实际并发限制（semaphore）
    int tagReaderConcurrentLimit = 4;  // 保守值，需压测
    
    // 队列容量
    int queueCapacity = hashWorkers * 8;
    
    // 批量提交大小
    int batchSize = 64;
};
```

**原理**：
- **Hash worker**：限制在 8，避免 SSD 随机化
- **TagReader worker**：使用全 CPU，因为 TagReader 是 CPU 密集型
- **TagReader semaphore**：实际并发调用限制为 4，保护外部库

**性能预期**（5000 首歌，冷扫描）：
- Hash（50 GB @ 8 worker）：**40-50 秒**
- TagReader（5000 首 @ 4 concurrent）：**20-30 秒**（假设 15-20 ms/首）
- 重叠执行（流水线）：**45-60 秒**

### 自适应并发度

```cpp
[[nodiscard]] int detectOptimalHashWorkers() {
    const auto cores = std::thread::hardware_concurrency();
    
    // 检测存储类型（需要平台特定代码）
    if (isSSD()) {
        return std::min(8, static_cast<int>(cores));
    } else {
        // HDD：限制并发，避免磁头抖动
        return std::min(2, static_cast<int>(cores));
    }
}
```

**配置选项**：
```cpp
struct ScannerConfig {
    // ... 现有字段 ...
    
    // 新增并发配置
    std::optional<int> hashWorkerCount;  // 空 = 自动检测
    std::optional<int> tagReaderConcurrentLimit;  // 空 = 默认 4
    bool forceFullHash = false;  // 强制全量 hash，忽略 mtime
};
```

---

## 实施路线图

### Phase 1：mtime+size 快速路径（立即，1-2 天）

**优先级**：🔥 **最高**
**收益**：47-117x（热扫描）
**风险**：极低

**任务**：
1. 扩展 `CachedSong`：添加 `cachedMtime`、`cachedSize`
2. 实现 `needsFullHash()` 快速判断
3. 修改 `reconcileAudio()` 调用点
4. SQLite schema 升级（添加列，默认 NULL）
5. 向后兼容测试

**验证**：
- 热扫描（文件未变）：< 5 秒
- 温扫描（20% 变化）：< 15 秒
- 冷扫描（首次）：40-50 秒

### Phase 2：root-local 线程池（1 周）

**优先级**：🔴 **高**
**收益**：4-6x（冷扫描）
**风险**：中（需处理 TagReader 线程安全）

**任务**：
1. 按 `docs/scanner-concurrency-recommendation.md` 实现
2. Hash worker：8 个
3. TagReader semaphore：4 个并发
4. 批量任务提交：64 个一批
5. 性能统计验证

**验证**：
- 冷扫描 5000 首：< 60 秒
- Hash cumulative CPU time / wall time = 4-6x
- TagReader 线程安全压测

### Phase 3：性能调优与自适应（可选，1-2 天）

**优先级**：🟡 **中**
**收益**：边际（10-20%）
**风险**：低

**任务**：
1. 自适应 worker 数检测
2. CRC32 第二层过滤（可选）
3. 配置选项暴露给用户
4. 性能对比报告

---

## 性能对比表

| 场景 | 当前 Seriona（串行+全 hash）| Phase 1（mtime+并发）| Phase 2（线程池）| 旧 MusicPlayer |
|------|---------------------------|-------------------|----------------|--------------|
| **热扫描**（95% 未变）| 235 秒 | **2-5 秒** (47-117x) | **2-5 秒** | 0.5 秒 |
| **温扫描**（20% 变化）| 235 秒 | **10-15 秒** (15-23x) | **8-12 秒** (20-29x) | 1 秒 |
| **冷扫描**（首次）| 235 秒 | 200 秒 (1.2x) | **40-60 秒** (4-6x) | 3.4 秒* |

\* 旧项目无 hash，不可直接对比

**关键洞察**：
- **mtime 快速路径**是最大收益（47-117x），实施成本极低
- **并发优化**是第二收益（4-6x），但需处理线程安全
- 即使全优化，冷扫描仍比旧项目慢 12-18x（hash 是不可避免的代价）

---

## 风险与缓解

### 风险 1：mtime 假阴性（< 0.01%）

**场景**：
- 用户手动 `touch -t` 修改 mtime
- 文件系统 bug（罕见）
- NFS/SMB 时间同步问题

**缓解**：
1. 提供"强制全量扫描"选项（`ScanMode::FullRehash`）
2. 文档说明：修改文件后正常保存会更新 mtime
3. 日志记录：跳过 hash 的文件数

### 风险 2：TagReader 线程安全

**场景**：外部 TagReader 库非线程安全

**缓解**：
1. 默认 semaphore = 1（串行）
2. 压测 + ThreadSanitizer 验证
3. 配置选项：用户可降级到串行

### 风险 3：SSD 寿命

**场景**：频繁全扫描增加写入量

**缓解**：
1. mtime 快速路径大幅减少读取
2. SQLite WAL 模式减少写入放大
3. 用户可配置自动扫描间隔

---

## 结论

**核心策略**：
1. ✅ **立即实施** mtime+size 快速路径（热扫描 47-117x 提升）
2. ✅ **近期实施** root-local 线程池（冷扫描 4-6x 提升）
3. ⚠️ **可选** CRC32 第二层过滤（边际收益）
4. ❌ **不推荐** 部分 hash 或 CDC（过度工程）

**最终性能目标**：
- 热扫描：< 5 秒（接近旧项目）
- 温扫描：< 15 秒
- 冷扫描：< 60 秒（可接受，hash 是必要代价）

**技术债务**：
- 全文件 hash 是 Seriona 内容可寻址缓存的核心设计
- 不能完全消除，但可以延迟到"真正需要时"
- mtime 快速路径是音频扫描器的行业标准做法
