# Seriona Backend 文档索引

## 📚 文档导航

### 🎯 核心指南（必读）⭐⭐⭐

1. **[AGENTS.md](./AGENTS.md)** - 项目边界和开发规范
   - 项目范围和约束
   - 构建和测试指南
   - 硬约束说明

2. **[Scanner 完整重构设计](./SCANNER_COMPLETE_REDESIGN.md)** ⭐⭐⭐ **[推荐]**
   - **整合方案**：预分配节点架构 + CUE 虚拟文件夹
   - **规模**：984 行完整设计（含详细实施说明）
   - **包含**：每个 Phase 的实现目标、输入输出、关键逻辑、验收标准、代码
   - **效果**：task 29 最终实测 5000 首歌 1,724 ms，`resultAgg` 47 ms；10000 首歌 4,318 ms，`resultAgg` 92 ms
   - **工作量**：7 天
   - **用途**：直接用于编写详细任务书

3. **[最终交付总结](./FINAL_SUMMARY.md)** - 工作完成总结
   - 完成的所有工作
   - 文档结构说明
   - 使用指南
   - 仅保留与 task 29 最终口径一致的总结

---

### 🏗️ 架构设计（参考）

#### Scanner 架构
1. **[最优架构设计](./optimal-scanner-architecture.md)**
   - Scanner 完整架构（1277 行）
   - 双 ID 系统（content_id + location_id）
   - 并发 Worker Pool
   - SQLite Schema V3
   - 增量扫描三阶段

2. **[架构对比分析](./scanner-architecture-comparison.md)**
   - 新旧项目对比
   - 设计决策解释

3. **[并发策略推荐](./scanner-concurrency-recommendation.md)**
   - BS::thread_pool 集成
   - Worker Pool 配置
   - 性能调优

4. **[SQLite 缓存设计](./sqlite-cache-design.md)**
   - 当前 Schema 分析
   - V2 → V3 迁移策略

5. **[哈希优化策略](./hash-optimization-strategy.md)**
   - XXH3 性能分析
   - Metadata hash 方案

6. **[基于元数据的内容哈希](./metadata-based-content-hash.md)**
   - 零文件读取方案
   - 性能权衡

---

### 📖 模块使用指南

1. **[File Scanner](./file-scanner.md)**
   - Scanner API 说明
   - 事件系统
   - CUE 虚拟文件夹与迁移说明

2. **[Audio Player](./audio-player.md)**
   - AudioPlayer API
   - 播放控制

3. **[Metadata Sharing](./metadata-sharing.md)**
   - 元数据共享服务
   - MPRIS 集成（Linux）

---

## 🗂️ 按主题分类

### 立即可做的重构 ⭐
- **核心重构**：[Scanner 完整重构设计](./SCANNER_COMPLETE_REDESIGN.md) - 7 天
  - 每个 Phase 都有：📋 实现目标、🔄 输入输出、🔑 关键逻辑、✅ 验收标准、💻 代码
  - 预分配节点架构（解决排序瓶颈）
  - CUE 虚拟文件夹支持
  - 统一的扫描流程
  - 可直接用于编写任务书

### Scanner 架构参考
- 完整架构：`optimal-scanner-architecture.md`
- 架构对比：`scanner-architecture-comparison.md`
- 并发策略：`scanner-concurrency-recommendation.md`
- 缓存系统：`sqlite-cache-design.md`
- 哈希优化：`hash-optimization-strategy.md`, `metadata-based-content-hash.md`

### 模块使用
- Scanner：`file-scanner.md`
- Audio：`audio-player.md`
- Metadata：`metadata-sharing.md`

### 开发规范
- 项目约束：`AGENTS.md`

---

## 🎯 快速查找

### 我想...

#### 实施 Scanner 重构（强烈推荐）⭐⭐⭐
→ 阅读 [Scanner 完整重构设计](./SCANNER_COMPLETE_REDESIGN.md)  
→ 包含预分配节点 + CUE 支持的完整方案  
→ 每个 Phase 都有详细的实现说明  
→ 可直接用于编写详细任务书

