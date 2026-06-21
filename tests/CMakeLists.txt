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
add_executable(seriona_ffmpeg_filter_pipeline_tests
  audio/ffmpeg_filter_pipeline_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
)
add_executable(seriona_pcm_buffer_queue_tests
  audio/pcm_buffer_queue_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
)
add_executable(seriona_audio_output_device_tests
  audio/audio_output_device_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
)
add_executable(seriona_playback_clock_tests
  audio/playback_clock_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
)
add_executable(seriona_playback_state_machine_tests
  audio/playback_state_machine_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_audio_event_dispatcher_tests
  audio/audio_event_dispatcher_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
)
add_executable(seriona_audio_event_dispatcher_shutdown_tests
  audio/audio_event_dispatcher_shutdown_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
)
add_executable(seriona_audio_player_single_track_tests
  audio/audio_player_single_track_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_audio_player_small_buffer_tests
  audio/audio_player_small_buffer_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_output_format_negotiation_tests
  audio/output_format_negotiation_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_audio_player_seamless_tests
  audio/audio_player_seamless_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_audio_error_matrix_tests
  audio/audio_error_matrix_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_audio_shutdown_lifecycle_tests
  audio/audio_shutdown_lifecycle_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
add_executable(seriona_scanner_test_harness_tests
  scanner/scanner_test_harness_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_contract_tests
  scanner/scanner_contract_tests.cpp
)
add_executable(seriona_metadata_contract_tests
  metadata/metadata_contract_tests.cpp
)
add_executable(seriona_metadata_mapper_tests
  metadata/metadata_mapper_tests.cpp
)
add_executable(seriona_metadata_service_tests
  metadata/metadata_service_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_service_testing.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_service_recording.cpp
)
add_executable(seriona_metadata_mpris_tests
  metadata/metadata_mpris_tests.cpp
)
add_executable(seriona_scanner_paths_tests
  scanner/scanner_paths_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/lrc_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/path_utils.cpp
)
add_executable(seriona_scanner_hash_tests
  scanner/scanner_hash_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/hash_utils.cpp
)
add_executable(seriona_scanner_tree_tests
  scanner/scanner_tree_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/playlist_tree_builder.cpp
)
add_executable(seriona_scanner_scheduler_tests
  scanner/scanner_scheduler_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/scan_scheduler.cpp
)
add_executable(seriona_scanner_cache_tests
  scanner/scanner_cache_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_scanner_cache.cpp
)
add_executable(seriona_scanner_tagreader_tests
  scanner/scanner_tagreader_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
)
add_executable(seriona_scanner_service_tests
  scanner/scanner_service_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_watcher_tests
  scanner/scanner_watcher_tests.cpp
  scanner/scanner_test_harness.cpp
)

target_compile_definitions(seriona_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_ffmpeg_audio_source_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_ffmpeg_filter_pipeline_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_pcm_buffer_queue_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_output_device_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_playback_clock_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_playback_state_machine_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_event_dispatcher_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_event_dispatcher_shutdown_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_player_single_track_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_player_small_buffer_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_output_format_negotiation_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_player_seamless_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_error_matrix_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_shutdown_lifecycle_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_test_harness_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_mapper_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_mpris_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_paths_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_hash_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tree_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_scheduler_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tagreader_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_watcher_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
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
target_include_directories(seriona_ffmpeg_filter_pipeline_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_pcm_buffer_queue_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_audio_output_device_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_playback_clock_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_playback_state_machine_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_audio_event_dispatcher_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_audio_event_dispatcher_shutdown_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_audio_player_single_track_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_player_small_buffer_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_output_format_negotiation_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_player_seamless_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_error_matrix_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_shutdown_lifecycle_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_scanner_test_harness_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_metadata_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_metadata_mapper_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_metadata_service_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_metadata_mpris_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_scanner_paths_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_hash_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_tree_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_scheduler_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_tagreader_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_service_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_watcher_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
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
target_link_libraries(seriona_ffmpeg_filter_pipeline_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_pcm_buffer_queue_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_audio_output_device_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_playback_clock_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_playback_state_machine_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_audio_event_dispatcher_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_audio_event_dispatcher_shutdown_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_audio_player_single_track_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_player_small_buffer_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_output_format_negotiation_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_player_seamless_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_error_matrix_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_audio_shutdown_lifecycle_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)
target_link_libraries(seriona_scanner_test_harness_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_scanner_contract_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_metadata_contract_tests PRIVATE
  seriona_metadata
)
target_link_libraries(seriona_metadata_mapper_tests PRIVATE
  seriona_metadata
)
target_link_libraries(seriona_metadata_service_tests PRIVATE
  seriona_metadata
)
target_link_libraries(seriona_metadata_mpris_tests PRIVATE
  seriona_metadata
)

seriona_enable_warnings(seriona_tests)
seriona_enable_warnings(seriona_audio_contract_tests)
seriona_enable_warnings(seriona_audio_fixture_tests)
seriona_enable_warnings(seriona_ffmpeg_audio_source_tests)
seriona_enable_warnings(seriona_ffmpeg_filter_pipeline_tests)
seriona_enable_warnings(seriona_pcm_buffer_queue_tests)
seriona_enable_warnings(seriona_audio_output_device_tests)
seriona_enable_warnings(seriona_playback_clock_tests)
seriona_enable_warnings(seriona_playback_state_machine_tests)
seriona_enable_warnings(seriona_audio_event_dispatcher_tests)
seriona_enable_warnings(seriona_audio_event_dispatcher_shutdown_tests)
seriona_enable_warnings(seriona_audio_player_single_track_tests)
seriona_enable_warnings(seriona_audio_player_small_buffer_tests)
seriona_enable_warnings(seriona_output_format_negotiation_tests)
seriona_enable_warnings(seriona_audio_player_seamless_tests)
seriona_enable_warnings(seriona_audio_error_matrix_tests)
seriona_enable_warnings(seriona_audio_shutdown_lifecycle_tests)
seriona_enable_warnings(seriona_scanner_test_harness_tests)
seriona_enable_warnings(seriona_scanner_contract_tests)
seriona_enable_warnings(seriona_metadata_contract_tests)
seriona_enable_warnings(seriona_metadata_mapper_tests)
seriona_enable_warnings(seriona_metadata_service_tests)
seriona_enable_warnings(seriona_metadata_mpris_tests)
seriona_enable_warnings(seriona_scanner_paths_tests)
seriona_enable_warnings(seriona_scanner_hash_tests)
seriona_enable_warnings(seriona_scanner_tree_tests)
seriona_enable_warnings(seriona_scanner_scheduler_tests)
seriona_enable_warnings(seriona_scanner_cache_tests)
seriona_enable_warnings(seriona_scanner_tagreader_tests)
seriona_enable_warnings(seriona_scanner_service_tests)
seriona_enable_warnings(seriona_scanner_watcher_tests)

target_link_libraries(seriona_scanner_hash_tests PRIVATE
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_tests PRIVATE
  SQLite3::SQLite3
)
target_link_libraries(seriona_scanner_tagreader_tests PRIVATE
  TagReaderCore
)
target_link_libraries(seriona_scanner_service_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_watcher_tests PRIVATE
  seriona_scanner
)

add_test(NAME seriona.doctest.placeholder COMMAND seriona_tests)
add_test(NAME seriona.audio_contract COMMAND seriona_audio_contract_tests)
add_test(NAME seriona.audio_fixture COMMAND seriona_audio_fixture_tests)
add_test(NAME seriona.ffmpeg_audio_source COMMAND seriona_ffmpeg_audio_source_tests --test-case=*decodes*)
add_test(NAME seriona.ffmpeg_audio_source_errors COMMAND seriona_ffmpeg_audio_source_tests --test-case=*maps*)
add_test(NAME seriona.ffmpeg_audio_source_seek COMMAND seriona_ffmpeg_audio_source_tests --test-case=*seek*)
add_test(NAME seriona.ffmpeg_filter_pipeline COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*converts*)
add_test(NAME seriona.ffmpeg_filter_pipeline_drain COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*drains*)
add_test(NAME seriona.ffmpeg_filter_pipeline_errors COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*invalid*)
add_test(NAME seriona.pcm_buffer_queue COMMAND seriona_pcm_buffer_queue_tests)
add_test(NAME seriona.audio_output_device COMMAND seriona_audio_output_device_tests)
add_test(NAME seriona.playback_clock COMMAND seriona_playback_clock_tests)
add_test(NAME seriona.playback_state_machine COMMAND seriona_playback_state_machine_tests --test-case-exclude=*cancellation*)
add_test(NAME seriona.playback_state_machine_cancellation COMMAND seriona_playback_state_machine_tests --test-case=*cancellation*)
add_test(NAME seriona.audio_event_dispatcher COMMAND seriona_audio_event_dispatcher_tests)
add_test(NAME seriona.audio_event_dispatcher_shutdown COMMAND seriona_audio_event_dispatcher_shutdown_tests)
add_test(NAME seriona.audio_player_single_track COMMAND seriona_audio_player_single_track_tests)
add_test(NAME seriona.audio_player_small_buffer COMMAND seriona_audio_player_small_buffer_tests)
add_test(NAME seriona.output_format_negotiation COMMAND seriona_output_format_negotiation_tests --test-case=*covers*)
add_test(NAME seriona.output_format_negotiation_failure COMMAND seriona_output_format_negotiation_tests --test-case=*failure*)
add_test(NAME seriona.audio_player_seamless_mix COMMAND seriona_audio_player_seamless_tests --test-case=*seamless_mix*)
add_test(NAME seriona.audio_player_direct_preload COMMAND seriona_audio_player_seamless_tests --test-case=*direct_preload*)
add_test(NAME seriona.audio_player_preload_failure COMMAND seriona_audio_player_seamless_tests --test-case=*preload_failure*)
add_test(NAME seriona.audio_error_matrix COMMAND seriona_audio_error_matrix_tests)
add_test(NAME seriona.audio_shutdown_lifecycle COMMAND seriona_audio_shutdown_lifecycle_tests)
add_test(NAME seriona.scanner_test_harness COMMAND seriona_scanner_test_harness_tests)
add_test(NAME seriona.scanner_contract COMMAND seriona_scanner_contract_tests)
add_test(NAME seriona.metadata_contract COMMAND seriona_metadata_contract_tests)
add_test(NAME seriona.metadata_mapper COMMAND seriona_metadata_mapper_tests)
add_test(NAME seriona.metadata_service COMMAND seriona_metadata_service_tests)
add_test(NAME seriona.metadata_mpris COMMAND seriona_metadata_mpris_tests)
add_test(NAME seriona.metadata_mpris_smoke COMMAND seriona_metadata_mpris_tests --test-case=*smoke*)
add_test(NAME seriona.scanner_paths COMMAND seriona_scanner_paths_tests)
add_test(NAME seriona.scanner_hash COMMAND seriona_scanner_hash_tests)
add_test(NAME seriona.scanner_tree COMMAND seriona_scanner_tree_tests)
add_test(NAME seriona.scanner_scheduler COMMAND seriona_scanner_scheduler_tests)
add_test(NAME seriona.scanner_cache COMMAND seriona_scanner_cache_tests)
add_test(NAME seriona.scanner_tagreader COMMAND seriona_scanner_tagreader_tests)
add_test(NAME seriona.scanner_service COMMAND seriona_scanner_service_tests)
add_test(NAME seriona.scanner_watcher COMMAND seriona_scanner_watcher_tests)
