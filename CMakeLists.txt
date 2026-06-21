cmake_minimum_required(VERSION 3.20)

project(Seriona VERSION 0.1.0 LANGUAGES CXX)

option(SERIONA_BUILD_TESTS "Build Seriona tests" ON)
option(SERIONA_BUILD_TOOLS "Build optional Seriona tools" OFF)
option(SERIONA_SCANNER_SIMULATE_MISSING_SQLITE "Simulate missing scanner SQLite3 dependency" OFF)
option(SERIONA_SCANNER_SIMULATE_MISSING_XXHASH "Simulate missing scanner libxxhash dependency" OFF)
option(SERIONA_METADATA_SIMULATE_MISSING_SDBUS "Simulate missing metadata sdbus-c++ dependency" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SERIONA_FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libavfilter
  libswresample
)

add_library(seriona_third_party_headers INTERFACE)
target_include_directories(seriona_third_party_headers INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/doctest>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/miniaudio>
)

add_library(seriona_watcher_headers INTERFACE)
target_include_directories(seriona_watcher_headers INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/watcher/include>
)

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

if(NOT TARGET TagReaderCore)
  set(SERIONA_BUILD_TESTING_BEFORE_TAGREADER ${BUILD_TESTING})
  set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
  add_subdirectory("/home/kaizen857/cppProject(app_and_lib)/TagReader" "${PROJECT_BINARY_DIR}/tagreader" EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${SERIONA_BUILD_TESTING_BEFORE_TAGREADER} CACHE BOOL "Build tests" FORCE)
  unset(SERIONA_BUILD_TESTING_BEFORE_TAGREADER)
  set_property(DIRECTORY "${PROJECT_BINARY_DIR}/tagreader/test" PROPERTY TESTS "")
endif()

add_library(seriona_scanner STATIC
  src/scanner/file_scanner_service.cpp
  src/scanner/file_scanner_orchestrator.cpp
  src/scanner/cache/sqlite_scanner_cache.cpp
  src/scanner/hash_utils.cpp
  src/scanner/lrc_parser.cpp
  src/scanner/path_utils.cpp
  src/scanner/playlist_tree_builder.cpp
  src/scanner/tag_reader_metadata_adapter.cpp
  src/scanner/scanner_module.cpp
)

add_library(seriona_metadata STATIC
  src/metadata/metadata_module.cpp
  src/metadata/metadata_mapper.cpp
  src/metadata/metadata_synchronizer.cpp
  src/metadata/metadata_service.cpp
  src/metadata/metadata_service_backend.cpp
  src/metadata/metadata_mpris.cpp
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
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_include_directories(seriona_metadata
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/metadata
)

target_link_libraries(seriona_scanner
  PRIVATE
    SQLite3::SQLite3
    PkgConfig::SERIONA_XXHASH
    TagReaderCore
    seriona_watcher_headers
)

seriona_enable_warnings(seriona_scanner)
seriona_enable_warnings(seriona_metadata)

if(TARGET PkgConfig::SERIONA_SDBUS_CPP)
  target_link_libraries(seriona_metadata PRIVATE PkgConfig::SERIONA_SDBUS_CPP)
endif()

add_subdirectory(app)

if(SERIONA_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SERIONA_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
