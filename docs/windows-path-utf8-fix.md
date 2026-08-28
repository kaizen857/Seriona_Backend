# Windows 路径 UTF-8 修复：阶段性总结

> 日期：2026-08-27　范围：Seriona_Backend / TagReader / Seriona（前端）三仓库协作修复
> 状态：**全部完成**——三仓库 MSVC 测试 100% 通过（前端 139/139、后端 117/117、TagReader 161 项含 4 项设计性 skip），打包流程 Smoke 全过

## 1. 问题所在

### 1.1 用户报告的现象

1. **扫描崩溃**：生成的应用扫描 `D:\cloudmusic`（2654 个含日文文件名的音频文件）时抛
   `scanner scan failed with an unhandled exception`，应用无法使用。
2. **打包脚本误选 CMake**：`build-package-windows.ps1` 从 PATH 解析 cmake 时命中了
   `C:\ST\STM32CubeCLT_1.18.0\CMake\bin\cmake.exe`（PATH 首位，用户机器上的 STM32 工具链），
   导致打包流程使用错误的 CMake。

### 1.2 根因分析

**崩溃根因**：MSVC 下 `std::filesystem::path::string()/generic_string()` 按 ANSI 代码页
（CP_ACP）转换；遇到代码页无法表示的字符（如日文）时抛 `std::system_error`
（ERROR_NO_UNICODE_TRANSLATION, 1113）。真实异常已从用户日志
`dist/Seriona-windows-x64/SerionaData/logs/seriona-20260827165615.log` 中捕获确认。

**架构级诱因**（跨三层边界，全部走窄字符路径文本化）：

| 层 | 崩溃点/缺陷 |
|---|---|
| 后端 scanner | 日志、SQLite 键、路径比较键、目录树哈希、LRC 侧车判断等 50+ 处 `generic_string()` |
| 后端 audio | `avformat_open_input` 输入路径（FFmpeg 要求 UTF-8）、波形生成、播放服务 |
| 后端 logging/app | spdlog 文件日志路径（spdlog Windows 版不支持非 ASCII 文件路径）、`runtime_paths.cpp` 解析可执行文件目录（用户目录 `kaizen857` 非 ASCII → `main.cpp:150` 崩溃） |
| TagReader | `ExportFolderCover` 等接口用 `std::string` 传路径（契约缺陷）；多文件 `.string()` 用于错误消息（异常替换风险） |
| 前端 | `toStdString()/toLocal8Bit()` 窄构造路径（`backend_bridge.cpp`、`library_model.cpp`、`main.cpp`） |

**验证通道限制**：命令行工具 `build/seriona` 的交互终端模式（`TerminalMode`）的 TTY 检测在
`#if defined(__unix__) || defined(__APPLE__)` 内，Windows 上恒报
`interactive terminal input is required`——Windows 上无法用 CLI 直接验证扫描，需前端应用或探针程序。

## 2. 全部解决方案（候选方案评估）

### 方案 A：局部 try/catch 兜底（保留 `path::string()`）

- 做法：在崩溃点逐个捕获 `system_error` 降级。
- 优点：改动面最小。
- 缺点：非 ASCII 路径全部落入"兜底/跳过"分支，功能受损（日文曲目无法入库）；崩溃点
  几十处难以穷举；异常路径频繁构造，性能差；治标不治本。

### 方案 B：统一"路径文本 = UTF-8"不变量（**选定，最优**）

- 做法：所有跨边界路径文本一律 UTF-8。
  - `path::generic_u8string()`（C++20 起非弃用，POSIX 上字节级不变）→ `std::string`
  - `std::filesystem::path{u8string}` 构造恢复 `path`
  - 集中 helper：后端 `path_utf8.h`（scanner）/ `path_text.h`（audio）/ `logging.h::pathText`，
    前端 `src/app/path_text.h`，TagReader 各文件局部 `Utf8Text`（u8string 重解释）
