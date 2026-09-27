# Real data source map — Step 14 verified audit

Audit basis: uploaded project snapshot reviewed on 2026-09-24. This document replaces the Step 13 "candidate source" worksheet with source mappings verified against the actual C++ contracts, persistence code and live deployment topology.

`VERIFIED_BASE` means the current codebase contains a canonical source sufficient for the core of that page/resource. It does **not** mean the Step 14 dashboard API has connected to it yet.

## Resource map

| Dashboard resource | Status | Canonical/verified source | Important gaps before full page parity |
| --- | --- | --- | --- |
| Shell status | PARTIAL / REAL AGGREGATE | PostgreSQL/NATS diagnostics; durable runtime snapshot; canonical market cycle; retained reconciliation evidence; derived alerts | Step 27 serves the real fail-closed aggregate; exchange connectivity, common service liveness/trading state and host clock sync remain unverified |
| Overview | PARTIAL | `trading_runtime_state.snapshot`, canonical SQLite closes, `trading_fills` | No accounting ledger/current PnL read model; no bounded equity curve / rolling Sharpe store |
| Positions | VERIFIED_BASE | `trading_runtime_state.snapshot.account_positions`, strategy positions/orders, latest PortfolioRisk decision, SQLite closes | Derived weights/notionals must carry exact valuation timestamp |
| Reconciliation | PARTIAL | `execution.exchange.snapshot.v1` + local runtime state | Latest snapshot/report is not durably persisted; `reconciled_` is ExecutionState memory only |
| Pipeline | PARTIAL | Strategy checkpoint → PortfolioRisk decision checkpoint → OrderPlanner checkpoint → ExecutionState orders | Live pre/post risk diagnostics are absent; RSI/rank must be recomputed from canonical market data/config |
| Execution | VERIFIED_BASE | current tracked orders in runtime snapshot; append-only `trading_fills`; order-planner plan payloads; order/fill subjects | Historical order lifecycle is not append-only persisted separately from the current order snapshot |
| Risk | PARTIAL | portfolio config + PortfolioRisk decision checkpoints + local positions/prices | No first-class live risk diagnostic payload showing raw sizing / binding rule transformations |
| Market Data | VERIFIED_BASE | canonical SQLite `ohlcv_data`, `market_volume_rank_daily`, `tracked_pairs`; `market.data.updated.v1`; strategy checkpoint | Candidate rejection reason is not persisted as a first-class contract |
| Infrastructure | PARTIAL | PostgreSQL connection, NATS monitor/JetStream, Docker deployment topology | No service HTTP health contract; no safe host/container metrics adapter yet |
| Alerts & Audit | PARTIAL | derived PostgreSQL/NATS/reconciliation/execution/market/risk evidence | No append-only alert history, acknowledgement/resolution state or human actor audit table |
| Live vs Expected | REAL / LIMITED | canonical SQLite market history + deterministic indicator reconstruction | Step 45 formalizes a versioned/fingerprinted real anomaly projection for market/strategy-input behaviour. Execution, accounting/PnL and accepted-replay baseline families remain explicit DEFERRED coverage. |
| Manual Control | PARTIAL / FAIL-CLOSED | authenticated dashboard preview + explicit symbol registry + public venue rules + isolated append-only operator-intent audit | Step 46 validates preview/admission, binds request hash + reference target, and durably audits OPERATOR intent; actual trading-control sink, PortfolioRisk manual transformation, private auth and order lifecycle remain DEFERRED |

## Verified PostgreSQL sources

The following tables are created by the current runtime itself.

### ExecutionState durable state

Source: `lib/src/persistence/postgres_state_store.cpp`

- `trading_runtime_state`
  - singleton runtime snapshot as JSONB
  - `schema_version`
  - `snapshot`
  - `updated_at`
- `trading_fills`
  - append-only by `fill_id`
  - `order_id`, `strategy_id`, `timestamp`, `coin`, `side`, `quantity`, `price`, `commission`

Verified JSON keys in `trading_runtime_state.snapshot`:

- `schema_version`
- `last_bar_close_timestamp`
- `last_execution_timestamp`
- `next_order_id`
- `account_cash`
- `account_positions`
- `strategies`
- `pending_plans`
- `orders`
- `processed_fill_ids`

Each tracked order currently contains the fields needed for the current lifecycle view: order id, strategy id, created/active timestamps, coin, side, quantity, status, filled quantity, updated timestamp, cancel flag, exchange order id and last message.

