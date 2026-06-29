# 文件扫描模块实现说明

## 目的

本文档面向开发者，记录 `seriona_scanner` 的集成边界、依赖策略、外部 `.lrc` 处理规则和当前延期项。扫描模块仍是纯 C++23 后端能力，不改变 `seriona` 命令行播放器当前只接收单个音频文件并播放的行为。

## 当前能力边界

1. 公共入口仍然是 `FileScannerService`，现有调用方继续走 `makeFileScannerService()` 这条路径。
2. 公共契约没有泄漏 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型，scanner 的私有实现细节仍留在 `src/scanner/`。
3. 当前实现已经覆盖普通音频、外部 `.lrc`、CUE 发现、CUE 虚拟文件夹和底层音频隐藏这些可见行为。

## 构建与集成边界

1. `seriona_scanner` 是固定名称的静态库目标，由根 `CMakeLists.txt` 创建，应用目标通过链接该库证明 scanner 已进入默认构建图。
2. app 只链接 scanner；当前不新增扫描 CLI、媒体库 CLI 或 watcher CLI，因此运行时行为仍由 `app/main.cpp` 的音频播放路径决定。
3. scanner 公共头只暴露标准库类型和 `FileScanner`/`FileScannerService` 契约，不暴露 TagReader、SQLite、watcher、FFmpeg、Qt/QML 或音频设备类型。

## 依赖策略

1. SQLite3 与 `libxxhash` 是 scanner 构建期必需依赖，缺失时由 CMake scanner dependency gate 失败并提示外部安装；仓库脚本不运行包管理器。
2. TagReader 只作为 `seriona_scanner` 的私有链接依赖使用，生产适配器仅调用 `TagReader::Read(path, coverExportDir)`。
3. `wtr/watcher` 以 vendored/pinned 单头文件形式接入，真实 watcher 只位于 scanner 私有实现层；默认测试使用 fake watcher。

## 外部 `.lrc` 规则

1. scanner 只匹配同目录、同 basename 的外部 `.lrc` 文件；`.lrc` 本身不是音频候选，也不会出现在播放列表树节点中。
2. 当前有效歌词优先级为 `ExternalLrc` 高于非空 TagReader 嵌入歌词 `EmbeddedTag`，两者都不存在时为 `None`。
3. 外部 `.lrc` 创建或内容变化只刷新歌词解析、歌词 hash、有效歌词来源和歌曲快照，不重新调用 TagReader。
4. 外部 `.lrc` 删除后会从缓存中的嵌入歌词回退到 `EmbeddedTag`，没有嵌入歌词时回退到 `None`。

## 扫描配置与环境变量

`ScannerConfig` 的并发字段都有向后兼容默认值；现有调用方可以继续使用 `ScannerConfig{}`，无需设置任何选项。

| 选项 | 默认值 | 行为 |
|------|--------|------|
| `workerCount` | `0` | `0` 表示使用运行时检测到的 CPU 线程数；正整数会传给 scanner worker pool。|
| `tagReaderConcurrency` | `0` | `0` 表示使用 worker 数的一半作为保守默认值；正整数限制同时进入 TagReader 的任务数，最终不会超过 worker 数。|
| `enableIncrementalScan` | `true` | 为 `false` 时，即使调用方请求 `ScanMode::Incremental`，运行时也按 Full 扫描执行。|
| `forceFull` | `false` | 为 `true` 时强制 Full 扫描，优先级高于传入的扫描模式。|

环境变量在运行时覆盖 `ScannerConfig` 的并发字段，适合排查机器差异或强制串行回退：

| 环境变量 | 合法值 | 优先级与 fallback |
|----------|--------|-------------------|
| `SERIONA_SCANNER_WORKERS` | 正整数 | 覆盖 `ScannerConfig::workerCount`；非法、空值或 `0` 会记录 warning 并继续使用配置/默认值。|
| `SERIONA_SCANNER_TAGREADER_CONCURRENCY` | 正整数 | 覆盖 `ScannerConfig::tagReaderConcurrency`；非法、空值或 `0` 会记录 warning 并继续使用配置/默认值。|
| `SERIONA_SCANNER_DISABLE_CONCURRENCY` | `0` 或 `1` | `1` 强制 `workerCount=1`、`tagReaderConcurrency=1`，优先级高于上述两个环境变量；非法值会记录 warning 并按未设置处理。|

示例：

```bash
SERIONA_SCANNER_WORKERS=4 SERIONA_SCANNER_TAGREADER_CONCURRENCY=2 ./seriona /path/to/music.flac
SERIONA_SCANNER_DISABLE_CONCURRENCY=1 ./seriona /path/to/music.flac
```

## CUE 状态

1. 当前扫描流程已经把 `.cue` 识别为 `CueSheet`。
2. 在最终树里，`.cue` 会表现为虚拟文件夹，tracks 嵌套在 `.cue` 节点下，引用的底层音频文件不会再作为普通歌曲重复出现。
3. 损坏或格式不完整的 CUE 仍会出现在扫描结果里，并附带错误记录；扫描会继续处理同目录下的其他文件。
4. 这个行为已经通过 `seriona.scanner.playlist_cue` 和 `seriona.scanner.e2e` 两个 CTest 入口覆盖。

## CUE 用户体验

1. 对用户来说，CUE 像一个虚拟文件夹入口：打开后看到的是这张 CUE 对应的轨道集合，而不是把底层音频文件和分轨信息混在普通文件列表里。
2. 当同目录里同时存在 `album.cue` 和 `album.flac` 时，`album.cue` 仍会被保留，`album.flac` 会因为被引用而隐藏。
3. 当 CUE 本身有问题时，用户仍能看到这份 CUE 的存在和对应错误，其他正常文件不会被连带影响。
4. 这不是新的 CLI 能力，现有命令行入口没有变，变化只发生在 scanner 结果的可见结构。

## 默认验证

1. scanner 测试通过 `ctest --test-dir build -R 'seriona.scanner' --output-on-failure` 分组运行。
2. CUE 相关验证可以单独跑 `ctest --test-dir build -R 'seriona\.scanner\.(playlist_cue|e2e)' --output-on-failure`，只看 CUE 收口和端到端路径。
3. 默认全量验证仍是 `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`、`cmake --build build`、`ctest --test-dir build --output-on-failure`。
4. `seriona_scanner_detailed_perf_test` 的入口是 `cmake --build build --target seriona_scanner_detailed_perf_test`，然后运行 `./tests/seriona_scanner_detailed_perf_test`。
5. 未添加默认启用的真实 watcher smoke；真实文件监听属于可选手工/后续 smoke，不应让默认构建依赖真实媒体目录或硬件。

## 迁移和兼容

1. 现有调用方不用改为新入口，继续使用 `FileScannerService` 即可。
2. scanner 的公共契约仍然保持纯净，没有把 TagReader/SQLite/watcher 类型提升到 `scanner_contracts.h` 或 `file_scanner_service.h`。
3. 如果上层只关心发现结果，可以把 CUE 视为目录型节点；如果只关心普通歌曲，也可以继续过滤掉虚拟文件夹节点。
4. 现在的变化是结果树的组织方式，不是命令行或播放流程的接口变化。
