add_executable(seriona_miniaudio_platform_probe EXCLUDE_FROM_ALL
  miniaudio_platform_probe.cpp
)

target_link_libraries(seriona_miniaudio_platform_probe PRIVATE
  seriona_third_party_headers
)

seriona_enable_warnings(seriona_miniaudio_platform_probe)

add_executable(seriona_scanner_cold_perf
  scanner_cold_perf.cpp
)

target_link_libraries(seriona_scanner_cold_perf PRIVATE
  seriona_scanner
  seriona_metadata
)

seriona_enable_warnings(seriona_scanner_cold_perf)

# 目录移出监视根审计程序：真实 FileScannerService + 真实 wtr::watch 监视器，
# 在临时目录做文件系统操作对照实验，验证
# "目录 mv 出监视根时 wtr.watcher 不产生事件 -> 扫描不触发 -> 播放列表不更新"。
# 只链接 seriona_scanner（其依赖 SQLite3/xxhash/TagReaderCore/BS::thread_pool/
# watcher 头/spdlog 均为 PUBLIC 传递，见根 CMakeLists.txt）。
add_executable(seriona_watch_root_move_audit
  watch_root_move_audit.cpp
)

target_include_directories(seriona_watch_root_move_audit PRIVATE
  ${PROJECT_SOURCE_DIR}/inc
  ${PROJECT_SOURCE_DIR}/src            # 私有头 scanner/file_scanner_service_internal.h
  ${PROJECT_SOURCE_DIR}/src/scanner
)

target_link_libraries(seriona_watch_root_move_audit PRIVATE
  seriona_scanner
)

seriona_enable_warnings(seriona_watch_root_move_audit)
