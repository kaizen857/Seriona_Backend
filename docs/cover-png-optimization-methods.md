# 封面 PNG 编解码优化方法排序

**日期**：2026-07-04  
**范围**：TagReader 内嵌封面导出、原图 PNG、缩略图 PNG、冷扫描性能  
**定位**：优化方法说明，不是详细实施计划

---

## 硬约束

1. 冷扫描必须同时提取 **full-size 原图封面** 和 **thumbnail 缩略图**。
2. 不接受懒加载、后台补图、非冷扫描阶段再生成封面或缩略图。
3. Seriona 后端已有高并发 worker，TagReader 内部再无界派发任务会放大线程数，默认不把“两段式流水线”作为首选。
4. 优先减少单个封面任务的实际 CPU/IO 工作量，再考虑新的并发结构。

当前性能 profile 已说明：`GenerateThumbnail` 不是热点，PNG 编码才是热点。大封面样本中 `EncodePngWithOptions` 达到 560.29ms / 2 次，`png_avcodec_send_frame` 达到 558.68ms。

---

## 当前路径的关键问题

当前默认路径是：

```text
内嵌图片 bytes
  -> DecodeAndEncodeCoverPng()
       -> 原始图片解码
       -> RGB24 转换
       -> EncodeFrameAsPng() 生成中间 PNG
  -> PNG decoder 再解码中间 PNG 为 AVFrame
  -> GenerateThumbnail()
  -> EncodePngWithOptions(full)
  -> EncodePngWithOptions(thumbnail)
  -> AtomicWriteFileIfAbsent(full)
  -> AtomicWriteFileIfAbsent(thumbnail)
```

这意味着每个新封面可能有三段 PNG 编码相关成本：

1. 中间 PNG 编码。
2. full-size PNG 输出编码。
3. thumbnail PNG 输出编码。

其中第一段是结构性冗余：它只是为了得到可缩放的 frame，却先把原图转成 PNG，再把 PNG 解回 frame。

---

## 优化方法排序

### 1. 去掉中间 PNG 往返，直接解码原始图片到 RGB frame

**优先级**：最高  
**预期收益**：最高  
**风险**：中低

当前 `DecodeImage()` 先调用 `DecodeAndEncodeCoverPng()` 生成中间 PNG，再用 PNG decoder 解回 `AVFrame`。这应该改为：

```text
原始内嵌图片 bytes -> 对应 FFmpeg image decoder -> RGB24 frame
```

然后同一个 RGB frame 同时用于：

- full-size PNG 编码；
- thumbnail 缩放；
- thumbnail PNG 编码。

这样在仍然冷扫描产出原图和缩略图的前提下，删除一整次中间 PNG 编码和一次中间 PNG 解码。

这个方法不改变输出契约，不引入新依赖，也不要求全局流水线，是最应该先做的优化。

### 2. 调整 FFmpeg PNG encoder 参数：`compression_level` 与 `pred`

**优先级**：高  
**预期收益**：高，需本地测量  
**风险**：低

当前 thumbnail 使用 `CoverProcessingOptions::pngCompression`，默认 `Fast=1`；但 full-size 输出仍固定为 `compressionLevel = 6`。对于冷扫描来说，full-size PNG 编码耗时明显大于缩略图缩放，应优先把 full-size 编码参数也纳入配置。

FFmpeg PNG encoder 支持：

- `compression_level`：0-9。
- `pred`：`none`、`sub`、`up`、`avg`、`paeth`、`mixed`，默认 Paeth。

外部资料显示，PNG 过滤策略会显著影响编码 CPU。PNG 规范建议真彩色图像固定过滤时 Paeth 通常较好，adaptive/mixed 往往压缩更好但需要更多计算。Chromium 的 PNG 编码路径曾为速度优化采用 level 3 + sub filter，理由是降低 zlib lazy match 和 filter 计算量。

建议本地基准顺序：

