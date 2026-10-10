# Current dashboard data sources

Checked against current C++ stores/services and Go source readers on 2026-10-07.
This guide describes implemented read paths and their limits. The runtime owns trading
truth; the dashboard must not invent missing canonical accounting or decision evidence.

PAPER liquidity reads `ohlcv_data.quote_volume` (actual completed-binance-kline
USDT turnover), while old LIVE/replay profiles read source `volume`. The display label
comes from the provider, not a hardcoded UI unit. Quote selection and rolling input
diagnostics share the same helper; cutover metadata prevents joining old signals or
mixing base-unit and quote-notional distributions.

## PostgreSQL: trading-owned state

Writer: `lib/src/persistence/postgres_state_store.cpp`.
Reader: `dashboard/dashboard-api/internal/integration/postgres/reader.go`.
Paths in this guide are relative to the repository root.

- `trading_runtime_state`: singleton JSONB `snapshot`, `schema_version`
  and `updated_at`.
- `trading_fills`: append-only by `fill_id`; fields include
  `order_id`, `strategy_id`, `timestamp`, `coin`, `side`,
  `quantity`, `price` and `commission`.

Important runtime snapshot keys: `schema_version`, `last_bar_close_timestamp`,
`last_execution_timestamp`, `next_order_id`, `account_cash`,
`account_positions`, `strategies`, `pending_plans`, `orders`
and `processed_fill_ids`. Current tracked orders include lifecycle status,
filled quantity, timestamps and exchange identity. This is current state, not a
separate complete append-only history of all order transitions.

Service checkpoints:

| Owner/source file | Durable tables and payloads |
| --- | --- |
| `live_trading/strategy_service/src/strategy_service_main.cpp` | `strategy_service_metadata`; `strategy_market_update_checkpoint` with timestamp, update and intent payloads. |
| `live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp` | `portfolio_risk_service_metadata`; `portfolio_risk_live_account_checkpoint`; `portfolio_risk_live_decision_checkpoint` with signals, account and decision payloads. |
| `live_trading/order_planner_service/src/order_planner_service_main.cpp` | `order_planner_live_notional_checkpoint` with timestamp, request and plan payloads. |

Pipeline joins anchor at the latest PortfolioRisk decision timestamp. Strategy update
and Planner request/plan must have that exact timestamp. The query does not use an
unrelated latest record to fill a missing stage. Execution orders/fills and retained
reconciliation evidence enrich that cycle when their identities align.

PostgreSQL queries are fixed, time-limited and read-only. Fill details are bounded;
ledger aggregate counts/totals cover the complete fill table. The API does not have a
generic SQL endpoint. A dedicated SELECT-only role is preferable in deployment.

## SQLite: market-owned history

Writer: `live_trading/market_data_service/src/market_store.cpp`.
Historical ingestion shares that store. Library reader:
`lib/src/market/canonical_market_data_reader.cpp`.
Dashboard reader: `dashboard/dashboard-api/internal/integration/sqlite/reader.go`.

Tables:

- `tracked_pairs`
- `ohlcv_data(pair,date,open,high,low,close,volume)`
- `date_of_start`
- `market_volume_rank_daily(date,rank,pair,quote_volume)`

Market writers commit before `MarketDataUpdated` publication. Dashboard queries
use bounded completed-date/history windows and explicit price timestamps. SMA-volume
and RSI diagnostics use verified C++ formulas; persisted strategy signals are joined
only on the exact corresponding market date.

The canonical database file is mounted read-only and queried with `-readonly`
plus `PRAGMA query_only=ON`. Its containing directory may need writable WAL
locking/sidecar files; this does not authorize canonical OHLCV writes.

## NATS: retained evidence and invalidation

Current subjects: `lib/src/transport/message_subjects.h`.
Serialization/types: `message_json.*`, `contracts/market_messages.h` and
`contracts/execution_messages.h`, plus individual decision/account contracts.

Relevant subjects include:

```text
market.data.updated.v1
strategy.intents.v1
decision.batch.v1
execution.plan.notional.request.v1
execution.plan.notional.v1
execution.event.order_update.v1
execution.event.fill.v1
execution.account.snapshot.v1
execution.cycle.complete.v1
execution.exchange.snapshot.v1
```

The dashboard reads bounded retained exchange evidence and uses a read-only subscriber
for UI invalidation. It does not publish trading commands or create a dashboard-owned
trading consumer. Retained evidence must carry freshness/identity; an old snapshot
cannot be declared a current reconciliation result.

## Implemented read models

