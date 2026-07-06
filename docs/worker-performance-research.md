# Scanner Worker 性能调研与代码级复核

**日期**：2026-07-04  
**范围**：Seriona 文件扫描模块、相邻 `TagReader` 解析/封面路径、流水线并发外部资料  
**状态**：代码级复核完成，结论已根据最新 profile 修正

---

## 摘要

最新扫描数据表明，当前瓶颈不在 Discovery、最终聚合或全局阶段串行等待，而在 Worker 阶段内部：

```text
reconcileRoot total=59838ms
discovery=449ms
task-prep=7335ms
worker-wait=51774ms
final-hash=277ms

scan total wall=60180ms
processed=5024
cumulative worker callback time=1642892ms
avg worker callback=327.0ms/file
```

有效并行度为：

```text
1642892ms / 51774ms ~= 31.7x
```

这说明 Worker 已经接近 32 路并行，问题不是“总并行度不够”，而是每个 Worker 任务本身太重。重新阅读代码与用户提供的 `TagReaderTest` profile 后，当前最高收益方向应调整为：

1. **先修 TagReader 封面处理链路与缩略图集成契约**。
2. **再做有界编码并发，避免 Worker 内部 `std::async` 放大线程数**。
3. **再考虑 Worker callback 变薄与缓存读并发安全**。
4. **Worker 内部流水线只应在新增分段 profile 后再决定**。
5. **全局流水线不是当前最高效率方案**。

一个特别重要的代码级发现是：Seriona 目前通过 `ProductionTagMetadataReader::read()` 调用 `TagReader::Read(path, coverExportDir)`，而 TagReader 这个重载会使用默认 `CoverProcessingOptions`，其中 `generateThumbnail` 默认为 `true`。但 Seriona 的 `RawTagMetadata`、`SongMetadata` 和 adapter 当前没有传播 `thumbnailPath`。这意味着扫描器可能已经支付缩略图生成成本，却没有把缩略图路径暴露给上层。

---

## 代码复核范围

### Seriona 文件扫描模块

已重新阅读并复核以下关键路径：

- `src/scanner/file_scanner_orchestrator.cpp`
  - `effectiveScannerConfig()`
  - `runScan()`
  - `reconcileRoot()`
  - `WorkerSongStore`
  - `readWorkerSong()`
  - worker 结果写入与后置聚合
- `src/scanner/worker_pool.cpp`
  - `getOptimalWorkerCount()`
  - `getOptimalTagReaderLimit()`
  - `ScannerWorkerPool::Impl::submitBatch()`
  - `waitAll()` / `waitForCompletion()`
  - `processTask()`
  - `WorkerPoolStatsSnapshot::tagReaderTime`
- `src/scanner/tag_reader_metadata_adapter.cpp`
  - `rawFromMusicTag()`
  - `ProductionTagMetadataReader::read()`
  - `mapRawTagMetadata()`
- `inc/seriona/scanner/scanner_contracts.h`
  - `ScannerConfig`
  - `SongMetadata`
- `inc/seriona/scanner/tag_reader_metadata_adapter.h`
  - `RawTagMetadata`
  - `TagMetadataReader`
- `src/scanner/cache/sqlite_cache_v3.cpp`
  - `loadContent()`
  - `loadLocation()`
- `src/scanner/cache/sqlite_cache_v3_connection.cpp`
  - SQLite 连接配置、WAL/PRAGMA、writer transaction
- 相关测试：worker pool stats、TagReader 并发压力、scanner perf support

### TagReader 相关路径

已重新阅读并复核以下关键路径：

- `include/TagReader.hpp`
  - `CoverProcessingOptions`
  - `TagReader::Read()` 重载
- `include/Tag.hpp`
  - `MusicTag::coverPath()`
  - `MusicTag::thumbnailPath()`
- `src/core/TagPipeline.cpp`
  - `ReadTag()`
  - `ReadMetadata()`
  - `BuildMusicTag()`
- `src/core/ReadContext.hpp`
  - `coverExportDir`
  - `coverOptions`
- `src/core/RawTagData.hpp`
  - `RawMetadata::coverPath`
  - `RawMetadata::thumbnailPath`
- `src/cover/CoverCache.cpp`
  - `ExportCoverFromContext()`
  - `WriteCoverWithThumbnail()`
  - `WriteCoverAsPng()`
  - `AtomicWriteFileIfAbsent()`
- `src/cover/CoverDecoder.cpp`
  - `DecodeAndEncodeCoverPng()`
  - `DecodeImage()`
  - `GenerateThumbnail()`
  - `EncodePngWithOptions()`
  - `EncodeFrameAsPng()`
- 格式 parser 中的封面入口：MP4、FLAC、APE、ASF、Matroska、ID3

---

## 最新性能数据解读