- 优点：**代码最优雅**（局部纯函数替换，无平台分叉）；**性能最优**（无异常路径、无额外拷贝、
  与 POSIX 语义完全一致）；**双端一致**（Linux 上字节级不变，满足"不能影响 Linux"硬约束）；
  覆盖所有边界（SQLite 键、比较、日志、FFmpeg 输入、TagReader 接口、前端桥）。
- 缺点：改动面大（三仓库数十文件）——但一次性根治。

### 方案 C：全程宽字符（wstring/wchar_t）

- 做法：Windows 侧全链路 wchar。
- 优点：Windows 原生。
- 缺点：双平台分叉代码量巨大；FFmpeg、TagReader、spdlog 接口均为窄字符 UTF-8 约定，
  两侧都要转换层；违背"代码最优雅、执行效果最好"的选择标准。

### 结论

方案 B 为最优：根治异常、性能无损、Linux 行为不变、代码形态最统一。
方案 A 与 C 均不满足用户约束（不影响 Linux、代码优雅、性能最优）。

## 3. 实施清单

### 3.1 Seriona_Backend

| 文件 | 修改 |
|---|---|
| `src/scanner/path_utf8.h`（新建） | `pathToUtf8` / `pathFromUtf8` 助手 |
| `src/scanner/file_scanner_orchestrator.cpp` | 全部路径文本化点转换（日志、快照键、findRootFor、rewriteAllSongsForRename、watcher 路径、removeLocation 等）；保留 89 行 Linux readlink 字节构造与 1549 行 ASCII `"."` 字面量 |
| `src/scanner/song_identity.cpp` | `canonicalPathText` → `pathToUtf8` |
| `src/scanner/directory_tree_hash.cpp` | `isLyricsSidecarPath` → `pathToUtf8(path.extension())` |
| `src/scanner/path_utils.cpp` | `normalizedExtension` + 5 处日志 |
| `src/scanner/hash_utils.cpp` | 7 处日志（文本包含 song_identity.cpp） |
| `src/scanner/lrc_parser.cpp` | 4 处日志 |
| `src/scanner/cache/sqlite_cache.cpp` | 路径键 `pathToUtf8`；读回点（rootPath/filePath/artworkPath/thumbnailPath/externalLrcPath 等）`pathFromUtf8` |
| `src/scanner/cache/sqlite_cache_connection.cpp` | 同上（315/331 等） |
| `src/scanner/tag_reader_metadata_adapter.cpp` | 日志、trackId/logicalTrackId；**注意** `RawTagMetadata::coverPath/thumbnailPath` 本就是 `path`，不加转换（已回退错误改动） |
| `src/audio/path_text.h`（新建） | `seriona::audio::pathToUtf8/pathFromUtf8` |
| `src/audio/ffmpeg/ffmpeg_audio_source.cpp` | `avformat_open_input` 输入路径 UTF-8 |
| `src/audio/waveform_ffmpeg.cpp` / `waveform_generator.cpp` / `audio_playback_service.cpp` | 路径文本化/恢复 |
| `src/logging/logging.h` / `logging.cpp` / `src/app/application_logging.cpp` | `pathText` helper + 日志路径 UTF-8 |
| `CMakeLists.txt` | `seriona_audio` 追加 `ksuser` 链接（MinGW 下 WASAPI 格式枚举 `KSDATAFORMAT_SUBTYPE_*` 未定义引用；MSVC 下无害、Linux 不受影响） |

### 3.2 TagReader

| 文件 | 修改 |
|---|---|
| `include/TagReader.hpp` / `src/TagReader.cpp` | `ExportFolderCover` 签名 `std::string` → `std::filesystem::path` |
| `src/cover/SidecarCover.cpp` / `src/cover/CoverCache.cpp` | `Utf8Text` helper（u8string→string 重解释）+ `.string()` 错误消息替换；`lockPath` 改 `path += ".lock"`（`PosixCompat.hpp` 已确认 Windows `open/unlink/link/stat` 收 `wchar_t*`，系统调用无需改） |
| `src/core/TagPipeline.cpp` / `src/media/FfmpegSession.cpp` / `src/media/MediaInfoReader.cpp` | `Utf8Text` + 文件路径/扩展名 `.string()` 替换 |

