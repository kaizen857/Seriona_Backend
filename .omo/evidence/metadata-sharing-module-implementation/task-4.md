# task 4 evidence

- Expanded `seriona_metadata` to compile three private source scaffolds: `metadata_mapper.cpp`, `metadata_service.cpp`, `metadata_mpris.cpp`.
- Added metadata test executables and CTest registrations for `seriona.metadata_mapper`, `seriona.metadata_service`, `seriona.metadata_mpris`, and `seriona.metadata_mpris_smoke`.
- Added Linux configure gate for `sdbus-c++` with `SERIONA_METADATA_SIMULATE_MISSING_SDBUS` failure path.
- Kept `sdbus-c++` usage private to the Linux metadata implementation file.