### Scanner 侧

当前分段耗时：

| 阶段 | 耗时 | 占 `reconcileRoot` |
|---|---:|---:|
| Discovery | 449 ms | 0.75% |
| Task Prep | 7335 ms | 12.26% |
| Worker Wait | 51774 ms | 86.53% |
| Final Hash | 277 ms | 0.46% |

因此，全局流水线即使完全隐藏 `discovery + task-prep + final-hash`，理论上限也只有约 8.1 秒；实际还要扣除队列、背压、批次有序聚合和错误传播成本。相比之下，优化 Worker 内部每个任务的 CPU/IO 工作量可以直接作用于 51.8 秒主瓶颈。

### TagReader profile 样本

用户提供的三个 `TagReaderTest` 样本都指向同一结论：metadata 解析不是主耗时，PNG 编码才是主耗时。

| 文件 | 封面特征 | `EncodePngWithOptions` | `WriteCoverWithThumbnail` | 内部 `png_avcodec_send_frame` | `GenerateThumbnail` |
|---|---|---:|---:|---:|---:|
| `Ringing Bloom.m4a` | 常规封面 | 82.22 ms / 2 次 | 80.96 ms | 77.46 ms | 0.73 ms |
| `01. 17才.flac` | 常规封面 | 45.54 ms / 2 次 | 43.35 ms | 42.06 ms | 0.55 ms |
| `Albemuth - 箱庭.flac` | 2400x2400 内嵌封面 | 560.29 ms / 2 次 | 561.25 ms | 558.68 ms | 1.82 ms |

三个样本中，`ReadMetadata`、`ReadMediaInfo`、`BuildMusicTag`、`OpenContext` 等都在微秒到少量毫秒级；而 PNG 编码在常规封面下几十毫秒，在 2400x2400 封面下超过 500ms。

这与代码完全一致：`GenerateThumbnail()` 自身很快，真正昂贵的是 PNG 编码与当前多次 PNG 往返。

---

## Scanner 侧代码证据

### 1. 默认并发已经接近硬件并发

`effectiveScannerConfig()` 在未显式配置时使用：

```cpp
workerCount = getOptimalWorkerCount();
tagReaderSlots = getOptimalTagReaderLimit(workerCount);
```

`getOptimalWorkerCount()` 返回 `std::thread::hardware_concurrency()`，`getOptimalTagReaderLimit()` 默认等于 worker 数。`ScannerWorkerPool` 构造时又通过 `boundedTagReaderSlots()` 把 `tagReaderSlots` 限制到 worker 数以内。

因此，最新数据中的 `31.7x` 有效并行度与代码一致：当前已经是一个高并发 task farm，而不是并行度不足的串行扫描。

### 2. `tagReaderTime` 实际统计整个 Worker callback

`ScannerWorkerPool::Impl::processTask()` 中：

```cpp
const auto tagReaderStart = std::chrono::steady_clock::now();
auto metadata = config_.tagReader(task);
const auto tagReaderElapsed = std::chrono::steady_clock::now() - tagReaderStart;
tagReaderTimeNs_.fetch_add(...);
```

因此日志里的：

```text
Cumulative Worker CPU Time
- TagReader Parse: 1642892 ms
```

并不只是 TagReaderCore 的纯 parse，而是整个 `config_.tagReader(task)` 回调耗时。这个回调在生产路径里包含 `readWorkerSong()`、cache-hit 读取、TagReader 调用、映射、`WorkerSongStore` 写入等。

文档和日志命名应避免把它理解为“纯 TagReader parse”。更准确的名字是 `worker callback cumulative time` 或 `metadata callback time`。

### 3. 当前扫描仍是批处理屏障，不是全局流水线

`reconcileRoot()` 的结构是：

1. `discoverScannerPaths()` 完成 Discovery。
2. 遍历 `entries`，构造 `audioTasks` 与 `workerTasks`。
3. 创建 `ScannerWorkerPool`。
4. `submitBatch(std::move(workerTasks))`。
5. `waitAll()` 等待所有 worker future 完成。
6. 后置遍历 `workerResults` 和 `indexedSongs` 聚合结果。

这说明 `discovery`、`task-prep`、`worker-wait` 之间目前存在阶段屏障。全局流水线可以隐藏一部分 `task-prep`，但不能减少 Worker 任务本身的主耗时。

### 4. Worker callback 目前有额外共享状态和二次取回

`WorkerSongStore` 是一个 `std::map` 加互斥锁：

```cpp
void put(path, CachedSong)
std::optional<CachedSong> take(path)
```

`readWorkerSong()` 先 `workerSongs->put()`，worker callback 内部又尝试 `take()` 写入 `indexedSongs`；`waitAll()` 后还会再根据 `workerResults` 二次 `take()`。这条路径功能上能工作，但对于性能而言有几个问题：