### 3.3 Seriona（前端）

| 文件 | 修改 |
|---|---|
| `src/app/path_text.h`（新建，入 `SERIONA_APP_LAYER_SOURCES`） | `pathTextUtf8` / `pathFromUtf8` |
| `src/app/backend_bridge.cpp` | `toBackendPath`、`root.path`、`resolveRuntimePaths(exePath)` UTF-8 化 |
| `src/app/library_model.cpp` | 快照路径 `fromBackendPath`/`toBackendPath`（189/222/546/581/595） |
| `src/app/backend_snapshot_mapper.cpp` / `waveform_provider.cpp` / `main.cpp` | 复用/新增 UTF-8 转换（main 113/124/150） |
| `scripts/build-package-windows.ps1` | 新增 `-CmakePath` 参数；`Resolve-CMake` 优先显式参数 → `C:\Program Files\CMake\bin\cmake.exe` → PATH 回退 |

## 4. 验证结果（最终）

- **后端 MinGW 构建成功**：149 目标，`seriona.exe` 5.7MB，无错误（仅 watcher.hpp 的
  `-Wmissing-field-initializers` 警告，无害）。ctest 117 项中 4 项失败经 **git stash 原版对照**
  确认全部为 MinGW/Windows 环境预存问题，与本次修改无关（零回归）。
- **MSVC 全量回归（最终基线）**：
  | 仓库/构建树 | 结果 |
  |---|---|
  | 后端 `build-msvc2`（VS2022 + dist vcpkg，`-DCMAKE_BUILD_TYPE=Release`） | **117/117 全过** |
  | 前端打包流程（`dist\.build`，139 项含后端测试关闭） | **139/139 全过** |
  | TagReader `build\msvc-full`（独立，全局 vcpkg） | **161 项 = 157 passed + 4 项 `_WIN32` 设计性 skip（DefaultCover 3 项 POSIX 语义 + Sidecar unreadable dir）** |
- **打包流程**：完整成功（vcpkg restore → configure → build → CTest → windeployqt → applocal →
  受限 PATH 下 6 个 Smoke 场景全过 → ZIP 发布至 `dist\Seriona-windows-x64.zip`）。
- **测试运行要求（Windows）**：后端与 TagReader 的 MSVC ctest 均需 PATH 前置
  `C:\msys64\mingw64\bin`（提供 ffmpeg CLI 生成测试 fixture；vcpkg 的 ffmpeg 无 CLI）；
  开发者模式已开启（symlink 用例可用）。

### 4.1 追加发现并修复：vcpkg ffmpeg 缺 zlib → Windows 封面 PNG 解码静默失败（生产级）

- **现象**：后端 MSVC ctest 中 `seriona.scanner.lazy_cover_gateway` 两个 case 稳定失败
  （期望导出 sidecar 缩略图，实际 `thumbnailPath` 为空）；前端打包 Smoke 未覆盖此路径。
- **排查过程**：`TagReader::Read`/`ExportFolderCover` 在"全局 vcpkg FFmpeg"环境下探针验证
  **成功**，在"dist FFmpeg"环境下**失败**；两份 `vcpkg_abi_info.txt` 对比发现 features 差异：
  - dist：`avcodec;avfilter;avformat;core;swresample;swscale`（**缺 zlib、avdevice**）
  - 全局：`...;zlib`（含 zlib，avcodec-61.dll 13.71MB vs 13.54MB）
- **根因**：FFmpeg 的 **PNG 解码器依赖 zlib**（PNG 为 zlib 压缩）；前端 `vcpkg.json` 的
  ffmpeg `default-features:false` + 手选 features **漏掉 `zlib`** → Windows 上 PNG 封面解码
  必然失败 → `WriteThumbnailAsPng` 静默返回空。Linux 用系统 ffmpeg（自带 zlib）不受影响，
  故只在 Windows 暴露。
