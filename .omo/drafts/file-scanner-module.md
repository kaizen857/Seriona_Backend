# file-scanner-module draft

status: awaiting-approval
pending_action: write `.omo/plans/file-scanner-module.md` after explicit user approval
intent: clear
size: architecture
date: 2026-06-20

## User request

根据 `DESIGN.md` 中的文件扫描模块描述，先收集本地与网络资料，再规划代码编写计划。首版只处理单曲，CUE 处理暂时留空不做；未来 CUE 解析仍交给外部 `TagReader`，扫描模块只保留可扩展字段/接口。文件扫描模块重性能，全量扫描必须使用多线程并行技术，可以考虑线程池。模块负责监控文件夹变动、并行分配扫描任务、与 SQLite 构建持久化缓存；具体标签/技术信息读取由 `/home/kaizen857/cppProject(app_and_lib)/TagReader/` 的外部 API 处理。

用户后续补充决策：允许当前项目 C++ 标准整体提升到 C++23；SQLite 缓存必须是完整内容缓存，包含完整歌词，不允许摘要/省略；第一次运行全量构建缓存并计算每个文件/文件夹哈希；第二次启动如果 root 路径匹配，则扫描文件系统哈希并与缓存比对，相同使用缓存，不同重新读取解析并更新缓存，新文件读取解析并创建缓存；缓存可理解为文件系统快照但必须可变并支持运行期实时更新。播放列表树用于 UI 文件浏览器式层级展示，子节点数量不定；可参考旧项目 `std::vector<std::shared_ptr<PlaylistNode>> children` 和 `std::weak_ptr<PlaylistNode> parent` 模型，但新项目应按调研选择更稳的架构。依赖需要考虑 Arch Linux 包可安装性；任何 `pacman`/`yay` 操作都必须由用户在外部执行，规划/执行代理不得运行这些命令。

## Components ledger

- C1 public contracts: `inc/seriona/scanner/...` 纯 C++ 数据契约、事件、服务接口；状态=planned；证据=`DESIGN.md:490`、`DESIGN.md:648`。
- C2 traversal and scheduling: 结构扫描、候选过滤、线程池元数据任务、取消/进度节流；状态=planned；证据=`DESIGN.md:541`、`DESIGN.md:669`。
- C3 TagReader adapter: 对 `TagReader::Read(path, coverExportDir)` 的异常隔离和单曲结果映射；CUE 当前留空不解析，只保留未来 reader 扩展点；状态=planned；证据=`TagReader/include/TagReader.hpp:10`、`TagReader/include/Tag.hpp:11`、`TagReader/src/core/TagPipeline.cpp:592`。
- C4 playlist tree: root/目录/歌曲节点、父子关系、排序、聚合统计、可变工作树和版本化发布视图；状态=planned；证据=`DESIGN.md:492`、`FILE_SCANNER_ANALYSIS.md:27`、`FILE_SCANNER_ANALYSIS.md:94`、用户补充的旧项目节点模型。
- C5 SQLite cache: root/目录/歌曲/错误表、完整 `MusicTag`/完整歌词缓存、WAL、批量事务、LRU/大小限制、哈希比对增量更新；状态=planned；证据=`DESIGN.md:550`、`DESIGN.md:562`、SQLite WAL 文档、用户补充缓存策略。
- C6 folder watching: `wtr/watcher` 文件事件适配器、事件去抖、溢出/丢失时退化为局部或 root 重扫；状态=planned；证据=`DESIGN.md:578`、wtr/watcher README、libuv `uv_fs_event_t` 文档、inotify man page。
- C7 build/tests: CMake 依赖接入、doctest 单元/集成测试、fake TagReader/fake watcher/fake clock；状态=planned；证据=`CMakeLists.txt:35`、`tests/CMakeLists.txt:1`。
- C8 hashing: 文件内容哈希和目录 Merkle 哈希、缓存比对、并行/限流 hash worker；状态=planned；证据=用户补充哈希缓存策略、xxHash README、本机 `pkg-config --modversion libxxhash`。