- 每个任务至少一次 map 插入和一次互斥锁操作。
- callback 内部直接写共享 `indexedSongs`。
- 后置聚合还保留二次取回逻辑。
- `WorkerResult` 只携带 `SongMetadata`，不携带完整 `CachedSong`，导致需要旁路 `WorkerSongStore`。

这不是最大瓶颈，但属于 Worker callback 变薄的明确候选项。

### 5. Cache 读路径需要单独审视并发安全

`SQLiteCacheV3` 当前只有 `writerMutex_` 保护 writer transaction；`loadContent()` 和 `loadLocation()` 复用成员 `sqlite3_stmt*`，但没有读互斥锁。

在 `reconcileRoot()` 中，`v3cache` 被捕获进 worker callback；cache-hit 分支会在多个 worker 中调用 `v3cache.loadLocation()` / `loadContent()`。SQLite 连接和 statement 的线程安全需要按 SQLite threading mode 和同一 statement 并发使用规则单独验证。

这不是当前冷扫描 `worker-wait` 的主要解释，因为最新数据是 `5024 scanned, 0 skipped`，但它是 hot/incremental 扫描中必须关注的正确性与性能风险。

### 6. Seriona 尚未传播 `thumbnailPath`

`TagReader::MusicTag` 已有 `thumbnailPath()`，TagReader 内部 `RawMetadata` 也有 `thumbnailPath`。但 Seriona 的 `RawTagMetadata` 只有：

```cpp
std::filesystem::path coverPath;
```

没有 `thumbnailPath`。`rawFromMusicTag()` 只映射：

```cpp
raw.coverPath = tag.coverPath();
```

`SongMetadata` 也只有：

```cpp
std::optional<std::filesystem::path> artworkPath;
```

没有缩略图路径字段。

这意味着：**TagReader 默认生成缩略图，但 Seriona 当前没有把缩略图路径带出扫描契约。** 这是阶段 4 集成缺口，也是性能判断中的关键事实。

---

## TagReader 侧代码证据

### 1. `TagReader::Read(path, coverExportDir)` 默认生成缩略图

`CoverProcessingOptions` 默认值：

```cpp
bool generateThumbnail{true};
thumbnailSize = 256x256;
scalingQuality = Fast;
pngCompression = Fast;
```

`TagReader::Read(filePath, coverExportDir)` 调用 `tagreader_core::ReadTag(filePath, coverExportDir)`，后者使用 `static const CoverProcessingOptions defaultOptions{}`。

Seriona 生产 adapter 当前调用：

```cpp
return rawFromMusicTag(TagReader::Read(path, coverExportDir));
```

所以 Seriona 当前路径会触发默认缩略图生成。

### 2. 各格式封面入口统一进入 `ExportCoverFromContext()`

以下格式路径都在解析内嵌封面后调用 `ExportCoverFromContext()`：

- MP4：`covr` atom
- FLAC：`ReadFlacPictureEntry()`
- APE：`ProcessApeCoverItem()`
- ASF：`ProcessPictureDescriptor()`
- Matroska：`ExportAttachedImage()`
- ID3：`ReadID3v2PictureFrame()` / `ReadID3v2ApicPayload()` / ID3v2.2 picture frame

因此，优化 `ExportCoverFromContext()`、`WriteCoverWithThumbnail()` 和 `CoverDecoder` 可以覆盖主流封面路径。

### 3. 当前 TagReader 如何处理封面

当前 TagReader 的封面处理是“格式 parser 发现内嵌图片 -> 统一交给 cover cache -> 解码/缩放/编码 -> 原子写入缓存文件 -> 把路径写回 metadata”的同步流程。完整链路如下。

#### 3.1 调用入口：`TagReader::Read()` 建立读取上下文

Seriona 生产路径调用：

```cpp
TagReader::Read(path, coverExportDir)
```

这个重载进入 `tagreader_core::ReadTag(filePath, coverExportDir)`，并使用默认 `CoverProcessingOptions`。默认配置会生成缩略图：

```cpp
generateThumbnail = true
thumbnailSize = 256x256
scalingQuality = Fast
pngCompression = Fast
```

`ReadTag()` 随后完成这些准备工作：

1. `ValidatePath()` 校验输入路径是普通文件。
2. `OpenContext()` 打开 FFmpeg format context，同时准备 `ReadContext::input`。
3. 选择 `coverExportDir`：调用方传入则使用调用方目录，否则使用默认私有运行时目录。
4. 把 `CoverProcessingOptions` 指针写入 `ReadContext::coverOptions`。
5. `DetectStream()`、`DetectTagFormat()`、`ReadMediaInfo()` 获取音频流、格式和媒体信息。
6. `ReadMetadata(context, tagFormat)` 根据格式分派到具体 parser。

