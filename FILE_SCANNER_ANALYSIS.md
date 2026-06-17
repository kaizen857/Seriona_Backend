# 旧项目文件扫描模块分析

本文档记录旧项目 `/home/kaizen857/qtProject/MusicPlayer/` 中 `FileScanner`、`PlaylistNode` 和扫描结果树的构建方式，作为 `seriona` 后端文件扫描模块设计参考。

## 1. 核心结论

旧项目的扫描结果不是扁平歌曲列表，而是一棵以 `PlaylistNode` 为节点的播放列表树。树根对应用户选择的扫描根目录或单个音频文件；目录节点保存子目录和音频文件节点；音频文件节点保存路径、元数据和封面缓存键。

扫描完成后，`FileScanner` 持有 `rootNode`，并通过 `setScanFinishedCallback()` 注册的回调把 `std::shared_ptr<PlaylistNode>` 树根传给 `MediaController`。上层也可以通过 `getPlaylistTree()` 读取当前树根。

## 2. PlaylistNode 树节点

旧项目的 `PlaylistNode` 定义在 `/home/kaizen857/qtProject/MusicPlayer/inc/PlaylistNode.hpp`。

关键字段：

- `_isDir`：标记节点是目录还是音频文件。
- `path`：节点对应的完整文件系统路径。
- `metaData`：文件元数据；目录节点也借用其中的 `coverPath` 保存目录封面路径。
- `children`：子目录和音频文件节点列表。
- `parent`：弱引用父节点，根节点为空。
- `totalSongs` / `totalDuration`：目录聚合出的歌曲数量和总时长。
- `_coverKey`：封面缓存键。

关键行为：

- `addChild()` 把子节点加入 `children`，并自动设置子节点父指针。
- `sortChildren()` 默认让目录排在文件前，再按路径和 CUE 偏移排序。
- `setTotalSongs()` / `setTotalDuration()` 保存目录聚合结果。
- `setCoverPath()` / `setCoverKey()` 保存目录或歌曲封面信息。

## 3. FileScanner 对外接口

旧项目的 `FileScanner` 定义在 `/home/kaizen857/qtProject/MusicPlayer/inc/FileScanner.hpp`。

核心成员：

- `rootDir`：扫描根路径。
- `scanThread`：`std::jthread` 后台扫描线程。
- `rootNode`：扫描完成后生成的播放列表树根。
- `hasScanCpld`：扫描完成标志。
- `m_callback`：扫描完成回调，参数是 `std::shared_ptr<PlaylistNode>`。

核心接口：

- `startScan()`：启动后台扫描。
- `stopScan()`：请求停止并等待扫描线程结束。
- `getPlaylistTree()`：返回当前 `rootNode`。
- `setScanFinishedCallback()`：注册扫描完成回调。
- `scanFile()`：同步扫描单个音频文件并返回歌曲节点。
- `scanDirectory()`：同步扫描目录并返回目录树根节点。

## 4. 异步扫描流程

旧项目真正的树构建逻辑在 `/home/kaizen857/qtProject/MusicPlayer/src/FileScanner.cpp` 的 `ScannerLogic` 内部。

流程如下：

1. `FileScanner::startScan()` 创建 `std::jthread`，在线程中执行 `scanDir()`。
2. `scanDir()` 规范化 `rootDir`，检查路径是否存在。
3. 如果根路径是普通文件：
   - 创建单个 `PlaylistNode(rootPath, false)`。
   - 调用 `processNodeTask()` 读取元数据和封面键。
   - 设置 `totalSongs = 1` 和文件时长。
   - 当前实现会直接返回，不触发完成回调。
4. 如果根路径是目录：
   - 创建目录根节点 `PlaylistNode(rootPath, true)`。
   - 调用 `scanAndDispatch(rootPath, rootNode, stoken)` 递归构建树。
   - 等待线程池完成音频元数据任务。
   - 调用 `postProcessAggregation(rootNode)` 聚合统计、排序、补目录封面。
   - 再次等待封面任务完成。
   - 设置完成标志并调用 `m_callback(rootNode)`。

## 5. 递归建树细节

