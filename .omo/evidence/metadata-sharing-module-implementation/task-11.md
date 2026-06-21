# Task 11 Evidence

- Changed: `app/CMakeLists.txt`
- Integration: `seriona` now links `seriona_metadata` via `target_link_libraries(seriona PRIVATE seriona_metadata)` only.
- Boundary check: `app/main.cpp` was left untouched; no `MetadataSharingService`, `MPRIS`, `SMTC`, `sdbus`, or `winrt` references were introduced.
- Verification pending: build + CTest + negative grep.
