# Canonical Replay Suite

From this step onward, replay/backtest work has **one public entry point**:

```bash
python3 research/replay.py <mode> ...
```

Old `tools/historical_replay/run_t*.py`, `step56a_*`, `step58_*` and
`step58a_*` runners remain frozen historical evidence. They are not deleted.
**No new replay engine should be added** for another speed, date window or UI mode;
add an option to `research/replay.py` instead.

## Partial history: `--days`

All three modes accept:

```bash
--days N
```

`--days 100` means **exactly the first 100 available historical daily bars**
starting at `--start` (default `2020-01-01`). The cutoff is the 100th historical
day, inclusive. Crypto data is daily, so this is normally the same as a 100-calendar-day
window, while remaining robust to a missing source day.

RealTest comparison is restricted to the selected `--start..cutoff` window:

- campaigns already closed by the cutoff are compared as complete trades;
- campaigns whose RealTest exit is later than the cutoff must still be open in the
  candidate and are compared only on **entry date, entry price and quantity**;
- exit date, exit price and PnL are deliberately ignored for those still-open campaigns;
- no mismatch identity is hard-coded: any difference found against `pureRSI.csv` is
  shown for manual review.

The numeric price/quantity/PnL tolerance remains the existing RealTest dynamic policy:

`min(20% + 0.2% * trade_id, 50%)`.

Examples:

```bash
# First 100 historical days, lightweight research Backtester only.
python3 research/replay.py fast --days 100 --label fast_100d

# First 100 days through Strategy/Risk/Planner/CanonicalVenueAdapter/MOCK.
# Unpaced unless a pacing option is selected.
python3 research/replay.py system --days 100 --label system_100d

# Same 100 days, literally paced through TimeHandler at 1500x.
python3 research/replay.py system \
  --days 100 \
  --pace-all \
  --speed 1500 \
  --label system_100d_1500x

# First seven days with dashboard, one historical day displayed every 10 minutes.
python3 research/replay.py dashboard \
  --days 7 \
  --visual-day-minutes 10 \
  --label dashboard_7d_visual
```

Omit `--days` for the frozen full-history window `2020-01-01..2025-10-13`.

## The three supported modes

### 1. `fast` — research-only backtest

```bash
python3 research/replay.py fast --days 100
```

Uses the existing research `Backtester`/PureRSI path. It does **not** instantiate the
canonical MOCK venue stack or dashboard. The legacy `backtesting_main.cpp` remains a
normal research program; the canonical wrapper supplies its date/output overrides through
process environment so there is still only one public replay CLI.

### 2. `system` — full system without dashboard

Runs:

`PureRSI -> StrategySignalEngine -> PortfolioRiskEngine ->
NotionalOrderPlannerEngine -> CanonicalVenueAdapter -> MOCK ->
Fill -> Accounting -> Recovery -> Reconciliation -> Ledger`

Unpaced example:

```bash
python3 research/replay.py system --days 180
```

Literal TimeHandler pacing for the whole selected window:

```bash
python3 research/replay.py system --days 30 --pace-all --speed 1500
```

Or pace only a selected segment:

```bash
python3 research/replay.py system \
  --days 180 \
  --speed 1500 \
  --pace-start 2020-04-15 \
  --pace-end 2020-04-15
```

### 3. `dashboard` — same system plus visual read model

```bash
python3 research/replay.py dashboard \
  --days 7 \
  --visual-day-minutes 10
```

If `--visual-start/--visual-end` are omitted, the selected replay window is the visual
window. `--visual-day-minutes 10` means approximately five minutes for OPEN and five
minutes for CLOSE. This is **display pacing only** and does not alter economics.

By default dashboard mode starts the existing Step58 simulation compose and serves the UI
at `http://localhost:8080`. Use `--no-dashboard-up` if it is already running.

## RealTest is mandatory in all three modes

Every canonical run ends with a direct comparison against
`storage/backtests/final_tests/pureRSI.csv` at the selected cutoff.

