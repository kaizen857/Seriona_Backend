# 决策记录

- 本任务只落最小 scaffold：`seriona_metadata` 静态库 + 一个私有实现文件 + 一个 doctest 断言，用来证明测试目标真实可跑。
- 平台 API 和真实 MPRIS/SMTC 适配仍留到后续任务；本步不引入 D-Bus、Windows SDK 或任何运行时平台依赖。
- 控制快照契约统一放在 `inc/seriona/control/control_contracts.h`，并保持纯标准库依赖；平台适配层后续只需消费这些值类型，不应在 public header 里泄漏任何 OS 专有命名。
- `PlayerSnapshot` 的内部新鲜度信息采用 `SnapshotFreshness` 子对象承载，避免把版本/采样时间当作业务字段散落在 snapshot 根部。
- 计划词汇优先：公共控制契约采用 `PlayerStateSnapshot` / `PlaybackCapabilities` / `PlaybackTimeline` / `ArtworkRef` / `SubscriptionHandle`，以减少后续 Todo 3-10 的映射歧义。
- `SetShuffle` 属于合同级 command 能力，必须与 repeat/seek/volume 并列存在，方便后续 MPRIS 直接桥接。
- metadata 服务契约应在 public header 中保持平台中立：用 `MetadataBackendKind` / `MetadataBackendCapabilities` / `PlatformMediaState` / `MetadataSyncResult` 描述能力与同步结果，平台差异通过 options/capabilities/opaque extension 承载，不新增 Windows 专属公共签名。
- `seriona_metadata` 的构建拆分会提前预留 mapper/service/MPRIS 测试入口，即使实现仍是最小骨架，也要让 `ctest -R seriona.metadata` 跑到真实断言，而不是空壳。
- Linux MPRIS 真实适配的 `sdbus-c++` 检查采用 configure gate；模拟缺失开关 `SERIONA_METADATA_SIMULATE_MISSING_SDBUS` 只用于测试依赖门，不会降级成 Noop。
- mapper 这一层只负责把 `PlayerStateSnapshot` 转成平台中立 DTO，不做同步器、服务生命周期或传输层封装，后续 Todo 仍需在各自模块里单独实现。
- 平台 DTO 统一保留 microseconds 级时间值，避免在 mapper 里混入平台特定时间单位转换规则；Windows DTO 刻意不声明 volume/mute 支持，避免提前承诺未实现能力。
- track number 采用显式可空 DTO 字段承载，但当前 snapshot 侧没有可靠来源时保持为空，避免 mapper 伪造序号。
- artwork 输出沿用 `ArtworkRef` 的 local path/URI/content hash 三元组，mapper 只做透传，不在 Todo 5 里引入额外的文件 I/O 或封面解析逻辑。
- Todo 6 的 synchronizer 采用双脏路径：static metadata 仅在 track / title / artwork / capability 变化时发出，timeline 仅在 1 秒节流或播放边界事件时发出，二者互不驱动。