## Evidence summary

### Repo facts

- `DESIGN.md:49` 到 `DESIGN.md:70` 定义扫描模块职责：遍历 root、识别音频、构建树、调用 `TagReader`、维护 SQLite、监听变更、向 `mediaController` 回传结果/错误。
- `DESIGN.md:490` 到 `DESIGN.md:607` 给出扫描模块详细设计：树状输出不是扁平列表；`MusicTag` 替代旧 `MetaData`；首版单曲，CUE 分轨未来作为同一真实音频文件上的多个歌曲节点；SQLite 是可重建缓存，文件系统是真相。
- `DESIGN.md:517` 到 `DESIGN.md:527` 要求扩展名白名单只是初筛，入库前仍需 probe 或 `TagReader` 读取确认存在音频且无视频。当前 `TagReader::Read` 的 `DetectStream`/`ReadMediaInfo` 已验证音频流，但公开 API 没有显式“无视频流”结果；计划中需保留 `AudioProbe` 抽象或 TagReader 后续字段适配点。
- `DESIGN.md:565` 要求 SQLite WAL、批量扫描后空闲 checkpoint、大量删除后按需 vacuum。
- `DESIGN.md:586` 首选 `efsw`，但本机 `pkg-config --modversion efsw` 当前不可用；计划需包含依赖获取策略，而不是假定系统已有。
- `CMakeLists.txt:1` 到 `CMakeLists.txt:45` 当前项目 C++20，使用 pkg-config 查 FFmpeg，根构建只加入 `app`、`tests`、可选 `tools`；用户已允许整体提升到 C++23，以匹配 `TagReader` 并减少标准不一致风险。
- `app/CMakeLists.txt:1` 到 `app/CMakeLists.txt:28` 和 `tests/CMakeLists.txt:1` 到 `tests/CMakeLists.txt:319` 目前每个目标显式列源码；扫描模块计划应先建立库目标，避免 app/tests 重复列扫描源码。
- 当前无 SQLite、efsw/libuv watcher、线程池、hashing 或扫描模块源码；`grep` 只在文档中找到相关词。
- git 工作树只有 `.omo/run-continuation/...json` 运行时文件改动；产品代码未见扫描相关未提交工作。
- 只读依赖探测结果：`pkg-config --modversion sqlite3` 返回 `3.53.2`，`cmake --find-package -DNAME=SQLite3 ... -DMODE=EXIST` 返回 found；`pkg-config --modversion libuv` 返回 `1.52.1`；`pkg-config --modversion libxxhash` 返回 `0.8.3`；`pkg-config --modversion efsw` 和 `pkg-config --modversion libfswatch` 均未找到。后续调研确认 `libuv` 虽易安装，但 Linux 端递归监听仍要项目自己维护每个目录 watcher，不符合“不要为了环境便利导致代码笨重”的目标。

### TagReader facts