- **修复**：`Seriona/vcpkg.json` 的 ffmpeg features 追加 `"zlib"` → 重新 `vcpkg install`
  （ffmpeg[...,zlib] 10 分钟重建）→ 探针验证 PNG 解码恢复（缩略图 351B 生成成功）→
  重新完整打包，包内 avcodec-61.dll 13,714,432B，CTest 139/139 + Smoke 全过。
- **说明**：打包脚本 configure 未传 `CMAKE_BUILD_TYPE` 时，vcpkg 工具链会把 debug lib 写进
  各配置节链接行；vcpkg 的 debug/release 均用 `/MD` 动态 CRT，配合 zlib 修复后功能不受影响，
  独立后端树（build-msvc2）以 `-DCMAKE_BUILD_TYPE=Release` 配置已消除该隐患。
- **待办（用户侧验证）**：`D:\cloudmusic` 探针扫描验证（CLI Windows 终端模式不可用，用
  `makeFileScannerService()` 探针替代）；本机运行 `dist\Seriona-windows-x64\appSeriona.exe`
  扫描实测。

### 4.2 追加发现并修复：扫描收尾阶段残留的窄路径转换（用户实机复现）

- **现象**：用户用 dist 应用扫描 `D:\cloudmusic`（654 个含日文文件名文件）时，`reconcileRoot`
  完成（`reconcileRoot phase timing` 日志正常）后约 1 秒仍弹窗
  `scanner scan failed with an unhandled exception: No mapping for the Unicode character...`
  （ERROR_NO_UNICODE_TRANSLATION / CP_ACP）。ctest 全过是因为测试路径全为 ASCII/GBK 可表示。
- **根因**：扫描聚合阶段 `resolveFolderThumbnails` 用 **UTF-8 displayName 窄字符串做
  `path /= std::string`**（1492 行 `prefix /= component`、1523 行 `relativePath /= *iterator`）——
  MSVC 的 `path` 窄字符串操作按 CP_ACP 解释字节，遇到 GBK 无法表示的日文字符（如 U+9AD9、
  U+20BB7）直接抛 `std::system_error`。
- **修复清单**（全部改为 `pathFromUtf8`/`pathToUtf8`/`generic_u8string` 往返）：
  | 位置 | 修复 |
  |---|---|
  | `src/scanner/file_scanner_orchestrator.cpp` 1492/1523 | `path /= pathFromUtf8(string)` |
  | `src/scanner/playlist_tree_builder.cpp` 375/393/396/430 | renameSubtree 内 `path{UTF-8 string}` 窄构造 → `pathFromUtf8` |
  | `src/scanner/path_utils.cpp` 108 | CUE FILE 行 UTF-8 → `pathFromUtf8` |
  | `src/control/media_controller.cpp` 5 处 | `generic_string()` → `pathText`（u8string）；`isPlaybackTarget` 改 path 直接比较 |
  | `src/control/sqlite_folder_sort_settings_store.cpp` 7 处 | 键读写 `generic_string()` → `pathText`；读回 `row.textColumn(0)` → `pathFromUtf8` |
  | `src/control/sqlite_app_settings_store.cpp` | `sqlite3_open_v2` 路径 `pathText` |
  | `src/metadata/metadata_mapper.cpp` 33 | `file://` URL 用 `generic_u8string` |
  | `TagReader/include/TagReader.hpp` 93 | `BuildMessage` 错误路径 `path.string()` → `u8string` |
  | `Seriona/src/app/library_model.cpp` 177 | `path.extension().string()` → `pathTextUtf8` |
