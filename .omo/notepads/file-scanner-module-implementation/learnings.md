## 2026-06-20 task 1 scanner foundation

- Current Seriona root CMake previously used C++20 and only created audio/app/test targets; `seriona_scanner` did not exist.
- `sqlite3` and `libxxhash` are installed locally (`sqlite3` 3.53.2, `libxxhash` 0.8.3), so implementation could continue without package-manager commands.
- Adding local TagReader with `EXCLUDE_FROM_ALL` still lets its `test/CMakeLists.txt` register CTest tests unless the child test directory TESTS property is cleared after import.
- `TagReaderCore` builds as the scanner private dependency; scanner public header remains independent from `TagReader.hpp` and `MusicTag`.
- LSP diagnostics were unavailable in this session (`Connection closed` / `Not connected`), so CMake configure/build/CTest are the hard verification source.