- `TagReader/AGENTS.md:11` 说明对外 API 只有 `TagReader::Read(path)` 和 `TagReader::Read(path, coverExportDir)`。
- `TagReader/include/TagReader.hpp:7` 到 `TagReader/include/TagReader.hpp:12` 公开静态 `Read`，返回 `MusicTag`。
- `TagReader/include/Tag.hpp:11` 到 `TagReader/include/Tag.hpp:162` 的 `MusicTag` 包含 title/artist/album/lyrics/filePath/coverPath/duration/offset/lastModified/sampleRate/bitDepth/bitRate/channels/format/playCount/rating/lastPlayed。
- `TagReader/src/core/TagPipeline.cpp:592` 到 `TagReader/src/core/TagPipeline.cpp:619` 的 `ReadTag` 流程会注册 FFmpeg 格式、校验路径、创建/校验封面导出目录、探测 stream、读媒体信息、读元数据和歌词，并构建 `MusicTag`。
- `TagReader/src/core/TagPipeline.cpp:125` 到 `TagReader/src/core/TagPipeline.cpp:151` 对空路径、不存在、非普通文件抛异常；`src/media/MediaInfoReader.cpp` 对无音频流/不完整音频信息抛异常。
- `TagReader/src/core/TagPipeline.cpp:599` 到 `TagReader/src/core/TagPipeline.cpp:607` 表明即使默认 `Read(path)` 也会使用默认封面导出目录；扫描模块应显式传入 Seriona 管理的 cover export dir，避免导出位置分散。
- `TagReader/CMakeLists.txt:1` 到 `TagReader/CMakeLists.txt:119`：TagReader 是 C++23 项目，静态库目标 `TagReaderCore`，依赖 FFmpeg libavformat/libavcodec/libavutil/libswscale 和 Iconv。Seriona 当前 C++20，计划应通过适配器和 CMake target 隔离，不把 TagReader 内部头泄露到扫描公共接口。
- 当前公开 API 未见 CUE 解析函数；用户已明确 CUE 处理部分可以留空暂时不做。计划只在数据模型中保留 `offset/duration/sourceFilePath` 等字段天然兼容未来 CUE 分轨，不新增 `CueSheetReader` 实现任务。

### External research

- SQLite WAL 官方文档：WAL 通常更快，读者不阻塞写者、写者不阻塞读者，但同一时刻只有一个 writer；默认 WAL 到 1000 pages 左右自动 checkpoint；长读事务会导致 checkpoint 饥饿和 WAL 增长。计划默认：单 writer 连接批量写入，读连接短事务，空闲 PASSIVE checkpoint，必要时 RESTART/TRUNCATE。
- SQLite `busy_timeout` PRAGMA：每个连接应设置 busy timeout，避免并发读写/恢复/checkpoint 短时锁冲突直接失败。
- SQLite `sqlite3_wal_checkpoint_v2` 官方文档：PASSIVE 不等待读写者，FULL/RESTART/TRUNCATE 会更积极且可能阻塞；计划默认扫描后空闲 PASSIVE，缓存维护窗口可使用 RESTART/TRUNCATE。
- Linux `inotify(7)` man page：目录监听不递归；递归树需要每个子目录 watch；事件会合并，不能用来计数；队列可能 `IN_Q_OVERFLOW` 丢事件；rename 配对有 race；文件名处理时对象可能已被删除/重命名。计划默认监听事件只作为 dirty hint，所有变更都用 `std::filesystem` 复查；溢出时触发受影响 root 复扫。
- efsw README：跨平台 C++ watcher，支持 Linux inotify、Windows IOCP、macOS FSEvents/kqueue、递归目录、异步事件；但用户确认 Arch 官方源/AUR 均难以直接安装，且本机 pkg-config 未找到。计划不再默认选用 `efsw`。
- libuv 官方 `uv_fs_event_t` 文档：提供跨平台文件事件句柄，使用各平台合适 backend；事件类型为 `UV_RENAME`/`UV_CHANGE`；目录回调 filename 可能为相对路径也可能为 NULL；`UV_FS_EVENT_RECURSIVE` 当前只支持 macOS/Windows，Linux 不支持递归 flag。因此如果选 `libuv`，Linux 端仍必须自维护 watch registry、子目录新增 watch、rename/overflow 复扫和事件生命周期；这把复杂度留在本项目内，不再作为默认方案。
- libuv filesystem guide：文件系统操作内部使用线程池运行阻塞调用，文件事件 API 封装 inotify/FSEvents/kqueue/ReadDirectoryChangesW 等；但它是通用异步 I/O 库，不是专门为“递归目录树 watcher”提供高层抽象。
- wtr/watcher README：C++17+ 单头文件/可 CMake 集成，公开 API 约为 `auto watcher = wtr::watch(path, callback)`；事件包含绝对路径、路径类型、effect 类型和 rename associated 事件；无外部依赖，MIT，Linux 内部选择 fanotify 或 inotify，并用 epoll/eventfd；项目自述 runtime 约 1579 LOC、测试约 881 LOC。它把 Linux 递归/事件适配复杂度封装在库内，最符合“宁可依赖或 vendor，也不要把本项目代码写笨重”的目标。
- wtr/watcher 包可得性：AUR 搜索 `wtr-watcher` 未命中；官方 README 显示可通过 Conan/Nix/Bazel/CMake 或复制单头文件消费。默认不要求系统包安装，而是 pin 上游 release commit 后 vendored 到 `third_party/watcher` 或用 `FetchContent` 获取。
- libfswatch/fswatch：官方文档提供 C++/C API 和多 backend，AUR 有 `fswatch 1.21.0-1`，但许可证是 GPLv3，且 C++ API 线程安全要求调用方隔离 monitor 实例，动态库/回调模型更重；对纯 C++ 后端播放器核心不作为默认依赖。
- 后台 watcher 对比补充：`libuv` 是 Arch extra 包但 Linux 递归复杂度高；`efsw`/`libfswatch` 能在库内处理递归但包源/许可证/集成重量各有问题；直接 `inotify`/`fanotify` 依赖最少但项目代码最重；`watchexec`、`inotify-tools`、`watchman` 更偏外部 CLI/daemon，不适合作为首选嵌入式 C++ 后端库。
- xxHash README：xxHash 是高性能非加密哈希，XXH3/XXH128 适合高速内容哈希；本机 `pkg-config --modversion libxxhash` 可用。计划默认用 `XXH3_128bits` 或等价 128-bit 内容哈希保存文件内容 hash，目录 hash 使用排序后的子项 Merkle 聚合。

