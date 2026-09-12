cmake_minimum_required(VERSION 3.27)

project(seriona_export_consumer
  LANGUAGES CXX
  DESCRIPTION "SerionaBackend 安装导出（find_package）最小消费者 smoke"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 本工程只消费已安装的 SerionaBackend 包，不 add_subdirectory 后端源码，
# 也不被 Seriona_Backend/tests/CMakeLists.txt 引用（独立项目）。
# 全部传递依赖（FFmpeg/xxhash/SQLite3/spdlog/Threads/TagReaderCore/pipewire/sdbus）
# 由 SerionaBackendConfig.cmake 的 find_dependency 解析；调用方只需提供
# CMAKE_PREFIX_PATH 指向安装前缀。
find_package(SerionaBackend CONFIG REQUIRED)

# 安装导出只携带真实 target 名（SerionaBackend::seriona_scanner）；构建树里的
# 别名 SerionaBackend::scanner 不会被 install(EXPORT) 导出。两种形态都兼容，
# 安装消费走后者。
if(TARGET SerionaBackend::scanner)
  set(SERIONA_CONSUMER_SCANNER_TARGET SerionaBackend::scanner)
elseif(TARGET SerionaBackend::seriona_scanner)
  set(SERIONA_CONSUMER_SCANNER_TARGET SerionaBackend::seriona_scanner)
else()
  message(FATAL_ERROR
    "SerionaBackend 包中未找到 scanner 组件目标（期望 SerionaBackend::scanner 或 SerionaBackend::seriona_scanner）")
endif()

add_executable(seriona_export_consumer main.cpp)
target_link_libraries(seriona_export_consumer PRIVATE ${SERIONA_CONSUMER_SCANNER_TARGET})
