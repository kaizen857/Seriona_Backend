
2026-06-17：T0 依赖预检通过。已验证 `c++`、`cmake`、`ctest`、`pkg-config`、Git 仓库状态以及 FFmpeg 开发库 `libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample` 均可用；证据见 `.omo/evidence/task-0-dependency-preflight.txt`。

2026-06-17：T1 完成了最小 CMake/CTest 基线。CMake 使用 `pkg-config` 探测 FFmpeg 五个库，测试目标显式包含 `third_party/doctest` 与 `third_party/miniaudio` 头路径，并通过了 `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`、`cmake --build build`、`ctest --test-dir build --output-on-failure`。
