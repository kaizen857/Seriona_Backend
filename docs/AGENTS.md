# AGENTS.md

## 项目边界
- 当前目录就是仓库根目录；不要扫描上级目录找 Seriona 源码或配置。
- 这是纯 C++23 音乐播放器后端；不要加入 Qt/QML/UI，系统媒体集成只能留在 metadata 私有实现层。
- `README.md` 只有标题；未发现 CI、formatter、lint、pre-commit、lockfile、`CMakePresets.json` 或 repo-local OpenCode 配置，优先相信 CMake 和源码。
- 面向用户的回答和新增项目文档使用中文；`.omo/` 是 OpenCode 运行记录，不是项目源码。

## 入口与权威来源
- 目标清单只看 `CMakeLists.txt`、`app/CMakeLists.txt`、`tests/CMakeLists.txt`、`tools/CMakeLists.txt`。
- 应用目标是 `seriona`，编译 `app/main.cpp`、`app/terminal_controller.cpp`、`app/terminal_io.cpp`、`src/app/runtime_paths.cpp`、`src/logging/logging.cpp`，并链接 `seriona_control`。
- 核心静态库目标：`seriona_audio`、`seriona_scanner`、`seriona_metadata`、`seriona_control`；公开 API 在 `inc/seriona/...`，实现主要在 `src/...`。
- `app/main.cpp` 只校验一个路径参数；真实终端控制流程在 `app/terminal_controller.cpp`，通过 `makeProductionMediaController()` 装配 audio、scanner、metadata。
- 音频契约在 `inc/seriona/audio/audio_contracts.h`：对外门面 `AudioPlayer`，服务接口 `AudioPlaybackService`。
- 扫描契约在 `inc/seriona/scanner/scanner_contracts.h` 和 `inc/seriona/scanner/file_scanner_service.h`；`makeFileScannerService()` 走内部依赖装配。
- 控制契约在 `inc/seriona/control/control_contracts.h`，连接 audio、scanner 和 metadata；metadata 对外入口在 `inc/seriona/metadata/metadata_contracts.h` 的 `MetadataSharingService`。

## 构建与测试
- 配置：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`；测试默认已开启，但显式传参更稳。
- 构建：`cmake --build build`。
- 全量测试：`ctest --test-dir build --output-on-failure`。
- 单个或分组测试：`ctest --test-dir build -R <regex> --output-on-failure`；测试名都是 `tests/CMakeLists.txt` 里的 `seriona.*`。
- 常用分组：`-R 'seriona.audio'`、`-R 'seriona.scanner'`、`-R 'seriona.metadata'`、`-R 'seriona.control'`。
- 可选工具默认不构建；需要 `seriona_miniaudio_platform_probe` 时重新配置 `-DSERIONA_BUILD_TOOLS=ON`。
- 配置期需要 CMake 3.20+、C++23、`pkg-config` 可找到 FFmpeg (`libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample`)、SQLite3、`libxxhash`；Linux 还需要 `sdbus-c++`。
- CMake 从相邻目录查找 `TagReaderCore`：先试 `../TagReader`，再试 `../../cppProject(app_and_lib)/TagReader`；两者都不存在会在配置期失败。

## 硬约束
- 音频模块边界：FFmpeg 解封装/解码/filter graph、PCM 队列、播放时钟、miniaudio 设备层和 `BackendEventSink` 上行事件。
- miniaudio 数据回调走 `AudioOutputDevice::renderCallback()`；实时路径只读 PCM、应用音量/静音、更新原子计数，禁止 FFmpeg、`BackendEventSink`、日志、动态分配、阻塞锁和设备生命周期调用。
- 音频测试默认用 fake `AudioOutputDeviceBackend` 或生成的短 fixture，不依赖真实音频硬件或版权媒体。
- scanner 的稳定契约头是 `scanner_contracts.h` 和 `file_scanner_service.h`；不要把 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型泄漏进这两个契约头。
- scanner 内部 TagReader 适配在 `inc/seriona/scanner/tag_reader_metadata_adapter.h` / `src/scanner/tag_reader_metadata_adapter.cpp`，watcher/SQLite 细节留在 scanner 内部实现。
- metadata 平台细节留在 `src/metadata/`；不要把 MPRIS、sdbus-c++ 或 Windows 私有类型泄漏到公共契约、audio、scanner 或实时路径。

## Scanner 模块优化计划

### 当前性能问题
- **全文件 hash**：每次扫描读取完整文件（5000 首歌约 40-50 秒）
- **串行处理**：无并发，TagReader 串行调用
- **用户数据丢失**：文件移动后 play_count 等统计丢失
- **预估性能**：5000 首歌约 235 秒（比旧 MusicPlayer 慢 70 倍）

### 最优化方案（已设计）
详见 `docs/optimal-scanner-architecture.md`（1277 行完整设计文档）

**核心创新**：
1. **双 ID 系统**：
   - `content_id`：稳定 ID（duration + title + artist），关联用户数据
   - `location_id`：易变 ID（path + size + mtime），检测文件变化
   - 解决文件移动丢失用户数据问题

2. **Metadata Hash**（零文件读取）：
   - 完全抛弃全文件 hash
   - 使用 `path + size + mtime` 计算 `location_id`
   - 性能提升 **4000 倍**（40 秒 → 10 毫秒）

3. **并发架构**（BS::thread_pool）：
   - 第三方库：`https://github.com/bshoshany/thread-pool`
   - Header-only，MIT 许可证
   - 全 CPU 线程数 worker
   - TagReader semaphore 限制并发（默认 4）
   - 批量任务提交（64 个一批）

