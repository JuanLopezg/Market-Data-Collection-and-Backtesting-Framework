# Canonical Replay Suite

Replay/backtest work has **one public entry point**:

```bash
python3 research/replay.py <mode> ...
```

Old `tools/historical_replay/run_t*.py`, `step56a_*`, `step58_*` and
`step58a_*` runners remain frozen historical evidence. They are not deleted.
**No new replay engine should be added** for another speed, date window or UI mode;
add an option to `research/replay.py` instead.

## Partial history: `--days`

For a single-command, five-minute local CPU/RAM study with a 100-day canonical
backtest and dashboard, run from the repository root under WSL:

```bash
python3 research/replay.py resources --days 100 --seconds 300
```

From a PowerShell terminal in the repository, prefix the command with
`wsl --exec`. Docker Desktop must be running. This resource campaign reuses
the current `dashboard` replay path; it is not another economic engine.
It creates its own Docker project, volumes, images, dashboard state and loopback
port. The complete historical service sandbox stays running beside the canonical
replay container, with API/web/watchdog/test-file notifier services included.
The dashboard uses the canonical **simulation** provider; background service
state comes from the separate bounded synthetic fixture, not the 100-day backtest.
The watchdog retains its required public TESTNET/dry-run configuration; the
internal network blocks venue egress, and it cannot submit private orders.

Preparation/builds and initial service recovery happen before the timed window.
The canonical run uses original historical inputs and direct RealTest comparison,
with deliberate display delays adding roughly 25% of the measurement window.
Remaining time measures the stack while quiet. The command stops its own stack,
retains evidence and prints the offline `resources.html` path. If replay fails or
exceeds the deadline, it reports failure with partial measurements.
No private venue, LIVE state, Telegram delivery or existing dashboard deployment
is used. An authenticated client polls six dashboard resources every five seconds;
you may also visit the printed URL during measurement using the local
`viewer / viewer-demo` account.
Add `--open` to open the dashboard during measurement and the HTML report at the
end in the Windows browser. The UI still requires its normal local demo login.

The HTML graph supports component selection and hover inspection. It reports
sampled mean/peak CPU and RAM, container totals and optional Windows/browser/WSL
host measurements as separate scopes. Docker CPU 100% means one logical core;
host CPU 100% means the whole machine. Missing/stale samples stay gaps, not zeros.
Windows/browser/VM memory overlaps host usage and must not be added to container
totals. The observer adds some overhead. This paced bounded run does not measure
maximum-speed throughput, full-history capacity, real trading or VPS hardware.
Browser detection covers Chrome, Edge and Firefox processes; shared working-set
pages may be counted more than once, and it does not isolate the dashboard tab.

After an accepted current service build, `--skip-build` avoids recompiling those
binaries; dashboard images still build from current source. Do not use it after
compiled-source changes. A short verification campaign is:

```bash
python3 research/replay.py resources --days 20 --seconds 30 --skip-build
```

Generated configuration, credentials, samples, HTML and failure evidence remain
ignored below `storage/local_service_campaign/`; replay exports use a unique
ignored directory below `deploy/historical_replay/run/research_replay/`.
Use the [local campaign runbook](../validation/LOCAL_SERVICE_CAMPAIGN.md) and its
explicit `--run` argument for later removal of only the generated project's state.

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
NotionalOrderPlanner -> CanonicalVenueAdapter -> MOCK ->
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


## Current implementation and research migration

The fast replay executable is built from `src/legacy/backtesting_main.cpp`
against CURRENT `lib/backtest/Backtester`; its directory name does not mean it
uses the frozen runtime. System/dashboard use `src/canonical/canonical_replay.cpp`
and CURRENT `lib/src/runtime/replay_runtime.*`.

All six useful research consumers now use CURRENT APIs. The frozen runtime,
its comparison target and conditional HTML implementation have been removed.
Regression checks preserve explicit report contracts and verify CURRENT fills,
accounting, fees, causal prefixes and determinism.
The HTML report source builds one target:
`algotrading_research_html` links CURRENT `lib/` for supported PureRSI studies
and research-only Donchian, XH and XH-ATR experiments.
All four HTML strategy definitions and the XH robustness executable now have
CURRENT paths. Initial parameter and statistics consumers also use CURRENT APIs
for all seven of their original strategy families. The BTC moving-average study
also uses the shared current short/momentum configurations. The scenario consumer
uses current signal, sizing,
execution and accounting APIs for all eight strategy families.

### Research consumer inventory

All entrypoints below live under `src/legacy/`; verify build linkage in its
`meson.build`, rather than inferring runtime from the directory name.

