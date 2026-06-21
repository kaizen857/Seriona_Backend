# 决策记录

- 本任务只落最小 scaffold：`seriona_metadata` 静态库 + 一个私有实现文件 + 一个 doctest 断言，用来证明测试目标真实可跑。
- 平台 API 和真实 MPRIS/SMTC 适配仍留到后续任务；本步不引入 D-Bus、Windows SDK 或任何运行时平台依赖。
- 控制快照契约统一放在 `inc/seriona/control/control_contracts.h`，并保持纯标准库依赖；平台适配层后续只需消费这些值类型，不应在 public header 里泄漏任何 OS 专有命名。
- `PlayerSnapshot` 的内部新鲜度信息采用 `SnapshotFreshness` 子对象承载，避免把版本/采样时间当作业务字段散落在 snapshot 根部。
