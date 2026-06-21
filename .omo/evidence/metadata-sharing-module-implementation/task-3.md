# Task 3 Evidence

- Added `inc/seriona/metadata/metadata_contracts.h` with a platform-neutral `MetadataSharingService` contract, `MetadataSharingOptions`, `MetadataBackendKind`, `MetadataBackendCapabilities`, `PlatformMediaState`, `MetadataSyncResult`, command callback registration, `start`/`update`/`stop`, and `makeMetadataSharingService(...)`.
- Added focused contract coverage in `tests/metadata/metadata_contract_tests.cpp` for Linux/Windows/Noop options and for capability degradation when a Windows host extension is absent.
- Updated `CMakeLists.txt` so `seriona_metadata` exports `inc/` publicly, keeping the contract consumable by the focused metadata contract test target.
- Verification pending: `cmake --build build --target seriona_metadata_contract_tests && ctest --test-dir build -R seriona.metadata_contract --output-on-failure`.
