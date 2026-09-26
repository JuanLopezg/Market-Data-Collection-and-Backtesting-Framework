# Step 24 — Real Alerts & Audit evidence

Step 24 enables `/api/alerts-audit` in real-provider mode.

## Real current alert derivation

The endpoint derives **current** alert conditions from:

- PostgreSQL and NATS health already observed by Infrastructure;
- current retained reconciliation evidence;
- durable rejected orders from `trading_runtime_state.snapshot.orders`;
- canonical Market Data freshness/integrity/strategy-cycle diagnostics;
- availability of the PortfolioRisk read model.

No alert is created merely because a future/unwired source is `UNKNOWN`.

## System evidence timeline

The lower table uses persisted/checkpoint evidence already available to the dashboard, including risk decisions, market cycles, persisted fills/rejected-order state and reconciliation evidence. It is labelled **System Evidence Timeline**, not a complete Audit Trail.

## Explicit gaps

The following remain unavailable and are never fabricated:

- append-only durable alert history;
- acknowledgement state;
- resolution lifecycle/history;
- durable operator/login/control/config/deployment audit rows.

The API exposes explicit booleans for these gaps. A later product step can add a dedicated alert/audit projection without making it a dependency of trading.