```text
full-size: level 1 / 3 / 6 x pred none / sub / paeth / mixed
thumbnail: level 1 / 3 x pred none / sub / paeth
```

选择标准应是冷扫描 wall time 优先，其次才是文件大小。对于播放器缓存，full-size 和 thumbnail 都是可再生缓存，适合偏向速度。

### 3. 移除每封面 `std::async`，改成有界编码并发或顺序编码

**优先级**：高  
**预期收益**：中到高，取决于 CPU oversubscription 程度  
**风险**：中低

Seriona worker 已经接近 32 路有效并行。`WriteCoverWithThumbnail()` 内部再对 full 和 thumbnail 使用 `std::async(std::launch::async)`，会造成嵌套并发：

```text
Seriona worker pool 并发 x 每个封面内部 1-2 个 async 编码任务
```

这可能降低整体吞吐，尤其是在 PNG 编码本身已是 CPU 热点时。

建议排序：

1. 先做一个“单任务内顺序编码”的基准，对比当前 `std::async`。
2. 如果顺序编码 wall time 更差但系统线程数明显下降，再做有界 encode pool。
3. encode pool 的并发上限应独立于 scanner worker 数，优先从 4/8/16 做 sweep。

该方法不减少编码总 CPU 工作量，但能避免无界嵌套并发造成的调度开销和缓存争用。

### 4. 用 zlib-ng 替换 PNG deflate 后端

**优先级**：中高  
**预期收益**：中到高  
**风险**：中

PNG 编码的核心压缩是 DEFLATE。zlib-ng 是 zlib 的优化替代实现，支持 zlib-compatible API，包含多种 SIMD/CPU 特化路径。OpenCV 相关讨论中，`zlib-ng + libpng` 在 PNG 编码测试里相对 `zlib + libpng` 有显著改善，示例数据为约 1600ms -> 510ms。

可选路径：

1. 如果继续使用 FFmpeg PNG encoder：验证当前系统 FFmpeg 链接的 zlib 是否可替换为 zlib-ng。这可能需要自建 FFmpeg 或控制动态链接环境，工程成本较高。
2. 如果改用 libpng：使用 `libpng + zlib-ng` 作为 full/thumbnail PNG encoder。
3. 如果只做实验：用同一批 RGB frame 对比 FFmpeg PNG、libpng+zlib、libpng+zlib-ng。

此方法不应先于“删除中间 PNG 往返”，因为后者直接删掉了一整段不必要工作；zlib-ng 是在保留 PNG 输出前提下优化剩余编码成本。

### 5. 评估 libpng 作为可控 PNG encoder

**优先级**：中  
**预期收益**：中，主要来自参数可控性  
**风险**：中

FFmpeg PNG encoder 已能设置 `compression_level` 和 `pred`，但当前代码只设置了 `compression_level`，没有使用 `pred`。如果 FFmpeg 侧参数足够，优先继续用 FFmpeg。

如果需要更可控的 filter/compression/memory 行为，可以评估 `libpng`：

- `png_set_compression_level()` 可设置 0-9。
- LSB 文档说明 level 3-6 通常接近 level 9 的 PNG 压缩效果，但计算更少。
- `png_set_filter()` 可限制过滤器，减少每行尝试所有 filter 的成本。

`libpng + zlib-ng` 是比 `libspng encode` 更稳妥的编码替代候选，因为 libspng 已有公开 issue 表明大图 encode 可能慢于 libpng。

### 6. 评估 libspng 作为 PNG decode 候选，不优先用于 encode

**优先级**：中低  
**预期收益**：主要在 PNG 输入解码  
**风险**：中

libspng 的目标是更简单、更快的 PNG 库，README 声称在常见 use case 中优于 libpng，并支持 OSS-Fuzz、较完整测试。它的文档提供了 buffer decode / encode API，可以直接把 PNG 解码到指定输出格式，也可以从内存 buffer 编码 PNG。它适合评估为“内嵌 PNG 封面 -> RGB/RGBA frame”的 decode 路径。