## Adopted defaults

- 首版扫描模块实现全量并行扫描、启动增量扫描、手动刷新和运行时监听；因为用户强调重性能和监控文件夹变动，且 `DESIGN.md` 把运行时监听列为首个正式版本必须能力。
- 项目 C++ 标准整体提升到 C++23：根 `CMakeLists.txt` 从 `CMAKE_CXX_STANDARD 20` 改为 `23`，app/tests/scanner/TagReader 统一标准，避免 TagReader C++23 target 私有链接时产生标准不一致。
- 使用项目内轻量 `ThreadPool`，位于 scanner/common 或 core util，并限制用途为扫描任务；不复用 `mediaController` 串行执行器，不影响音频实时路径。
- SQLite 接入先使用 `sqlite3` C API 的薄 RAII 封装，而不是引入大型 C++ ORM；原因是需求是缓存而非复杂业务数据库，且 `DESIGN.md` 未指定封装方式。
- SQLite 缓存必须保存完整内容：完整 `MusicTag` 字段、完整 `Lyrics` 行列表/时间戳、完整封面路径、技术参数、播放统计、扫描错误、文件内容 hash 和目录 Merkle hash。不得把歌词缩成摘要，不得把完整元数据只存在内存。
- 缓存不是只读快照：SQLite 是可变的文件系统状态缓存。初次扫描创建 root 快照；后续启动和运行期监听会对 root、目录、歌曲、错误记录做 insert/update/delete，并发布新的树版本/增量事件。
- 哈希策略默认使用 `libxxhash` 的高速非加密 hash：文件 hash 对文件内容做 streaming XXH3 128-bit；目录 hash 对排序后的子项元组做 Merkle 聚合，元组包含子项名、类型、大小、mtime、文件 hash/目录 hash。目录 hash 变化驱动子树重建/更新。
- 第二次启动 root 路径匹配时执行 hash-first 增量：遍历文件系统并计算文件/目录 hash；与 SQLite 缓存 hash 相同则直接加载缓存的完整 `MusicTag`/lyrics；hash 不同或缺失则调用 `TagReader::Read(...)` 重新解析并更新缓存；新文件解析并插入缓存；删除文件从缓存和树中删除。
- TagReader 通过内部 adapter/facade 调用；公共 `seriona::scanner` 头不暴露全局命名空间的 `MusicTag` 细节以外的 TagReader internals。若必须保存 `MusicTag`，仅在节点 payload 或 internal contract 中明确所有权。
- CUE 处理当前留空：不扫描 `.cue` 为可播放节点、不解析 cue、不生成 cue track 任务；只让歌曲节点字段天然支持未来 `offset/duration/sourceFilePath/logicalTrackId`。
- 格式过滤先实现扩展名白名单 + TagReader 成功读取确认；“无视频流”如果 TagReader 当前不暴露，计划中作为 `AudioProbe` 接口和后续 TagReader API 扩展点，不在扫描模块直接打开 FFmpeg 二次 probe，避免重复职责。
- 全量扫描三阶段：结构遍历收集目录/候选并建立临时目录拓扑；hash worker pool 并行计算文件内容 hash 和目录 Merkle hash；metadata worker pool 只对 hash miss/new/changed 的候选调用 `TagReader::Read(...)`。树构建和 SQLite 写入在受控归并阶段串行/批量执行，避免多线程写同一树和多 writer SQLite 争用。
- 播放列表树采用“不定子节点数量”的目录树。推荐公共/发布层使用 `PlaylistNode` 值语义字段 + `std::vector<std::shared_ptr<const PlaylistNode>> children`，内部构建层使用 mutable builder nodes；若需要父指针，使用 `std::weak_ptr<const PlaylistNode>` 或节点 ID，避免 shared_ptr 循环。目录节点和歌曲节点可共用一个 node 类型，以 `PlaylistNodeKind::Directory/Song` 替代旧项目的裸 `bool _isDir`，保留旧项目 `children` vector + weak parent 的直观结构优势。
- 内部工作树可变，发布给 `mediaController` 的视图按版本更新。为避免跨线程读写竞态，scanner 在自己的线程中维护 mutable working tree/DB；每次完成全量或增量批次后发布新的树版本或节点级变更事件。这里的“版本化发布”不等于 SQLite 快照不可变，缓存仍实时更新。
- 事件节流：扫描进度按时间窗口或批量节点数合并，完成/错误/取消事件单独发布，事件 sink 只 post 到 mediaController。
- 文件监控默认从 `libuv` 改为 `wtr/watcher`：原因是进一步确认 `libuv` 在 Linux 不支持递归目录 flag，选择它会让本项目维护较重的 watch registry/目录生命周期代码；`wtr/watcher` 是更专用的 C++ watcher，单头文件、无外部依赖、Linux 内部封装 fanotify/inotify/epoll/eventfd，更符合“代码不要笨重”的目标。
- `libuv` 降级为备选方案：仅当用户强制要求全依赖来自 Arch 官方源且接受本项目维护 Linux 递归 watch 代码时使用。`libfswatch` 仅作为备选调研记录，不默认使用，原因是 GPLv3 和集成/线程模型更重。直接手写 inotify/fanotify 只作为最后备选。
- 外部 CLI/daemon 方案不默认采用：`watchexec`/`inotify-tools`/`watchman` 可用于人工调试或未来工具验证，但不应成为核心扫描模块依赖；原因是它们会引入进程管理、输出解析和错误恢复边界，反而让后端嵌入更复杂。
- 依赖安装不由代理执行。若采用默认 `wtr/watcher`，用户无需为 watcher 安装系统包；worker 需通过 pinned `FetchContent` 或 vendored single header 接入。若 SQLite/xxHash 缺失，向用户提示外部执行：`sudo pacman -S sqlite xxhash`。TagReader 仍从本地路径 `/home/kaizen857/cppProject(app_and_lib)/TagReader/` 接入；FFmpeg/Iconv 按现有项目和 TagReader 要求处理。

