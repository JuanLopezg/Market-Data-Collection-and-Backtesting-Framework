# Trading library

`lib/` contains the reusable trading logic shared by replay, backtesting and live services.
Start with an orchestration file, then follow the components it calls. Service entrypoints live
in `live_trading/`; research entrypoints live in `research/`.

Daily bars retain source `volume` and optional actual `quote_volume`; NaN means the
older source has no quote turnover. `PriceField::QuoteVolume` supports notional
liquidity selection without changing existing `Volume` semantics or historical dataset units. Canonical SQLite
reads accept old schemas as unavailable quote data. Optional market-slice JSON fields
preserve quote turnover and reject conflicting redeliveries; old wire output is
unchanged when quote data is unavailable. SMA recovers after missing rows leave its
window. PAPER opts into the quote profile; the accepted historical baseline does not.

## Reading the code

For the simulation that is compared with RealTest, read these files in order:

1. `research/replay.py` — public CLI, build, date window and RealTest comparison.
2. `research/src/canonical/canonical_replay.cpp` — configuration, historical OPEN/CLOSE loop,
   dashboard publication and checkpoint/resume.
3. `src/runtime/replay_runtime.h` and `.cpp` — the main economic flow. Start at
   `onClosedSlice()` and `onExecutionOpen()`.
4. `src/strategy/validated/pure_rsi.h` — entry/exit signals, without sizing or fill accounting.
5. `src/risk/portfolio_risk_engine.cpp` — capital allocation, sizing and risk.
6. `src/execution/planning/notional_order_planner.cpp` — orders needed to reach approved targets.
7. `src/exchange/mock/exchange.h` — the simulated venue entrypoint; use the table below
   to follow an order, fill or recovery operation.

For the lightweight research path, start at `src/backtest/backtester.cpp`. It uses
`DecisionEngine` and `ExecutionEngine` through `TradingEngine`; the service-style replay
instead separates signal generation, risk and notional planning. These orchestration paths
have different jobs and are not duplicate implementations to delete indiscriminately.

`src/strategy/validated/` contains concrete strategies with accepted simulation
evidence; PureRSI is currently the only one. Shared interfaces, instances and signal
orchestration stay at the root of `src/strategy/`. Strategies should enter `validated/`
only after their intended behavior has accepted test or replay evidence.

The frozen research runtime and comparison target have been removed. Useful
research experiments use current APIs under
`research/src/common/`; they are separate from the validated catalog. The ignored local
`src/strategy/on_hold/` experiments are outside the supported build.

## Start here

A useful way to read the current system is:

```text
market data
    -> strategy / signal
    -> portfolio sizing + risk
    -> target / rebalance decision
    -> order planning
    -> execution
    -> canonical venue adapter
    -> order/fill/account events
    -> portfolio / persistence / recovery
```

Components live in the folder that describes their work:

- `market/rolling_market_state.*` holds released market history.
- `strategy/strategy_signal_engine.*` runs strategies and produces intent.
- `risk/portfolio_risk_engine.*` applies allocation, sizing, risk and rebalance policy.
- `execution/planning/notional_order_planner.*` plans monetary orders from completed closes.
- `execution/planning/quantity_order_planner.*` plans quantity orders using execution prices.
- `execution/execution_engine.*` owns executable order state and applies fills.
- `utils/time_handler.*` defines business time and its configuration;
  `utils/time_handler_factory.*` parses shared environment settings.

`runtime/` contains only the workflows that connect those components:

- `replay_runtime.*`: deterministic historical simulation and checkpoint state.
- `trading_engine.*`: in-process trading, persistence and recovery coordination.
- `decision_engine.*`: strategy updates and rebalance decisions for the in-process path.
- `manual_trading.h`: manual targets routed through risk, planning and the MOCK venue.

The planners remain separate because monetary planning at close(T) and quantity
resolution at execution prices have different timing responsibilities. Position
containers and the execution-owned `StrategyPositionSnapshot` share
`position/position_state.h`. There are no forwarding headers for former locations.

## Names and file boundaries

The folder names describe responsibilities. Keep the existing flat layout for small
modules such as indicators, risk, sizing and recovery. Execution planning, exchange
implementations and validated strategies have subfolders because they form distinct
parts of larger modules.

Related small definitions share a header:

- `contracts/market_messages.h`: readiness updates, completed slices, replay release requests and close prices.
- `contracts/execution_messages.h`: submit/cancel commands, order/fill events, execution prices and cycle barriers.
- `portfolio/portfolio_targets.h`: convert approved weights into monetary targets and aggregate strategy targets.
- `sizing/portfolio_sizer.h`: sizing interface and its optional diagnostics.

`backtest/backtester.*` runs the historical loop; `backtest/backtest_results.cpp`
exports SQLite and CSV results. `BacktestAccountSnapshot` records cash, balance and
equity history; it is distinct from the service `AccountSnapshot` message and the
namespaced venue account snapshot. The inline context has no empty `.cpp` companion.