`scanAndDispatch()` 是目录树构建核心：

- 先收集当前目录所有 `directory_entry`，避免边遍历边处理造成状态混乱。
- 第一轮优先处理 `.cue` 文件：
  - CUE 中每个 track 会生成一个音频文件节点。
  - 节点预置 CUE 标题、艺术家、起始偏移和持续时间。
  - 被 CUE 接管的真实音频文件会加入 `cueHandledFiles`，防止第二轮重复生成普通歌曲节点。
- 第二轮处理普通文件和子目录：
  - 音频文件通过 `isffmpeg()` 判断后创建 `PlaylistNode(path, false)`，挂到当前目录节点下。
  - 封面图片文件只记录为当前目录的 `coverPath`，供歌曲和目录封面回退使用。
  - 子目录路径先收集，后续递归扫描。
- 音频节点元数据读取被批量提交到线程池。
- 子目录会创建 `PlaylistNode(subDir, true)`，递归扫描后如果非空再挂到当前节点。

这个设计保证最终树结构与文件系统层级一致：目录节点保存目录，文件节点保存歌曲，CUE 分轨作为歌曲节点插入到对应目录下。

## 6. 后处理聚合

`postProcessAggregation()` 自底向上处理目录节点：

- 递归统计子树中的歌曲数。
- 汇总子树总时长。
- 调用 `sortChildren()` 排序，使目录优先并保持稳定展示顺序。
- 为目录选择封面：优先当前目录封面，其次子目录封面，再其次子歌曲内嵌封面。
- 封面处理仍使用线程池异步计算缓存键。

因此扫描完成回调拿到的树已经是可直接展示的播放列表树，而不是需要上层再次聚合的原始扫描结果。

## 7. MediaController 接收方式

旧项目 `MediaController` 构造时注册扫描完成回调：

```text
FileScanner::setScanFinishedCallback(
  tree -> MediaController::handleScanFinished(tree)
)
```

`handleScanFinished()` 的职责：

- 将 `rootNode` 更新为扫描得到的树根。
- 将 `currentDir` 指向树根，作为 UI 当前浏览目录。
- 调用 `DatabaseService::saveFullTree(rootNode)` 持久化整棵树。
- 通知 UI 扫描完成。

也就是说，旧项目的上层模型是“控制器持有播放列表树根，UI/播放逻辑都围绕树节点指针工作”。

## 8. 数据库保存与恢复

旧项目数据库并不保存一份扁平播放列表作为权威结构，而是把树拆成目录表和歌曲表：

- `saveFullTree()` 递归遍历 `PlaylistNode` 树。
- 目录节点写入 `table_directories`，保存父目录 ID、目录路径和封面键。
- 歌曲节点写入 `table_songs`，保存父目录 ID、元数据、文件路径、封面键、时长、偏移、采样率、位深等。
- `loadFullTree()` 先加载目录表重建目录节点和父子关系，再加载歌曲表挂到目录节点下。
- 恢复后重新聚合 `totalSongs`、`totalDuration` 并排序。

这说明旧项目的缓存也是围绕“树可重建”设计的，而不是围绕虚拟歌单设计的。

## 9. 对 seriona 的设计启示

`seriona` 后端可以保留旧项目的核心结构，但需要调整边界：

- 文件扫描模块最终输出应是一棵播放列表树的根节点指针或智能指针。
- 播放列表树应表达真实文件系统层级，而不是虚拟歌单。
- 扫描模块负责构建树、挂接父子节点、做扫描进度和错误汇总。
- 元数据读取应委托给新的 `TagReader`，扫描模块不再直接耦合 TagLib/FFmpeg 技术信息读取细节。
- SQLite 缓存应保存足够信息以重建树，同时仍以文件系统为最终真相。
- `mediaController` 应持有当前播放列表树根，UI 和播放控制可通过节点指针定位目录或歌曲。

推荐设计表述：

```text
文件扫描模块扫描完成后返回 PlaylistNode 树根；
根节点代表扫描 root，目录节点代表文件系统目录，歌曲节点代表可播放音频条目；
mediaController 持有这棵树作为媒体库权威视图。
```