#### 3.2 格式 parser 从音频文件中取出内嵌封面 bytes

封面不是在 `ReadTag()` 主流程中直接读取，而是在各格式的 metadata parser 中被发现。各 parser 会把音频文件里的内嵌封面 payload 定位出来，并把原始图片字节传给统一导出函数。

主要格式入口如下：

| 格式 | 解析位置 | 内嵌封面来源 | 交给导出层的数据 |
|---|---|---|---|
| MP4/M4A | `ReadMp4DataAtom()` | `covr` atom 的 `data` payload | `payload, payloadSize` |
| FLAC | `ReadFlacPictureEntry()` | `METADATA_BLOCK_PICTURE` 中 picture type 3 | `imageBytes->data(), imageBytes->size()` |
| ID3v2/MP3 | `ReadID3v2PictureFrame()` / `ReadID3v2ApicPayload()` | `APIC` / `PIC` frame | APIC 描述字段后的图片 bytes |
| APE | `ProcessApeCoverItem()` | cover item value，跳过可选描述前缀 | `imageData, imageSize` |
| ASF/WMA | `ProcessPictureDescriptor()` | `WM/Picture` / `Picture` descriptor | descriptor 中 imageOffset 后的 bytes |
| Matroska | `ExportAttachedImage()` | attachment 且 media type 为 `image/*` | 读取 attachment bytes 后传入 |

这些入口都会调用：

```cpp
ExportCoverFromContext(context, data, size)
```

如果已经有 `metadata.coverPath`，多数 parser 会直接跳过后续封面，避免同一文件导出多个封面。

#### 3.3 统一导出分支：是否生成缩略图

`ExportCoverFromContext()` 根据 `context.coverOptions` 选择导出路径：

```cpp
if (context.coverOptions != nullptr && context.coverOptions->generateThumbnail) {
    return WriteCoverWithThumbnail(context.coverExportDir, data, size, *context.coverOptions);
}
return {WriteCoverAsPng(context.coverExportDir, data, size), {}};
```

当前 Seriona 调用的是默认 options，因此会走 `WriteCoverWithThumbnail()`。

如果显式关闭 `generateThumbnail`，才会走旧的 `WriteCoverAsPng()`，只导出原图 PNG，不生成 thumbnail。

#### 3.4 路径与去重：按原始封面 bytes 做 SHA-256 content-addressed 缓存

`WriteCoverWithThumbnail()` 首先对原始内嵌图片 bytes 计算 SHA-256：

```cpp
contentHash = HashEmbeddedImageBytes(data, size)
```

然后构造两个缓存路径：

```text
full:  <coverExportDir>/<hash[0:2]>/<hash[2:]>.png
thumb: <coverExportDir>/thumbnails/<hash[0:2]>/<hash[2:]>.png
```

例如：

```text
/run/user/1000/tagreader-covers/ae/6971...a4a8.png
/run/user/1000/tagreader-covers/thumbnails/ae/6971...a4a8.png
```

如果 full 已存在，且需要 thumbnail 时 thumbnail 也已存在，函数直接返回路径，不再解码或写入。这是当前封面去重的主要机制：同一封面 bytes 只导出一次。

#### 3.5 解码阶段：当前会先转成中间 PNG，再解回 frame

缓存未命中时，`WriteCoverWithThumbnail()` 调用：

```cpp
DecodedImage decoded = DecodeImage(data, size);
```

当前 `DecodeImage()` 的内部流程是：

1. `DecodeAndEncodeCoverPng(data, size)`：把原始图片 bytes 先转换成 PNG bytes。
2. 用 PNG decoder 再把这份中间 PNG bytes 解码成 `AVFrame`。
3. 返回 `DecodedImage{frame, width, height}`。

`DecodeAndEncodeCoverPng()` 会先识别原始图片格式：

```text
PNG / JPEG / BMP / WebP / GIF / TIFF / Unknown fallback
```

然后调用 `ConvertImageToPng()`：

1. `avcodec_find_decoder(codecId)` 找对应图片 decoder。
2. `avcodec_alloc_context3()` 创建 decoder context。
3. `avcodec_open2()` 打开 decoder。
4. `CopyImageBytesToPacket()` 把原始封面 bytes 拷贝进 `AVPacket`。
5. `DecodePacketToFrame()` 解码出原始图片 frame。
6. `DecodedFrameWithinCoverLimits()` 检查尺寸和像素数量限制。
7. `ConvertFrameToRgb24()` 用 swscale 转成 RGB24。
8. `EncodeFrameAsPng()` 把 RGB24 frame 编码成中间 PNG bytes。

随后 `DecodeImage()` 又会：

