# Live Trading

This directory contains the executable services that connect the trading domain library to
market data, durable messaging, persistence, planning, and exchange execution.

Start at each service's `*_main.cpp`: its real startup, recovery, subscriptions and service
loop remain visible there. Follow its domain components into `lib/` for trading formulas
and reusable behavior. Separate service files exist for actual responsibilities, such as
ingestion and HTTP serving. Each service has one `src/meson.build`; the top-level
build enters it directly without an intermediate forwarding build file.

PAPER defaults to quote-notional liquidity: actual Binance daily quote turnover,
SMA 25, top 20 inside the exchange top-50 candidate set. The ingestor stores and
backfills `ohlcv_data.quote_volume` for completed candles; invalid quote data prevents
publication. The strategy refuses missing required quote history. Ordinary LIVE and
the canonical historical profile keep their existing configuration. Existing PAPER
upgrades require a backed-up, checked configuration-identity migration preserving
completed checkpoints; see `deploy/paper_trading/README.md`.

## Start here

Read the services in this order if you are new to the project:

1. `market_data_service` — gets the latest completed daily market data and commits it to SQLite.
2. `strategy_service` — reads committed history and publishes strategy intents.
3. `portfolio_risk_service` — combines strategy intents, account state, sizing, and risk rules.
4. `execution_state_service` — owns durable execution/account state and coordinates each execution cycle.
5. `order_planner_service` — converts a notional planning request into an executable notional plan.
6. `exchange_gateway` — forwards exchange commands and normalizes exchange events.
7. `simulated_exchange_service` — durable exchange backend used when a real exchange is not used.
8. `historical_market_data_service` — historical/replay source that preserves the same anti-lookahead boundary.
9. `binance_simulator` — standalone local HTTP simulator for Binance-style market-data requests.
10. `execution_service` — direct execution-engine service path; inspect deployment/configuration before assuming it replaces `execution_state_service`.

## Service message flow

The full service/component execution chain is shown below. The current default
`deploy/live/` topology stops at `NotionalOrderPlan`; its execution-state
service does not submit private orders. The gateway is an optional transport-only
profile. Treat the downstream chain as a component responsibility map, not proof
that the default LIVE deployment has exchange execution connected.

The separate [paper deployment](../deploy/paper_trading/README.md) connects these
services to the simulated exchange using current public data. Opt-in ingestion
publishes only open(T+1) for execution, outside completed feature bars. It does not
enable private orders in default LIVE.

The chain is:

```text
Market data commit
    -> MarketDataUpdated
    -> Strategy service
    -> StrategyIntentBatch
    -> Portfolio/risk service  <--- AccountSnapshot
    -> DecisionBatch
    -> Execution-state service
    -> NotionalOrderPlanningRequest
    -> Order-planner service
    -> NotionalOrderPlan
    -> Execution-state service
    -> exchange commands through MessageExchange
    -> Exchange gateway
    -> exchange backend / simulated exchange
    -> OrderUpdate + Fill + ExchangeSnapshot
    -> Execution-state service
    -> updated AccountSnapshot / ExecutionCycleComplete
```

The exact contract types and subjects are defined outside this directory. Do not infer a
contract from a filename: follow the concrete message type and `MessageSubjects` value used
by the service.

## What each service owns

| Service | Main responsibility | Important inputs | Important outputs |
| --- | --- | --- | --- |
| `market_data_service` | Daily live ingestion | Binance REST + config | SQLite commit + `MarketDataUpdated` |
| `historical_market_data_service` | Anti-lookahead replay feed | Historical CSV + business time | SQLite commit, `MarketDataUpdated`, execution-open prices |
| `strategy_service` | Strategy evaluation | `MarketDataUpdated` + canonical history | `StrategyIntentBatch` |
| `portfolio_risk_service` | Portfolio sizing/risk decisions | Strategy intents + account snapshot + history | `DecisionBatch` |
| `order_planner_service` | Order-plan construction | `NotionalOrderPlanningRequest` | `NotionalOrderPlan` |
| `execution_state_service` | Durable execution/account state | decisions, plans, market updates, exchange events | account snapshots, planning requests, execution-cycle completion |
| `exchange_gateway` | Exchange boundary | submit/cancel/snapshot commands | normalized order/fill/snapshot events |
| `simulated_exchange_service` | Durable simulated backend | execution prices + backend commands | backend order/fill/snapshot events |
| `execution_service` | Direct execution-engine path | decisions, prices, order updates, fills | persisted trading state / exchange actions |
| `binance_simulator` | Local market-data HTTP simulation | historical CSV | Binance-compatible HTTP responses |