| Entrypoint | Useful capabilities | Current implementation |
| --- | --- | --- |
| `backtesting_metrics_main_html_reports.cpp` | Isolated baselines, full Cartesian grids, invalid/failed run CSVs, parameter medians, equity/drawdown/tail risk, closed-trade distributions and seeded Monte Carlo HTML. | All four definitions use CURRENT APIs; one reporting target, explicit contract and accounting checks. |
| `btc_moving_average.cpp` | MRShort and PureMom BTC SMA sensitivity; grid, metadata and HTML reports. | CURRENT shared short/momentum configurations; original 10..200 step-5 sweep and report contracts retained. |
| `initial_strategy_parameters.cpp` | Initial parameter grids and HTML for BargainChaser, ATRBreakout, MRShort, PureMom, PureRSI, MRRSILong and XHBreakout. | CURRENT APIs/economics; original ranges, medians and HTML filenames retained; bounded CLI and verification exports added. |
| `multi_strategy_main.cpp` | Eight isolated strategy families across 18 database/timeframe scenarios, chart index and cross-strategy return correlations. | CURRENT APIs/economics; distinct settings, benchmarks, original scenario list, HTML names and correlation controls retained. |
| `stats.cpp` | Seven isolated strategy baselines and in-memory/logged statistical metrics. | CURRENT APIs/economics using shared initial-study configurations; logged summaries and in-memory cache retained. |
| `testing_xhbreakout.cpp` | XH and XH-ATR robustness grids, rejection accounting, CSV metadata and HTML. | CURRENT XH definitions, sizing and fill accounting; original full ranges and rejection/report contracts retained. |

### Bounded HTML study

From the repository root under WSL:

```bash
meson compile -C build -j 3 algotrading_research_html
build/research/src/legacy/algotrading_research_html \
  --start 20200101 --end 20200416 --grid compact --study-id my_current_rsi_study
python3 validation/research_html_report_gate.py
```

Defaults use `storage/databases/1d_cmc.csv`, a cold 107-day window, PureRSI and
no sensitivity grid. `--grid compact` runs eight combinations (six valid, two
entry/exit rejections); `--grid full` explicitly requests the original large RSI
grid. `--help` lists input, timeframe/benchmark, commission and output controls.
Existing study directories are rejected to preserve evidence. Generated studies
belong under ignored `storage/`.

The retained contracts are `PureRSI.csv`, `parameter_grid.csv`,
`study_metadata.csv` and `reports/PureRSI.html`. CSV columns and inline SVG/Monte
Carlo/sensitivity sections retain their existing formats. Additional
`execution_settings.csv` identifies runtime/window/grid/cutoff;
`reports/PureRSI_trades.csv` and `reports/PureRSI_account.csv` expose CURRENT
fill-derived campaigns and timestamped cash/balance/equity for verification.
The runner also writes `reports/<strategy>_metrics.csv` for its baseline, using
the sensitivity CSV columns with run ID zero. This permits numeric comparison
across current studies even when no sensitivity grid is requested.

CURRENT sizing uses 10% fixed weight, full strategy allocation, 1.5 gross/asset
risk limits and entry/exit-only rebalance, matching the public fast configuration.
Market decisions at close(T) execute at the next available open. Commission is netted once
from actual fills. Open campaigns stay marked at cutoff, enter exposure, and are
excluded from closed-trade statistics/Monte Carlo. Holding duration counts observed
asset bars inclusively when CURRENT analytics has no legacy counter, without counting
another asset's dates through a data gap. Monte Carlo remains
the existing seeded bootstrap of closed PnL, with no regime/overlap preservation.

`--strategy DonchianBreakout --grid none` selects the research-only current signal
experiment. It preserves entry `close > prior DonchianHigh`, exit `close < current
DonchianMid`, liquidity/ROC ranking and the optional benchmark-SMA entry filter.
Both sides execute at next open; the removed implementation exited at signal close.
Its original full sensitivity ranges and parameter columns remain available.
Compact grids contain four Donchian/XH combinations and eight XH-ATR combinations.
This migration does not add Donchian
to the library's validated strategy catalog or claim RealTest acceptance for it.

`--strategy XHBreakout` and `--strategy XHBreakout_ATR` use research-only
`src/common/xh_breakout_strategy.h` with current sizing, execution and accounting.
The close selects a highest-high stop for one following processed bar. Once that
bar is complete, its high determines crossing; fill price is `max(trigger, open)`.
Gap quantity decreases to preserve the approved monetary amount. The unused
quantity cap is canceled after the fill, leaving no pending exposure. Untriggered
orders expire, including when the asset has no bar in that following slice.