| Resource | Sources and current limits |
| --- | --- |
| Shell / global readiness | Source diagnostics, runtime/checkpoint/market/reconciliation evidence and alerts. Private execution, common service liveness and host clock synchronization remain unproven. |
| Overview | Runtime cash/positions, canonical closes and fills. Complete cost basis, durable attributed PnL and equity history remain unavailable. |
| Positions | Runtime quantities/orders, strategy positions, approved targets and timestamped canonical prices. Missing valuation stays unavailable. |
| Execution | Runtime order snapshot, bounded fills and planner evidence. Unpersisted latency/slippage/replacement lineage is not inferred. |
| Reconciliation | Local snapshot plus retained exchange snapshot through the verified reconciler; missing/stale evidence stays pending. |
| Pipeline / Why | Timestamp-aligned daily checkpoint chain and order/fill proof. Quiet/HOLD cycles retain observed assets and explain empty lineage; absent first decision/planner remains pending. Replay uses market/account/retained-order observations without claiming cycle-aligned proof. Unpersisted intermediate transformations remain unavailable. |
| Risk | Exact approved DecisionBatch and persisted signals/account inputs; policy from `portfolio_risk_service_metadata.portfolio_config`. New cycles store optional `risk_diagnostics_payload` atomically in the risk decision checkpoint: sized/capped weights, annualized volatility, cap reductions, HOLD/FLAT/TARGET_WEIGHT actions and cycle configuration identity. Its SHA-256 is the displayed fingerprint. Old schemas/rows and canonical replay snapshots retain unavailable evaluations. Combined approved weights use combined notionals/reference capital. Whole-account/venue breaches and marked real PnL remain unavailable. |
| Market Data | Canonical daily ranking/OHLCV, formula-checked indicators and date-aligned signals. Candidate rejection explanations are not first-class persisted evidence. |
| Infrastructure | PostgreSQL/NATS diagnostics and canonical file probes. Optional `DASHBOARD_HOST_METRICS_FILE` supplies fresh CPU/RAM/disk and project container observations; unavailable/stale telemetry stays unknown. The paper stack configures this read-only snapshot without a Docker socket mount. Container health does not prove pipeline readiness or clock sync. |
| Ledger | Immutable fill-derived cash/asset deltas, aggregate diagnostics and deterministic recent-window fingerprint. Not durable cost basis, realized/unrealized PnL or historical equity. |
| Live vs Expected | Versioned/fingerprinted market and strategy-input anomaly projection. Execution, accounting and accepted-replay baseline families remain deferred. |
| Alerts / Audit | Derived evidence plus durable watchdog lifecycle, manual-intent audit, human acknowledgements and notifier status. These are observability records, not fills. |
| Manual Control | Current risk target/reference, canonical universe, symbol registry and public venue rules. Preview/admission/audit implemented; actual submission disabled. |

Provider dispatch/builders are under `dashboard-api/internal/provider/`.
`real_mapping.go` still contains some historical integration-planning labels.
Current dispatch, response capability flags and readiness handlers take precedence;
the catalog is not proof that an action may execute.

## Separate observability stores

`dashboard-watchdog` records alert lifecycle independently of browser/API sessions.
The API reads that store read-only. Manual operator-intent audit and acknowledgement
stores are separate durable append-only observability data.

Acknowledgements bind to the current durable lifecycle event and are idempotent for
that instance. An ACK neither resolves the alert nor changes readiness/trading state.
The independent notifier persists delivery/deduplication evidence. `TEST_FILE`
is the default sink; the optional Telegram adapter takes deployment-only credentials
and has its own retry/receipt state. No notification is an execution command.

Production volumes must preserve these stores across restart. Session storage remains
in-memory and is unrelated to durable alerts/audit.

## Public venue and registry boundary

The public client queries fixed Hyperliquid TESTNET metadata/mid-price operations.
Symbol registry and precision/rule checks are explicit and detect mapping drift.
These public reads do not verify private authentication, account balances, signing,
submit/cancel/fill behavior or MAINNET readiness.

See [API_CONTRACT.md](API_CONTRACT.md) for routes and
[the dashboard README](../README.md) for current overlays and run commands.

## Remaining runtime-owned work

Complete accounting needs a durable runtime-owned cost-basis/realized/unrealized-PnL
ledger and historical equity projection. New service Risk evaluations persist sizing
and strategy constraint provenance; whole-account/venue breaches and broader Pipeline
transforms remain pending. In-loop application heartbeats/control responsiveness
remain pending. The host collector's optional fresh snapshot includes external trading
executable presence/state and systemd NTP synchronization status, with unavailable
probes kept UNKNOWN. Infrastructure observes daily PAPER progress from persisted
last_bar_close_timestamp/last_execution_timestamp, with an explicit 30-minute UTC
rollover grace; neither updated_at nor fills substitute for business dates. These
observations do not establish private trading readiness. Clock offset remains unmeasured.
Private venue execution and trading-control
delivery must use the normal strategy/risk/planning/execution lifecycle.

These gaps must be filled at their owners; dashboard reconstruction cannot establish
canonical economic truth.
