cmake_minimum_required(VERSION 3.20)

project(Seriona VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

include(FetchContent)

option(SERIONA_BUILD_APP "Build Seriona standalone app" ON)
option(SERIONA_BUILD_TESTS "Build Seriona tests" ON)
option(SERIONA_BUILD_TOOLS "Build optional Seriona tools" OFF)
option(SERIONA_INSTALLED_MODE "Resolve runtime paths via XDG Base Directory (installed layout)" OFF)
option(SERIONA_SCANNER_SIMULATE_MISSING_SQLITE "Simulate missing scanner SQLite3 dependency" OFF)
option(SERIONA_SCANNER_SIMULATE_MISSING_XXHASH "Simulate missing scanner libxxhash dependency" OFF)
option(SERIONA_METADATA_SIMULATE_MISSING_SDBUS "Simulate missing metadata sdbus-c++ dependency" OFF)
option(SERIONA_INSTALL_EXPORT "Install and export SerionaBackend CMake package (CI build-chain reuse)" OFF)
set(SERIONA_TAGREADER_SOURCE_DIR "" CACHE PATH "Path to the TagReader source directory")
set(SERIONA_TAGREADER_BINARY_DIR "")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # 源码与注释为 UTF-8 无 BOM；MSVC 默认按系统 ANSI 代码页（如 GBK/936）解析，
  # 中文注释的 UTF-8 字节可能被误解码为续行符而吞掉代码行（C4819 系列）。
  # /utf-8 强制按 UTF-8 读取与输出，与 GCC/Clang 默认行为一致（Linux 不受影响）。
  add_compile_options(/utf-8)
endif()

# Release 优化基线（三仓库统一方案）：编译选项由 CMake 默认 Release
# （GCC -O3 -DNDEBUG / MSVC /O2 /DNDEBUG）+ CMakePresets.json 的
# CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE（GCC -flto / MSVC /GL+/LTCG）提供；
# 指令集显式 x86-64 基线（替代 -march=native，保证产物可跨机器分发）。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  add_compile_options(-march=x86-64)
endif()

add_compile_definitions($<$<CONFIG:Release>:SERIONA_RELEASE_BUILD>)

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SERIONA_FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libavfilter
  libswresample
)

find_package(spdlog CONFIG REQUIRED)