XH exits when close falls below its fast SMA. XH-ATR initializes from actual entry
price and entry-bar ATR, excludes entry-bar high, and ratchets upward using only
subsequent completed bars. Both exits become pending market orders for a later
available open. The removed XH implementations normally read that future open
while processing the current close, with a same-close fallback at cutoff;
CURRENT does neither. Open cutoff positions remain marked. ATR eligibility is
checked at signal close, rather than accepting/rejecting an entry using future ATR.
Trailing state reconstructs from fill-derived campaigns and completed history.

Stop entries are supported only by the in-process research simulator. Service JSON,
unsupported adapters and schema-v1 durable stores reject them. In-memory exchange
restoration and trailing reconstruction are tested; disk restart, canonical venue
stops and live conditional routing are not supported by this migration.

Use `algotrading_research_html --strategy all --grid none` for a bounded baseline
study of all four CURRENT definitions. Earlier migration comparisons remain
historical evidence in `CURRENT_STATE.md`; they do not require keeping the old engine.

### Bounded XH robustness study

The XH robustness executable uses the same current strategy and timing rules, with
its own original full ranges (2,508 XH combinations and 2,299 XH-ATR combinations):

```bash
meson compile -C build -j 3 algotrading_research_xhbreakout
build/research/src/legacy/algotrading_research_xhbreakout \
  --start 20200101 --end 20200416 --grid compact --study-id my_xh_robustness
```

Defaults run both baselines over 107 days with no grid. Compact sweeps run eight
combinations per strategy using the original enabled axes and spacings;
`--grid full` explicitly requests the full robustness study. The disabled ATR
multiplier, quantity and position-count axes retain their original definitions.
Sensitivity CSV columns, parameter medians, metadata fee-factor columns and
invalid/failed counters remain intact. Current commissions come from actual
taker fills; old strategy-side fee factors remain zero in metadata. Baseline
trades, account snapshots, metrics and execution settings use the HTML runner's
export formats. The reporting gate checks baseline/fee equality with that runner,
original ranges, deterministic exports/HTML, prefixes and injected invalid/failed runs.

### Isolated scenario study

`algotrading_research_multi_strategy` runs each strategy/dataset pair with its own
fresh strategy, indicators, account and campaigns. It does not combine strategies
into one portfolio. Its eight original families and 18 dataset definitions remain
available, including stocks, intraday, shifted bars, multiday bars and OOS data.
Original strategy settings differ from the initial parameter study and are explicit
in `src/legacy/multi_strategy_main.cpp`. Report embedding and correlation presentation
live in the cohesive `src/common/scenario_reports.*` component.

```bash
meson compile -C build -j 3 algotrading_research_multi_strategy
build/research/src/legacy/algotrading_research_multi_strategy \
  --strategy all --dataset 1d_cmc,stocks_daily,4h_binance \
  --start 20200101 --end 20200416 --study-id my_scenario_study
python3 validation/research_scenario_gate.py
```

Defaults retain the three originally enabled strategies (PureRSI, XH and Donchian),
but select only daily CMC and the cold 107-day window. `--strategy all` and
`--dataset all` explicitly select the full catalogs; comma-separated subsets are
supported. Date bounds refer to dataset keys. Intraday files may use synthetic
YYYYMMDD keys, so 107 keys need not mean 107 calendar days or warmed 300-bar filters.
Do not reinterpret those keys as real exchange timestamps. OOS data needs its own
appropriate bounds. Benchmark SMA lengths preserve the original per-dataset bar
counts. Annualization is explicit: 252 for stocks, 365 for daily crypto, 365 times
bars per day for intraday and 365 divided by bar days for multiday data.

Outputs live under ignored `storage/backtests/multi_strategy/<study-id>/`; existing
directories are rejected. `<strategy_slug>_all_datasets.html` embeds one chart per
successful scenario, retaining balance/equity and buy-and-hold curves.
`1d_cmc_strategy_equity_correlations.html` retains its equity display, pairwise
daily-return Pearson matrix, overlap counts, scale controls and CSV downloads.
The benchmark curve uses the exact bounded trading input. Open positions remain
marked at cutoff. New `<slug>_<dataset>_trades.csv`, `_account.csv` and
`study_metadata.json` expose actual campaigns, cash/balance/equity, timeframe and
benchmark settings, run results and failure reasons. Any failed/skipped run yields
a nonzero exit while retaining successful reports and explicit counters.

Short and momentum use each scenario's exact benchmark symbol and SMA bar count;
their shared constructors retain BTC defaults for earlier consumers. Other entry
filters and research-only conditional-order limitations remain as documented above.
This migration adds no strategy to the library's validated catalog.