`ranker/alphabetical_ranker.h` supplies deterministic symbol ordering when no indicator
ranking is configured. The small default methods of ranking, filtering and universe
selection live beside their interfaces; substantive implementations keep their own
source files.

`data_types/data_types.h` remains the shared vocabulary for bars, identifiers and
compatibility trade/order records. Some research tools still need those records;
remove them only when their consumers have migrated.

## Main domains

| Directory | Responsibility |
| --- | --- |
| `account/` | Account-level state used by trading logic. |
| `analytics/` | Trade records and diagnostics. |
| `backtest/` | Historical backtest context, storage and orchestration. |
| `contracts/` | Stable messages and venue-facing contract types. |
| `data_types/` | Shared market/trading data types. |
| `exchange/` | Canonical venue interfaces, MOCK venue behavior and gateway adapters. |
| `execution/` | Orders, fills, execution plans, order tracking and target resolution. |
| `filter/` | Market/universe filters. |
| `indicator/` | Indicator definitions, calculation and cached indicator access. |
| `logging/` | Shared logger and service logging setup. |
| `market/` | Market-data reading, released history and price snapshots. |
| `persistence/` | Durable trading-state storage. |
| `portfolio/` | Portfolio targets, weights and aggregation. |
| `position/` | Current, target and virtual position state. |
| `ranker/` | Ranking logic used by strategy/universe decisions. |
| `rebalance/` | Rules deciding whether target changes require action. |
| `recovery/` | Startup recovery and reconciliation coordination. |
| `risk/` | Portfolio target approval, constraints and volatility estimation. |
| `runtime/` | High-level domain orchestration. |
| `signal/` | Signal state shared across strategy/runtime boundaries. |
| `sizing/` | Portfolio sizing policies. |
| `strategy/` | Strategy interfaces, instances, signal generation and active strategies. |
| `testing/` | Reusable test doubles. |
| `transport/` | Message buses and contract serialization. |
| `universe/` | Tradable-universe selection. |
| `utils/` | Small cross-cutting helpers such as time, JSON, CSV and database utilities. |

## Important boundaries

### Strategy versus risk

Strategies should describe what they want to hold or how they currently signal. Capital allocation, portfolio sizing and hard risk limits belong outside the strategy.

### Planning versus execution

Planning decides which orders should exist. It must not pretend an order was filled. Execution owns order state and applies actual venue events/fills.

### Execution navigation

`execution/` contains the stateful execution components. Its single `planning/`
subfolder contains pure planning and the immutable state supplied to planners.

| File | Responsibility |
| --- | --- |
| `execution_engine.h/.cpp` | Own execution state; persist commands before dispatch and apply actual fills once. |
| `order_manager.h` | Track orders, remaining quantities, lifecycle transitions and processed fill IDs. |
| `execution_order.h`, `fill.h` | Order/update vocabulary and actual execution records. |
| `planning/notional_order_planner.h/.cpp` | `NotionalOrderPlanner`: plan monetary deltas at completed close(T). |
| `planning/quantity_order_planner.h/.cpp` | `QuantityOrderPlanner`: resolve approved targets using execution prices and build cancel/submit commands. |
| `planning/target_resolution.h` | Strategy monetary targets, quantity targets and their shared resolvers. |
| `planning/quantity_order_builder.h` | Build quantity orders and check that filled plus pending quantities cannot duplicate or overshoot a target. |
| `planning/planning_state.h/.cpp` | Build and fingerprint an immutable execution-state snapshot for planning. |

Read the notional planner for canonical replay; read the quantity planner for the
in-process trading/backtest path. Both treat existing fills and pending orders as
real state. Plans do not mutate positions, and replacement orders wait for cancel
confirmation. `ExecutionReferencePrices` shares `market/price_snapshot.h`; it remains
a reference-price alias, not an assumption about actual fill prices.

### Canonical venue boundary

Higher-level code should use `CanonicalVenueAdapter` and the canonical venue contracts rather than branch on a concrete exchange name. Venue-specific behavior belongs behind an adapter.

### Messaging

`transport/` has one durable messaging interface and one NATS JetStream adapter.
Read the interface first, then message serialization and routing names. Read the
adapter implementation when investigating delivery, acknowledgements or restart binding.

| File | Responsibility |
| --- | --- |
| `message_bus.h` | Message envelope, durable consumer settings and the transport-neutral bus interface. |
| `jetstream_bus.h/.cpp` | `JetStreamBus`: publish, deduplicate, consume, acknowledge and bind durable consumers after restart. |
| `message_json.h/.cpp` | `MessageJson`: encode/decode service messages with the same validated wire representations. |
| `message_subjects.h` | `MessageSubjects`: shared routing names and stream subject lists. |

Services return an acknowledgement decision after their state changes and persistence
succeed. The adapter preserves that ordering. Routing strings and JSON fields are
protocol data; shortening C++ names does not change them.