1. 找 PNG decoder。
2. 为中间 PNG bytes 创建新的 packet。
3. `avcodec_send_packet()` / `avcodec_receive_frame()` 解码成最终用于缩放和输出的 `AVFrame`。

因此，当前解码阶段实际上包含一次“原始图片 -> 中间 PNG”的编码，以及一次“中间 PNG -> frame”的再解码。

#### 3.6 缩略图阶段：从 full-size RGB frame 缩放到目标尺寸

如果 `generateThumbnail=true`，`WriteCoverWithThumbnail()` 会调用：

```cpp
GenerateThumbnail(decoded, thumbOpts)
```

当前缩略图处理逻辑：

1. 根据 `thumbnailSize.width/height` 和 `maintainAspectRatio` 计算目标宽高。
2. 如果原图已经小于目标尺寸，则 `av_frame_clone()`，不放大。
3. 否则分配 RGB24 `AVFrame`。
4. 根据 `scalingQuality` 选择 swscale flag：
   - `Fast` -> `SWS_FAST_BILINEAR`
   - `Good` -> `SWS_BILINEAR`
   - `Best` -> `SWS_LANCZOS`
5. `sws_getContext()` 创建缩放上下文。
6. `sws_scale()` 输出缩略图 frame。

用户 profile 中 `GenerateThumbnail` 只有 0.5-1.8ms，说明缩放本身不是当前主瓶颈。

#### 3.7 写入阶段：full 与 thumbnail 分别 PNG 编码，再原子写文件

完成 full-size frame 和 thumbnail frame 后，`WriteCoverWithThumbnail()` 获取基于 `contentHash` 的 4096 分片互斥锁，并做锁内二次存在性检查。

之后它启动两个异步编码/写入任务：

```cpp
fullFuture  -> EncodePngWithOptions(decoded, compression=6) -> AtomicWriteFileIfAbsent(fullPath)
thumbFuture -> EncodePngWithOptions(thumbnail, compression=options.pngCompression) -> AtomicWriteFileIfAbsent(thumbPath)
```

注意这里的两个输出都仍是 PNG：

- full-size 输出固定 `.png`。
- thumbnail 输出也固定 `.png`。
- full 输出压缩级别当前写死为 `6`。
- thumbnail 输出使用 `CoverProcessingOptions::pngCompression`，默认 `Fast=1`。

`EncodePngWithOptions()` 的步骤是：

1. 找 PNG encoder。
2. 分配 encoder context。
3. 设置宽高、`AV_PIX_FMT_RGB24`、`compression_level`。
4. `avcodec_open2()`。
5. `avcodec_send_frame()`。
6. `avcodec_receive_packet()`。
7. 把 packet 拷贝成 `std::vector<uint8_t>`。

用户 profile 中 `EncodePngWithOptions` 出现 2 次，正对应 full 和 thumbnail 两次最终输出编码。

#### 3.8 文件发布阶段：临时文件、硬链接发布、fsync

`AtomicWriteFileIfAbsent()` 负责把编码后的 PNG bytes 写入缓存路径：

1. 创建父目录。
2. 如果目标文件已存在，直接返回成功。
3. 生成同目录临时文件名。
4. `open(O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC)` 创建临时文件。
5. `WriteAll()` 写入完整 PNG bytes。
6. `fsync(file)` 刷新文件内容。
7. `close()` 文件。
8. `link(temp, final)` 用硬链接发布到最终路径。
9. `fsync(directory)` 刷新目录项。
10. 删除临时文件。

这个流程保证并发进程/线程下的原子发布和较强持久化语义，但冷扫描大量新封面时也会带来同步 I/O 成本。

#### 3.9 路径回填：从 `CoverPaths` 回到 `MusicTag`

`WriteCoverWithThumbnail()` 返回：

```cpp
CoverPaths{fullSizePath, thumbnailPath}
```

格式 parser 收到后写入 `RawMetadata`：

```cpp
metadata.coverPath = paths.fullSizePath;
metadata.thumbnailPath = paths.thumbnailPath;
```

`ReadTag()` 最后调用 `BuildMusicTag()`：

```cpp
tag.setCoverPath(metadata.coverPath);
tag.setThumbnailPath(metadata.thumbnailPath);
```

所以在 TagReader 库内部，`MusicTag` 已经同时携带原图路径和缩略图路径。当前问题出在 Seriona adapter 没有把 `MusicTag::thumbnailPath()` 映射到 Seriona 的扫描契约。

#### 3.10 当前封面处理流程总览

用一条链路概括当前默认路径：