There is no hard-coded mismatch-exception acceptance rule. Matching is trade-level,
using the historical research comparison semantics. Differences are displayed one-by-one;
a full-history run asks the user for the final manual accept/reject decision.

For partial history, closed campaigns are compared completely while campaigns still open
at the cutoff compare entry date/time, entry price and quantity only.

## Outputs

Each run writes below:

`deploy/historical_replay/run/research_replay/<label>/`

Typical artifacts:

- `summary.json`
- `runtime_summary.json` in full-system modes
- `fills.csv` in full-system modes
- `trades.csv` in fast mode
- `candidate_trades.csv` in full-system modes
- `fill_structure.json`
- `realtest_trade_comparison.csv`
- `realtest_partial_summary.json`

Dashboard state remains at the existing Step58 path:

`deploy/historical_replay/run/step58_dashboard/state.json`

## RealTest-parity execution isolation

`realtest-parity` reproduces the execution semantics of the historical
`research/src/legacy/backtesting_main.cpp` while still traversing the modern full-system
Strategy -> Risk -> Planner -> CanonicalVenueAdapter -> MOCK path.

The boundary is intentionally simple and explicit:

- Strategy sees the original historical OHLCV unchanged.
- Strategy and Risk make the decision from the completed `close(T)`.
- Risk fixes the monetary target (`target_notional_usd`) at `close(T)`.
- The asset quantity is resolved only when `open(T+1)` becomes available, exactly as
  the historical Backtester did: `target_quantity = target_notional_usd / open(T+1)`.
- A FLAT closes the actually held research-parity quantity exactly.
- The fill price is the historical `open(T+1)`.
- The parity economic mirror keeps the same double-precision cash/position arithmetic as
  the historical Backtester, so venue grid rounding cannot drift later sizing.
- `fills.csv` in `realtest-parity` is exported from that exact parity execution mirror for
  direct apples-to-apples comparison with the historical Backtester/RealTest CSV.
- The canonical MOCK venue still receives and reconciles its own rule-conforming order/fill
  state, so the modern adapter/accounting/reconciliation path remains exercised.
- Commission/fees are zero.
- Slippage is zero.
- Historical volume is not used as execution capacity and is never modified.
- Normal partial fills are disabled by the full-open parity contract.
- Decimal price conversion preserves valid historical prices across the canonical
  MOCK price grid boundary.

This parity profile does **not** redefine `mock-default`.  Normal MOCK retains the
Step51/52 venue simulation semantics (volume capacity, participation, slippage,
price/size grids, fees, accounting, reconciliation and ledger behavior).

RealTest acceptance is direct against
`storage/backtests/final_tests/pureRSI.csv`. No frozen mismatch identities are used as
acceptance rules. Differences are shown to the user one-by-one and full-history acceptance
remains a manual decision.

When `--start` is later than the dataset beginning, the comparator correctly restricts
RealTest to `[start, end]`, but the strategy also starts cold at that date. Tests that need
mature indicator/universe state should replay from the normal beginning and use
`--pace-start/--pace-end` only for the segment being timed or observed.

## Safe day-boundary restart / resume (system + dashboard)

`system` and `dashboard` can persist a deterministic restart checkpoint without changing replay economics.
The checkpoint is written only after a complete historical close, together with the MOCK
Step53 durable checkpoint. On resume, any journal entries after that safe boundary are
rolled back, the durable MOCK venue is recovered, and the in-memory Strategy/Risk/Planner
state is rebuilt deterministically from historical input up to the same close before the
replay continues.

Example restart test over 107 days:

```bash
python3 research/replay.py system --days 107 --label restart_107d \
  --checkpoint-every 25 --stop-after-days 50

python3 research/replay.py system --days 107 --label restart_107d \
  --checkpoint-every 25 --resume
```

The resumed final `fullRunFingerprint` must equal an uninterrupted run of the same
campaign. `--resume` requires the same explicit `--label`, start and selected window.
The same invariant is required in both `system` and `dashboard` modes.