### Strategy checkpoint

Source: `live_trading/strategy_service/src/strategy_service_main.cpp`

- `strategy_service_metadata`
- `strategy_market_update_checkpoint`
  - `timestamp`
  - `update_payload`
  - `intent_payload`

This is a durable daily bridge from `MarketDataUpdated` to `StrategyIntentBatch` and is useful for Pipeline/Why history.

### PortfolioRisk checkpoints

Source: `live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp`

- `portfolio_risk_service_metadata`
- `portfolio_risk_live_account_checkpoint`
  - bounded tail of account snapshots used for the daily join
- `portfolio_risk_live_decision_checkpoint`
  - `signals_payload`
  - `account_payload`
  - `decision_payload`

This is the strongest current source for Strategy → account → approved DecisionBatch traceability.

### OrderPlanner checkpoint

Source: `live_trading/order_planner_service/src/order_planner_service_main.cpp`

- `order_planner_live_notional_checkpoint`
  - `timestamp`
  - `request_payload`
  - `plan_payload`

This preserves the notional planning request and resulting plan by decision timestamp.

## Verified canonical SQLite market-data source

Source: `live_trading/market_data_service/src/market_data_store.cpp` and `lib/src/market/canonical_market_data_reader.cpp`.

Tables:

- `tracked_pairs`
- `ohlcv_data(pair,date,open,high,low,close,volume)`
- `date_of_start`
- `market_volume_rank_daily(date,rank,pair,quote_volume)`

The runtime explicitly commits canonical SQLite state before publishing `MARKET_DATA_UPDATED`. Dashboard reads of this database must therefore be **read-only** and timestamp-aware.

This source is suitable for:

- latest completed market date
- freshness by asset
- OHLCV windows
- latest/reference closes for equity/weights
- liquidity universe/rank
- recomputation of strategy indicators for targeted Why diagnostics

## Verified NATS / JetStream subjects

Source: `lib/src/transport/transport_subjects.h`.

Core subjects relevant to dashboard observability:

```text
market.data.updated.v1
strategy.intents.v1
decision.batch.v1
execution.plan.notional.request.v1
execution.plan.notional.v1
execution.command.submit.v1
execution.command.cancel.v1
execution.event.order_update.v1
execution.event.fill.v1
execution.account.snapshot.v1
execution.cycle.complete.v1
execution.exchange.snapshot.v1
execution.exchange.snapshot.request.v1
```

Other canonical runtime subjects also exist (`market.release.v1`, market slice events, execution prices and older quantity-plan boundaries). The Go constants in `dashboard-api/internal/integration/nats/subjects.go` mirror the audited C++ strings but are not connected in Step 14.

## Verified wire contracts useful to the dashboard

All transport DTOs share `ContractMetadata`:

```text
schema_version
message_id
correlation_id
produced_at
```

Important payloads:

### AccountSnapshot

Owned/published by ExecutionState:

```text
timestamp
cash
positions
strategy_positions
```

### StrategyIntentBatch

```text
timestamp
strategies[]:
  strategy_id
  strategy_name
  signals{coin -> [-1,+1]}
```

It is signal state only; it deliberately contains no sizing, capital, targets or orders.

### DecisionBatch

Produced by PortfolioRisk:

```text
decision_timestamp
strategies[]:
  strategy_id
  decision_timestamp
  reference_capital
  target_notional_usd
  decisions[] { coin, action, target_weight }
```

This is already the approved economic target boundary, not the raw pre-risk target.

### NotionalOrderPlanBatch

Produced by OrderPlanner:

```text
decision_timestamp
state_revision
decisions
reference_closes
next_order_id
cancel_order_ids
submit_orders[]
global_target_notional_usd
```

Each planned order includes:

```text
economic_order_id
order_id
strategy_id
created_at
decision_timestamp
state_revision
coin
side
reference_close
target_notional_usd
current_notional_usd
pending_notional_usd
delta_notional_usd
notional_usd
```

This is excellent input for the detailed Pipeline and Execution pages.

### OrderUpdateEvent / FillEvent

Order updates carry lifecycle status, exchange order id and a message/reject reason. Fill carries executed quantity, fill price and commission. ExecutionState persists fills append-only and current tracked order state inside its snapshot.

### ExchangeSnapshotEvent

Contains normalized venue truth:

```text
timestamp
cash
positions
open_orders[]
```

