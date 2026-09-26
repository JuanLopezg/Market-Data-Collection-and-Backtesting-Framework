# Step 14 — Real project audit & source mapping

Step 14 is the first dashboard step based on the uploaded **real algoTrading codebase**, not on inferred backend structure.

## Added

- Verified PostgreSQL table/snapshot map.
- Verified canonical SQLite market-data schema.
- Verified NATS/JetStream runtime subject strings.
- Verified wire-contract field map for AccountSnapshot, StrategyIntentBatch, DecisionBatch, NotionalOrderPlanBatch, execution events and ExchangeSnapshotEvent.
- Resource-by-resource integration catalog with explicit `VERIFIED_BASE`, `PARTIAL`, `MISSING_PROJECTION` and `NOT_IMPLEMENTED` statuses.
- Authenticated `GET /api/integration-catalog` endpoint.
- Go constants for audited NATS subjects and PostgreSQL/SQLite schema names.
- `scripts/verify-project-contracts.py` to detect source-contract drift before later integration work.
- Detailed gap list so later steps do not invent missing accounting/reconciliation/risk/health/audit data.

## Important conclusions

- Market Data, base Positions and base Execution have strong existing read sources.
- Pipeline is substantially reconstructable from durable checkpoints, but the final Why trace still lacks live risk transformation diagnostics.
- Reconciliation logic exists, but its latest report/status is not durably exposed.
- Current PnL/ledger requirements are not fully supported by the runtime persistence yet.
- Current live compose is pre-exchange: `exchange-gateway` is profile-gated and must not be presented as connected by default.
- There is no normalized service health/heartbeat API across all C++ services.
- Alerts/Audit and Live-vs-Expected still need dedicated read models.
- Manual Control remains visual only.

## Still deliberately NOT implemented

- No PostgreSQL connection from dashboard-api.
- No SQLite connection from dashboard-api.
- No NATS client in dashboard-api.
- No C++ service modifications.
- No trading action endpoints.
- No mock → real fallback.

`DASHBOARD_DATA_PROVIDER=real` remains fail-closed with 503 reads until Step 15+ adapters are implemented.
