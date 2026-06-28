---
slug: seriona-log-cleanup
status: plan-written
intent: clear
pending-action: start work or request high-accuracy review
approach: Add a thin pre-startup log directory cleanup function using <filesystem>, generate timestamped log filenames per session, and keep spdlog rotating_file_sink_mt for intra-session rotation — no custom spdlog sink needed.
---

# Draft: seriona-log-cleanup

## Components (topology ledger)
<!-- Lock the SHAPE before depth. One row per top-level component that can succeed or fail independently. -->
<!-- id | outcome (one line) | status: active|deferred | evidence path -->

## Open assumptions (announced defaults)
<!-- Record any default you adopt instead of asking, so the user can veto it at the gate. -->
<!-- assumption | adopted default | rationale | reversible? -->

## Findings (cited - path:lines)

## Decisions (with rationale)

## Scope IN

## Scope OUT (Must NOT have)

## Open questions

## Approval gate
status: drafting
<!-- When exploration is exhausted and unknowns are answered, set status: awaiting-approval. -->
<!-- That durable record is the loop guard: on a later turn read it and resume at the gate instead of re-running exploration. -->
