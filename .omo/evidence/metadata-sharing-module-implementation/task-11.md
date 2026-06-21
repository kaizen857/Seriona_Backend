# Task 11 Evidence

- Changed: `app/CMakeLists.txt`
- Integration: `seriona` now links `seriona_metadata` via `target_link_libraries(seriona PRIVATE seriona_metadata)` only.
- Boundary check: `app/main.cpp` was left untouched; no `MetadataSharingService`, `MPRIS`, `SMTC`, `sdbus`, or `winrt` references were introduced.
- Verification passed: `cmake --build build --target seriona seriona_metadata_mpris_tests` completed successfully, `ctest --test-dir build -R seriona.metadata --output-on-failure` passed, and `rg 'makeMetadataSharingService|MetadataSharingService|MPRIS|SMTC|sdbus|winrt' app/main.cpp` returned no matches.
