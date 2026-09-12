# SerionaBackend 导出消费 smoke（find_package 最小消费者）

## 用途

本目录是一个**独立的最小 CMake 工程**，用于端到端验证 `SERIONA_INSTALL_EXPORT=ON`
安装出的 `SerionaBackend` CMake 包（`find_package(SerionaBackend CONFIG REQUIRED)`）
可以被外部消费者正确配置、编译、链接并运行。它对应 CI 顺序链
`TagReader install → Backend install → 消费方 find_package` 的最后一环，
是导出正确性（`find_dependency` 清单、静态库导出接口、efsw 打包）的唯一端到端证明。

- `CMakeLists.txt`：`cmake_minimum_required(VERSION 3.27)`、C++23、`find_package` + 链接 scanner 组件。
- `main.cpp`：调用 scanner 最小符号（`scannerModuleLinked()`、`makeFileScannerService()`），成功后输出固定标记。
- 本工程**不被** `Seriona_Backend/tests/CMakeLists.txt` 引用，也不 `add_subdirectory` 后端源码；只能消费已安装前缀。

## 前置条件

- Linux（本机验证环境）：CMake ≥ 3.27、C++23 编译器；系统包提供
  FFmpeg 开发库（`libavformat/libavcodec/libavutil/libavfilter/libswresample`，经 pkg-config）、
  `libxxhash`、`spdlog`、`SQLite3`、`Threads`，以及 `UNIX AND NOT APPLE` 时的
  `libpipewire-0.3`、`sdbus-c++`（与后端构建要求一致）。
- `TagReaderCore` 必须以**已安装的 CMake 包**形式存在于同一前缀
  （`<prefix>/lib/cmake/TagReaderCore/TagReaderCoreConfig.cmake`）；
  后端 `SerionaBackendConfig.cmake` 里有 `find_dependency(TagReaderCore CONFIG)`。
- 若系统前缀尚未安装 `TagReaderCore`，按下面第 1 步用相邻仓库 `../TagReader` 先装到同一前缀。

## 完整命令序列

以下命令假定：后端仓库根为 `Seriona_Backend`，相邻 TagReader 仓库为 `../TagReader`，
安装前缀统一为 `/tmp/seriona-prefix`，consumer 构建目录为 `/tmp/seriona-consumer`。

### 1. 安装 TagReaderCore 到共享前缀（CI 顺序链 ①）

```bash
cd ../TagReader
cmake --preset default
cmake --build --preset default -j8
cmake --install build/default --prefix /tmp/seriona-prefix
```

### 2. 构建并安装 SerionaBackend（CI 顺序链 ②）

```bash
cd Seriona_Backend
cmake -S . -B build/w3-t10 \
  -DSERIONA_INSTALL_EXPORT=ON \
  -DSERIONA_BUILD_TESTS=OFF \
  -DCMAKE_PREFIX_PATH=/tmp/seriona-prefix
cmake --build build/w3-t10 -j8
cmake --install build/w3-t10 --prefix /tmp/seriona-prefix
```

> `-DCMAKE_PREFIX_PATH=/tmp/seriona-prefix` 让后端复用第 1 步安装的 `TagReaderCore`
> （配置输出含 `Seriona backend: reusing installed TagReaderCore via find_package`），
> 与 CI 顺序链一致；不加时后端会退回相邻 `../TagReader` 源码嵌入链，也能构建，
> 但消费方仍需前缀里的 TagReaderCore。

### 3. 配置、构建、运行 consumer（CI 顺序链 ③）

```bash
cd Seriona_Backend
cmake -S tests/export_consumer -B /tmp/seriona-consumer \
  -DCMAKE_PREFIX_PATH=/tmp/seriona-prefix
cmake --build /tmp/seriona-consumer -j8
/tmp/seriona-consumer/seriona_export_consumer
```

成功时输出：

```text
seriona export consumer ok
```

### 4. 隔离验证（证明只依赖安装前缀）

安装完成后把后端构建目录改名，再用全新目录消费：

```bash
cd Seriona_Backend
mv build/w3-t10 build/w3-t10.moved
cmake -S tests/export_consumer -B /tmp/seriona-consumer-isolation \
  -DCMAKE_PREFIX_PATH=/tmp/seriona-prefix
cmake --build /tmp/seriona-consumer-isolation -j8
/tmp/seriona-consumer-isolation/seriona_export_consumer
# 验证完毕后恢复目录名：
mv build/w3-t10.moved build/w3-t10
```

隔离构建成功说明消费链完全不引用构建机上的后端构建目录，只用 `/tmp/seriona-prefix`。

## 说明

- 安装导出只携带真实 target 名（`SerionaBackend::seriona_scanner`）；构建树别名
  `SerionaBackend::scanner` 不会被 `install(EXPORT)` 导出。本工程对两种形态都兼容
  （`CMakeLists.txt` 内做一次目标解析）。
- consumer 只链接 scanner 组件；其导出接口已带全部传递依赖
  （`efsw-static`、`TagReaderCore`、`SQLite3`、xxhash、spdlog、Threads、头库），
  由 `SerionaBackendConfig.cmake` 的 `find_dependency` 在配置阶段重建。
- 该 smoke 不做实际扫描/播放，只验证消费与链接面；行为测试由后端 CTest 负责。