ExecutionState compares it with local persisted state through `Reconciler`. However, the latest exchange snapshot and `ReconciliationReport` are currently not persisted for dashboard reads.

## Current live topology fact that matters

`deploy/live/docker-compose.yml` is currently a **pre-exchange** topology. `exchange-gateway` is under the `exchange-edge` profile and is not started by default. Therefore the dashboard must not report a live exchange as connected simply because the gateway code/contracts exist.

The current live deployment does start:

- NATS / JetStream
- PostgreSQL
- MarketData
- Strategy
- PortfolioRisk
- ExecutionState
- OrderPlanner

## Derived values that are safe to compute in dashboard-api

Provided the valuation timestamp is explicit and canonical prices are used:

- current equity = cash + Σ(position qty × canonical close)
- position current notional
- position current weight
- gross/net exposure
- asset-cap utilisation from current positions vs verified config
- order remaining quantity from tracked-order quantity/fills
- fill fees from `trading_fills`

The dashboard should **not** independently reproduce trading decisions and call the result canonical. Recomputing RSI/rank for explainability is acceptable only as a diagnostic view anchored to the same canonical market window/config; approved targets must continue to come from PortfolioRisk checkpoints.

## Gaps that require runtime/product work rather than SQL guessing

1. **Accounting / PnL ledger** — fills and current cash/positions exist, but a durable economic ledger with realized/unrealized attribution does not yet exist.
2. **Reconciliation read model** — `ReconciliationReport` and clean/blocked state need a durable/observable projection.
3. **Risk transformation diagnostics** — current live `DecisionBatch` exposes the approved output, not raw target → sizing → constraint diagnostics.
4. **Service readiness/heartbeats** — there is no normalized health contract for every service.
5. **Alerts/Audit** — structured logs are not a durable alert/audit product model.
6. **Live-vs-Expected baseline** — must be versioned and materialized intentionally.
7. **Manual control API** — must be built as a safe business workflow, never browser → exchange.

## Step 17 status and next recommendation

Step 16 enabled **Infrastructure**. Step 17 enables **Positions** from the verified durable `trading_runtime_state.snapshot`, including physical account quantities and per-strategy virtual quantities. Price valuation, approved targets, PnL and exchange reconciliation remain explicitly unavailable.

Next: implement real **Execution** from tracked orders in the runtime snapshot plus append-only `trading_fills`. Do not fabricate lifecycle history that is not persisted.


## Step 18 update
`Execution` now has a bounded real read model from `trading_runtime_state.snapshot.orders` + `trading_fills`. Latency/slippage/replacement lineage remain unavailable unless a canonical durable source is added.


## Step 19 reconciliation read

Enabled source path: `trading_runtime_state.snapshot` + JetStream latest-by-subject `execution.exchange.snapshot.v1` in `ALGOTRADING_RUNTIME`. This is a read-only reconstruction of the verified `Reconciler::compare` boundary. Planner target quantities are intentionally not inferred here.


## Step 20 — Pipeline / Why

Real Pipeline lineage is now read-only and cycle-aligned across the durable Strategy, PortfolioRisk and OrderPlanner checkpoints, with ExecutionState/reconciliation enrichment. The risk checkpoint is the anchor because it persists the exact StrategyIntentBatch and AccountSnapshot used to produce the DecisionBatch. Missing planner data is `PENDING`; no cross-cycle mixing is allowed. RSI/rank and intermediate risk transforms remain unavailable unless a canonical persisted source is added.


## Step 21 — Risk

Real Risk now reads the latest `portfolio_risk_live_decision_checkpoint` as the canonical approved economic boundary. The page exposes approved target weights/notionals and the exact persisted signal/account inputs. Active policy config, intermediate transformations and breach reports remain unavailable until a first-class runtime/config projection exists.


## Step 22 — Market Data

Enabled as a real read-only resource. Canonical frontier and OHLCV come from `market_volume_rank_daily` + `ohlcv_data`. The current PureRSI universe is recomputed as top 20 by SMA Volume(25) inside the configured canonical top-N, and RSI(7) uses the same Wilder implementation as the C++ runtime. `strategy_market_update_checkpoint` is joined only on exact timestamp equality; otherwise signal state is explicitly not aligned.

## Step 23 — Overview