但对本项目当前热点而言：

- 主耗时是 PNG encode，不是 PNG decode。
- 删除中间 PNG 往返后，PNG decode 只影响“原始内嵌封面本身就是 PNG”的文件。
- libspng issue #224 中有大图 encode 慢于 libpng 的反例，且维护者确认 encode 性能不一定对齐 libpng。
- libspng README 提到支持 miniz 构建，但调研资料没有证明 miniz 是本项目 PNG encode 的性能优先选项；它更像依赖简化/可移植性选项，仍需本地 benchmark。

因此，libspng 可以作为 PNG decode 的候选，但不应作为 PNG encode 的首选替代。

### 7. libdeflate 只作为高复杂度候选，不作为近期主线

**优先级**：低  
**预期收益**：未知  
**风险**：高

libdeflate 是 whole-buffer DEFLATE/zlib/gzip 压缩库，README 明确它显著快于 zlib，但不是 zlib-compatible API，也不支持 streaming。PNG IDAT 写入通常需要 PNG 容器、filter、chunk、CRC 与 deflate 流组合。

除非决定自写 PNG encoder 或引入支持 libdeflate 的 PNG 库，否则直接接入 libdeflate 的工程复杂度过高。它应保留为长期候选，而不是当前优化顺序中的主线。

### 8. 缩略图格式改为 JPEG/WebP 仅作为契约变更候选

**优先级**：低到中，取决于产品契约  
**预期收益**：可能很高  
**风险**：中到高

如果“缩略图必须存在”但不要求“缩略图必须是 PNG”，则 JPEG/WebP 可能比 PNG 更适合封面缩略图，尤其是照片型专辑封面。FFmpeg 的 `libwebp` encoder 支持 lossy/lossless，并支持 alpha；JPEG 则适合不需要 alpha 的封面缩略图。Navidrome 的 artwork 优化经验也显示，缩略图格式和并发上限会显著影响 CPU、内存和缓存尺寸。

但这会改变输出格式契约，涉及 UI、缓存、metadata schema 和兼容性，因此不能作为默认 PNG 优化路线。如果产品契约要求 thumbnail 也必须是 PNG，本项直接跳过；只有在确认消费者接受非 PNG thumbnail 后再评估。

### 9. TagReader 内部两段式流水线最后再考虑

**优先级**：最后  
**预期收益**：未知  
**风险**：中高

在上面的方法都做完后，如果 Worker 仍然明显被封面处理拖慢，再考虑 TagReader 内部两段式流水线：

```text
阶段 A：读取 metadata 和内嵌封面 bytes
阶段 B：有界封面 decode/resize/encode/write
```

它不是当前首选，因为：

- Seriona 外层已经高度并发。
- 当前最大问题是单任务 PNG 工作量过多。
- 两段式流水线会引入封面 bytes / frame 队列、内存背压、错误传播、取消语义。

只有当 PNG 路径优化后仍有明确的 I/O/CPU 阶段错配时，才值得实施。

---

## 推荐执行顺序

```text
1. 删除中间 PNG 往返，直接解码到 RGB frame
2. 暴露并调优 full/thumbnail 的 compression_level 与 pred
3. 对比 std::async、顺序编码、有界 encode pool
4. 评估 zlib-ng 作为 PNG deflate 后端
5. 评估 libpng + zlib-ng 替代 FFmpeg PNG encoder
6. 仅对 PNG 输入解码评估 libspng
7. 长期研究 libdeflate 或自定义 PNG encoder
8. 若产品接受，另线评估 JPEG/WebP thumbnail
9. 最后才考虑 TagReader 内部两段式流水线
```

---

## 本地基准建议

优化选择必须以同一批封面样本本地测量为准。建议样本至少覆盖：

