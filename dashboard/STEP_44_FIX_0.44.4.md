# Step 44 fix 0.44.4 — transient critical-alert evidence

## Problem reproduced

A Step 44 readiness snapshot can contain several known transient read blockers plus
`critical-alerts=BLOCKED`. The alert requirement previously carried only the count,
not the active alert identity/detail, and therefore was always classified as
non-retryable. This could stop the bounded retry loop even when the critical alert
was itself a transient PostgreSQL/NATS source probe captured by the independent
Alerts & Audit read.

## Fix

- Read the active alert rows from `/api/alerts-audit` into the readiness snapshot.
- Include active CRITICAL service/event/detail evidence in the Step 44 requirement.
- Keep CRITICAL conditions fail-closed.
- Mark `critical-alerts` retryable only when **every** declared active CRITICAL:
  - has full detail available;
  - is `SOURCE_CRITICAL`;
  - belongs to PostgreSQL or NATS; and
  - contains a bounded transient transport/read marker (timeout, connection reset,
    connection refused, locked/busy, etc.).
- A missing alert detail, reconciliation contradiction, registry divergence,
  ledger integrity failure, configuration error, or mixed transient+semantic set
  remains non-retryable.

No alert severity is downgraded and no blocker is converted to PASS. The retry loop
simply gets enough evidence to take a fresh readiness snapshot when the complete
blocking set is known to be transient.

## Safety

Unchanged:
- private auth DEFERRED;
- order routing DISABLED;
- manual routing DISABLED;
- no signing;
- no submit/cancel;
- no capital movement.
