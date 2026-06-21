# Task 13 Evidence

- Linux configure command: `cmake -S . -B build -DSERIONA_BUILD_TESTS=ON` — passed.
- Configure note: CMake emitted a developer warning from `FetchContent.cmake` about `CMP0135` / `DOWNLOAD_EXTRACT_TIMESTAMP`; configuration still completed successfully.
- Initial build command: `cmake --build build` — failed on the first run because `seriona_metadata` did not link `src/metadata/metadata_mpris_linux.cpp`, producing an undefined reference to `seriona::metadata::detail::makeLinuxMetadataServiceBackend(...)` from `metadata_service_backend.cpp`.
- Fix applied: added `src/metadata/metadata_mpris_linux.cpp` to the `seriona_metadata` target in `CMakeLists.txt` so the Linux backend implementation is linked into the metadata library.
- Rebuild command: `cmake --build build` — passed after the CMake fix.
- Focused metadata test command: `ctest --test-dir build -R seriona.metadata --output-on-failure` — passed (`6/6` tests).
- Full verification command: `ctest --test-dir build --output-on-failure` — passed (`41/41` tests).
- Result: Linux metadata sharing verification is complete and recorded; no unrelated failures were changed.
