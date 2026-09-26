# Real data integration plan — Step 14 audited

The actual project code has now been reviewed. The unverified Step 13 worksheet has been superseded by:

- `REAL_DATA_SOURCE_MAP.md` — verified resource/source mapping
- `RUNTIME_CONTRACT_AUDIT.md` — source evidence index
- `INTEGRATION_GAPS.md` — missing projections/contracts that must not be guessed

## Rules that remain unchanged

1. Browser talks only to `dashboard-api`.
2. Dashboard is never a dependency of the trading engine.
3. Read paths respect runtime ownership and use bounded queries.
4. Manual actions must later route through the normal business pipeline.
5. No real deployment may silently fall back to mocks.
6. Business timestamps come from the trading system; the dashboard does not invent economic time.
7. Canonical market-data SQLite is opened read-only by the dashboard.
8. Runtime-owned PostgreSQL tables are read server-side only; the browser never submits SQL.

## Step 17 status

PostgreSQL authenticated read-only queries, NATS monitoring and the canonical SQLite file probe remain wired for **Infrastructure**. Step 17 additionally enables **Positions** from the durable `trading_runtime_state.snapshot`.

Positions exposes only facts supported by that snapshot: account cash, physical quantities and strategy virtual quantities. Valuation/PnL, approved targets and exchange reconciliation remain unavailable instead of being guessed. All other normal resources continue to fail closed in real mode until their resource-specific mapper is implemented.


## Step 18 update
`Execution` now has a bounded real read model from `trading_runtime_state.snapshot.orders` + `trading_fills`. Latency/slippage/replacement lineage remain unavailable unless a canonical durable source is added.


### Step 19 — Reconciliation

Reconciliation is now a real read-only projection. It combines PostgreSQL ExecutionState with the latest retained canonical exchange snapshot in JetStream. The dashboard does not publish a snapshot request, so its presence cannot alter the trading pipeline. Stale or absent exchange evidence remains explicitly pending.


## Step 20 — Pipeline / Why

Real Pipeline lineage is now read-only and cycle-aligned across the durable Strategy, PortfolioRisk and OrderPlanner checkpoints, with ExecutionState/reconciliation enrichment. The risk checkpoint is the anchor because it persists the exact StrategyIntentBatch and AccountSnapshot used to produce the DecisionBatch. Missing planner data is `PENDING`; no cross-cycle mixing is allowed. RSI/rank and intermediate risk transforms remain unavailable unless a canonical persisted source is added.

## Step 28 streaming rule

NATS is used only as an internal invalidation signal. `dashboard-api` subscribes read-only to verified runtime subjects and emits resource names over authenticated SSE. React then refetches the same bounded REST projections used before Step 28. Do not evolve this into raw NATS payloads in the browser or a browser-side NATS client; that would couple the UI to trading transport contracts and broaden the security boundary unnecessarily.

## Step 34 — public venue metadata

The real provider now owns a bounded Hyperliquid public-info client for TESTNET. This is a read-only observability integration, not an execution adapter. It queries only public perpetual metadata (`meta`) and public mids (`allMids`) and exposes a bounded summary through `/api/venue-public`.

This does not alter the C++ ExchangeGateway or its `hyperliquid-dry-run` no-submission boundary. Execution/private connectivity therefore remains independently fail-closed.