## Risks and mitigations

- TagReader 和 Seriona 标准差异：用户已允许 Seriona 全仓提升到 C++23，风险变为需要确认音频模块在 C++23 下仍通过现有测试；计划中必须先改标准并跑现有音频全量测试。
- watcher 依赖策略：`wtr/watcher` 不在 Arch 官方源/AUR，需要 vendored single header 或 pinned `FetchContent`；这是为了减少本项目 Linux watcher 代码复杂度。必须固定上游 release commit，并在 `third_party/` 或 CMake 下载记录许可证。
- `wtr/watcher` 风险：项目体量较小且不是发行版系统库；必须用 fake watcher 单测隔离业务逻辑，并增加一个可选真实 watcher smoke test。运行期仍必须把事件当 dirty hint，遇到 watcher warning/error/overflow 类消息时触发目录或 root 重扫。
- libuv 备选风险：Linux 不支持 `UV_FS_EVENT_RECURSIVE`，必须维护 watch registry 并在新增目录时立即 add watch + 扫描子树；事件 filename 可能为 NULL，必须能退化为目录重扫。仅作为备选，不作为默认计划。
- 哈希换时间的边界：严格计算每个文件内容 hash 仍需读取文件全文，启动时 I/O 可能重；收益是避免重复 TagReader 解析、封面导出和 SQLite 大量重写。计划需使用有界 hash worker 和 I/O 限流，避免把磁盘打满。
- SQLite WAL 与多线程：同一时刻只有一个 writer；计划让扫描 workers 不直接写库，只把结果送入 writer 队列/批量事务。
- 文件监听不可靠：wtr/watcher 底层 fanotify/inotify 事件只作 hint；去抖后重新 stat/遍历，溢出/rename 不确定时复扫目录或 root。
- 完整歌词缓存增加 DB 体积：这是用户明确选择的空间换时间。计划中不得摘要歌词；但需要把歌词独立表/JSON blob 与歌曲表分离，避免普通目录浏览查询总是加载完整歌词。
- 目录 hash 稳定性：必须固定排序规则、路径 UTF-8 规范化、大小写策略和 symlink 策略，否则跨平台 hash 不稳定。默认不跟随 symlink，路径序列化为相对 root 的 UTF-8。
- `.omo/run-continuation/...json` dirty_worktree：运行时文件变化不属于产品代码，计划执行时不得覆盖或提交。

