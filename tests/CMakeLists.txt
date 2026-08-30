# TagReader 公共头（find_package 模式）：IMPORTED target 的 include 经
# seriona_scanner 静态库递归传播到测试 target 不可靠（CMake 4.x 行为），
# 这里目录级全局补充；add_subdirectory 模式该变量为空，跳过。
if(SERIONA_TAGREADER_INCLUDE_DIR)
  include_directories(SYSTEM "${SERIONA_TAGREADER_INCLUDE_DIR}")
endif()

add_executable(seriona_tests main.cpp)
add_executable(seriona_audio_contract_tests
  audio/audio_contract_tests.cpp
)
add_executable(seriona_audio_fixture_tests
  audio/audio_fixture_tests.cpp
)
add_executable(seriona_waveform_generator_tests
  audio/waveform_generator_tests.cpp
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
add_executable(seriona_audio_enumerate_devices_tests
  audio/audio_enumerate_playback_devices_tests.cpp
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
add_executable(seriona_audio_device_format_enumerator_tests
  audio/device_format_enumerator_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_device_format_caching.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)
# PipeWire 真实枚举测试：仅平台条件注册（参照 metadata_mpris_tests 守卫），
# 无 PipeWire 守护进程时测试体 SUCCEED 跳过。
if(UNIX AND NOT APPLE)
  add_executable(seriona_audio_pipewire_device_enumerator_tests
    audio/pipewire_device_enumerator_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/device/audio_device_format_enumerator.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/device/audio_device_format_caching.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/device/pipewire_device_format_enumerator.cpp
  )
endif()
add_executable(seriona_miniaudio_device_capabilities_tests
  audio/miniaudio_device_capabilities_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
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
add_executable(seriona_scanner_preallocate_tests
  scanner/scanner_preallocate_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_cue_nodes_tests
  scanner/scanner_cue_nodes_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_playlist_cue_tests
  scanner/scanner_playlist_cue_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_complete_e2e_tests
  scanner/scanner_complete_e2e_tests.cpp
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
if(UNIX AND NOT APPLE)
  add_executable(seriona_metadata_mpris_tests
    metadata/metadata_mpris_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/metadata/metadata_mpris.cpp
    ${PROJECT_SOURCE_DIR}/src/metadata/metadata_mpris_linux.cpp
  )
endif()
add_executable(seriona_control_contract_tests
  control/control_contract_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_configure_output_command_tests
  control/configure_output_command_tests.cpp
)
add_executable(seriona_configure_output_reducer_tests
  control/configure_output_reducer_tests.cpp
)
add_executable(seriona_configure_output_executes_tests
  control/configure_output_executes_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_media_controller_tests
  control/media_controller_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_control_folder_sort_settings_store_tests
  control/folder_sort_settings_store_tests.cpp
)
add_executable(seriona_control_app_settings_store_tests
  control/app_settings_store_tests.cpp
)
add_executable(seriona_control_persistence_guardrail_tests
  control/persistence_guardrail_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_control_playback_context_builder_tests
  control/playback_context_builder_tests.cpp
)
add_executable(seriona_artwork_resolver_tests
  control/artwork_resolver_tests.cpp
)
add_executable(seriona_media_controller_artwork_shutdown_tests
  control/media_controller_artwork_shutdown_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_terminal_controller_tests
  app/terminal_controller_tests.cpp
  control/control_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/app/terminal_controller.cpp
)
add_executable(seriona_delete_commands_tests
  control/delete_commands_executes_tests.cpp
  control/control_test_harness.cpp
)
add_executable(seriona_track_switch_snapshot_reducer_tests
  control/track_switch_snapshot_reducer_tests.cpp
)
add_executable(seriona_play_queue_reducer_tests
  control/play_queue_reducer_tests.cpp
)
add_executable(seriona_scanner_remove_location_tests
  scanner/remove_location_tests.cpp
  scanner/scanner_test_harness.cpp
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
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/directory_tree_hash.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/hash_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/lrc_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/path_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/playlist_tree_builder.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/folder_thumbnail_resolver.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/worker_pool.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
)
add_executable(seriona_scanner_incremental_plan_tests
  scanner/scanner_incremental_plan_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/directory_tree_hash.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/hash_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/lrc_parser.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/path_utils.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/playlist_tree_builder.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/folder_thumbnail_resolver.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/tag_reader_metadata_adapter.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/worker_pool.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
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
add_executable(seriona_scanner_detailed_perf_test
  scanner/scanner_detailed_perf_test.cpp
  scanner/scanner_perf_logging.cpp
  scanner/scanner_perf_report.cpp
  scanner/scanner_perf_support.cpp
  scanner/scanner_test_harness.cpp
)
# DISABLED
# add_executable(seriona_scanner_cache_tests
#   scanner/scanner_cache_tests.cpp
#   scanner/scanner_test_harness.cpp
# )
add_executable(seriona_scanner_cache_basic_tests
  scanner/scanner_cache_basic_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
# DISABLED
# add_executable(seriona_scanner_cache_content_tests
#   scanner/scanner_cache_content_tests.cpp
#   scanner/scanner_test_harness.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
# )
add_executable(seriona_scanner_cache_locations_tests
  scanner/scanner_cache_locations_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_auxiliary_tests
  scanner/scanner_cache_auxiliary_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
add_executable(seriona_scanner_cache_perf_tests
  scanner/scanner_cache_perf_tests.cpp
  scanner/scanner_test_harness.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
  ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
)
# DISABLED
# add_executable(seriona_scanner_migration_v2_to_v3_tests
#   scanner/scanner_migration_v2_to_v3_tests.cpp
#   scanner/scanner_test_harness.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
# )
# DISABLED
# add_executable(seriona_scanner_backup_rollback_tests
#   scanner/scanner_backup_rollback_tests.cpp
#   scanner/scanner_test_harness.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
# )
# DISABLED
# add_executable(seriona_scanner_phase1_integration_tests
#   scanner/scanner_phase1_integration_tests.cpp
#   scanner/scanner_test_harness.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/cache/sqlite_cache_connection.cpp
#   ${PROJECT_SOURCE_DIR}/src/scanner/song_identity.cpp
# )
add_executable(seriona_scanner_cache_schema_tests
  scanner/scanner_cache_schema_tests.cpp
)
target_link_libraries(seriona_scanner_cache_schema_tests PRIVATE SQLite3::SQLite3)
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
add_executable(seriona_scanner_wtr_integration_tests
  scanner/scanner_wtr_integration_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_lazy_cover_gateway_tests
  scanner/scanner_lazy_cover_gateway_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_scanner_error_logging_tests
  scanner/scanner_error_logging_tests.cpp
  scanner/scanner_test_harness.cpp
)
add_executable(seriona_runtime_paths_tests
  app/runtime_paths_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/app/runtime_paths.cpp
)
add_executable(seriona_application_logging_tests
  app/application_logging_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/app/application_logging.cpp
  ${PROJECT_SOURCE_DIR}/src/app/runtime_paths.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
)

add_executable(seriona_logging_tests
  logging/logging_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
)

add_executable(seriona_application_log_level_tests
  app/application_log_level_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/app/application_logging.cpp
  ${PROJECT_SOURCE_DIR}/src/app/runtime_paths.cpp
  ${PROJECT_SOURCE_DIR}/src/logging/logging.cpp
)

target_compile_definitions(seriona_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_waveform_generator_tests PRIVATE
  DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
  SERIONA_WAVEFORM_FIXTURE_DIR="${CMAKE_CURRENT_BINARY_DIR}/generated_waveform_fixtures"
)
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
target_compile_definitions(seriona_audio_enumerate_devices_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_audio_device_format_enumerator_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
if(TARGET seriona_audio_pipewire_device_enumerator_tests)
target_compile_definitions(seriona_audio_pipewire_device_enumerator_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
endif()
target_compile_definitions(seriona_miniaudio_device_capabilities_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_test_harness_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_internal_types_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cue_adapter_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_two_pass_discovery_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cue_error_isolation_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_two_pass_integration_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_preallocate_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cue_nodes_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_playlist_cue_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_complete_e2e_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_mapper_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_metadata_service_recording_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
if(TARGET seriona_metadata_mpris_tests)
  target_compile_definitions(seriona_metadata_mpris_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
endif()
target_compile_definitions(seriona_control_contract_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_configure_output_command_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_configure_output_reducer_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_configure_output_executes_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_media_controller_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_control_folder_sort_settings_store_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_control_app_settings_store_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_control_persistence_guardrail_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_terminal_controller_tests PRIVATE
  DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
  SERIONA_TERMINAL_CONTROLLER_TESTING
)
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
# target_compile_definitions(seriona_scanner_cache_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_basic_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
# target_compile_definitions(seriona_scanner_cache_content_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_locations_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_auxiliary_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_perf_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
# target_compile_definitions(seriona_scanner_migration_v2_to_v3_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
# target_compile_definitions(seriona_scanner_backup_rollback_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
# target_compile_definitions(seriona_scanner_phase1_integration_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_cache_schema_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tagreader_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_tagreader_concurrency_stress_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_service_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_watcher_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_wtr_integration_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_lazy_cover_gateway_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_error_logging_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_runtime_paths_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_application_logging_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_logging_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_application_log_level_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_control_playback_context_builder_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_artwork_resolver_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_media_controller_artwork_shutdown_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_delete_commands_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_track_switch_snapshot_reducer_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_play_queue_reducer_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_compile_definitions(seriona_scanner_remove_location_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
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
target_include_directories(seriona_audio_enumerate_devices_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
target_include_directories(seriona_audio_device_format_enumerator_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
  ${PROJECT_SOURCE_DIR}/third_party/miniaudio
)
if(TARGET seriona_audio_pipewire_device_enumerator_tests)
target_include_directories(seriona_audio_pipewire_device_enumerator_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
endif()
target_include_directories(seriona_miniaudio_device_capabilities_tests PRIVATE
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
target_include_directories(seriona_scanner_preallocate_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cue_nodes_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_playlist_cue_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_complete_e2e_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
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
if(TARGET seriona_metadata_mpris_tests)
  target_include_directories(seriona_metadata_mpris_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/inc
    ${PROJECT_SOURCE_DIR}/third_party/doctest
    ${PROJECT_SOURCE_DIR}/src/metadata
  )
endif()
target_include_directories(seriona_control_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_configure_output_command_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_configure_output_reducer_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_configure_output_executes_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_media_controller_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_control_folder_sort_settings_store_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_control_app_settings_store_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_control_persistence_guardrail_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_control_playback_context_builder_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_artwork_resolver_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_media_controller_artwork_shutdown_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_delete_commands_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_track_switch_snapshot_reducer_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_play_queue_reducer_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_remove_location_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
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
target_include_directories(seriona_scanner_detailed_perf_test PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
)
# target_include_directories(seriona_scanner_cache_tests PRIVATE
#   ${PROJECT_SOURCE_DIR}/inc
#   ${PROJECT_SOURCE_DIR}/third_party/doctest
# )
target_include_directories(seriona_scanner_cache_basic_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
# target_include_directories(seriona_scanner_cache_content_tests PRIVATE
#   ${PROJECT_SOURCE_DIR}/inc
#   ${PROJECT_SOURCE_DIR}/third_party/doctest
# )
target_include_directories(seriona_scanner_cache_locations_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_auxiliary_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_cache_perf_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
# target_include_directories(seriona_scanner_migration_v2_to_v3_tests PRIVATE
#   ${PROJECT_SOURCE_DIR}/inc
#   ${PROJECT_SOURCE_DIR}/third_party/doctest
# )
# target_include_directories(seriona_scanner_backup_rollback_tests PRIVATE
#   ${PROJECT_SOURCE_DIR}/inc
#   ${PROJECT_SOURCE_DIR}/third_party/doctest
# )
# target_include_directories(seriona_scanner_phase1_integration_tests PRIVATE
#   ${PROJECT_SOURCE_DIR}/inc
#   ${PROJECT_SOURCE_DIR}/third_party/doctest
# )
target_include_directories(seriona_scanner_cache_schema_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
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
target_include_directories(seriona_scanner_wtr_integration_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_lazy_cover_gateway_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_scanner_error_logging_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_runtime_paths_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_application_logging_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_logging_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_application_log_level_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_include_directories(seriona_terminal_controller_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/app
  ${PROJECT_SOURCE_DIR}/inc
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
target_link_libraries(seriona_waveform_generator_tests PRIVATE
  seriona_audio
  seriona_third_party_headers
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
target_link_libraries(seriona_audio_enumerate_devices_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
target_link_libraries(seriona_audio_device_format_enumerator_tests PRIVATE
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
  spdlog::spdlog
)
if(TARGET seriona_audio_pipewire_device_enumerator_tests)
target_link_libraries(seriona_audio_pipewire_device_enumerator_tests PRIVATE
  PkgConfig::SERIONA_PIPEWIRE
  spdlog::spdlog
)
endif()
target_link_libraries(seriona_miniaudio_device_capabilities_tests PRIVATE
  seriona_third_party_headers
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
target_link_libraries(seriona_scanner_preallocate_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_cue_nodes_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_playlist_cue_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_scanner_complete_e2e_tests PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
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
if(TARGET seriona_metadata_mpris_tests)
  target_link_libraries(seriona_metadata_mpris_tests PRIVATE
    seriona_metadata
  )
endif()
target_link_libraries(seriona_control_contract_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_configure_output_command_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_configure_output_reducer_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_configure_output_executes_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_media_controller_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_control_folder_sort_settings_store_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
  SQLite3::SQLite3
)
target_link_libraries(seriona_control_app_settings_store_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
  SQLite3::SQLite3
)
target_link_libraries(seriona_control_persistence_guardrail_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
  SQLite3::SQLite3
)
target_link_libraries(seriona_control_playback_context_builder_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_artwork_resolver_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
)
target_link_libraries(seriona_media_controller_artwork_shutdown_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
)
target_link_libraries(seriona_delete_commands_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_track_switch_snapshot_reducer_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_play_queue_reducer_tests PRIVATE
  seriona_third_party_headers
  seriona_control
)
target_link_libraries(seriona_scanner_remove_location_tests PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_terminal_controller_tests PRIVATE
  seriona_third_party_headers
  seriona_control
  spdlog::spdlog
)

if(TARGET seriona_metadata_mpris_tests)
  target_link_libraries(seriona_metadata_mpris_tests PRIVATE PkgConfig::SERIONA_SDBUS_CPP)
endif()

seriona_enable_warnings(seriona_tests)
seriona_enable_warnings(seriona_audio_contract_tests)
seriona_enable_warnings(seriona_audio_fixture_tests)
seriona_enable_warnings(seriona_waveform_generator_tests)
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
seriona_enable_warnings(seriona_audio_enumerate_devices_tests)
seriona_enable_warnings(seriona_audio_device_format_enumerator_tests)
if(TARGET seriona_audio_pipewire_device_enumerator_tests)
seriona_enable_warnings(seriona_audio_pipewire_device_enumerator_tests)
endif()
seriona_enable_warnings(seriona_miniaudio_device_capabilities_tests)
seriona_enable_warnings(seriona_control_contract_tests)
seriona_enable_warnings(seriona_configure_output_command_tests)
seriona_enable_warnings(seriona_configure_output_reducer_tests)
seriona_enable_warnings(seriona_configure_output_executes_tests)
seriona_enable_warnings(seriona_media_controller_tests)
seriona_enable_warnings(seriona_control_folder_sort_settings_store_tests)
seriona_enable_warnings(seriona_control_app_settings_store_tests)
seriona_enable_warnings(seriona_control_persistence_guardrail_tests)
seriona_enable_warnings(seriona_control_playback_context_builder_tests)
seriona_enable_warnings(seriona_artwork_resolver_tests)
seriona_enable_warnings(seriona_media_controller_artwork_shutdown_tests)
seriona_enable_warnings(seriona_delete_commands_tests)
seriona_enable_warnings(seriona_track_switch_snapshot_reducer_tests)
seriona_enable_warnings(seriona_play_queue_reducer_tests)
seriona_enable_warnings(seriona_scanner_remove_location_tests)
seriona_enable_warnings(seriona_terminal_controller_tests)
seriona_enable_warnings(seriona_scanner_test_harness_tests)
seriona_enable_warnings(seriona_scanner_contract_tests)
seriona_enable_warnings(seriona_scanner_internal_types_tests)
seriona_enable_warnings(seriona_scanner_cue_adapter_tests)
seriona_enable_warnings(seriona_scanner_two_pass_discovery_tests)
seriona_enable_warnings(seriona_scanner_cue_error_isolation_tests)
seriona_enable_warnings(seriona_scanner_two_pass_integration_tests)
seriona_enable_warnings(seriona_scanner_preallocate_tests)
seriona_enable_warnings(seriona_scanner_cue_nodes_tests)
seriona_enable_warnings(seriona_scanner_playlist_cue_tests)
seriona_enable_warnings(seriona_scanner_complete_e2e_tests)
seriona_enable_warnings(seriona_metadata_contract_tests)
seriona_enable_warnings(seriona_metadata_mapper_tests)
seriona_enable_warnings(seriona_metadata_service_tests)
if(TARGET seriona_metadata_mpris_tests)
  seriona_enable_warnings(seriona_metadata_mpris_tests)
endif()
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
seriona_enable_warnings(seriona_scanner_detailed_perf_test)
# seriona_enable_warnings(seriona_scanner_cache_tests)
seriona_enable_warnings(seriona_scanner_cache_basic_tests)
# seriona_enable_warnings(seriona_scanner_cache_content_tests)
seriona_enable_warnings(seriona_scanner_cache_locations_tests)
seriona_enable_warnings(seriona_scanner_cache_auxiliary_tests)
seriona_enable_warnings(seriona_scanner_cache_perf_tests)
# seriona_enable_warnings(seriona_scanner_migration_v2_to_v3_tests)
# seriona_enable_warnings(seriona_scanner_backup_rollback_tests)
# seriona_enable_warnings(seriona_scanner_phase1_integration_tests)
seriona_enable_warnings(seriona_scanner_tagreader_tests)
seriona_enable_warnings(seriona_scanner_service_tests)
seriona_enable_warnings(seriona_scanner_watcher_tests)
seriona_enable_warnings(seriona_scanner_wtr_integration_tests)
seriona_enable_warnings(seriona_scanner_lazy_cover_gateway_tests)
seriona_enable_warnings(seriona_scanner_error_logging_tests)
seriona_enable_warnings(seriona_runtime_paths_tests)
seriona_enable_warnings(seriona_application_logging_tests)

seriona_enable_warnings(seriona_logging_tests)
seriona_enable_warnings(seriona_application_log_level_tests)

target_link_libraries(seriona_scanner_hash_tests PRIVATE
  PkgConfig::SERIONA_XXHASH
  spdlog::spdlog
)
# target_link_libraries(seriona_scanner_cache_tests PRIVATE
#   SQLite3::SQLite3
#   spdlog::spdlog
# )
target_link_libraries(seriona_scanner_cache_basic_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
# target_link_libraries(seriona_scanner_cache_content_tests PRIVATE
#   SQLite3::SQLite3
#   PkgConfig::SERIONA_XXHASH
# )
target_link_libraries(seriona_scanner_cache_locations_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_auxiliary_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_cache_perf_tests PRIVATE
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
# target_link_libraries(seriona_scanner_migration_v2_to_v3_tests PRIVATE
#   SQLite3::SQLite3
#   PkgConfig::SERIONA_XXHASH
# )
# target_link_libraries(seriona_scanner_backup_rollback_tests PRIVATE
#   SQLite3::SQLite3
#   PkgConfig::SERIONA_XXHASH
# )
# target_link_libraries(seriona_scanner_phase1_integration_tests PRIVATE
#   SQLite3::SQLite3
#   PkgConfig::SERIONA_XXHASH
# )
target_link_libraries(seriona_scanner_tagreader_tests PRIVATE
  ${SERIONA_TAGREADER_TARGET}
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_tagreader_concurrency_stress_tests PRIVATE
  seriona_scanner
  ${SERIONA_TAGREADER_TARGET}
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
  ${SERIONA_TAGREADER_TARGET}
  BS::thread_pool
  spdlog::spdlog
)
target_link_libraries(seriona_scanner_incremental_plan_tests PRIVATE
  seriona_watcher_headers
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
  ${SERIONA_TAGREADER_TARGET}
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
target_link_libraries(seriona_scanner_detailed_perf_test PRIVATE
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
target_link_libraries(seriona_scanner_wtr_integration_tests PRIVATE
  seriona_scanner
  seriona_watcher_headers
  SQLite3::SQLite3
)
target_link_libraries(seriona_scanner_lazy_cover_gateway_tests PRIVATE
  seriona_scanner
  SQLite3::SQLite3
  PkgConfig::SERIONA_XXHASH
)
target_link_libraries(seriona_scanner_error_logging_tests PRIVATE
  seriona_scanner
)
target_link_libraries(seriona_runtime_paths_tests PRIVATE
  seriona_third_party_headers
)
target_link_libraries(seriona_application_logging_tests PRIVATE
  seriona_third_party_headers
  spdlog::spdlog
)

target_link_libraries(seriona_logging_tests PRIVATE
  spdlog::spdlog
)

target_link_libraries(seriona_application_log_level_tests PRIVATE
  seriona_third_party_headers
  spdlog::spdlog
)

add_test(NAME seriona.doctest.placeholder COMMAND seriona_tests)
add_test(NAME seriona.audio_contract COMMAND seriona_audio_contract_tests)
add_test(NAME seriona.audio_fixture COMMAND seriona_audio_fixture_tests)
add_test(NAME seriona.audio.waveform.contract COMMAND seriona_waveform_generator_tests --test-case=*contract*)
add_test(NAME seriona.audio.waveform.basic COMMAND seriona_waveform_generator_tests --test-case=*basic*)
add_test(NAME seriona.audio.waveform.ffmpeg COMMAND seriona_waveform_generator_tests --test-case=*waveform*ffmpeg*)
add_test(NAME seriona.audio.waveform.scalar COMMAND seriona_waveform_generator_tests --test-case=*waveform*scalar*)
add_test(NAME seriona.audio.waveform.simd COMMAND seriona_waveform_generator_tests --test-case=*waveform*simd*)
add_test(NAME seriona.audio.waveform.id3v1 COMMAND seriona_waveform_generator_tests --test-case=*waveform*id3v1*)
add_test(NAME seriona.audio.waveform.strategy_a COMMAND seriona_waveform_generator_tests --test-case=*waveform*strategy_a*)
add_test(NAME seriona.audio.waveform.strategy_b COMMAND seriona_waveform_generator_tests --test-case=*waveform*strategy_b*)
add_test(NAME seriona.audio.waveform.thread_pool COMMAND seriona_waveform_generator_tests --test-case=*waveform*thread_pool*)
add_test(NAME seriona.audio.waveform.public COMMAND seriona_waveform_generator_tests --test-case=*waveform*public*)
add_test(NAME seriona.audio.waveform.perf COMMAND seriona_waveform_generator_tests --test-case=*waveform*perf*)
set_tests_properties(seriona.audio.waveform.perf PROPERTIES TIMEOUT 240)
add_test(NAME seriona.ffmpeg_audio_source COMMAND seriona_ffmpeg_audio_source_tests --test-case=*decodes*)
add_test(NAME seriona.ffmpeg_audio_source_errors COMMAND seriona_ffmpeg_audio_source_tests --test-case=*maps*)
add_test(NAME seriona.ffmpeg_audio_source_seek COMMAND seriona_ffmpeg_audio_source_tests --test-case=*seek*)
add_test(NAME seriona.ffmpeg_audio_source_id3v1_tail COMMAND seriona_ffmpeg_audio_source_tests --test-case=*id3v1*)
add_test(NAME seriona.ffmpeg_filter_pipeline COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*converts*)
add_test(NAME seriona.ffmpeg_filter_pipeline_drain COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*drains*)
add_test(NAME seriona.ffmpeg_filter_pipeline_errors COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*invalid*)
add_test(NAME seriona.ffmpeg_filter_pipeline_int24 COMMAND seriona_ffmpeg_filter_pipeline_tests --test-case=*Int24*)
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
add_test(NAME seriona.audio_player_redundant_play COMMAND seriona_audio_player_seamless_tests --test-case=*redundant_play*)
add_test(NAME seriona.audio_player_direct_preload COMMAND seriona_audio_player_seamless_tests --test-case=*direct_preload*)
add_test(NAME seriona.audio_player_reload_param_change COMMAND seriona_audio_player_seamless_tests --test-case=*different_stream_parameters*)
add_test(NAME seriona.audio_player_preload_failure COMMAND seriona_audio_player_seamless_tests --test-case=*preload_failure*)
add_test(NAME seriona.audio_error_matrix COMMAND seriona_audio_error_matrix_tests)
add_test(NAME seriona.audio_shutdown_lifecycle COMMAND seriona_audio_shutdown_lifecycle_tests)
add_test(NAME seriona.audio.enumerate_playback_devices COMMAND seriona_audio_enumerate_devices_tests)
add_test(NAME seriona.audio.device_format_enumerator COMMAND seriona_audio_device_format_enumerator_tests)
if(UNIX AND NOT APPLE)
  add_test(NAME seriona.audio.pipewire_device_enumerator COMMAND seriona_audio_pipewire_device_enumerator_tests)
endif()
add_test(NAME seriona.audio.miniaudio_device_capabilities COMMAND seriona_miniaudio_device_capabilities_tests)
add_test(NAME seriona.scanner_test_harness COMMAND seriona_scanner_test_harness_tests)
add_test(NAME seriona.scanner_contract COMMAND seriona_scanner_contract_tests)
add_test(NAME seriona.scanner.internal_types COMMAND seriona_scanner_internal_types_tests)
add_test(NAME seriona.metadata_contract COMMAND seriona_metadata_contract_tests)
add_test(NAME seriona.metadata_mapper COMMAND seriona_metadata_mapper_tests)
add_test(NAME seriona.metadata_service COMMAND seriona_metadata_service_tests)
add_test(NAME seriona.metadata_service_recording COMMAND seriona_metadata_service_recording_tests)
if(TARGET seriona_metadata_mpris_tests)
  add_test(NAME seriona.metadata_mpris COMMAND seriona_metadata_mpris_tests)
  add_test(NAME seriona.metadata_mpris_smoke COMMAND seriona_metadata_mpris_tests --test-case=*smoke*)
endif()
add_test(NAME seriona.control_contract COMMAND seriona_control_contract_tests)
add_test(NAME seriona.control.contract.configure_output_command COMMAND seriona_configure_output_command_tests)
add_test(NAME seriona.control.reducer.configure_output COMMAND seriona_configure_output_reducer_tests)
add_test(NAME seriona.control.configure_output_executes COMMAND seriona_configure_output_executes_tests --test-case=*configure_output*)
add_test(NAME seriona.control.enumerate_devices COMMAND seriona_configure_output_executes_tests --test-case=*enumerate_devices*)
add_test(NAME seriona.control_controller COMMAND seriona_media_controller_tests)
add_test(NAME seriona.control_folder_sort_settings_store COMMAND seriona_control_folder_sort_settings_store_tests)
add_test(NAME seriona.control_app_settings_store COMMAND seriona_control_app_settings_store_tests)
add_test(NAME seriona.control_persistence_guardrails COMMAND seriona_control_persistence_guardrail_tests)
add_test(NAME seriona.control_playback_context_builder COMMAND seriona_control_playback_context_builder_tests)
add_test(NAME seriona.control_artwork_resolver COMMAND seriona_artwork_resolver_tests)
add_test(NAME seriona.control_artwork_shutdown COMMAND seriona_media_controller_artwork_shutdown_tests)
add_test(NAME seriona.control.delete_commands COMMAND seriona_delete_commands_tests)
add_test(NAME seriona.control.reducer.track_switch_snapshot COMMAND seriona_track_switch_snapshot_reducer_tests)
add_test(NAME seriona.control.reducer.play_queue COMMAND seriona_play_queue_reducer_tests)
add_test(NAME seriona.scanner.remove_location COMMAND seriona_scanner_remove_location_tests)
set_tests_properties(seriona.control_artwork_resolver PROPERTIES TIMEOUT 60)
add_test(NAME seriona.terminal_controller COMMAND seriona_terminal_controller_tests)
add_test(NAME seriona.scanner_paths COMMAND seriona_scanner_paths_tests)
add_test(NAME seriona.scanner_hash COMMAND seriona_scanner_hash_tests)
add_test(NAME seriona.scanner.directory_tree_hash COMMAND seriona_scanner_hash_tests "--test-case=scanner directory tree hash*")
add_test(NAME seriona.scanner.song_identity COMMAND seriona_scanner_hash_tests "--test-case=scanner song identity*")
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
# add_test(NAME seriona.scanner_cache COMMAND seriona_scanner_cache_tests)
add_test(NAME seriona.scanner.cache.basic COMMAND seriona_scanner_cache_basic_tests)
# add_test(NAME seriona.scanner.cache.content COMMAND seriona_scanner_cache_content_tests)
add_test(NAME seriona.scanner.cache.locations COMMAND seriona_scanner_cache_locations_tests)
add_test(NAME seriona.scanner.cache.auxiliary COMMAND seriona_scanner_cache_auxiliary_tests)
add_test(NAME seriona.scanner.cache.perf COMMAND seriona_scanner_cache_perf_tests)
add_test(NAME seriona.scanner.cache.schema COMMAND seriona_scanner_cache_schema_tests)
# add_test(NAME seriona.scanner.migration_v2_to_v3 COMMAND seriona_scanner_migration_v2_to_v3_tests)
# add_test(NAME seriona.scanner.migration_phase1_integration COMMAND seriona_scanner_phase1_integration_tests)
# add_test(NAME seriona.scanner.backup_rollback COMMAND seriona_scanner_backup_rollback_tests)
add_test(NAME seriona.scanner_tagreader COMMAND seriona_scanner_tagreader_tests)
add_test(NAME seriona.scanner.tagreader_concurrency_stress COMMAND seriona_scanner_tagreader_concurrency_stress_tests)
add_test(NAME seriona.scanner_service COMMAND seriona_scanner_service_tests)
add_test(NAME seriona.scanner.event_ordering COMMAND seriona_scanner_service_tests "--test-case=scanner service publishes worker results in discovered file order")
add_test(NAME seriona.scanner.cache_lookup COMMAND seriona_scanner_service_tests "--test-case=scanner service cache lookup reuses a high-count cached root")
add_test(NAME seriona.scanner.incremental_integration COMMAND seriona_scanner_service_tests "--test-case=scanner service incremental integration reuses unchanged scans only added and changed and prunes deleted")
add_test(NAME seriona.scanner.config_concurrency COMMAND seriona_scanner_service_tests --test-case=*configured*)
add_test(NAME seriona.scanner.config_serial COMMAND seriona_scanner_service_tests --test-case=*serial*fallback*)
add_test(NAME seriona.scanner.env_serial COMMAND seriona_scanner_service_tests --test-case=*disable*concurrency*)
add_test(NAME seriona.scanner.env_malformed COMMAND seriona_scanner_service_tests --test-case=*malformed*env*)
add_test(NAME seriona.scanner.config_scan_mode COMMAND seriona_scanner_service_tests --test-case=*force*full*)
add_test(NAME seriona.scanner_watcher COMMAND seriona_scanner_watcher_tests)
add_test(NAME seriona.scanner.wtr_integration COMMAND seriona_scanner_wtr_integration_tests)
set_tests_properties(seriona.scanner.wtr_integration PROPERTIES TIMEOUT 180)
add_test(NAME seriona.scanner.lazy_cover_gateway COMMAND seriona_scanner_lazy_cover_gateway_tests)
add_test(NAME seriona.scanner.error_logging COMMAND seriona_scanner_error_logging_tests)
add_test(NAME seriona.scanner.cue_adapter COMMAND seriona_scanner_cue_adapter_tests)
add_test(NAME seriona.scanner.two_pass_discovery COMMAND seriona_scanner_two_pass_discovery_tests)
add_test(NAME seriona.scanner.cue_error_isolation COMMAND seriona_scanner_cue_error_isolation_tests)
add_test(NAME seriona.scanner.two_pass_integration COMMAND seriona_scanner_two_pass_integration_tests)
add_test(NAME seriona.scanner.preallocate COMMAND seriona_scanner_preallocate_tests)
add_test(NAME seriona.scanner.cue_nodes COMMAND seriona_scanner_cue_nodes_tests)
add_test(NAME seriona.scanner.playlist_cue COMMAND seriona_scanner_playlist_cue_tests)
add_test(NAME seriona.scanner.e2e COMMAND seriona_scanner_complete_e2e_tests)

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

add_executable(seriona_scanner_folder_thumbnail_tests
  scanner/folder_thumbnail_tests.cpp
)
target_compile_definitions(seriona_scanner_folder_thumbnail_tests PRIVATE DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN)
target_include_directories(seriona_scanner_folder_thumbnail_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src/scanner
  ${PROJECT_SOURCE_DIR}/third_party/doctest
)
target_link_libraries(seriona_scanner_folder_thumbnail_tests PRIVATE
  seriona_scanner
)
seriona_enable_warnings(seriona_scanner_folder_thumbnail_tests)
add_test(NAME seriona.scanner.folder_thumbnail COMMAND seriona_scanner_folder_thumbnail_tests)

add_test(NAME seriona.logging COMMAND seriona_logging_tests)
add_test(NAME seriona.runtime_paths COMMAND seriona_runtime_paths_tests)
add_test(NAME seriona.application_logging COMMAND seriona_application_logging_tests)
add_test(NAME seriona.application_log_level COMMAND seriona_application_log_level_tests)
