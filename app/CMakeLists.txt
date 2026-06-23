add_executable(seriona
  main.cpp
  terminal_controller.cpp
  terminal_io.cpp
)

set_target_properties(seriona PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)

target_include_directories(seriona PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
)

target_link_libraries(seriona PRIVATE
  seriona_control
)

seriona_enable_warnings(seriona)
