# Task 1 Evidence

- 已更新 `AGENTS.md`：保留后端纯 C++23、禁止 Qt/QML/UI 的边界，同时允许仅 `src/metadata/` 下的平台适配实现文件使用 MPRIS、SMTC、`sdbus-c++`、WinRT，并显式允许 `.omo/evidence/metadata-sharing-module-implementation/` 作为提交证据目录。
- 已添加最小元数据骨架：`seriona_metadata` 静态库、私有 `src/metadata/metadata_module.cpp`、`tests/metadata/metadata_contract_tests.cpp`，以及 `seriona.metadata_contract` CTest 注册。
- 已补首个真实断言：`tests/metadata/metadata_contract_tests.cpp` 验证 `moduleName()` 返回 `seriona_metadata`，不是占位符测试。
- 验证结果：`cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` 通过；`cmake --build build --target seriona_metadata_contract_tests` 通过；`ctest --test-dir build -R seriona.metadata_contract --output-on-failure` 通过。
- 复核清理：移除了 `tests/metadata/metadata_contract_tests.cpp` 中重复的 `metadata_module.h` include，并删除了 `tests/CMakeLists.txt` 里对 `${PROJECT_SOURCE_DIR}/src/metadata/metadata_module.cpp` 的直接编译，让测试只通过 `seriona_metadata` 链接该实现。