### BTC moving-average study

`algotrading_research_btc_ma` uses CURRENT short/momentum signals, sizing and fill
accounting. Its original baselines match the initial study. Only BTC SMA length
varies: 10 through 200 in steps of 5, giving 39 runs per strategy. MRShort requires
BTC below its SMA; PureMom requires BTC above it. No separate filter toggle exists.

```bash
meson compile -C build -j 3 algotrading_research_btc_ma
build/research/src/legacy/algotrading_research_btc_ma \
  --start 20200101 --end 20200416 --grid full --study-id my_btc_ma_study
```

Defaults run two baselines over 107 daily bars without a sweep. Compact mode
samples SMA lengths 10 and 200; full mode runs all 78 combinations over the
selected dates. Original ranges remain in `parameter_grid.csv` in either mode.
Existing directories are rejected. Outputs remain under ignored
`storage/backtests/sensitivity_results/<study-id>/`: `MRShort.csv`, `PureMom.csv`,
`parameter_grid.csv`, `study_metadata.csv` and `reports/<strategy>.html` retain
their contracts, including invalid/failed counters and parameter medians.
Additional execution settings, baseline metrics, trades and account histories
use the current HTML runner's export formats. Fees come from actual taker fills;
open campaigns remain marked at cutoff. These remain research experiments.

### Initial parameter and statistics studies

Both consumers share the original seven configurations in
`src/common/research_strategy_definitions.h`. The initial study retains full
Cartesian ranges, valid-combination filtering, marginal medians and
`<strategy>.html` filenames. Its 37,702 valid full-grid combinations are opt-in;
compact mode uses the first and last reachable value on each enabled axis for
82 valid runs. Defaults run all seven baselines over the cold 107-day window.

```bash
meson compile -C build -j 3 algotrading_research_initial_params algotrading_research_stats
build/research/src/legacy/algotrading_research_initial_params \
  --grid compact --study-id my_initial_parameters
build/research/src/legacy/algotrading_research_stats \
  --start 20200101 --end 20200416
```

Initial reports live under ignored `storage/backtests/strategy_reports/<study-id>/`;
existing directories are rejected. Additional `metrics.csv`, `parameter_grid.csv`,
`study_metadata.csv`, `<strategy>_trades.csv`, `<strategy>_account.csv` and compact/full
`<strategy>_sensitivity.csv` provide reviewable evidence. The original consumer had
no sensitivity CSV contract. Statistics retain logged metrics/summaries and an
in-memory cache, without report files. Both accept `--strategy` and `--fee-taker`.
They are daily studies with BTC-based annualization; timeframe comparisons belong
to the remaining scenario consumer.

Timed market strategies count actual observed asset bars, including entry, and
signal an exit for the next open after their holding period. ATRBreakout enters
at signal high plus ATR multiple and excludes entry-bar timed exits. MRShort
requires high RSI and BTC below its SMA; it enters at `min(trigger, open)` with
trigger `signal close - ATR multiple`. Its fixed protective stop is signal close
plus ATR, and covers only actual filled units at `max(stop, open)`. The frozen
bullish entry-bar open-low-high-close assumption permits a same-bar cover;
bearish open-high-low-close suppresses it. This is an OHLC assumption, not tick
ordering evidence. A timed exit cancels the protective order before market cover.

PureMom uses momentum ranking and the BTC-above-SMA entry filter, without a
positive-ROC predicate. MRRSILong uses low RSI without a BTC filter; old comments
suggested otherwise. BargainChaser retains its one-bar fall and close-above-SMA
predicate. CURRENT money is sized at executable prices, removing its frozen
signal-close quantity rounding and cent-rounded PnL. All money/commissions come
from fills; no future open lookup or artificial close-out at cutoff occurs.
These experiments are research-only and do not expand the validated catalog.
Missing-entry expiry and holding-bar counts have focused gap fixtures. The shared
backtester still requires prices for held-position marking and target resolution;
these checks do not establish end-to-end sparse held-asset replay support.

Research runtime migration is complete. Generated `.ai/` navigation is described in
the [index guide](../docs/codex/AI_INDEX.md). VPS logging and readiness work remain
pending; further strategy research remains subject to validation.

The default WSL release check, from the repository root, is:

```bash
bash validation/step59_canonical_replay_release_gate.sh
```

See [CURRENT_STATE.md](../CURRENT_STATE.md) for validated fingerprints and outstanding
full-history/browser acceptance, and [validation/README.md](../validation/README.md)
for focused checks. Frozen step runners remain diagnostic history; new replay options
belong in the public CLI.
