# Step 14B — Operational Core / Real PostgreSQL Reads

Version: **v0.14.1**

Step 14B enables the first concrete `RealProvider` reads against the trading runtime.
It does **not** claim that the full dashboard is real yet.

## Enabled real resources

- `GET /api/positions`
  - source: `trading_runtime_state.snapshot.account_positions`
  - owner: ExecutionState
  - exact local quantity only
  - valuation, target and exchange alignment remain unavailable until their owning projections exist

- `GET /api/execution`
  - source: `trading_runtime_state.snapshot.orders`
  - source: append-only `trading_fills`
  - current tracked-order state and persisted fills are real
  - full lifecycle, slippage and latency history are not inferred

## Write safety

The Go PostgreSQL adapter uses libpq and immediately sets:

- `default_transaction_read_only = on`
- `statement_timeout = 2000ms`
- `lock_timeout = 500ms`

Provider health refuses readiness unless PostgreSQL reports the session as read-only and both required runtime tables exist.
There is no arbitrary SQL endpoint and no write method in the adapter boundary.

## Readiness semantics

`GET /api/readiness` still means **dashboard data-provider readiness**.
A 200 response in Step 14B means the read-only PostgreSQL source for the enabled resources is accessible.
It does **not** mean `TRADING READY`.

`/api/shell-status` remains unavailable in real mode until SystemReadiness is projected from its actual owners.

## Fail closed

All other real resources continue to return HTTP 503. There is no automatic real-to-mock fallback.

## Gates

- **14B.1**: source/build validation.
- **14B.2**: read-only smoke against the user's actual PostgreSQL runtime.

Do not close Step 14B until both gates pass.