#### 理解具体 Phase 的实施细节
→ 打开 [Scanner 完整重构设计](./SCANNER_COMPLETE_REDESIGN.md)  
→ 查找对应的 Phase  
→ 每个 Phase 包含：
  - 📋 实现目标（要达成什么）
  - 🔄 输入输出（接收什么，产出什么）
  - 🔑 关键逻辑（核心处理流程）
  - ✅ 验收标准（如何验证完成）
  - 💻 实现代码（完整参考实现）

#### 编写任务书
→ 阅读 [最终交付总结](./FINAL_SUMMARY.md)  
→ 查看"任务书拆分建议"章节  
→ 每个 Phase 的说明可直接复制到任务书

#### 理解 Scanner 架构
→ 阅读 [最优架构设计](./optimal-scanner-architecture.md)

#### 配置并发和缓存
→ 阅读 [并发策略推荐](./scanner-concurrency-recommendation.md)  
→ 阅读 [SQLite 缓存设计](./sqlite-cache-design.md)

#### 了解项目边界
→ 阅读 [AGENTS.md](./AGENTS.md)

---

## 📊 重构效果汇总

### 性能提升

| 口径 | 数值 | 说明 |
|------|------:|------|
| 计划早期理论预期 | 约 68s / 约 66s `resultAgg` | 设计阶段口径，只能作为目标，不是实测 |
| Task 15 权威实测基线 | 5000 首歌 `71,041 ms / 37 ms`，10000 首歌 `317,970 ms / 70 ms` | 当前对比基准 |
| Task 29 最终实测 | 5000 首歌 `1,724 ms / 47 ms`，10000 首歌 `4,318 ms / 92 ms` | 最新最终结果 |

### 对比结果

| 场景 | Task 15 基线 | Task 29 结果 | 加速比 |
|------|------:|------:|------:|
| **5000 首歌扫描** | 71,041 ms | 1,724 ms | **41.2x** |
| **`resultAgg` 阶段** | 37 ms | 47 ms | n/a |
| **10000 首歌扫描** | 317,970 ms | 4,318 ms | **73.6x** |
| **`resultAgg` 阶段** | 70 ms | 92 ms | n/a |

### 功能增强

| 功能 | 当前 | 重构后 |
|------|------|--------|
| **排序性能** | O(n² log n) | O(n) |
| **CUE 支持** | ❌ 无 | ✅ 完整支持 |
| **虚拟文件夹** | ❌ 无 | ✅ CUE 作为容器 |
| **底层文件隐藏** | ❌ 无 | ✅ 自动隐藏 |

---

## 🔄 重构设计核心思想

### 1. 预分配节点架构

**传统做法**（有问题）：
```
扫描 → Worker 返回结果 → 排序（66 秒！）→ 构建树
```

**新设计**（最优）：
```
扫描 → 预分配节点树 → Worker 填充节点 → 树已完成（无需排序）
```

**关键**：节点按扫描顺序创建，Worker 只负责填充内容。

---

### 2. CUE 虚拟文件夹

**当前行为**：CUE 文件显示为虚拟文件夹，tracks 嵌套在 `.cue` 节点下，底层音频文件从最终树中隐藏

**示例**：
```
文件系统：                     播放器树：
  album.cue          →          album.cue/
  album.flac                      ├── 01 - Track 1
  cover.jpg                       ├── 02 - Track 2
                                  └── 03 - Track 3
```

**特点**：
- ✅ CUE 文件名保留后缀（`album.cue`）
- ✅ 所有 tracks 显示在虚拟文件夹下
- ✅ 底层音频文件（`.flac`, `.wav`）**完全隐藏**

### 3. 口径区分