## Approval brief

推荐计划方向：新增 `seriona::scanner` 模块，按“C++23 升级 -> 公共契约 -> 线程池/任务调度 -> hash-first 增量缓存 -> TagReader 适配器 -> 播放列表树 -> SQLite 完整缓存 -> wtr/watcher 监听 -> app/测试接入”的顺序实施。首版支持单曲文件节点，高性能全量扫描必须用线程池并行计算 hash 和读取变更文件元数据；CUE 留空不做；SQLite 采用 WAL、单 writer 批量事务和完整内容缓存（含完整歌词）；watcher 事件去抖后重新确认文件系统状态并实时更新可变缓存/工作树。

Open decision for user approval: 是否同意改用依赖策略：SQLite 走系统 `find_package(SQLite3)`/`pkg-config`，内容/目录 hash 使用系统 `libxxhash`，文件监控使用 pinned `wtr/watcher`（vendored single header 或 CMake `FetchContent`），TagReader 从本地路径 `/home/kaizen857/cppProject(app_and_lib)/TagReader/` 作为外部子项目/target 私有链接。若 SQLite/xxHash 缺失，由用户在外部运行 `sudo pacman -S sqlite xxhash`，代理不得执行 `pacman`/`yay`。

If approved, generate `.omo/plans/file-scanner-module.md` via scaffold script and append decision-complete implementation todos. Approval writes the plan only; implementation still requires `$start-work`.
