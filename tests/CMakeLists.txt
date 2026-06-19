add_executable(seriona_tests main.cpp)
add_executable(seriona_audio_contract_tests
  audio/audio_contract_tests.cpp
)
add_executable(seriona_audio_fixture_tests
  audio/audio_fixture_tests.cpp
)
add_executable(seriona_ffmpeg_audio_source_tests
  audio/ffmpeg_audio_source_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
)
add_executable(seriona_playback_state_machine_tests
  audio/playback_state_machine_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)

target_compile_definitions(seriona_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_ffmpeg_audio_source_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_playback_state_machine_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
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
target_include_directories(seriona_ffmpeg_audio_source_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_playback_state_machine_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
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
target_link_libraries(seriona_ffmpeg_audio_source_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_playback_state_machine_tests PRIVATE
  seriona_third_party_headers
)

seriona_enable_warnings(seriona_tests)
seriona_enable_warnings(seriona_audio_contract_tests)
seriona_enable_warnings(seriona_audio_fixture_tests)
seriona_enable_warnings(seriona_ffmpeg_audio_source_tests)
seriona_enable_warnings(seriona_playback_state_machine_tests)

add_test(NAME seriona.doctest.placeholder COMMAND seriona_tests)
add_test(NAME seriona.audio_contract COMMAND seriona_audio_contract_tests)
add_test(NAME seriona.audio_fixture COMMAND seriona_audio_fixture_tests)
add_test(NAME seriona.ffmpeg_audio_source COMMAND seriona_ffmpeg_audio_source_tests --test-case=*decodes*)
add_test(NAME seriona.ffmpeg_audio_source_errors COMMAND seriona_ffmpeg_audio_source_tests --test-case=*maps*)
add_test(NAME seriona.ffmpeg_audio_source_seek COMMAND seriona_ffmpeg_audio_source_tests --test-case=*seek*)
add_test(NAME seriona.playback_state_machine COMMAND seriona_playback_state_machine_tests --test-case-exclude=*cancellation*)
add_test(NAME seriona.playback_state_machine_cancellation COMMAND seriona_playback_state_machine_tests --test-case=*cancellation*)