```text
音频文件
  -> TagReader::Read(path, coverExportDir)
  -> ReadTag()
  -> ReadMetadata()
  -> 格式 parser 定位内嵌封面 bytes
  -> ExportCoverFromContext()
  -> WriteCoverWithThumbnail()
  -> SHA-256(content bytes) 生成 full/thumb 缓存路径
  -> 若 full+thumb 已存在：直接返回路径
  -> DecodeImage()
       -> DecodeAndEncodeCoverPng()
            -> 原始图片解码
            -> RGB24 转换
            -> EncodeFrameAsPng() 生成中间 PNG
       -> PNG decoder 再解码中间 PNG 为 AVFrame
  -> GenerateThumbnail()
  -> std::async 编码 full PNG
  -> std::async 编码 thumbnail PNG
  -> AtomicWriteFileIfAbsent(fullPath)
  -> AtomicWriteFileIfAbsent(thumbPath)
  -> RawMetadata.coverPath / thumbnailPath
  -> MusicTag.coverPath / thumbnailPath
  -> Seriona 当前只读取 coverPath，丢失 thumbnailPath
```

这条链路解释了为什么当前 profile 中：

- `GenerateThumbnail` 很小；
- `EncodePngWithOptions` 很大且调用 2 次；
- `png_avcodec_send_frame` 很大，且对应中间 PNG 编码；
- 大尺寸封面会使 PNG 编码成本呈数量级上升；
- Seriona Worker wall time 会被封面编码主导，而不是 metadata 文本解析主导。

### 4. 当前封面链路的性能含义

上面的完整链路说明，当前默认路径每个新封面可能包含 **3 次 PNG 编码相关成本**：

1. `EncodeFrameAsPng()`：原始图片解码后先编码为中间 PNG。
2. `EncodePngWithOptions(decoded)`：full-size 输出 PNG。
3. `EncodePngWithOptions(thumbnail)`：thumbnail 输出 PNG。

这与用户 profile 完全吻合：

- `png_avcodec_send_frame` 对应中间 PNG 编码，在大封面样本中达到 558.68ms。
- `EncodePngWithOptions` 出现 2 次，分别对应 full PNG 和 thumbnail PNG。
- `GenerateThumbnail` 只有 0.5-1.8ms，说明缩放不是主瓶颈。

所以当前封面性能问题不是“缩略图缩放很慢”，而是“为得到 full/thumbnail PNG，路径上存在中间 PNG 往返和两次最终 PNG 输出编码”。

### 5. `std::async` 会在 Worker 内部再放大并发

`WriteCoverWithThumbnail()` 中：

```cpp
std::future<bool> fullFuture = std::async(std::launch::async, ... EncodePngWithOptions(decoded) ...);
std::future<bool> thumbFuture = std::async(std::launch::async, ... EncodePngWithOptions(thumbnail) ...);
```

Scanner Worker 外层已经有约 32 个 worker；如果每个 worker 的新封面都进入这里，内部又可能启动两个 `std::async` 编码任务。这样会形成：

```text
Scanner worker pool 并发
  x 每个任务内部 1-2 个 async 编码任务
```

这会造成 CPU oversubscription 和调度开销。用户 profile 中 `WriteCoverWithThumbnail` 的 wall time 接近两个 `EncodePngWithOptions` 中较慢者，是因为 full/thumb 编码被并行化了；但累计 CPU 工作量仍然存在，在全库扫描时会放大总 CPU 压力。

### 6. 原子写入路径还有同步刷盘成本

`AtomicWriteFileIfAbsent()` 对每个输出文件执行：

```cpp
WriteAll(...)
fsync(file)
link(temp, final)
fsync(directory)
```

这保证缓存文件可靠发布，但在冷扫描大量新封面时会增加 I/O 同步压力。用户提供的单文件 profile 中主导成本仍是 PNG 编码；因此该项应列为次级测量项，而不是当前首要结论。

---

## 外部资料对照

### oneTBB / Taskflow：流水线要有界，吞吐由慢阶段决定

oneTBB `parallel_pipeline` 和 Taskflow `Pipeline` 都使用 token/line 限制在途任务数。核心思想不是无界增加阶段，而是让上游生成速率受下游吞吐约束。

参考资料：

- oneTBB pipeline 文档：<https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Working_on_the_Assembly_Line_pipeline.html>
- oneTBB token-based system：<https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/create_token_based_system.html>
- oneTBB concurrency limits：<https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/use_concurrency_limits.html>
- Taskflow task-parallel pipeline：<https://taskflow.github.io/taskflow/TaskParallelPipeline.html>

对 Seriona 当前数据的含义：

- 全局流水线适合隐藏阶段间等待，但不能消除最慢阶段本身。
- 当前最慢阶段是 `worker-wait=51774ms`。
- Worker 已有 `31.7x` 有效并行度，因此先继续扩大并行不是最佳方向。
- 如果做流水线，必须用有界队列/有限 token，不能用无界队列积压封面帧或图片字节。

### Navidrome artwork 案例：封面输出格式和并发上限比“多套流水线”更关键

