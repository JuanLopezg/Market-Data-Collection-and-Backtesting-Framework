# Core Library

`lib/` contains the reusable trading logic shared by replay, backtesting and live services.
It should contain domain behavior and infrastructure adapters, not service entrypoints or UI code.

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

For the current service-style path, the most useful orchestration classes are in `src/runtime/`:

- `strategy_signal_engine.*` runs strategies and produces strategy intent.
- `portfolio_risk_engine.*` applies allocation, sizing, risk limits and rebalance policy.
- `order_planner_engine.*` converts approved targets into cancel/submit plans.
- `execution_engine.*` owns executable order/fill handling.
- `trading_engine.*` is the in-process facade that connects decision and execution behavior.
- `full_system_replay_runtime_v1.h` connects the current components to deterministic historical replay.

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
| `market/` | Canonical market-data reading and price snapshots. |
| `persistence/` | Durable trading-state storage. |
| `portfolio/` | Portfolio targets, weights and aggregation. |
| `position/` | Current, target and virtual position state. |
| `ranker/` | Ranking logic used by strategy/universe decisions. |
| `rebalance/` | Rules deciding whether target changes require action. |
| `recovery/` | Startup recovery and reconciliation coordination. |
| `risk/` | Portfolio-level constraints and volatility estimation. |
| `runtime/` | High-level domain orchestration. |
| `signal/` | Signal state shared across strategy/runtime boundaries. |
| `sizing/` | Portfolio sizing policies. |
| `strategy/` | Strategy interfaces, instances and active strategy implementations. |
| `testing/` | Reusable test doubles. |
| `transport/` | Message buses and contract serialization. |
| `universe/` | Tradable-universe selection. |
| `utils/` | Small cross-cutting helpers such as time, JSON, CSV and database utilities. |

## Important boundaries

### Strategy versus risk

Strategies should describe what they want to hold or how they currently signal. Capital allocation, portfolio sizing and hard risk limits belong outside the strategy.

### Planning versus execution

Planning decides which orders should exist. It must not pretend an order was filled. Execution owns order state and applies actual venue events/fills.

### Canonical venue boundary

Higher-level code should use `CanonicalVenueAdapter` and the canonical venue contracts rather than branch on a concrete exchange name. Venue-specific behavior belongs behind an adapter.

### MOCK venue

The large `mock_*_v1.h` files implement distinct parts of one deterministic simulated venue: order admission/lifecycle, matching, accounting, durable recovery, reconciliation and fault injection. Their historical fingerprint/version names are compatibility evidence and should not be renamed casually.

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