| Overview block | Canonical source | Status |
|---|---|---|
| Cash / physical positions | `trading_runtime_state.snapshot` | REAL |
| Approved gross target | `portfolio_risk_live_decision_checkpoint` | REAL |
| Execution observations | `trading_runtime_state.snapshot.orders` + `trading_fills` | REAL |
| Reconciliation summary | retained `execution.exchange.snapshot.v1` + durable local state | REAL / PENDING if evidence unavailable or stale |
| Market-cycle health | canonical SQLite + exact-cycle Strategy checkpoint | REAL |
| Total equity / PnL / equity curve | canonical accounting/mark-to-market projection | MISSING PROJECTION |
| Live vs Expected | canonical rolling market-input baseline | REAL / LIMITED — Step 45 versioned/fingerprinted anomaly projection; accepted replay/accounting/execution baseline artifacts remain DEFERRED |
| Global READY/PAUSED aggregation | Step 27 shell aggregate | REAL fail-closed aggregate; READY withheld until remaining health contracts exist |


## Step 24 — Alerts & Audit

The dashboard now derives current operational conditions from real, already-wired sources. This is intentionally not called a complete durable alert/audit product model. Canonical system evidence can be shown, but historical alert lifecycle, acknowledgement/resolution state and human/operator action audit require new persistence contracts before they can be represented as durable truth.

## Step 27 — Global Shell / Readiness

`/api/shell-status` is now an enabled real read-only aggregate. It combines PostgreSQL/NATS source health, durable ExecutionState presence, canonical MarketData/Strategy cycle diagnostics, retained reconciliation evidence and current derived alert counts. It does not infer ExchangeGateway connectivity, process liveness, global trading-enabled state or host clock synchronization; therefore `READY` remains fail-closed until those missing contracts are added.

## Step 28 — SSE / NATS invalidation bridge

| Stream element | Source | Status |
|---|---|---|
| Browser transport | authenticated `GET /api/stream` SSE | REAL |
| Invalidation source | read-only subscriptions to audited runtime NATS subjects | REAL |
| Page payloads | existing authenticated REST read models | REAL; unchanged |
| Burst handling | 175 ms resource coalescing | REAL |
| Heartbeat | 15 s SSE comment | REAL |
| Fail-safe mounted-view refresh | 30 s | REAL |
| Browser direct NATS access | none | INTENTIONALLY ABSENT |
| Dashboard NATS publish/control | none | INTENTIONALLY ABSENT |
| Durable event/audit history | dedicated projection/store | STILL MISSING |

## Step 33 venue foundation update

The dashboard now has explicit **execution venue identity** sourced from deployment configuration and audited against the current C++ ExchangeGateway dry-run boundary. This is not connectivity evidence.

Current identity:

- execution venue: `HYPERLIQUID`;
- target environment: `TESTNET`;
- gateway mode: `hyperliquid-dry-run`;
- public connectivity: not checked until Step 34;
- private auth/order routing: disabled.

`Infrastructure.exchange.connected` therefore remains `false` and its health remains `UNKNOWN` in Step 33. No exchange readiness is inferred from configuration alone.

## Step 34 public venue connectivity update

A new read-only source is available directly to `dashboard-api`:

| Read model | External source | Method | Safety |
|---|---|---|---|
| Venue public connectivity | Hyperliquid TESTNET `https://api.hyperliquid-testnet.xyz/info` | `POST {"type":"meta"}` + `POST {"type":"allMids"}` | Public only; no secret/signature/account/order action |

The public probe proves that the configured Hyperliquid TESTNET endpoint is reachable and returns structurally usable perpetual metadata and mids. It deliberately does **not** make the C++ ExchangeGateway connected, authenticated, or trading-ready. The existing execution-connectivity read model remains `UNKNOWN` until an owner-side runtime contract exists.

Step 35 may use the observed venue universe to validate an explicit internal-asset -> execution-symbol mapping, but the mapping must remain explicitly configured rather than inferred heuristically.

## Step 35 update

Venue symbol identity is now an explicit read-only mapping contract. The required set is the current canonical strategy universe from SQLite/Strategy diagnostics; target coin names are checked against current Hyperliquid TESTNET public metadata. The dashboard does not infer `BTCUSDT -> BTC` or any other suffix/alias transformation at runtime. Missing or stale mappings block the Step 35 gate.

## Step 36 update — public trading-rule diagnostics

For Step 35-supported Hyperliquid TESTNET mappings only, the dashboard now reads public `meta` and `allMids` and builds a read-only rule projection containing `szDecimals`, size step, price precision limits, max leverage, margin flags, delisted state, public mid and a diagnostic quantity estimate for the 10 USD minimum order-notional floor.

