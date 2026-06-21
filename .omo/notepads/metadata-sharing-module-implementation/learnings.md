# 学习记录

- 元数据模块第一步只需要建立可编译、可测试的最小骨架即可；把平台依赖限制在 `src/metadata/` 私有实现层，可以先让后续契约工作有稳定的 CMake/CTest 入口。
- `ctest` 的测试名和可执行体路径需要和构建输出目录一致；这次 `seriona.metadata_contract` 已经对齐到 `build/tests/seriona_metadata_contract_tests`。
