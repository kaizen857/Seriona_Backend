
2026-06-17：T0 依赖预检通过。已验证 `c++`、`cmake`、`ctest`、`pkg-config`、Git 仓库状态以及 FFmpeg 开发库 `libavformat`、`libavcodec`、`libavutil`、`libavfilter`、`libswresample` 均可用；证据见 `.omo/evidence/task-0-dependency-preflight.txt`。

2026-06-17：T1 完成了最小 CMake/CTest 基线。CMake 使用 `pkg-config` 探测 FFmpeg 五个库，测试目标显式包含 `third_party/doctest` 与 `third_party/miniaudio` 头路径，并通过了 `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON`、`cmake --build build`、`ctest --test-dir build --output-on-failure`。

2026-06-17：第三方头已切换为上游官方快照：`doctest` 取自 `v2.5.2`，`miniaudio` 取自 `master` 的当前单头；`third_party/` 继续保留显式 include 路径契约。

2026-06-17：系统环境同时提供了 `/usr/include/doctest/doctest.h` 与 `/usr/include/miniaudio/miniaudio.h`，后续以系统安装源文件作为可追溯快照复制到 `third_party/`。

2026-06-17：T2 已开始定义音频公共契约，新增 `inc/seriona/audio/audio_contracts.h`，把 `AudioPlayer`、`AudioPlaybackService`、`TrackPlaybackRequest`、`AudioOutputConfig`、`AudioDeviceFormat`、`PlaybackClockSnapshot`、`PlaybackEvent`、`PlaybackState`、`PlaybackErrorCode`、`BackendEvent` 和 `BackendEventSink` 收敛到同一公共边界。

2026-06-18：T2 在重启/中断后已重新复验，`seriona_audio_contract_tests` 与 `audio_contract` 通过，公共边界 grep 仍为空。
2026-06-18：T3 采用测试时生成的 WAV fixture 方案，`seriona.audio_fixture` 已通过；生成文件落在构建树下的 `generated_audio_fixtures/`，不需要提交二进制素材。
2026-06-18：T3 guardrail 修正：补齐 `tests/fixtures/` 与 `tools/` 占位目录，并清理 contract test 中的 `artist`/`flac` 字面量误报，方便精确 no-copyright grep 通过。
