# 学习记录

- 元数据模块第一步只需要建立可编译、可测试的最小骨架即可；把平台依赖限制在 `src/metadata/` 私有实现层，可以先让后续契约工作有稳定的 CMake/CTest 入口。
- `ctest` 的测试名和可执行体路径需要和构建输出目录一致；这次 `seriona.metadata_contract` 已经对齐到 `build/tests/seriona_metadata_contract_tests`。
- 测试目标应只链接 `seriona_metadata`，不要再把同一实现源文件直接塞进测试可执行体；否则会把 scaffold 变成重复编译结构，后续扩展更难维护。
- 公共控制契约应通过 `inc/seriona/control/control_contracts.h` 暴露，测试目标只需要把 `${PROJECT_SOURCE_DIR}/inc` 加入 include path，就能直接验证平台中立 API，而无需把控制契约揉进 `src/metadata/` 私有头。
- `version` / `sampledAt` 这类内部新鲜度元数据适合封装成独立的 `SnapshotFreshness` 值对象，既保留快照追踪能力，也避免把平台 payload 字段和时钟字段混在一起。
- 契约命名最好直接贴合计划语言：`ArtworkRef`、`PlaybackCapabilities`、`PlaybackTimeline`、`PlayerStateSnapshot`、`SubscriptionHandle` 比更泛化的名字更容易和后续 Todo 3-10 对齐。
- `MediaControlCommand` 需要显式覆盖 `SetShuffle`，否则后续 MPRIS 命令桥接会在合同层缺一个可表达的输入。
- `MetadataSharingService` 适合直接复用控制层的 `MediaControlCommand` / `SubscriptionHandle` / `PlayerStateSnapshot` 语义；metadata 契约只需要补自己的 backend kind、capabilities、sync result 和平台扩展入口，不需要再发明一套独立命令/订阅模型。
- 如果未来确实需要平台宿主句柄，应该放进 `MetadataSharingOptions` 的 opaque extension，而不是把 HWND/WinRT 之类平台名抬进核心 API。