Navidrome 在 2026 年的 artwork 优化中加入了 decode、resize、encode、E2E 和并发 cache benchmark，并围绕 JPEG/WebP/PNG 输出格式做了性能调整。相关 PR/issue 显示：

- resized cover art 使用 JPEG/WebP 相比 PNG 可以显著降低编码和输出体积。
- WebP 在低功耗硬件或 WASM fallback 下可能造成严重回退。
- 增大 UI 封面尺寸、增加并行图像处理、缓存失效叠加会导致 CPU/内存尖峰。
- 后续修正包括降低默认尺寸、降低 artwork 并发、让 WebP 可配置。

参考资料：

- Navidrome PR #5181：<https://github.com/navidrome/navidrome/pull/5181>
- Navidrome issue #5280：<https://github.com/navidrome/navidrome/issues/5280>
- Navidrome PR #5286：<https://github.com/navidrome/navidrome/pull/5286>

对 Seriona/TagReader 的含义：

- 缩略图不一定应该强制 PNG。
- 输出尺寸、输出格式、编码质量、并发上限都必须作为性能参数。
- WebP 是否启用应基于本机 native libwebp 与目标平台测量，不能只按理论选择。

---

## 方案优先级判断

### P0：修正 Seriona 缩略图契约或临时关闭未使用缩略图

**状态**：最高优先级，代码证据强。

当前 Seriona 通过 `TagReader::Read(path, coverExportDir)` 默认生成缩略图，但没有传播 `thumbnailPath`。因此有两条互斥路径：

1. 如果前端/上层马上需要缩略图：扩展 `RawTagMetadata`、`SongMetadata`、cache schema/映射，完整传播 `thumbnailPath`。
2. 如果当前阶段还不消费缩略图：Seriona 调用 TagReader 时显式传 `CoverProcessingOptions{.generateThumbnail=false}`，避免支付缩略图成本。

这不是单纯性能优化，而是“成本与产品契约不一致”的问题。

### P1：重构 TagReader 封面解码链路，去掉 PNG 中间往返

**状态**：最高收益技术优化，代码证据强，profile 直接支持。

目标不是增加流水线，而是减少每个新封面的实际工作量。当前 `DecodeImage()` 会先生成中间 PNG，再解码 PNG 成 frame；这与 profile 中单文件 42-558ms 的内部 `png_avcodec_send_frame` 对应。

重构目标应是：

```text
原始封面 bytes -> 直接解码为 RGB frame -> 生成 thumbnail -> 输出所需文件
```

避免：

```text
原始封面 -> 中间 PNG -> 再解码 PNG
```

这会直接降低 Worker 主体耗时。

### P1：移除每文件 `std::async`，改为有界编码并发

**状态**：高优先级，代码证据强。

外层 Scanner Worker 已经高并发；内层 `std::async(std::launch::async)` 对 full/thumb 编码没有全局上限，容易造成 CPU oversubscription。应改为明确的、有界的编码并发模型。

候选形态：

- TagReader 内部共享小型 encode pool。
- Seriona/TagReader 之间显式传入并发策略。
- 单任务内顺序编码，但全局由 worker pool 控制并发。

无论具体选择哪种，原则是：**编码并发总量要可控**。

### P2：缩略图输出格式/压缩策略可配置

**状态**：高潜力，需要产品契约决定。

用户 profile 表明 `EncodePngWithOptions` 是主耗时。外部 Navidrome 经验也显示 resized artwork 不必默认 PNG。可考虑：

- 原图仍保持 PNG 或 content-addressed 原格式缓存。
- 缩略图使用 JPEG 或 native WebP。
- PNG 仅用于透明图或明确要求无损的场景。
- 尺寸、质量、格式进入 `CoverProcessingOptions`。

这个方向可能比 Worker 流水线收益更大，但需要先明确 UI/缓存/兼容契约。

### P2：Worker callback 变薄，减少 `WorkerSongStore` 旁路

**状态**：中等收益，代码证据明确。

当前 `WorkerResult` 只返回 `SongMetadata`，完整 `CachedSong` 通过 `WorkerSongStore` 旁路传递。可以考虑让 `WorkerResult` 直接携带完整 song 或建立更清晰的结果类型，减少：

- map 插入/删除
- 互斥锁竞争
- callback 内部写 `indexedSongs`
- `waitAll()` 后二次取回

该方案不会解决 PNG 编码主瓶颈，但能让 Worker 路径更薄、更容易测量和维护。

### P3：Worker 内部两段式流水线，仅在新增分段 profile 后实施

**状态**：有条件推荐。

之前讨论的 Worker 内部流水线是：

```text
metadata read / tag parse -> cover processing / encode
```

代码复核后，该方案不应立即实施，原因是：

