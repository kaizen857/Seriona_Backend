cmake_minimum_required(VERSION 3.20)

project(Seriona VERSION 0.1.0 LANGUAGES CXX)

option(SERIONA_BUILD_TESTS "Build Seriona tests" ON)

set(CMAKE_CXX_STANDARD 20)
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

function(seriona_enable_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

if(SERIONA_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