FetchContent_Declare(thread_pool
  GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
  GIT_TAG v4.1.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(thread_pool)

if(NOT TARGET BS::thread_pool)
  add_library(BS_thread_pool INTERFACE)
  add_library(BS::thread_pool ALIAS BS_thread_pool)
  target_include_directories(BS_thread_pool SYSTEM INTERFACE
    $<BUILD_INTERFACE:${thread_pool_SOURCE_DIR}/include>
  )
endif()

add_library(seriona_third_party_headers INTERFACE)
target_include_directories(seriona_third_party_headers INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/doctest>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/miniaudio>
)

add_library(seriona_watcher_headers INTERFACE)
target_include_directories(seriona_watcher_headers INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/watcher/include>
)

if(APPLE)
  # watcher 在 macOS 下走 FSEvents（Linux inotify / Windows ReadDirectoryChangesW
  # 无需额外库），CFArrayCreate/FSEventStreamCreate 等符号来自这两个框架。
  target_link_libraries(seriona_watcher_headers INTERFACE
    "-framework CoreFoundation"
    "-framework CoreServices"
  )
endif()

add_library(seriona_audio STATIC
  src/audio/audio_playback_service.cpp
  src/audio/buffer/pcm_buffer_queue.cpp
  src/audio/clock/playback_clock.cpp
  src/audio/device/audio_device_format_enumerator.cpp
  src/audio/device/audio_output_device.cpp
  src/audio/device/audio_device_format_enumerator.cpp
  src/audio/device/audio_device_format_caching.cpp
  src/audio/device/miniaudio_output_device_backend.cpp
  src/audio/events/audio_event_dispatcher.cpp
  src/audio/ffmpeg/ffmpeg_audio_source.cpp
  src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  src/audio/playback_state_machine.cpp
  src/audio/transition/gain_envelope.cpp
  src/audio/waveform_ffmpeg.cpp
  src/audio/waveform_generator.cpp
  src/audio/waveform_scalar.cpp
  src/audio/waveform_simd.cpp
  src/audio/waveform_simd_avx2.cpp
  src/audio/waveform_strategy_a.cpp
  src/audio/waveform_strategy_b.cpp
)

if(MSVC)
  set_source_files_properties(src/audio/waveform_simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set_source_files_properties(src/audio/waveform_simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

target_include_directories(seriona_audio
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(seriona_audio
  PUBLIC
    seriona_third_party_headers
    PkgConfig::SERIONA_FFMPEG
)

target_link_libraries(seriona_audio PRIVATE
  BS::thread_pool
  spdlog::spdlog
)

# 设备采样率/位深格式枚举的平台分支（结构同 metadata 模块）：
# Linux 用 PipeWire 原生 SPA EnumFormat（libpipewire-0.3），Windows 用
# WASAPI 格式矩阵探测（ole32）；其余平台由工厂返回空枚举器，无需源文件。
if(UNIX AND NOT APPLE)
  pkg_check_modules(SERIONA_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  target_sources(seriona_audio PRIVATE
    src/audio/device/pipewire_device_format_enumerator.cpp
  )
  target_link_libraries(seriona_audio PRIVATE PkgConfig::SERIONA_PIPEWIRE)
elseif(WIN32)
  target_sources(seriona_audio PRIVATE
    src/audio/device/windows_device_format_enumerator.cpp
  )
  target_link_libraries(seriona_audio PRIVATE ole32 ksuser)
endif()

get_target_property(SERIONA_AUDIO_LINK_LIBRARIES seriona_audio LINK_LIBRARIES)
if(NOT "BS::thread_pool" IN_LIST SERIONA_AUDIO_LINK_LIBRARIES)
  message(FATAL_ERROR "seriona_audio requires BS::thread_pool for waveform generator private thread-pool headers")
endif()

function(seriona_enable_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

if(SERIONA_SCANNER_SIMULATE_MISSING_SQLITE)
  message(FATAL_ERROR "seriona_scanner dependency gate: SQLite3 is required for scanner caching. Install externally, for example: sudo pacman -S sqlite")
endif()

find_package(SQLite3 REQUIRED)

if(TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
  add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

if(SERIONA_SCANNER_SIMULATE_MISSING_XXHASH)
  message(FATAL_ERROR "seriona_scanner dependency gate: libxxhash is required for scanner hashing. Install externally, for example: sudo pacman -S xxhash")
endif()

pkg_check_modules(SERIONA_XXHASH REQUIRED IMPORTED_TARGET libxxhash)

if(UNIX AND NOT APPLE)
  if(SERIONA_METADATA_SIMULATE_MISSING_SDBUS)
    message(FATAL_ERROR "seriona_metadata dependency gate: sdbus-c++ is required for Linux MPRIS metadata sharing. Install externally, for example: sudo pacman -S sdbus-c++")
  endif()
  pkg_check_modules(SERIONA_SDBUS_CPP REQUIRED IMPORTED_TARGET sdbus-c++)
endif()

# 优先复用已安装的 TagReaderCore（CI 顺序链：TagReader 先独立构建+全量测试并
# install 到共享前缀，后端 find_package 命中后不再重复编译）；未命中走源码嵌入链。
find_package(TagReaderCore CONFIG QUIET)
set(SERIONA_TAGREADER_INCLUDE_DIR "")
if(TARGET TagReaderCore::TagReaderCore)
  message(STATUS "Seriona backend: reusing installed TagReaderCore via find_package")
  set(SERIONA_TAGREADER_TARGET TagReaderCore::TagReaderCore)
  # IMPORTED target 的 INTERFACE_INCLUDE_DIRECTORIES 经静态库递归传播到
  # 间接链接方（测试）不可靠，scanner 直接公开该目录最稳（BUILD_INTERFACE
  # 在 install(EXPORT) 时剥离，不影响导出）。
  get_target_property(SERIONA_TAGREADER_INCLUDE_DIR TagReaderCore::TagReaderCore INTERFACE_INCLUDE_DIRECTORIES)
elseif(NOT TARGET TagReaderCore)
  if(SERIONA_TAGREADER_SOURCE_DIR STREQUAL "")
    set(SERIONA_TAGREADER_CANDIDATES
      "../TagReader"
    )
    foreach(SERIONA_TAGREADER_CANDIDATE IN LISTS SERIONA_TAGREADER_CANDIDATES)
      set(SERIONA_TAGREADER_CANDIDATE_SOURCE_DIR "${SERIONA_TAGREADER_CANDIDATE}")
      cmake_path(ABSOLUTE_PATH SERIONA_TAGREADER_CANDIDATE_SOURCE_DIR
        BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
        NORMALIZE
      )
      if(EXISTS "${SERIONA_TAGREADER_CANDIDATE_SOURCE_DIR}/CMakeLists.txt")
        set(SERIONA_TAGREADER_SOURCE_DIR "${SERIONA_TAGREADER_CANDIDATE_SOURCE_DIR}" CACHE PATH "Path to the TagReader source directory" FORCE)
        set(SERIONA_TAGREADER_BINARY_DIR "${PROJECT_BINARY_DIR}/tagreader")
        break()
      endif()
    endforeach()
    unset(SERIONA_TAGREADER_CANDIDATE_SOURCE_DIR)
    unset(SERIONA_TAGREADER_CANDIDATE)
    unset(SERIONA_TAGREADER_CANDIDATES)
    if(SERIONA_TAGREADER_SOURCE_DIR STREQUAL "")
      message(STATUS "TagReader source not found next to Seriona_Backend; fetching from https://github.com/kaizen857/TagReader.git")
      FetchContent_Populate(tagreader
        GIT_REPOSITORY https://github.com/kaizen857/TagReader.git
        GIT_TAG main
        GIT_SHALLOW TRUE
      )
      set(SERIONA_TAGREADER_SOURCE_DIR "${tagreader_SOURCE_DIR}" CACHE PATH "Path to the TagReader source directory" FORCE)
      set(SERIONA_TAGREADER_BINARY_DIR "${tagreader_BINARY_DIR}")
    endif()
  else()
    cmake_path(ABSOLUTE_PATH SERIONA_TAGREADER_SOURCE_DIR
      BASE_DIRECTORY "${PROJECT_SOURCE_DIR}"
      NORMALIZE
      OUTPUT_VARIABLE SERIONA_TAGREADER_NORMALIZED_SOURCE_DIR
    )
    set(SERIONA_TAGREADER_SOURCE_DIR "${SERIONA_TAGREADER_NORMALIZED_SOURCE_DIR}" CACHE PATH "Path to the TagReader source directory" FORCE)
    unset(SERIONA_TAGREADER_NORMALIZED_SOURCE_DIR)
    if(NOT EXISTS "${SERIONA_TAGREADER_SOURCE_DIR}/CMakeLists.txt")
      message(FATAL_ERROR "TagReaderCore is required. SERIONA_TAGREADER_SOURCE_DIR does not contain CMakeLists.txt: ${SERIONA_TAGREADER_SOURCE_DIR}")
    endif()
    set(SERIONA_TAGREADER_BINARY_DIR "${PROJECT_BINARY_DIR}/tagreader")
  endif()

  set(SERIONA_BUILD_TESTING_BEFORE_TAGREADER ${BUILD_TESTING})
  set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
  add_subdirectory("${SERIONA_TAGREADER_SOURCE_DIR}" "${SERIONA_TAGREADER_BINARY_DIR}" EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${SERIONA_BUILD_TESTING_BEFORE_TAGREADER} CACHE BOOL "Build tests" FORCE)
  unset(SERIONA_BUILD_TESTING_BEFORE_TAGREADER)
  set_property(DIRECTORY "${SERIONA_TAGREADER_BINARY_DIR}/test" PROPERTY TESTS "")
  set(SERIONA_TAGREADER_TARGET TagReaderCore)
else()
  # 已有 TagReaderCore 目标（嵌入链提前构建），沿用
  set(SERIONA_TAGREADER_TARGET TagReaderCore)
endif()

add_library(seriona_scanner STATIC
  src/scanner/file_scanner_service.cpp
  src/scanner/file_scanner_orchestrator.cpp
  src/scanner/cache/sqlite_cache.cpp
  src/scanner/cache/sqlite_cache_connection.cpp
  src/scanner/directory_tree_hash.cpp
  src/scanner/hash_utils.cpp
  src/scanner/lrc_parser.cpp
  src/scanner/path_utils.cpp
  src/scanner/playlist_tree_builder.cpp
  src/scanner/folder_thumbnail_resolver.cpp
  src/scanner/tag_reader_metadata_adapter.cpp
  src/scanner/scanner_module.cpp
  src/scanner/worker_pool.cpp
  src/logging/logging.cpp
)

add_library(seriona_metadata STATIC
  src/metadata/metadata_module.cpp
  src/metadata/metadata_mapper.cpp
  src/metadata/metadata_synchronizer.cpp
  src/metadata/metadata_service.cpp
  src/metadata/metadata_service_backend.cpp
  src/metadata/metadata_mpris.cpp
)

add_library(seriona_app STATIC
  src/app/application_logging.cpp
  src/app/runtime_paths.cpp
  src/logging/logging.cpp
)

add_library(seriona_control STATIC
  src/control/artwork_resolver.cpp
  src/control/control_event_loop.cpp
  src/control/control_state_reducer.cpp
  src/control/media_controller.cpp
  src/control/media_controller_module.cpp
  src/control/playback_context_builder.cpp
  src/control/sqlite_app_settings_store.cpp
  src/control/sqlite_folder_sort_settings_store.cpp
  src/control/subscription_store.cpp
)

if(UNIX AND NOT APPLE)
  target_sources(seriona_metadata PRIVATE
    src/metadata/metadata_mpris_backend.cpp
    src/metadata/metadata_mpris_linux.cpp
  )
endif()

if(WIN32)
  target_sources(seriona_metadata PRIVATE
    src/metadata/metadata_windows_private.cpp
  )
endif()

target_include_directories(seriona_scanner
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<BUILD_INTERFACE:${SERIONA_TAGREADER_INCLUDE_DIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_include_directories(seriona_metadata
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/metadata
)

target_include_directories(seriona_app
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(seriona_app PRIVATE
  spdlog::spdlog
)

target_include_directories(seriona_control
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(seriona_scanner
  PUBLIC
    SQLite3::SQLite3
    PkgConfig::SERIONA_XXHASH
    ${SERIONA_TAGREADER_TARGET}
    BS::thread_pool
    seriona_watcher_headers
    spdlog::spdlog
)


target_link_libraries(seriona_control
  PRIVATE
    seriona_audio
    seriona_scanner
    seriona_metadata
    SQLite3::SQLite3
    spdlog::spdlog
)

seriona_enable_warnings(seriona_audio)
seriona_enable_warnings(seriona_scanner)
seriona_enable_warnings(seriona_metadata)
seriona_enable_warnings(seriona_app)
seriona_enable_warnings(seriona_control)

target_link_libraries(seriona_metadata PRIVATE spdlog::spdlog)

if(TARGET PkgConfig::SERIONA_SDBUS_CPP)
  target_link_libraries(seriona_metadata PRIVATE PkgConfig::SERIONA_SDBUS_CPP)
endif()

add_library(SerionaBackend::app ALIAS seriona_app)
add_library(SerionaBackend::control ALIAS seriona_control)
add_library(SerionaBackend::audio ALIAS seriona_audio)
add_library(SerionaBackend::scanner ALIAS seriona_scanner)
add_library(SerionaBackend::metadata ALIAS seriona_metadata)

if(SERIONA_BUILD_APP)
  add_subdirectory(app)
endif()

if(SERIONA_INSTALLED_MODE)
  # 仅施加于生产目标（seriona_app 静态库与 seriona 可执行文件），测试目标不携带：
  # 测试依赖 resolveRuntimePaths 的便携行为，installed 分支由测试直接调用
  # resolveInstalledRuntimePaths() + 环境变量注入覆盖。
  # seriona 可执行文件在 SERIONA_BUILD_APP=OFF（前端嵌入）时不存在，需 TARGET 保护。
  target_compile_definitions(seriona_app PRIVATE SERIONA_INSTALLED_MODE=1)
  if(TARGET seriona)
    target_compile_definitions(seriona PRIVATE SERIONA_INSTALLED_MODE=1)
  endif()
endif()

if(SERIONA_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SERIONA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# 安装导出（CI 顺序链复用：Seriona 前端 find_package(SerionaBackend) 命中后不再
# add_subdirectory 后端；仅独立构建时启用 SERIONA_INSTALL_EXPORT=ON，
# 前端嵌入（add_subdirectory）时默认 OFF，不生成导出规则）
# ---------------------------------------------------------------------------
if(SERIONA_INSTALL_EXPORT)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  # 头库补 INSTALL_INTERFACE（install(EXPORT) 不允许导出构建机源码绝对路径）
  target_include_directories(seriona_third_party_headers INTERFACE
    $<INSTALL_INTERFACE:include/third_party/doctest>
    $<INSTALL_INTERFACE:include/third_party/miniaudio>
  )
  target_include_directories(seriona_watcher_headers INTERFACE
    $<INSTALL_INTERFACE:include/third_party/watcher>
  )
  target_include_directories(BS_thread_pool SYSTEM INTERFACE
    $<INSTALL_INTERFACE:include/third_party/thread_pool>
  )

  install(TARGETS seriona_audio seriona_scanner seriona_metadata seriona_app seriona_control
          seriona_third_party_headers seriona_watcher_headers BS_thread_pool
    EXPORT SerionaBackendTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(DIRECTORY inc/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY third_party/doctest third_party/miniaudio
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/third_party)
  install(DIRECTORY third_party/watcher/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/third_party/watcher)
  install(DIRECTORY "${thread_pool_SOURCE_DIR}/include/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/third_party/thread_pool)

  install(EXPORT SerionaBackendTargets
    FILE SerionaBackendTargets.cmake
    NAMESPACE SerionaBackend::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SerionaBackend
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SerionaBackendConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SerionaBackendConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SerionaBackendConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SerionaBackend
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SerionaBackendConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SerionaBackendConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SerionaBackend
  )
endif()
