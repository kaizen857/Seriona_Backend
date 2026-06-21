# Task 12 Evidence

## 变更文件

- `docs/metadata-sharing.md`
- `.omo/notepads/metadata-sharing-module-implementation/learnings.md`

## 验证记录

- `cmake --build build --target seriona_metadata_mpris_tests`，通过，`ninja: no work to do.`
- `ctest --test-dir build -R seriona.metadata --output-on-failure`，通过，6 个 metadata 相关测试全部通过。
- `rg 'Linux|Windows|1 秒|sdbus-c\+\+|MPRIS|ISystemMediaTransportControlsInterop|file://|RandomAccessStreamReference' docs/metadata-sharing.md`，通过，命中所需术语。
- `rg 'Windows.*(已验证|已测试|runtime-tested|运行测试通过)' docs/metadata-sharing.md`，无结果，符合禁止声明。

## 结论

文档已用中文说明模块边界、统一 API、Linux MPRIS 字段映射、Windows SMTC 官方流程、1 秒时间线策略、静态元数据脏标记策略、Windows HWND 要求、Windows 当前阶段未测试状态、Linux 测试门禁，以及未发布的内部字段。