- 小封面、中等封面、2400x2400 大封面。
- JPEG 内嵌封面、PNG 内嵌封面、FLAC picture、MP4 covr。
- 重复封面与完全不重复封面。

每个方案至少记录：

- full PNG encode 时间。
- thumbnail PNG encode 时间。
- 原始图片 decode 时间。
- 是否仍有中间 PNG encode/decode。
- 输出文件大小。
- 冷扫描总 wall time。
- 线程数峰值和 CPU 利用率。
- full/thumbnail 文件是否全部在冷扫描完成。

---

## 外部资料摘要

- FFmpeg PNG encoder 支持 `compression_level` 和 `pred` 私有选项；源码中 `compression_level` 会进入 deflate 初始化，`pred` 可选 `none/sub/up/avg/paeth/mixed`。
- FFmpeg PNG encoder 的可调项主要是上述压缩级别和预测方法；没有看到面向 PNG 压缩本身的 zlib backend 切换或专用并行编码旋钮。
- PNG 规范建议真彩色图像固定 filter 时 Paeth 常见效果较好，adaptive filtering 压缩更好但计算更多。
- libpng `png_set_compression_level()` 支持 0-9；LSB 文档指出 level 3-6 通常接近 level 9 的 PNG 压缩效果且计算更少。
- Chromium PNG 编码代码曾为速度采用 compression level 3 和 sub filter，减少 zlib lazy match 与 filter 计算。
- zlib-ng README 说明其是带 SIMD/CPU 特化的 zlib 替代，支持 zlib-compatible API；OpenCV 讨论中 `zlib-ng + libpng` 对 PNG 编码有明显改善案例。
- libspng README 声称常见场景快于 libpng且 API 更简单，文档提供 buffer decode/encode API；但 issue #224 明确提示大图 encode 可能慢于 libpng，因此更适合先评估 decode，不适合作为 encode 首选。
- libdeflate README 说明其 whole-buffer DEFLATE/zlib/gzip 压缩显著快于 zlib，但不是 zlib-compatible API、无 streaming，因此直接用于 PNG encoder 的集成成本较高。
- FFmpeg `libwebp` encoder 支持 lossy/lossless 和 alpha；若 thumbnail 输出格式契约可变，WebP/JPEG 是另一条线，不属于默认 PNG 优化主线。

参考链接：

- FFmpeg codecs PNG 文档：<https://ffmpeg.org/ffmpeg-codecs.html>
- FFmpeg `pngenc.c`：<https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/pngenc.c>
- PNG encoder recommendations：<https://libpng.org/pub/png/spec/1.2/PNG-Encoders.html>
- libpng compression level：<https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Desktop-generic/LSB-Desktop-generic/libpng12-png-set-compression-level.html>
- Chromium PNG speed tuning 示例：<https://chromium.googlesource.com/chromium/blink.git/+/master/Source/platform/image-encoders/skia/PNGImageEncoder.cpp>
- zlib-ng：<https://github.com/zlib-ng/zlib-ng>
- OpenCV zlib-ng 讨论：<https://github.com/opencv/opencv/issues/22573>
- libspng：<https://github.com/randy408/libspng>
- libspng decode 文档：<https://libspng.org/docs/decode/>
- libspng encode 文档：<https://libspng.org/docs/encode/>
- libspng encode 性能 issue：<https://github.com/randy408/libspng/issues/224>
- libdeflate：<https://github.com/ebiggers/libdeflate>
- FFmpeg `libwebp` source：<https://www.ffmpeg.org/doxygen/8.0/libwebpenc__common_8c_source.html>
- Navidrome artwork optimization PR：<https://github.com/navidrome/navidrome/pull/5181>
- Navidrome artwork performance regression issue：<https://github.com/navidrome/navidrome/issues/5280>
- Navidrome artwork performance fix PR：<https://github.com/navidrome/navidrome/pull/5286>
