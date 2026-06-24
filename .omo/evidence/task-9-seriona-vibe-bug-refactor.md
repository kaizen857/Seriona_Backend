# Task 9 Evidence - Phase 4 snapshot subscriptions

## Scope
- Task: `9. Phase 4: make snapshot subscriptions value-safe and nonblocking`.
- Modified source/test paths: `inc/seriona/control/control_contracts.h`, `src/control/subscription_store.cpp`, `tests/control/control_contract_tests.cpp`, `tests/control/media_controller_tests.cpp`.
- `src/control/media_controller.cpp` was inspected but not changed; publish integration already routes through `SubscriptionStore`.

## Baseline and Red Proof
- Baseline before new regressions:
  - `cmake --build build --target seriona_control_contract_tests seriona_media_controller_tests` passed.
  - `ctest --test-dir build -R 'seriona\.control_contract' --output-on-failure` passed.
  - `./build/tests/seriona_media_controller_tests` passed with 18/18 tests.
- Added regression `media controller facade slow snapshot subscribers do not starve control work`.
- Old synchronous delivery failed deterministically: `./build/tests/seriona_media_controller_tests` hung in that test until the 120s command timeout, proving a slow snapshot subscriber could starve later control work.
- Added contract regression `snapshot subscription callbacks take owned values` to prove public snapshot/notification callbacks are by value.

## Implementation Notes
- Snapshot and notification callbacks now take owned values in `control_contracts.h`.
- `SubscriptionStore` copies callbacks and snapshot values outside the subscriber mutex and submits delivery work to a store-scoped worker queue.
- Initial snapshot delivery and later `publish()` delivery use the same queued `invokeSubscriber`/worker semantics.
- Callback execution occurs after lock release; queue tasks re-check active subscription state before calling the captured callback.
- `unsubscribe()` removes the subscriber, then waits for the store delivery queue to become idle so tests and clients can safely release callback-captured state.

## Verification
- `cmake --build build --target seriona_control_contract_tests seriona_media_controller_tests` passed.
- `ctest --test-dir build -R 'seriona\.control_contract' --output-on-failure` passed: 1/1 test passed.
- `./build/tests/seriona_media_controller_tests` passed: 19/19 test cases, 174/174 assertions.
- Grep check found no remaining public snapshot callback aliases of `std::function<void(const PlayerStateSnapshot&)>`, `std::function<void(const LibraryStateSnapshot&)>`, or `std::function<void(const ControlDomainNotification&)>`; remaining callback invocations in `subscription_store.cpp` use `snapshotCopy` from the worker path.

## Root Verification Regression Follow-up
- Observed regression: root verification failed in `media controller facade rejects unplayable commands and publishes command notifications` at the second rejection assertion because `notifications.back()` sometimes held another asynchronously delivered notification (`kind == 0`, `errorCode == 0`).
- Diagnosis: production semantics still publish the expected `CommandRejected` value; the test retained the old synchronous ordering assumption that the target notification must be the last vector element after `size() >= 2`.
- Fix: changed the test to wait for eventual presence of the exact `CommandRejected` notification and error code instead of inspecting `notifications.back()`.
- Rerun results:
  - `cmake --build build --target seriona_control_contract_tests seriona_media_controller_tests` passed.
  - `ctest --test-dir build -R 'seriona\.control_contract' --output-on-failure` passed: 1/1 test passed.
  - `./build/tests/seriona_media_controller_tests` passed: 19/19 test cases, 172/172 assertions.