The unused basic NATS and in-memory adapters were removed. `MessageBus` now exposes
the previously used durable interface; no replacement wrappers or new subfolders exist.

### Exchange navigation

`exchange/` contains three shared boundaries and two implementation folders:

| Location | Responsibility |
| --- | --- |
| `exchange.h` | In-process execution boundary and its ordered order-update/fill event type. |
| `canonical_venue_adapter.h` | Canonical venue capabilities, commands and event stream used by full-system replay. |
| `exchange_gateway_adapter.h` | Gateway-to-backend contract used by executable services. |
| `adapters/message_exchange.h/.cpp` | `MessageExchange`: execution commands published through the durable message bus. |
| `adapters/backend_gateway.h/.cpp` | `BackendGateway`: gateway routing to a private JetStream backend stream. |
| `adapters/simulated_exchange.h` | Next-open market simulator, one-bar long/short research stops and fill-sized short protective covers, matched after completed bars. |
| `mock/` | Complete deterministic venue used by canonical replay and fault/recovery validation. |

These boundaries serve different current workflows. The lightweight simulator and
the complete MOCK venue have different matching/accounting responsibilities.
Research stop entries carry a trigger in the in-process quantity planner and
simulator; gap fills preserve approved monetary value. They are not supported
by service JSON, schema-v1 durable storage or other exchange adapters, which reject
them explicitly. MRShort brackets create one protective cover from actual filled
units, with parent linkage preventing duplicate covers. The simulator cancels the
cover before a timed market exit. Stateful research strategies also reject
durable-store attachment. Research experiment definitions remain outside the
validated catalog.
All nested sources are listed by the existing exchange build file; there are no
forwarding headers or separate build files for the subfolders.

### MOCK venue

MOCK components live in `src/exchange/mock/`, in the `MockVenue` namespace.
Their filenames omit the redundant `mock_` prefix; callers include explicit paths
such as `mock/exchange.h` or `mock/orders.h`.

| File within `mock/` | What to read it for |
| --- | --- |
| `exchange.h` | Public adapter: commands in, ordered venue events out. |
| `orders.h/.cpp` | Validation, request idempotency and order lifecycle. |
| `matching.h/.cpp` | Bar matching, partial fills, latency and slippage. |
| `account.h/.cpp` | Positions, cash, margin, fees and PnL. |
| `recovery.h/.cpp` | Checkpoints, event journal, snapshots and stream backfill. |
| `recovery_codec.h/.cpp` | The durable byte format and file I/O. |
| `reconciliation.h/.cpp` | Compare recovered venue state with the expected ledger. |
| `faults.h/.cpp` | Deterministic fault injection, delivery disturbances and rate limits. |
| `catalog.h`, `catalog_data.inc`, `rules.h` | Supported markets and their exact trading rules. |
| `decimal.h`, `hash.h`, `random.h` | Checked decimal arithmetic, SHA-256 and deterministic random helpers. |

The components remain separate because each owns different state or lifecycle rules.
Their implementations stay out of headers except where templates or small helpers
require them. The catalog include stays inside its initializer; its data and accepted
hashes retain their original bytes.

Source files and implementation classes no longer carry `v1` suffixes. The
`VenueContracts::V1` namespace identifies the adapter contract format. Version strings in
messages, checkpoints, catalog metadata and fingerprints remain unchanged because they
are part of compatibility and accepted replay evidence.

Small related definitions now share a file:

- `execution_order.h`: order command, status, tracked state and venue update.
- `position_state.h`: position container plus target/virtual aliases.
- `portfolio.h` and `portfolio_weights.h`: monetary and dimensionless containers plus target aliases.
- `rebalance_plan.h`: rebalance decisions and their timestamp/capital snapshot.
- `covariance_estimator.h`: covariance matrix and estimation interface.
- `venue_identity.h`, `venue_errors.h`, `venue_capabilities.h`: each vocabulary and its adapter contract.

### Replay timing

Historical replay must use historical event timestamps. Wall-clock or monotonic time may be used for process mechanics, never as business/economic time. The RealTest parity path also has deliberately frozen close/open timing semantics; do not simplify those rules without validating canonical fingerprints.

## Refactoring rule

Prefer a middle ground:

- keep a file together when it represents one coherent component,
- split a large header when interface and implementation can be separated meaningfully,
- do not create wrapper files that only move one function elsewhere,
- do not mix readability changes with trading-semantic changes,
- compile the whole project and run the canonical Step59 release gate after meaningful cross-cutting changes.

Some names still contain historical `StepXX` identifiers because they are part of frozen fingerprints, durable formats or validation evidence. Those identifiers are intentionally different from obsolete development comments.

Validate changes in WSL with `meson compile -C build`, followed by
`bash validation/step59_canonical_replay_release_gate.sh`. The accepted compact fingerprint
is `94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
