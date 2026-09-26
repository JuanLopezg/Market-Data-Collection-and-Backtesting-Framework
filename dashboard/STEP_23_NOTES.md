# Step 23 — Real Overview

Step 23 enables `/api/overview` in `DASHBOARD_DATA_PROVIDER=real` mode.

The Overview is now an aggregate read model over sources already verified in prior steps:

- `trading_runtime_state.snapshot` for durable cash and physical positions.
- `portfolio_risk_live_decision_checkpoint` lineage for approved gross target and risk decision context.
- `trading_fills` plus durable tracked orders for recent execution observations.
- retained `execution.exchange.snapshot.v1` for reconciliation state when present/current.
- canonical market-data SQLite plus aligned Strategy checkpoint for market-cycle health.

## Deliberately unavailable

Step 23 does **not** fabricate any of the following:

- total marked equity;
- realized or unrealized PnL;
- historical equity curve;
- rolling Sharpe/volatility;
- Live-vs-Expected baseline;
- global trading readiness.

Those require canonical accounting/valuation/baseline/readiness contracts that are not yet wired. The page labels these sections explicitly as unavailable.

## Readiness semantics

Overview never reports global `READY` in Step 23. It reports:

- `PAUSED` if current reconciliation evidence is `BLOCKED`;
- otherwise `DEGRADED`, because exchange connectivity, alert state and full global readiness are not yet aggregated.

This is intentional fail-closed behaviour. Step 27 owns global shell/readiness.

## Recent observations

The right-hand timeline is named **Recent Canonical Observations** in real mode. It is built only from already-persisted/retained execution, risk, reconciliation and market read models. It is not yet the durable Alerts & Audit product timeline; that is a later step.
