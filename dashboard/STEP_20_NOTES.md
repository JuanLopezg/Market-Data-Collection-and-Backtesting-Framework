# Step 20 — Real Pipeline + Why

Step 20 enables the Pipeline page and Why inspector in `DASHBOARD_DATA_PROVIDER=real`.

## Cycle alignment

The latest `portfolio_risk_live_decision_checkpoint` is the anchor. Its persisted `signals_payload`, `account_payload` and `decision_payload` are decoded as the exact inputs/output of PortfolioRisk. Strategy `update_payload` and OrderPlanner `request_payload`/`plan_payload` are joined only at the same business timestamp. The dashboard never combines checkpoints from different cycles.

## Enrichment

The lineage is enriched with `trading_runtime_state.snapshot` for durable order state and the existing read-only reconciliation projection for local/exchange alignment.

## Explicit gaps

RSI(7), liquidity rank, raw pre-risk target, intermediate volatility sizing and detailed risk transformations are not persisted in the durable checkpoint chain. They render as `Not persisted`; Step 20 does not recompute them and does not present diagnostic inference as canonical trading truth.

## Safety

All reads remain bounded/read-only. No trading subject is published and no PostgreSQL/SQLite state is mutated.
