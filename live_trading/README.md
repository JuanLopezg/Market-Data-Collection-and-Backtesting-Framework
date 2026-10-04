# Live Trading

This directory contains the executable services that connect the trading domain library to
market data, durable messaging, persistence, planning, and exchange execution.

The code is intentionally organized so a new reader can start at a tiny `*_main.cpp`, jump to
one `*_application.cpp`, and then follow the domain types named in that application. The
application layer should explain **when** and **why** components are called. Trading formulas
and reusable domain behavior belong in the library, not here.

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

## Simple mental model

The main live message flow visible in this directory is:

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
    -> exchange commands through MessageBusExchange
    -> Exchange gateway
    -> exchange backend / simulated exchange
    -> OrderUpdate + Fill + ExchangeSnapshot
    -> Execution-state service
    -> updated AccountSnapshot / ExecutionCycleComplete
```

The exact contract types and subjects are defined outside this directory. Do not infer a
contract from a filename: follow the concrete message type and `TransportSubjects` value used
by the application.

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

A recurring safety pattern is **persist before publish/ack**. Do not reverse that ordering
without understanding crash recovery and idempotency.

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
    -> tiny executable entrypoint
*_application.h
    -> one public run function
*_application.cpp
    -> CLI options
    -> persistence/recovery helpers (if local to the service)
    -> runtime class
    -> message handlers
    -> service loop
other *.h/*.cpp
    -> focused reusable components owned by that service
```

The application file may still be long when it represents one cohesive state machine. Prefer a
clear single state machine with named sections over scattering one runtime class across many tiny
files. Split a component only when it has a genuinely independent responsibility.

## Readability rules for this directory

- Code, comments, log messages, and technical names are in English.
- Prefer explicit code over clever code.
- Prefer a descriptive 10-line function over a dense 3-line expression.
- Keep `main()` boring.
- Comments explain *why* ordering or state matters; they do not narrate obvious syntax.
- Avoid historical `PATCH XX` / `STEP XX` comments in active code. Describe the current rule.
- Do not duplicate trading calculations in services if the domain library already owns them.
- Do not silently mix business time with technical time.
- Do not hide message ordering, durability, or idempotency requirements.
- A new reader should be able to answer: what does this service consume, what does it change,
  what does it persist, and what does it publish?

## Building

The repository Meson build owns these executables. From the repository root, the normal check is:

```bash
meson setup <build-dir>
meson compile -C <build-dir>
```

After structural changes that could affect canonical replay behavior, also run the repository's
canonical release gate. Build/test execution is intentionally left to the developer so validation
happens in the real local environment.
