add_executable(seriona
  main.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_player.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/audio_playback_service.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/buffer/pcm_buffer_queue.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/clock/playback_clock.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/audio_output_device.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/device/miniaudio_output_device_backend.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/events/audio_event_dispatcher.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_audio_source.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/ffmpeg/ffmpeg_filter_pipeline.cpp
  ${PROJECT_SOURCE_DIR}/src/audio/playback_state_machine.cpp
)

set_target_properties(seriona PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
)

target_include_directories(seriona PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
)

target_link_libraries(seriona PRIVATE
  seriona_scanner
  seriona_metadata
  seriona_third_party_headers
  PkgConfig::SERIONA_FFMPEG
)

seriona_enable_warnings(seriona)