## Market-data files

Within `market_data_service/src/`, filenames describe their local responsibility:

| Files | Responsibility |
| --- | --- |
| `market_data_service_main.cpp` | Startup, daily schedule, retries and shutdown |
| `config.h/.cpp` | Configuration values and JSON validation |
| `binance_client.h/.cpp` | Binance REST calls and bounded parallel downloads |
| `ingestion.h/.cpp` | One daily refresh, validation and commit coordination |
| `market_store.h/.cpp` | Canonical SQLite transactions and tracked-symbol history |
| `update_publisher.h/.cpp` | Durable notification after a successful commit |

The historical feed shares `market_store` so both writers use the same schema and commit
rules. The HTTP simulator keeps its own candle store and HTTP server; it serves requests
rather than writing the canonical database. These are useful responsibility boundaries,
so they remain separate files. Service entrypoints retain their descriptive `*_main.cpp`
names for navigation and operational tooling.

## Time has two meanings

This code deliberately distinguishes two time domains:

- **Business time** decides which market day is economically visible. `TimeHandler` controls it.
- **Technical time** controls polling, retry backoff, and process responsiveness. It uses real wall time.

Never use technical sleeps to decide whether a candle, signal, order, or fill is economically
allowed. Never accelerate retry/backoff just because replay business time is accelerated.

## Durability and message handling

When reading a message handler, look for the return value:

- `Ack` means the message was handled durably and does not need redelivery.
- `Retry` means the service is not ready or hit a recoverable failure; redelivery is required.
- `Terminate` means the message is invalid for this consumer and retrying it would not help.

For durable routes, the ordering depends on which state the service owns:

- Market-data ingestion commits SQLite before publishing its update notification.
- Strategy, portfolio/risk, and order-planner services publish deterministic output, then
  checkpoint, then acknowledge their input. A crash between publish and checkpoint causes
  the same logical message to be published again.
- Execution persists local order/cancel intent before dispatching exchange commands.
- With PostgreSQL persistence, the simulated backend commits account/order state and its
  outbox together, then publishes
  the outbox. A restart can finish publication without applying the fills again.

Keep these existing orders explicit; changing them changes crash recovery.

## Logs

Operational logs are written in English. Service logs should prefer structured fields:

```text
service=<name> event=<event_name> symbol=<...> order_id=<...> reason=<...>
```

Use:

- `DEBUG` for detailed state useful during investigation.
- `INFO` for normal lifecycle/business events.
- `WARN` for recoverable abnormal situations.
- `ERROR` for failed operations that require retry/recovery.
- `ALERT`/fatal-level logging for process-ending failures.

Help text printed to the terminal may use `stdout`; operational failures should use the service
logger where that service has logging initialized.

## How to read one service

For a typical service:

```text
*_main.cpp
    -> CLI options
    -> persistence/recovery helpers (if local to the service)
    -> runtime class
    -> message handlers
    -> service loop
other *.h/*.cpp
    -> focused reusable components owned by that service
```

The main file may still be long when it represents one cohesive state machine. Prefer a
clear single state machine with named sections over scattering one runtime class across many tiny
files. Split a component only when it has a genuinely independent responsibility.

## Readability rules for this directory

- Code, comments, log messages, and technical names are in English.
- Prefer explicit code over clever code.
- Prefer a descriptive 10-line function over a dense 3-line expression.
- Keep real orchestration readable in `main()`; do not introduce forwarding-only application wrappers.
- Comments explain *why* ordering or state matters; they do not narrate obvious syntax.
- Avoid historical `PATCH XX` / `STEP XX` comments in active code. Describe the current rule.
- Do not duplicate trading calculations in services if the domain library already owns them.
- Do not silently mix business time with technical time.
- Do not hide message ordering, durability, or idempotency requirements.
- A new reader should be able to answer: what does this service consume, what does it change,
  what does it persist, and what does it publish?

## Building

Use WSL with the repository Meson build. From the repository root:

```bash
meson compile -C build -j 3
bash validation/live_pre_exchange_e2e_audit.sh
bash validation/market_data_time_handler_audit.sh
bash validation/historical_visibility_no_lookahead_test.sh
```

For a new build directory, run `meson setup <build-dir>` first. Cross-service structural
changes also require the canonical replay release gate:

```bash
bash validation/step59_canonical_replay_release_gate.sh
```

The compact gate proves canonical replay behavior; it does not replace broker/database
integration acceptance for the deployed live services.