The projection never mutates Strategy/Risk/Planner state and is not consumed by an order-routing path. Unsupported or venue-absent Step 35 symbols remain outside executable coverage. Before private testnet execution is enabled, the execution adapter/order planner must consume a separately reviewed canonical venue-rule contract rather than treating dashboard diagnostics as the trading authority.

## Step 37A update — multi-exchange symbol registry

Symbol identity is no longer treated only as a Hyperliquid mapping concern. A versioned explicit registry now represents the chain `market-data symbol -> internal symbol -> execution venue symbol` and is designed to hold additional explicit venue legs later.

Current observed inputs:

- canonical Binance/source symbols from the SQLite ranking / strategy diagnostics;
- accepted Step 35 internal -> Hyperliquid TESTNET mapping/classification;
- current public Hyperliquid TESTNET metadata.

The dashboard raises derived alarms when a current market-data/strategy symbol is unregistered, when the registry disagrees with the accepted Step 35 manifest, or when a mapped execution symbol disappears from current venue metadata. These alarms do not mutate Strategy/Risk/Planner state and do not create an order route.

## Step 42 update

The dashboard now exposes a bounded, deterministic read-only ledger foundation derived from the verified append-only `trading_fills` source. Fill identity/economic validity and complete-table aggregates are checked server-side; the browser receives only a bounded recent event window. This improves observability but does not change ownership of accounting state: durable cost basis, realized/unrealized PnL attribution and historical equity remain runtime/product gaps.

## Step 43 observability-owned durable alert store

Step 43 adds an **observability-only** append-only lifecycle store owned by the separate `dashboard-watchdog` process. It is not a trading source of truth and never feeds order/risk/execution decisions.

- Current alert inputs: existing canonical read-only dashboard sources plus Step 37A symbol-registry validation and Step 42 ledger-integrity validation.
- Durable lifecycle file: `/data/watchdog/events.jsonl` in Docker volume `dashboard-watchdog-data`.
- Heartbeat/status file: `/data/watchdog/status.json`, atomically replaced by the watchdog.
- `dashboard-api` mounts this volume read-only.
- Lifecycle transitions: `OPENED`, `UPDATED`, `RESOLVED`.
- Human acknowledgement is not persisted by Step 43.
- Trading PostgreSQL and canonical market-data DB remain read-only from the dashboard/watchdog boundary.

## Step 44 global readiness aggregate

`GET /api/global-readiness` is a read-only aggregate over existing canonical/read-only evidence. It does not become a new trading source of truth. The endpoint correlates provider/source health, public venue evidence, symbol registry, ledger, watchdog, manual-route state and explicit deferred private/live contracts. `VALIDATED_FAIL_CLOSED` means only that dashboard work may continue without a verified contradiction; it never means orders may be routed.

The contract also records the future replay boundary: a `MockExchangeAdapter` must sit behind the same canonical venue-adapter interface as Hyperliquid so replay drives the same order/fill/reconciliation/ledger/read-model path observed by the dashboard.


## Step 46 update — Manual Control safe routing admission

Step 46 adds a dashboard-owned **admission contract**, not an execution route. `POST /api/manual-control/route` is OPERATOR + CSRF protected, requires an explicit confirmation token, recomputes the CSV preview/hash server-side, verifies the current target reference, and rechecks explicit registry/public venue-rule evidence. Every request remains `submitted=false` and `routeEnabled=false` until a separate trading-control/PortfolioRisk/private-venue lifecycle exists.

Operator intent is written only to the isolated append-only `/data/manual-audit/events.jsonl` volume owned by `dashboard-api`; it is projected into Alerts & Audit as HUMAN evidence. This store is observability/audit state only and is not trading state. The dashboard still has no NATS trading publish, exchange submit/cancel, signing or order mutation path.

## Step 46A update — durable human acknowledgement

Alert acknowledgement is now a separate observability-owned append-only source: `/data/alert-ack/events.jsonl` in `dashboard-alert-ack-data`. It is written only by `dashboard-api` after OPERATOR + CSRF validation and an exact match to the current durable watchdog lifecycle event. The watchdog does not mount this volume. ACK changes presentation/audit state only; unresolved severity continues to count toward readiness and no trading database, NATS subject, exchange endpoint, signer or order state is mutated.
