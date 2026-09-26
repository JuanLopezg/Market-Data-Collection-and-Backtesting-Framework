# Step 27 — Global Shell / Readiness real

Step 27 enables `GET /api/shell-status` in `DASHBOARD_DATA_PROVIDER=real`.

The global shell now aggregates only already-verified, read-only evidence:

- PostgreSQL authenticated diagnostics and durable runtime-snapshot presence;
- NATS / JetStream reachability and monitor health;
- canonical SQLite + exact-cycle Strategy market-data diagnostics;
- retained exchange-snapshot reconciliation evidence;
- current derived operational alert counts.

## Fail-closed semantics

`READY` is intentionally **not** asserted yet. The master dashboard contract requires independent evidence for exchange connectivity, common trading-service liveness/control state and host clock synchronization. Those contracts are still missing.

Current aggregate semantics:

- hard source failure, missing durable runtime snapshot, unavailable canonical market data, or current reconciliation `BLOCKED` -> `PAUSED`;
- otherwise -> `DEGRADED` until the remaining readiness dependencies are independently observable;
- fresh retained exchange evidence is shown as `EVIDENCE FRESH`, never promoted to `Connected`;
- global trading state is `UNVERIFIED` unless a verified blocker makes it `BLOCKED`.

The top bar now exposes Mode, Exchange evidence, Data, Trading, Reconciliation, Readiness, current alert count and authoritative API UTC time.