4. **SQLite Schema V3**（重新设计）：
   - `content` 表：内容元数据 + 用户统计
   - `locations` 表：文件位置（外键 → content）
   - 分离内容和位置，支持去重

5. **增量扫描**（三阶段）：
   - 检测删除、新增、变化文件
   - 只处理变化部分（95% 未变 → < 2 秒）

**性能目标**：
| 场景 | 当前实现 | 优化后 | 提升 |
|------|---------|--------|------|
| 热扫描（95% 未变）| 235 秒 | **< 5 秒** | **47x** |
| 温扫描（20% 变化）| 235 秒 | **8-12 秒** | **20-29x** |
| 冷扫描（首次）| 235 秒 | **20-30 秒** | **8-12x** |
| 文件移动 | 用户数据丢失 ❌ | **数据保留** ✅ | 质的飞跃 |

**实施路线**（9 天）：
- Phase 1：数据库重构（3 天）
- Phase 2：并发 Worker Pool + BS::thread_pool（2 天）
- Phase 3：增量扫描（2 天）
- Phase 4：性能调优与测试（2 天）

### 相关文档
- `docs/optimal-scanner-architecture.md`：完整架构设计（主文档）
- `docs/metadata-based-content-hash.md`：Metadata hash 方案
- `docs/sqlite-cache-design.md`：当前 SQLite 分析
- `docs/scanner-architecture-comparison.md`：旧新项目对比
- `docs/scanner-concurrency-recommendation.md`：并发策略调研
- `docs/hash-optimization-strategy.md`：Hash 优化策略

### 关键技术决策
1. ✅ **使用 BS::thread_pool**（而非手动实现）
   - 代码量减少 70%
   - 任务提交开销 < 200 纳秒
   - 维护成本低，社区活跃

2. ✅ **Metadata hash**（而非 mtime+CRC32 或部分 hash）
   - Path + size + mtime 组合
   - 零碰撞风险（路径全局唯一）
   - 跨平台支持

3. ✅ **双 ID 系统**（而非单一 track_id）
   - 用户数据永不丢失
   - 支持文件去重
   - 增量扫描高效

### 注意事项
- 实施前务必阅读 `docs/optimal-scanner-architecture.md` 完整文档
- Schema 迁移（v2 → v3）需要自动迁移脚本
- TagReader 线程安全性需要压测验证（默认 semaphore=4 保守配置）
- BS::thread_pool 使用 FetchContent 集成（CMake 3.11+）