- **回归测试**（此前零覆盖，现已锁定）：
  - `scanner_incremental_e2e_tests.cpp` 新增「scanner e2e resolves folder thumbnails for
    non-ANSI UTF-8 directory names」：日文目录 + 日文文件名完整扫描 + 缩略图解析（GBK
    不可表示字符 U+20BB7/U+9AD9），10 断言；
  - 「playlist tree builder renameSubtree round-trips non-ANSI UTF-8 relative paths」：
    日文路径 renameSubtree 全流程；
  - 测试 fixture helper（`writeAudioFixture` 等、`treeSong`/`treeCueContainer`）的
    `path / std::string` 与 `generic_string()` 一并 UTF-8 化（ASCII 输入字节不变）。
- **验证**：后端 `build-msvc2` **117/117**（e2e 二进制 26 用例 / 615 断言全过）、TagReader
  **161 项全过**；重新打包 CTest 139/139 + 6 Smoke 全过（新包待发布——见下）。
- **发布受阻说明**：打包发布阶段被用户进程锁定（notepad.exe 正打开旧包
  `SerionaData\logs` 下的日志、explorer.exe 正浏览该目录），非代码问题；关闭后重跑
  `scripts\build-package-windows.ps1 -Force` 即可（staging 验证已全部通过）。

### 4.3 增量扫描缓存水合读回窄转换（2026-08-28，第三轮）

- **现象**：Windows 实机（本机 `D:\cloudmusic` 与另一台电脑 `C:\Users\Amadeus\Music`）二次扫描时
  大量 `failed to hydrate scanner cache hit for <路径>: No mapping for the Unicode character
  exists in the target multi-byte code page`；且部分非 ASCII 文件**不报错但路径被污染**
  （播放日志 `loading track 'C:/Users/Amadeus/Music/Lube - 袛邪胁邪泄 蟹邪.mp3'` → ffmpeg
  file not found——`袛邪胁邪泄 蟹邪` 正是 `Боевая машина` 的 UTF-8 字节被 GBK 解码的乱码）。
- **根因**：`SQLiteCache::loadLocation`（缓存水合路径）内联 `const char* → std::string →
  std::filesystem::path` **隐式窄构造**（MSVC 按 CP_ACP 解释 UTF-8 字节）：GBK 不可映射
  字节抛 `ERROR_NO_UNICODE_TRANSLATION`（hydrate 失败、回退全量重扫）；GBK 恰好可映射
  字节（中文/俄语）**静默产生乱码路径**（hydrate"成功"但后续播放/file 操作全部指向错误
  文件）。而计划阶段 `loadLocationsByRoot` 已走 `readLocation`（`pathFromUtf8`），
  **两个入口不一致**——第一轮修复只覆盖了后者。
- **修复**：`readLocation` 改为裸 `sqlite3_stmt*` 单一实现（路径列 2/3/6/14/15/17 全部
  `pathFromUtf8`），`Statement` 暴露 `raw()`，`loadLocation` 复用预编译 `locationStmt_`
  调用 `readLocation`——统一解码、零重复、无 prepare 性能损失。
- **回归测试**（新增「scanner incremental e2e hydrates non-ANSI UTF-8 paths from cache
  without narrow conversion」）：日文目录/文件名（U+20BB7/U+9AD9）全量扫描 → 增量二次
  扫描断言 `skipped=1`（缓存命中成功）且 reader 不重读——修复前该用例 skipped=0（抛
  1113 回退重扫）。
- **验证**：后端 `build-msvc2` **117/117**（e2e 二进制 27 用例）；`D:\cloudmusic` 实机
  扫描 0 错误。已入库的缓存数据本身是 UTF-8（写入端 `pathText` 对称），**无需重建库**，
  升级后重扫即恢复正确路径。

## 5. 文档与约束更新

- `Seriona_Backend/AGENTS.md`「构建、运行与依赖」与根 `AGENTS.md`「跨仓库约定」已写入：
  **Windows 下构建工具统一使用 MSVC（Visual Studio 2022 x64 + Visual Studio 生成器，
  依赖经 vcpkg 提供），禁止使用 MinGW/msys2 等其它工具链构建或验证**；并注明 CLI 终端
  模式仅支持 Unix 类系统（Windows 上属预期）。
