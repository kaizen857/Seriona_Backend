add_executable(seriona_tests main.cpp)
add_executable(seriona_audio_contract_tests
  audio/audio_contract_tests.cpp
)
add_executable(seriona_audio_fixture_tests
  audio/audio_fixture_tests.cpp
)

target_compile_definitions(seriona_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_include_directories(seriona_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_fixture_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_link_libraries(seriona_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_contract_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_fixture_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)

seriona_enable_warnings(seriona_tests)
seriona_enable_warnings(seriona_audio_contract_tests)
seriona_enable_warnings(seriona_audio_fixture_tests)

add_test(NAME seriona.doctest.placeholder COMMAND seriona_tests)
add_test(NAME seriona.audio_contract COMMAND seriona_audio_contract_tests)
add_test(NAME seriona.audio_fixture COMMAND seriona_audio_fixture_tests)
