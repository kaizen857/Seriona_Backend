add_executable(seriona
  main.cpp
  terminal_controller.cpp
  terminal_io.cpp
  ${PROJECT_SOURCE_DIR}/src/app/runtime_paths.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
)

set_target_properties(seriona PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)

target_include_directories(seriona PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(seriona PRIVATE
  seriona_control
  $<$<CONFIG:Release>:PkgConfig::SERIONA_FFMPEG>
)

seriona_enable_warnings(seriona)
