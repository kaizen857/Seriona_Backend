# Task 2 Evidence

- 已定义平台中立控制快照契约：新增 `inc/seriona/control/control_contracts.h`，覆盖轨道身份、展示元数据、封面引用、播放状态、重复模式、能力集合、时间线、玩家快照、媒体控制命令、订阅/回调别名与命令 sink seam。
- 已将内部新鲜度信息收束为 `SnapshotFreshness` 值对象，`PlayerSnapshot` 默认态保持“无当前曲目”，并保留 `version` / `sampledAt` 作为快照新鲜度元数据而非平台 payload。
- 已补 baseline characterisation 测试：`tests/metadata/metadata_contract_tests.cpp` 现在验证 `PlayerSnapshot{}` 的默认曲目为空、播放状态为 `Stopped`、重复模式为 `Off`，并检查快照新鲜度默认值。
- 已修正测试 target 的 include 传播：`tests/CMakeLists.txt` 为 `seriona_metadata_contract_tests` 补入 `${PROJECT_SOURCE_DIR}/inc`，使公共头作为真实契约可被测试目标直接消费。
- 验证结果：`cmake --build build --target seriona_metadata_contract_tests` 通过；`ctest --test-dir build -R seriona.metadata_contract --output-on-failure` 通过。
- 追加入参语义修正：公共契约现对齐为 `ArtworkRef`、`PlaybackCapabilities`、`PlaybackTimeline`、`PlayerStateSnapshot`、`SubscriptionHandle` 与 `SetShuffle` 命令，测试同时覆盖默认 shuffle/capability/command 语义。
- 额外验证：修复一次 C++20 designated initializer 顺序问题后重新构建，`cmake --build build --target seriona_metadata_contract_tests` 和 `ctest --test-dir build -R seriona.metadata_contract --output-on-failure` 均保持通过。
