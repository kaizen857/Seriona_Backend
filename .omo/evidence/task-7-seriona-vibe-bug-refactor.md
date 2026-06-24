# Task 7 Evidence - metadata snapshot updates asynchronous from control

## Scope
- Implemented only Phase 4 / Task 7 behavior: metadata snapshot updates now run on a metadata-owned worker with latest-state coalescing.
- Did not touch Task 9/10 files or plan/boulder/report files.

## Red proof
- Added a deterministic slow-backend regression in `tests/metadata/metadata_service_tests.cpp`.
- Before the async worker implementation, running the rebuilt media-controller-style slow metadata proof showed the old synchronous path could hang until timeout: `./build/tests/seriona_media_controller_tests` ended with `SIGTERM` after the new test blocked in synchronous metadata update.
- The final regression is anchored at the metadata service boundary instead, because arbitrary test fakes injected directly into `MediaControllerDependencies` can bypass the real metadata-owned queue. The slow `MetadataServiceBackend` now proves `MetadataSharingService::update()` returns quickly while backend work is still blocked.

## Implementation evidence
- `src/metadata/metadata_service_backend.cpp`: `MetadataSharingServiceImpl::update()` stores only the latest `PlatformMediaState` in `pendingUpdate_`, notifies a metadata-owned worker, and returns an accepted `MetadataSyncResult` without calling the backend on the caller thread.
- `src/metadata/metadata_service_backend.cpp`: the worker serially calls `backend_->update(...)`; pending states are coalesced so stale snapshots can be skipped and the final backend-observed state is the newest queued snapshot.
- `src/metadata/metadata_service_backend.cpp`: `stop()` and the destructor signal shutdown, join the worker, clear pending state, then call backend `stop()` once when the service had started.
- `src/control/media_controller.cpp`: unchanged control boundary still calls `metadata->update(platformStateFromSnapshot(snapshot))`, but that call is now a fast metadata-service enqueue for production services.

## Verification
- `cmake --build build --target seriona_metadata_service_tests seriona_metadata_service_recording_tests seriona_media_controller_tests` — passed.
- `ctest --test-dir build -R 'seriona\.(metadata_service|metadata_service_recording)' --output-on-failure` — passed: 2/2 tests.
- `./build/tests/seriona_media_controller_tests` — passed: 18/18 test cases, 172/172 assertions.
- Grep checks: `src/control` has only the expected `metadata->update(...)` enqueue call; touched metadata/control tests and metadata implementation contain no new TODO/FIXME matches.

## Follow-up verification
- Strengthened the slow-backend regression to enqueue version 3 and then version 4 while the first backend update is blocked; after release, the backend-observed final state is version 4, proving deterministic latest-state coalescing.
- Re-ran `cmake --build build --target seriona_metadata_service_tests seriona_metadata_service_recording_tests seriona_media_controller_tests` — passed.
- Re-ran `ctest --test-dir build -R 'seriona\.(metadata_service|metadata_service_recording)' --output-on-failure` — passed: 2/2 tests.
- Re-ran `./build/tests/seriona_media_controller_tests` — passed: 18/18 test cases, 172/172 assertions.
