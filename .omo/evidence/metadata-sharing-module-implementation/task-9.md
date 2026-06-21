# Task 9 Evidence

- Added a Windows-only private metadata backend skeleton under `src/metadata/metadata_windows_private.h` and `src/metadata/metadata_windows_private.cpp`.
- Wired `src/metadata/metadata_service_backend.cpp` to select the Windows backend only under `_WIN32`; Linux continues to use the existing path.
- Kept Windows-specific implementation details out of `inc/seriona/` and preserved the top-level metadata API unchanged.
- Added a small contract test in `tests/metadata/metadata_contract_tests.cpp` to keep the public header boundary stable.
- Next verification: Linux configure/build plus structural grep checks for guarded Windows implementation evidence.