- 当前 TagReader API 没有 `ReadMetadataOnly()` / `ProcessCover()`。
- 各格式 parser 当前在读取 metadata 时同步调用 `ExportCoverFromContext()`。
- 分离封面意味着要在 TagReader 内部保留原始封面 bytes 或延迟处理，这会增加内存与生命周期复杂度。
- 当前 profile 已经证明 metadata 读取很轻，封面编码很重；直接减少封面编码工作量比拆流水线更直接。

只有在新增分段 profile 后确认存在可重叠的异质阶段，才值得做 Worker 内部流水线。

### P4：全局流水线不作为当前主方案

**状态**：低优先级。

全局流水线最多隐藏约 8.1 秒的非 Worker 阶段，而当前主瓶颈是 51.8 秒 Worker。它还会引入：

- 多队列和背压控制
- 批次错误传播
- 取消语义
- 有序聚合
- 进度统计复杂化

在 TagReader 封面链路未优化前，全局流水线不是最高效率方案。

---

## 被证实、被修正和仍需测量的判断

| 判断 | 状态 | 依据 |
|---|---|---|
| Worker 是主瓶颈 | 已证实 | `worker-wait=51774ms`，占 86.5% |
| 并行度不足是主因 | 被否定 | 有效并行度约 31.7x |
| `TagReader Parse` 是纯 TagReader parse | 被修正 | 代码显示统计整个 `config_.tagReader(task)` |
| PNG 编码是主要单文件热点 | 已证实 | 3 个 profile 样本均显示 `EncodePngWithOptions`/`png_avcodec_send_frame` 主导 |
| 缩略图生成本身很慢 | 被否定 | `GenerateThumbnail` 只有 0.5-1.8ms |
| 当前存在 PNG 中间往返 | 已证实 | `DecodeImage()` 调用 `DecodeAndEncodeCoverPng()`，再解码 PNG |
| 当前存在嵌套并发 | 已证实 | Scanner worker pool 外层 + `WriteCoverWithThumbnail()` 内部 `std::async` |
| Seriona 已完整集成缩略图路径 | 被否定 | Seriona `RawTagMetadata`/`SongMetadata` 未传播 `thumbnailPath` |
| 全局流水线最高效 | 被否定 | 上限受 8.1s 非 Worker 阶段约束，不能减少 Worker 主体 |
| Worker 内部流水线一定收益最高 | 需要测量 | 需要 TagReader 分段 API 与 per-stage profile |
| Cache 读路径共享 statement 并发安全 | 需要验证 | 读路径复用成员 `sqlite3_stmt*`，未见读锁 |

---

## 建议的下一步测量

在动代码前，建议补三组非常小的测量，避免再次基于假设设计：

1. **TagReader 封面分段 profile**
   - 原图 decode
   - RGB 转换
   - 中间 PNG encode
   - PNG re-decode
   - thumbnail scale
   - full encode
   - thumbnail encode
   - file write/fsync

2. **Scanner Worker callback 分段 profile**
   - cache-hit loadLocation/loadContent/loadLyrics
   - `metadataReader_->read()`
   - `mapRawTagMetadata()`
   - `WorkerSongStore::put/take`
   - `indexedSongs` 写入

3. **并发上限 sweep**
   - `SERIONA_SCANNER_WORKERS=8/16/24/32`
   - `SERIONA_SCANNER_TAGREADER_CONCURRENCY=8/16/24/32`
   - 对比内层 `std::async` 开启/关闭或有界化后的结果

验收指标应包括：

- 总 wall time
- Worker callback cumulative time
- 每文件平均耗时
- CPU 利用率
- 上下文切换数量
- 峰值线程数
- 峰值 RSS
- 生成封面数量与 cache hit 数

---

## 最终结论

当前“优化 worker 的性能，使得其执行时间进一步缩短”的最高效率路径，不是先做全局流水线，也不是先拆 Worker 为 metadata/cover 两段流水线，而是：

1. **立即修正 Seriona 缩略图契约**：要么传播 `thumbnailPath`，要么在未消费前关闭缩略图生成。
2. **重构 TagReader 封面解码链路**：去掉 `DecodeImage()` 中的 PNG 中间往返。
3. **有界化封面编码并发**：移除每文件 `std::async` 造成的无界嵌套并发。
4. **评估缩略图输出格式**：PNG 不是缩略图的必然最佳格式。
5. **再清理 Worker callback 结构**：让结果类型携带完整 song，减少旁路 store 和共享写入。
6. **最后再考虑流水线**：只有在分段 profile 证明存在可重叠的异质阶段时，才实施 Worker 内部流水线；全局流水线保留为低优先级方案。

这组结论已经被当前代码结构、用户提供的 scanner 最新数据、三个 TagReader profile 样本以及外部流水线/封面处理资料共同支持。