- 计划早期理论预期：`~68s / ~66s resultAgg`，只用于说明设计目标
- Task 15 实测基线：`71,041 ms / 37 ms`（5000 首歌），`317,970 ms / 70 ms`（10000 首歌）
- Task 29 最终实测：`1,724 ms / 47 ms`（5000 首歌），`4,318 ms / 92 ms`（10000 首歌）

---

## 📝 文档特点

### SCANNER_COMPLETE_REDESIGN.md 的完整性

**每个 Phase 都包含**：

```
### Phase X：阶段名称

#### 📋 实现目标
- 这个阶段要达成什么

#### 🔄 输入输出
**输入**：接收什么
**输出**：产出什么

#### 🔑 关键逻辑
1. 步骤 1
2. 步骤 2

#### ✅ 验收标准
- [ ] 如何验证完成

#### 💻 实现代码
```cpp
// 完整的参考实现
```
```

**示例**（Phase 1）：
- 📋 实现目标：先识别 CUE，再识别音频，跳过被引用的文件
- 🔄 输入输出：输入根目录，输出 ClassifiedPath 列表
- 🔑 关键逻辑：两遍遍历，第一遍 CUE，第二遍音频
- ✅ 验收标准：CUE 优先识别，被引用文件不重复，错误隔离
- 💻 实现代码：完整的 `discoverScannerPaths()` 实现

---

## 📈 文档更新历史

### 2026-06-29 v3.0（最新）✅
- ✅ **补充完整实施说明**：为每个 Phase 添加详细的四段说明
- ✅ **文档规模**：从 718 行扩充到 984 行（+266 行）
- ✅ **可操作性**：每个 Phase 都可直接理解和实施
- ✅ **可拆分性**：可直接拆分为详细任务书
- 📌 **最终状态**：完全完成，可立即使用

### 2026-06-29 v3.1（task 30 口径修正）✅
- ✅ **修正重构效果汇总**：去掉 `74x/915ms/66,462 ms resultAgg` 旧口径
- ✅ **补充最终实测数据**：写入 task 29 的 5000/10000 首歌最终结果
- ✅ **统一 CUE 说明**：与 `file-scanner.md` 保持一致

### 2026-06-29 v2.0
- 创建统一的完整重构设计
- 整合预分配节点 + CUE 支持
- 删除分离的优化和 CUE 文档

### 2026-06-29 v1.0
- 首次创建文档体系

---

## 🔗 外部资源

### 代码库
- 仓库根目录：`/home/kaizen857/cppProject(app_and_lib)/Seriona_Backend`
- 构建目录：`build/`
- 测试目录：`tests/`

### 关键源文件
- Scanner 编排器：`src/scanner/file_scanner_orchestrator.cpp`
- Worker Pool：`src/scanner/worker_pool.cpp`
- SQLite 缓存：`src/scanner/cache/sqlite_scanner_cache.cpp`

### 构建和测试
```bash
# 配置
cmake -S . -B build -DSERIONA_BUILD_TESTS=ON

# 构建
cmake --build build

# 测试
ctest --test-dir build -R 'seriona.scanner' --output-on-failure

# 关键 scanner 过滤
ctest --test-dir build -R 'seriona\.scanner\.(playlist_cue|e2e)' --output-on-failure

# 性能测试
./build/tests/scanner/scanner_detailed_perf_test
```

---

## 📝 使用建议

### 立即开始

1. **阅读核心文档**
   ```bash
   cat docs/SCANNER_COMPLETE_REDESIGN.md
   ```

2. **理解每个 Phase**
   - 每个 Phase 都有完整的四段说明 + 代码
   - 可以立即理解要做什么、怎么做、如何验证

3. **编写任务书**
   - 参考 [最终交付总结](./FINAL_SUMMARY.md) 的"任务书拆分建议"
   - 每个 Phase 的说明可直接复制到任务书

---

**索引版本**：v3.0  
**最后更新**：2026-06-29  
**维护者**：Seriona Backend Team  
**状态**：✅ 完全完成，可立即使用
