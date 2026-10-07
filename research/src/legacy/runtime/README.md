# Frozen legacy research runtime

Purpose: compile the historical research executables without changing the
modern Strategy/SignalState architecture used by canonical replay/live code.

Historical core source:
`89eb80cb440d44c31a762c402074871d56610190`

The breakout/older strategy headers were local/ignored files in this working
tree and therefore do not exist in Git history. Copies are frozen here for
legacy research only.

`pureRSI.h` in this runtime is a compatibility implementation for the old
Strategy API. The production/current PureRSI is not modified.

Do not use this runtime from canonical replay, dashboard, TESTNET or LIVE.


Six consumers remain in the parent directory, linked through `legacy_research_dep`
in `../meson.build`:

- `backtesting_metrics_main_html_reports.cpp`
- `btc_moving_average.cpp`
- `initial_strategy_parameters.cpp`
- `multi_strategy_main.cpp`
- `stats.cpp`
- `testing_xhbreakout.cpp`

`../backtesting_main.cpp` instead links CURRENT `lib/` and backs the public
fast replay mode. Migrate the six consumers before deleting this temporary runtime;
begin with the HTML-report executable. See the
[current handoff](../../../../docs/codex/CONTEXT_FULL.txt) and
[replay guide](../../../REPLAY.md).
