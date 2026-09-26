# Step 16 — First real dashboard read: Infrastructure

Version: 0.16.0

Step 16 is the first dashboard page served from real project sources.

## Enabled in real mode

`GET /api/infrastructure` now returns a bounded read-only view built from:

- authenticated PostgreSQL diagnostics;
- `trading_runtime_state` summary when that table/row exists;
- `trading_fills` row count/latest timestamp when available;
- NATS TCP connectivity plus the NATS monitoring endpoints `/jsz` and `/varz`;
- canonical market-data SQLite file/header reachability.

The page deliberately reports `UNKNOWN` instead of inventing values for:

- VPS/host CPU, RAM, disk and clock sync;
- Docker container resource metrics;
- trading-service process liveness;
- exchange connectivity;
- reconciliation state;
- durable outbox health.

Therefore `TRADING DEGRADED` on this page means **dashboard observability is incomplete**, not that the trading engine has independently declared itself degraded.

## PostgreSQL implementation

The API image now includes the official PostgreSQL `psql` client. The Go service executes only fixed, bounded `SELECT` queries with:

- `default_transaction_read_only=on`;
- a short statement timeout;
- a short connection timeout;
- credentials passed through environment variables rather than query arguments.

No schema mutation, INSERT, UPDATE or DELETE is used.

## New diagnostic endpoint

Authenticated only:

```text
GET /api/runtime-state-summary
```

It returns PostgreSQL table presence and a compact summary of the durable execution state. It never returns the full snapshot payload.

## Still fail-closed

All other real dashboard resources still return 503 until their specific mapper is implemented:

- Overview
- Positions
- Reconciliation
- Pipeline / Why
- Execution
- Risk
- Market Data
- Alerts & Audit
- Live vs Expected
- Manual Control

## Next

Step 17 should implement **Positions + execution-state durable snapshot reads**, using the already verified `trading_runtime_state.snapshot` contract without inventing prices/PnL that are not present in that snapshot.
