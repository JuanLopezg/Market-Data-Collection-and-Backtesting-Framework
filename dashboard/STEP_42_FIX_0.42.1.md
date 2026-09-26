# Step 42 fix 0.42.1 — bounded retry for transient canonical SQLite reads

Observed during the Step 42 precondition chain on 2026-09-26:

- Step 37A's registry bootstrap had already stabilized its read-only dependencies.
- The subsequent Step 36 -> Step 35 regression gate then hit a transient canonical
  strategy-universe SQLite timeout (`SQLite query timed out after 2s`).
- The Step 35 endpoint correctly remained BLOCKED/fail-closed, but its shell gate
  had no retry for this already-audited transient read condition.

Fix:

- `scripts/step35-symbol-mapping.sh` now retries `/api/venue-symbol-map` at most
  five times, sleeping two seconds between attempts, **only** when the endpoint
  reports `canonical strategy universe unavailable` caused by a SQLite query
  timeout.
- Mapping gaps, unsupported-classification mistakes, venue mismatches, invalid
  policy/state, malformed responses, HTTP failures, and all other blockers still
  fail immediately.
- No endpoint semantics were weakened: `/api/venue-symbol-map` itself remains
  fail-closed whenever canonical evidence is unavailable.
- No wallet, private authentication, signing, `/exchange`, submit/cancel, or
  order-routing surface was introduced.

This is gate resiliency only; Step 42's ledger semantics are unchanged.
