# Step 44 fix 0.44.5 — Read amplification / self-contention

Observed failure after 0.44.4:

- the readiness endpoint correctly classified all remaining blockers as retryable;
- nevertheless six consecutive snapshots continued to see PostgreSQL TCP probe timeouts and canonical SQLite 2s timeouts;
- the final message incorrectly called the exhausted-retry case "non-transient".

Root cause found in the read-model topology:

`/api/global-readiness` fans out to shell, infrastructure, alerts, pipeline, manual control, venue rules, symbol registry and ledger. Several of those read models fan out again. In particular shell -> alerts, and alerts -> infrastructure + market data + symbol registry, while global readiness was simultaneously requesting those same projections directly. This can launch several identical SQLite/PostgreSQL reads at once from a container intentionally limited to 0.20 CPU, producing self-induced contention/timeouts.

Fix:

- coalesce duplicate in-flight `marketDataResource` reads inside one dashboard-api process;
- reuse successful canonical market-data snapshots for only 2 seconds;
- coalesce/reuse `infrastructure` snapshots for only 2 seconds;
- coalesce/reuse `alertsAudit` snapshots for only 2 seconds;
- failed market-data reads are shared only with current waiters and are NOT cached for future snapshots;
- watchdog remains a separate process/provider instance and still derives/persists alerts independently;
- no trading state is written and no readiness blocker is converted to PASS;
- improve the Step 44 gate message when all blockers remain retryable but retries are exhausted.

Safety remains unchanged:

- private auth: DEFERRED;
- order routing: DISABLED;
- manual routing: DISABLED;
- no signing, submit/cancel, `/exchange`, or capital movement.
