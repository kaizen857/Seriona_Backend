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
add_executable(seriona_scanner_internal_types_tests
  scanner/scanner_internal_types_tests.cpp
)
add_executable(seriona_scanner_cue_adapter_tests
  scanner/scanner_cue_adapter_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_two_pass_discovery_tests
  scanner/scanner_two_pass_discovery_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_cue_error_isolation_tests
  scanner/scanner_cue_error_isolation_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_two_pass_integration_tests
  scanner/scanner_two_pass_integration_tests.cpp
  scanner/scanner_test_harness.cpp
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
add_executable(seriona_metadata_service_recording_tests
  metadata/metadata_service_recording_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_service_testing.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_service_recording.cpp
)
add_executable(seriona_metadata_mpris_tests
  metadata/metadata_mpris_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_mpris.cpp
  ${PROJECT_SOURCE_DIR}/src/metadata/metadata_mpris_linux.cpp
)
add_executable(seriona_control_contract_tests
  control/control_contract_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_media_controller_tests
  control/media_controller_tests.cpp
  control/control_test_harness.cpp
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
  ${PROJECT_SOURCE_DIR}/src/scanner/directory_tree_hash.cpp
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
add_executable(seriona_scanner_thread_pool_tests
  scanner/test_thread_pool_integration.cpp
)
add_executable(seriona_scanner_worker_pool_basic_tests
  scanner/scanner_worker_pool_basic_tests.cpp
)
add_executable(seriona_scanner_worker_pool_process_tests
  scanner/scanner_worker_pool_process_tests.cpp
)
add_executable(seriona_scanner_worker_pool_batch_tests
  scanner/scanner_worker_pool_batch_tests.cpp
)
add_executable(seriona_scanner_worker_pool_stats_tests
  scanner/scanner_worker_pool_stats_tests.cpp
)
add_executable(seriona_scanner_worker_pool_config_tests
  scanner/scanner_worker_pool_config_tests.cpp
)
add_executable(seriona_scanner_phase2_tsan_stress_tests
  scanner/scanner_phase2_tsan_stress_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_scan_mode_decision_tests
  scanner/scanner_scan_mode_decision_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_scanner_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/directory_tree_hash.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/hash_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/lrc_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/path_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/playlist_tree_builder.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/worker_pool.cpp
)
add_executable(seriona_scanner_incremental_plan_tests
  scanner/scanner_incremental_plan_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_scanner_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/directory_tree_hash.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/hash_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/lrc_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/path_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/playlist_tree_builder.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/worker_pool.cpp
)
add_executable(seriona_scanner_incremental_e2e_tests
  scanner/scanner_incremental_e2e_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_perf_test
  scanner/scanner_perf_test.cpp
  scanner/scanner_perf_logging.cpp
  scanner/scanner_perf_report.cpp
  scanner/scanner_perf_support.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_cache_tests
  scanner/scanner_cache_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_scanner_cache.cpp
)
add_executable(seriona_scanner_cache_v3_basic_tests
  scanner/scanner_cache_v3_basic_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_v3_content_tests
  scanner/scanner_cache_v3_content_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_v3_locations_tests
  scanner/scanner_cache_v3_locations_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_v3_auxiliary_tests
  scanner/scanner_cache_v3_auxiliary_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_migration_v2_to_v3_tests
  scanner/scanner_migration_v2_to_v3_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_backup_rollback_tests
  scanner/scanner_backup_rollback_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_phase1_integration_tests
  scanner/scanner_phase1_integration_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_v3_migration.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_schema_v3_tests
  scanner/scanner_cache_schema_v3_tests.cpp
)
target_link_libraries(seriona_scanner_cache_schema_v3_tests PRIVATE sqlite3)
add_executable(seriona_scanner_tagreader_tests
  scanner/scanner_tagreader_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
)
add_executable(seriona_scanner_tagreader_concurrency_stress_tests
  scanner/scanner_tagreader_concurrency_stress_tests.cpp
  scanner/scanner_test_harness.cpp
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
add_executable(seriona_runtime_paths_tests
  app/runtime_paths_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/app/runtime_paths.cpp
)

add_executable(seriona_logging_tests
  logging/logging_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
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
target_compile_definitions(seriona_scanner_internal_types_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cue_adapter_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_two_pass_discovery_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cue_error_isolation_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_two_pass_integration_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_mapper_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_service_recording_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_mpris_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_control_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_media_controller_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_paths_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_hash_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tree_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_scheduler_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_thread_pool_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_worker_pool_basic_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_worker_pool_process_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_worker_pool_batch_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_worker_pool_stats_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_worker_pool_config_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_phase2_tsan_stress_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_scan_mode_decision_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_incremental_plan_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_incremental_e2e_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_v3_basic_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_v3_content_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_v3_locations_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_v3_auxiliary_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_migration_v2_to_v3_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_backup_rollback_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_phase1_integration_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_schema_v3_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tagreader_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tagreader_concurrency_stress_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_watcher_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_runtime_paths_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_logging_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
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
target_include_directories(seriona_scanner_internal_types_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cue_adapter_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_two_pass_discovery_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cue_error_isolation_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_two_pass_integration_tests PRIVATE
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
target_include_directories(seriona_metadata_service_recording_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_metadata_mpris_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/src/metadata
)
target_include_directories(seriona_control_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_media_controller_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
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
target_include_directories(seriona_scanner_thread_pool_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_worker_pool_basic_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_worker_pool_process_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_worker_pool_batch_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_worker_pool_stats_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_worker_pool_config_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_phase2_tsan_stress_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_scan_mode_decision_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_incremental_plan_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_incremental_e2e_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_perf_test PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
)
target_include_directories(seriona_scanner_cache_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_v3_basic_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_v3_content_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_v3_locations_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_v3_auxiliary_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_migration_v2_to_v3_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_backup_rollback_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_phase1_integration_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_schema_v3_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_link_libraries(seriona_scanner_cache_schema_v3_tests PRIVATE sqlite3)
target_include_directories(seriona_scanner_tagreader_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_tagreader_concurrency_stress_tests PRIVATE
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
target_include_directories(seriona_runtime_paths_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_logging_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
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
  spdlog::spdlog
)
target_link_libraries(seriona_ffmpeg_filter_pipeline_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_pcm_buffer_queue_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_audio_output_device_tests PRIVATE
  seriona_third_party_headers
  spdlog::spdlog
)
target_link_libraries(seriona_playback_clock_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_playback_state_machine_tests PRIVATE
  seriona_third_party_headers
  spdlog::spdlog
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
  spdlog::spdlog
)
target_link_libraries(seriona_audio_player_small_buffer_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_output_format_negotiation_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_audio_player_seamless_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_audio_error_matrix_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_audio_shutdown_lifecycle_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_test_harness_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_scanner_contract_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_internal_types_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_scanner_cue_adapter_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_two_pass_discovery_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_cue_error_isolation_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_two_pass_integration_tests PRIVATE
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
target_link_libraries(seriona_metadata_service_recording_tests PRIVATE
  seriona_metadata
)
target_link_libraries(seriona_metadata_mpris_tests PRIVATE
  seriona_metadata
)
target_link_libraries(seriona_control_contract_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_media_controller_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)

if(UNIX AND NOT APPLE)
  target_link_libraries(seriona_metadata_mpris_tests PRIVATE PkgConfig::SERIONA_SDBUS_CPP)
endif()

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
seriona_enable_warnings(seriona_control_contract_tests)
seriona_enable_warnings(seriona_media_controller_tests)
seriona_enable_warnings(seriona_scanner_test_harness_tests)
seriona_enable_warnings(seriona_scanner_contract_tests)
seriona_enable_warnings(seriona_scanner_internal_types_tests)
seriona_enable_warnings(seriona_scanner_cue_adapter_tests)
seriona_enable_warnings(seriona_scanner_two_pass_discovery_tests)
seriona_enable_warnings(seriona_scanner_cue_error_isolation_tests)
seriona_enable_warnings(seriona_scanner_two_pass_integration_tests)
seriona_enable_warnings(seriona_metadata_contract_tests)
seriona_enable_warnings(seriona_metadata_mapper_tests)
seriona_enable_warnings(seriona_metadata_service_tests)
seriona_enable_warnings(seriona_metadata_mpris_tests)
seriona_enable_warnings(seriona_scanner_paths_tests)
seriona_enable_warnings(seriona_scanner_hash_tests)
seriona_enable_warnings(seriona_scanner_tree_tests)
seriona_enable_warnings(seriona_scanner_scheduler_tests)
seriona_enable_warnings(seriona_scanner_thread_pool_tests)
seriona_enable_warnings(seriona_scanner_worker_pool_basic_tests)
seriona_enable_warnings(seriona_scanner_worker_pool_process_tests)
seriona_enable_warnings(seriona_scanner_worker_pool_batch_tests)
seriona_enable_warnings(seriona_scanner_worker_pool_stats_tests)
seriona_enable_warnings(seriona_scanner_worker_pool_config_tests)
seriona_enable_warnings(seriona_scanner_phase2_tsan_stress_tests)
seriona_enable_warnings(seriona_scanner_scan_mode_decision_tests)
seriona_enable_warnings(seriona_scanner_incremental_plan_tests)
seriona_enable_warnings(seriona_scanner_incremental_e2e_tests)
seriona_enable_warnings(seriona_scanner_perf_test)
seriona_enable_warnings(seriona_scanner_cache_tests)
seriona_enable_warnings(seriona_scanner_cache_v3_basic_tests)
seriona_enable_warnings(seriona_scanner_cache_v3_content_tests)
seriona_enable_warnings(seriona_scanner_cache_v3_locations_tests)
seriona_enable_warnings(seriona_scanner_cache_v3_auxiliary_tests)
seriona_enable_warnings(seriona_scanner_migration_v2_to_v3_tests)
seriona_enable_warnings(seriona_scanner_backup_rollback_tests)
seriona_enable_warnings(seriona_scanner_phase1_integration_tests)
seriona_enable_warnings(seriona_scanner_tagreader_tests)
seriona_enable_warnings(seriona_scanner_service_tests)
seriona_enable_warnings(seriona_scanner_watcher_tests)
seriona_enable_warnings(seriona_runtime_paths_tests)

seriona_enable_warnings(seriona_logging_tests)

target_link_libraries(seriona_scanner_hash_tests PRIVATE
  PkgConfig::SERIONA_XXHASH
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_cache_tests PRIVATE
  SQLite3::SQLite3
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_cache_v3_basic_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_v3_content_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_v3_locations_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_v3_auxiliary_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_migration_v2_to_v3_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_backup_rollback_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_phase1_integration_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_tagreader_tests PRIVATE
  TagReaderCore
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_tagreader_concurrency_stress_tests PRIVATE
  seriona_scanner
  TagReaderCore
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_paths_tests PRIVATE
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_tree_tests PRIVATE
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_scheduler_tests PRIVATE
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_thread_pool_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_worker_pool_basic_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_worker_pool_process_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_worker_pool_batch_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_worker_pool_stats_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_worker_pool_config_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_phase2_tsan_stress_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_scan_mode_decision_tests PRIVATE
  seriona_watcher_headers
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
  TagReaderCore
  BS::thread_pool
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_incremental_plan_tests PRIVATE
  seriona_watcher_headers
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
  TagReaderCore
  BS::thread_pool
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_incremental_e2e_tests PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_perf_test PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_service_tests PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_watcher_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_runtime_paths_tests PRIVATE
  seriona_third_party_headers
)

target_link_libraries(seriona_logging_tests PRIVATE
  spdlog::spdlog
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
add_test(NAME seriona.scanner_internal_types COMMAND seriona_scanner_internal_types_tests --test-case="scanner internal node type and cue info expose stable fields")
add_test(NAME seriona.metadata_contract COMMAND seriona_metadata_contract_tests)
add_test(NAME seriona.metadata_mapper COMMAND seriona_metadata_mapper_tests)
add_test(NAME seriona.metadata_service COMMAND seriona_metadata_service_tests)
add_test(NAME seriona.metadata_service_recording COMMAND seriona_metadata_service_recording_tests)
add_test(NAME seriona.metadata_mpris COMMAND seriona_metadata_mpris_tests)
add_test(NAME seriona.metadata_mpris_smoke COMMAND seriona_metadata_mpris_tests --test-case=*smoke*)
add_test(NAME seriona.control_contract COMMAND seriona_control_contract_tests)
add_test(NAME seriona.control_controller COMMAND seriona_media_controller_tests)
add_test(NAME seriona.scanner_paths COMMAND seriona_scanner_paths_tests)
add_test(NAME seriona.scanner_hash COMMAND seriona_scanner_hash_tests)
add_test(NAME seriona.scanner.directory_tree_hash COMMAND seriona_scanner_hash_tests --test-case="scanner directory tree hash*")
add_test(NAME seriona.scanner.song_identity COMMAND seriona_scanner_hash_tests --test-case="scanner song identity*")
add_test(NAME seriona.scanner_tree COMMAND seriona_scanner_tree_tests)
add_test(NAME seriona.scanner_scheduler COMMAND seriona_scanner_scheduler_tests)
add_test(NAME seriona.scanner.thread_pool_basic COMMAND seriona_scanner_thread_pool_tests)
add_test(NAME seriona.scanner.worker_pool_basic COMMAND seriona_scanner_worker_pool_basic_tests)
add_test(NAME seriona.scanner.worker_pool_process COMMAND seriona_scanner_worker_pool_process_tests)
add_test(NAME seriona.scanner.worker_pool_batch COMMAND seriona_scanner_worker_pool_batch_tests)
add_test(NAME seriona.scanner.worker_pool_stats COMMAND seriona_scanner_worker_pool_stats_tests)
add_test(NAME seriona.scanner.worker_pool_config COMMAND seriona_scanner_worker_pool_config_tests)
add_test(NAME seriona.scanner.phase2_tsan_stress COMMAND seriona_scanner_phase2_tsan_stress_tests)
add_test(NAME seriona.scanner.scan_mode_decision COMMAND seriona_scanner_scan_mode_decision_tests)
add_test(NAME seriona.scanner.incremental_plan COMMAND seriona_scanner_incremental_plan_tests)
add_test(NAME seriona.scanner.incremental_e2e COMMAND seriona_scanner_incremental_e2e_tests)
add_test(NAME seriona.scanner_cache COMMAND seriona_scanner_cache_tests)
add_test(NAME seriona.scanner.cache_v3_basic COMMAND seriona_scanner_cache_v3_basic_tests)
add_test(NAME seriona.scanner.cache_v3_content COMMAND seriona_scanner_cache_v3_content_tests)
add_test(NAME seriona.scanner.cache_v3_locations COMMAND seriona_scanner_cache_v3_locations_tests)
add_test(NAME seriona.scanner.cache_v3_auxiliary COMMAND seriona_scanner_cache_v3_auxiliary_tests)
add_test(NAME seriona.scanner.migration_v2_to_v3 COMMAND seriona_scanner_migration_v2_to_v3_tests)
add_test(NAME seriona.scanner.migration_phase1_integration COMMAND seriona_scanner_phase1_integration_tests)
add_test(NAME seriona.scanner.backup_rollback COMMAND seriona_scanner_backup_rollback_tests)
add_test(NAME seriona.scanner_tagreader COMMAND seriona_scanner_tagreader_tests)
add_test(NAME seriona.scanner.tagreader_concurrency_stress COMMAND seriona_scanner_tagreader_concurrency_stress_tests)
add_test(NAME seriona.scanner_service COMMAND seriona_scanner_service_tests)
add_test(NAME seriona.scanner.event_ordering COMMAND seriona_scanner_service_tests --test-case="scanner service publishes worker results in discovered file order")
add_test(NAME seriona.scanner.cache_lookup COMMAND seriona_scanner_service_tests --test-case="scanner service cache lookup reuses a high-count cached root")
add_test(NAME seriona.scanner.incremental_integration COMMAND seriona_scanner_service_tests --test-case="scanner service incremental integration reuses unchanged scans only added and changed and prunes deleted")
add_test(NAME seriona.scanner.config_concurrency COMMAND seriona_scanner_service_tests --test-case=*configured*)
add_test(NAME seriona.scanner.config_serial COMMAND seriona_scanner_service_tests --test-case=*serial*fallback*)
add_test(NAME seriona.scanner.env_serial COMMAND seriona_scanner_service_tests --test-case=*disable*concurrency*)
add_test(NAME seriona.scanner.env_malformed COMMAND seriona_scanner_service_tests --test-case=*malformed*env*)
add_test(NAME seriona.scanner.config_scan_mode COMMAND seriona_scanner_service_tests --test-case=*force*full*)
add_test(NAME seriona.scanner_watcher COMMAND seriona_scanner_watcher_tests)
add_test(NAME seriona.scanner.cue_adapter COMMAND seriona_scanner_cue_adapter_tests)
add_test(NAME seriona.scanner.two_pass_discovery COMMAND seriona_scanner_two_pass_discovery_tests)
add_test(NAME seriona.scanner.cue_error_isolation COMMAND seriona_scanner_cue_error_isolation_tests)
add_test(NAME seriona.scanner.two_pass_integration COMMAND seriona_scanner_two_pass_integration_tests)

add_executable(seriona_scanner_cue_parsing_tests
  scanner/scanner_cue_parsing_tests.cpp
  scanner/scanner_test_harness.cpp
)
target_compile_definitions(seriona_scanner_cue_parsing_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_include_directories(seriona_scanner_cue_parsing_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_link_libraries(seriona_scanner_cue_parsing_tests PRIVATE
  seriona_scanner
)
seriona_enable_warnings(seriona_scanner_cue_parsing_tests)
add_test(NAME seriona.scanner.cue_parsing COMMAND seriona_scanner_cue_parsing_tests)

add_test(NAME seriona.logging COMMAND seriona_logging_tests)
add_test(NAME seriona.runtime_paths COMMAND seriona_runtime_paths_tests)
