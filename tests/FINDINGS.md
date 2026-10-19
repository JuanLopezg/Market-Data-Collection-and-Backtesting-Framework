# Findings from the behaviour tests (2026-10-19)

Each finding is a place where the code disagrees with the behaviour its specification
(`tests/specs/<area>.md`, section "Findings") requires. Every one is kept as a test with a
`DISABLED_` prefix and a `// FINDING:` comment, with its original expectation; run them with
`--gtest_also_run_disabled_tests`. No production code was changed. When a fix is approved and
made, remove the `DISABLED_` prefix: the test is the acceptance check.

Severity (orchestrator's arbitration): **High** can stop trading or change money/positions in
the deployed chain; **Medium** wrong accounting, PAPER/replay fidelity or a crash on bad data;
**Low** missing validation of inputs the normal pipeline does not produce, or doc/contract drift.

## High

| # | Area | What happens | Test | Suggested fix |
| --- | --- | --- | --- | --- |
| H1 | chain / portfolio_risk_service | If a day's `AccountSnapshot` arrives more than ~20 s after the strategy intents (slow venue fills or reconciliation, execution restart near midnight), risk's `Retry` spends all 20 JetStream deliveries, the intents are dropped and **the chain never trades again** until someone intervenes. Reproduced with a 40 s execution outage. | `PaperChainTest.DISABLED_C10_LateAccountSnapshotDelaysButDoesNotStopTrading` (chain F1) | Waiting for the snapshot must not spend the redelivery budget: e.g. NAK with delay and no max-deliver for that case, or have risk re-run pending intents when the snapshot arrives. |
| H2 | execution / OrderManager | `onFill` records the fill ID **before** validating the fill. A rejected delivery (wrong asset/side/strategy, unknown order, overlarge) poisons the ID, so the correct delivery of that fill is ignored: position and cash drift from the venue. | `OrderManager.DISABLED_R7_*` (2 tests, execution F2) | Insert the ID only after all checks pass (two-line change in `order_manager.h`). |
| H3 | risk / target_decisions | Manual Control resync rebuilds every decision from approved weights, so an explicit FLAT exit becomes `TARGET_WEIGHT`; the header promises non-HOLD decisions are kept. | `TargetDecisions.DISABLED_R15_ManualResyncPreservesAnExplicitExitWhileResizingOtherHolds` (risk F3) | Only replace prior HOLD decisions. |
| H4 | persistence / PostgresStateStore | `load()` returns a snapshot written with a newer `schema_version`; SQLite refuses it. The VPS uses PostgreSQL, so an older binary would read newer state instead of failing closed (the release `STATE_FORMAT` check is a coarser guard). | `PostgresBackend.DISABLED_R6_LoadRejectsUnknownSchema` (persistence F3) | Check `schema_version` on load as SQLite does. |

## Medium

| # | Area | What happens | Test |
| --- | --- | --- | --- |
| M1 | mock_exchange | Reduce-only is accepted but not enforced in matching: a reduce-only BUY from flat opens a position; a reduce-only SELL 0.3 against +0.2 leaves a -0.1 short. PAPER (simulated exchange) can diverge from Kraken, which enforces it. | `MockVenueSafety.DISABLED_R9_*` (2) |
| M2 | runtime / MOCK reconciliation | `MockReconciliation::beginLedger` hard-codes 100,000 initial cash; any replay with other initial cash reports false cash drift and blocks every order. | `ReplayBehaviour.DISABLED_R10/R12/R14/R16/R17/R18*`, `ManualBehaviour.DISABLED_R19_*` (8) |
| M3 | market / canonical reader | A corrupt OHLCV row crashes the process (SIGSEGV): the error message reads `stmt` after `sqlite3_finalize`. | `CanonicalMarket.DISABLED_R4_InvalidOhlcvRowsCannotReachStrategyOrRisk` |
| M4 | execution_state_service / equity history | Funding dated on a position's entry day is applied before that day's fills and left out of the row's `funding` total (cash includes it), so the day's PnL breakdown does not add up. Relevant to the Commission/Funding reporting requirement. | `EquityHistory.DISABLED_A12_*` |
| M5 | market_data_service / ingestion | When Binance returns only the next day's opening candle and not the day's own candle, the day is committed as complete (tracker, rank, start rows) without its bar; a restart reuses it. | `MarketIngestionBehaviour.DISABLED_R10_*` |
| M6 | runtime / replay | A cross-section with a duplicate symbol is rejected, but only after the venue already filled orders on it. | `ReplayBehaviour.DISABLED_R15_*` |
| M7 | adapters / canonical venue | One partial fill emits two identical `PartiallyFilled` order updates. | `CanonicalAdapter.DISABLED_R13_*` |
| M8 | execution / OrderManager | An older update of the same lifecycle stage overwrites newer `updated_at` and exchange ID. | `OrderManager.DISABLED_R4_*` |
| M9 | strategy / signal engine | Restoring a checkpoint that is invalid for a later strategy has already applied it to earlier strategies (not atomic). | `StrategyIntents.DISABLED_R14_*` (2) |
| M10 | mock_exchange | A modify can shrink an order below its already-filled quantity. | `MockAdmission.DISABLED_R4_*` |
| M11 | runtime / decision engine | A newer flat signal leaves the older unexecuted entry pending. | `DecisionBehaviour.DISABLED_R4_*` |
| M12 | risk / rebalance | The relative-threshold rule misses its exact boundary (10% -> 12% under a 20% rule holds) because of floating-point comparison. Affects `*_rel25` portfolios. | `RebalancePolicies.DISABLED_R11_*` |
| M13 | risk / sizing | Volatility target 0 still requires covariance history and returns "unavailable" instead of an empty target, so old holdings are kept. | `RiskSizing.DISABLED_R6_*` |
| M14 | persistence / SQLite | `SQLiteStateStore` drops `reference_price`, `fill_at_reference_price` and `position_stop_price` (a stop becomes a plain order after restart). Only `research/src/tools/fake_live_main.cpp` uses it. | `SQLiteBackend.DISABLED_R8_*` (2) |

## Low

| Area | What happens | Test |
| --- | --- | --- |
| indicators | `calculateATR` uses Wilder smoothing; its header documents a simple average (doc/code drift, not used by PureRSI). | `IndicatorCalculations.DISABLED_R6_*` |
| indicators | Infinite feature values count as available; unavailable-score ties and alphabetical ranks are not ordered deterministically. | `IndicatorCache.DISABLED_R10_*`, `LiquiditySelection.DISABLED_R12_*`, `EntryRanking.DISABLED_R13/R14_*`, `BenchmarkGate.DISABLED_R15_*` |
| market | Rolling state accepts bars whose high/low do not enclose open/close; corrupt stored quote volume (negative, infinite) is not rejected. | `RollingMarket.DISABLED_R4_*`, `CanonicalMarket.DISABLED_R2_*` |
| planning / runtime | Market orders are not rejected when the execution date is at or before the decision close (upstream timing makes this unreachable today). | `QuantityPlanner.DISABLED_R11_*`, `TradingBehaviour.DISABLED_R5_*` |
| execution / adapters | Order DTOs mutated after construction (quantity 0/NaN, empty asset) are dispatched without revalidation; a non-finite global target partially advances cycle state. | `ExecutionEngine.DISABLED_R5/R12_*`, `MessageExchange.DISABLED_R4_*` |
| transport | JSON objects where arrays are required decode as empty collections; two decisions for one asset silently keep the first. | `MessageJsonBehaviour.DISABLED_R3/R9_*` |
| execution_state_service | Duplicate fill IDs in replayed fills; unexplained live positions not detected by `fillsReachAccount`; manual weight `1e400` reported as "Failed" instead of "Invalid"; UTC parser accepts trailing text; non-JSON Binance price answer throws the wrong exception type. | `EquityHistory.DISABLED_A13/A14_*`, `ManualFolder.DISABLED_C4_*`, `ManualTime.DISABLED_C9_*`, `BinancePrices.DISABLED_D3_*` |
| market_data_service | A JSON config that is not an object starts with defaults instead of failing. | `MarketDataConfigBehaviour.DISABLED_R1_*` |
