#!/usr/bin/env python3
"""Bounded CURRENT HTML migration proof; run under WSL from the repository root."""

import csv
import math
import os
import re
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run(command, log, env=None, expected=0):
    result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    assert result.returncode == expected, (command, result.returncode, log.name)


def rows(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def check_report_contract(study, name, sensitivity=False):
    # These are the retained public CSV columns, independent of the writer.
    parameters = {
        "PureRSI": ["maxPositionsOpen", "maxRankingPosition", "quantityPercent",
                    "rsiEntry", "rsiExit", "rsiLength"],
        "DonchianBreakout": ["benchmarkMovingAverageLength", "donchianLookback", "maxPositionsOpen",
                             "maxRankingPosition", "momentumLength", "quantityPercent", "useMarketStateFilter"],
        "XHBreakout": ["fastMovingAverageLength", "maxPositionsOpen", "maxRankingPosition",
                       "momentumLength", "quantityPercent", "xH"],
        "XHBreakout_ATR": ["atrLength", "atrMultiplier", "maxPositionsOpen", "maxRankingPosition",
                           "momentumLength", "quantityPercent", "xH"],
    }
    metrics = ["net_return_percent", "annualized_return_percent", "max_drawdown_percent",
               "sharpe_ratio", "sortino_ratio", "calmar_ratio", "profit_factor", "expectancy_per_trade",
               "trade_count", "exposure_percent", "turnover_multiple", "win_rate_percent", "average_win",
               "average_loss", "max_consecutive_losses", "worst_loss_streak", "average_holding_bars",
               "net_profit", "final_equity"]
    expected = ["study_id", "strategy", "run_id", "status", "failure_reason", *parameters[name], *metrics]
    assert list(rows(study / "reports" / f"{name}_metrics.csv")[0]) == expected
    if sensitivity:
        assert list(rows(study / f"{name}.csv")[0]) == expected
    assert list(rows(study / "parameter_grid.csv")[0]) == [
        "study_id", "strategy", "parameter_name", "display_name", "current_value", "enabled",
        "minimum", "maximum", "spacing"]
    assert list(rows(study / "study_metadata.csv")[0]) == [
        "record_type", "study_id", "created_utc", "database_path", "initial_balance", "fee_maker", "fee_taker",
        "commission_entry_factor", "commission_exit_factor", "periods_per_year", "database_timeframe",
        "annualization_benchmark_symbol", "exclude_simulated_trades", "sensitivity_monte_carlo_enabled",
        "strategy", "planned_runs", "successful_runs", "invalid_runs", "failed_runs"]
    settings = rows(study / "execution_settings.csv")[0]
    assert list(settings) == ["runtime", "start", "end", "grid", "cutoff_policy"]
    assert settings["runtime"].startswith("current-lib-")
    assert settings["cutoff_policy"] == "mark-open-positions"
    html = (study / "reports" / f"{name}.html").read_text()
    for marker in ("Equity curve", "Drawdown", "Historical net return per trade distribution",
                   "Simulated risk scenarios", "Monte Carlo return fan", "<svg"):
        assert marker in html, (name, marker)
    if sensitivity:
        assert "Parameter sensitivity" in html, name


def main(build=True):
    with tempfile.TemporaryDirectory(prefix="research-html-") as temporary:
        work = Path(temporary)
        with (work / "gate.log").open("w+") as log:
            try:
                # A caller that already built these targets can avoid launching
                # a second Ninja process against the same build directory.
                if build:
                    run(["meson", "compile", "-C", "build", "-j", "3",
                         "algotrading_research_html",
                         "algotrading_research", "algotrading_research_xhbreakout",
                         "algotrading_research_initial_params", "algotrading_research_stats",
                         "algotrading_research_btc_ma"], log)
                library = ROOT / "build/lib/src"
                fixture = work / "metrics_test"
                include_dirs = [f"-I{p}" for p in (ROOT / "lib/src").rglob("*")
                                if p.is_dir() and "on_hold" not in p.parts]
                run(["g++", "-std=c++23", "-Iresearch/src/common", *include_dirs,
                     "validation/research_report_metrics_test.cpp",
                     "research/src/common/backtest_metrics.cpp", f"-L{library}",
                     f"-Wl,-rpath,{library}", "-lalgolib", "-lfmt", "-llog4cpp",
                     "-o", str(fixture)], log)
                run([str(fixture)], log)
                stop_fixture = work / "stop_test"
                run(["g++", "-std=c++23", "-Iresearch/src/common", *include_dirs,
                     "validation/research_stop_entry_test.cpp", f"-L{library}",
                     f"-Wl,-rpath,{library}", "-lalgolib", "-lfmt", "-llog4cpp",
                     "-o", str(stop_fixture)], log)
                run([str(stop_fixture)], log)
                current = ROOT / "build/research/src/legacy/algotrading_research_html"
                data = ROOT / "storage/databases/1d_cmc.csv"
                arguments = ["--database", str(data), "--start", "20200101", "--end", "20200416",
                             "--output-root", str(work)]
                initial = ROOT / "build/research/src/legacy/algotrading_research_initial_params"
                grid_source = work / "initial_grid_test.cpp"
                grid_source.write_text(
                    '#define main initialParameterStudyMain\n'
                    f'#include "{ROOT / "research/src/legacy/initial_strategy_parameters.cpp"}"\n'
                    '#undef main\n'
                    'int main() {\n'
                    '  const auto definitions = makeStrategyDefinitions();\n'
                    '  const std::vector<std::size_t> full{9450,7260,11440,98,874,7700,880};\n'
                    '  const std::vector<std::size_t> compact{16,16,16,4,6,16,8};\n'
                    '  for (std::size_t i=0; i<definitions.size(); ++i) {\n'
                    '    compactSensitivity = false;\n'
                    '    if (countSensitivityCombinations(definitions[i]) != full[i]) return 2;\n'
                    '    compactSensitivity = true;\n'
                    '    if (countSensitivityCombinations(definitions[i]) != compact[i]) return 3;\n'
                    '  }\n'
                    '  return definitions.size() == 7 ? 0 : 4;\n'
                    '}\n'
                )
                grid_fixture = work / "initial_grid_test"
                run(["g++", "-std=c++23", "-Iresearch/src/common", *include_dirs,
                     str(grid_source), "research/src/common/backtest_metrics.cpp",
                     "research/src/common/backtest_html_report.cpp", f"-L{library}",
                     f"-Wl,-rpath,{library}", "-lalgolib", "-lfmt", "-llog4cpp", "-lboost_program_options",
                     "-o", str(grid_fixture)], log)
                run([str(grid_fixture)], log)
                for study in ("initial", "initial_rerun"):
                    run([str(initial), *arguments, "--grid", "compact", "--study-id", study], log)
                run([str(initial), *arguments, "--fee-taker", "0.001", "--study-id", "initial_fees"], log)
                run([str(initial), "--database", str(data), "--start", "20200101", "--end", "20200315",
                     "--output-root", str(work), "--study-id", "initial_prefix"], log)
                expected_counts = {"BargainChaser": 16, "ATRBreakout": 16, "MRShort": 16,
                                   "PureMom": 4, "PureRSI": 6, "MRRSILong": 16, "XHBreakout": 8}
                for name, count in expected_counts.items():
                    report = work / "initial"
                    assert len(rows(report / f"{name}_sensitivity.csv")) == count
                    for suffix in (".html", "_trades.csv", "_account.csv", "_sensitivity.csv"):
                        assert (report / f"{name}{suffix}").read_bytes() == (
                            work / "initial_rerun" / f"{name}{suffix}").read_bytes()
                    for study in ("initial", "initial_fees"):
                        account_rows = rows(work / study / f"{name}_account.csv")
                        campaign_rows = rows(work / study / f"{name}_trades.csv")
                        assert len(account_rows) == 107
                        assert all(math.isfinite(float(r["equity"])) for r in account_rows)
                        closed_rows = [r for r in campaign_rows if r["exited"] == "1"]
                        final = account_rows[-1]
                        assert math.isclose(float(final["balance"]), 100000 + sum(
                            float(t["pnl"]) for t in closed_rows), abs_tol=1e-7)
                        assert math.isclose(float(final["equity"]), 100000 + sum(
                            float(t["pnl"]) for t in campaign_rows), abs_tol=1e-7)
                        for trade in campaign_rows:
                            assert 20200101 < int(trade["start_ts"]) <= int(trade["end_ts"]) <= 20200416
                            if trade["exited"] == "1":
                                direction = -1 if name == "MRShort" else 1
                                net = direction * (float(trade["exit_price"]) - float(trade["entry_price"])) * float(
                                    trade["peak_quantity"]) - float(trade["commission"])
                                assert math.isclose(net, float(trade["pnl"]), abs_tol=1e-7)
                            if study == "initial_fees":
                                assert float(trade["commission"]) > 0
                    if name in ("ATRBreakout", "MRShort", "PureMom", "MRRSILong", "XHBreakout"):
                        assert rows(report / f"{name}_trades.csv"), f"Missing exercised baseline: {name}"
                    prefix = rows(work / "initial_prefix" / f"{name}_account.csv")
                    full = rows(report / f"{name}_account.csv")
                    assert prefix == full[:len(prefix)], f"Future data changed {name} prefix account"
                statistics = ROOT / "build/research/src/legacy/algotrading_research_stats"
                for study, extra in (("initial", []), ("initial_fees", ["--fee-taker", "0.001"])):
                    result = subprocess.run([str(statistics), "--database", str(data), "--start", "20200101",
                                             "--end", "20200416", *extra], cwd=ROOT, text=True,
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    log.write(result.stdout)
                    assert result.returncode == 0
                    logged = re.findall(r"Summary (\w+): return=([-\d.]+)%.*?trades=(\d+)", result.stdout)
                    assert len(logged) == 7
                    expected = {r["strategy"]: r for r in rows(work / study / "metrics.csv")}
                    for name, net_return, trade_count in logged:
                        assert abs(float(net_return) - float(expected[name]["net_return_percent"])) <= 0.0051
                        assert int(trade_count) == int(expected[name]["trade_count"])
                run([str(initial), *arguments, "--study-id", "initial"], log, expected=1)
                run([str(initial), "--start", "20200230"], log, expected=1)
                run([str(initial), "--strategy", "unknown"], log, expected=1)
                print("RESEARCH-INITIAL: PASS: seven baselines, 82 compact runs, original 37702 valid grid points; "
                      "deterministic reports/exports; long/short PnL, fees, accounts, prefixes and statistics parity")
                btc_study = ROOT / "build/research/src/legacy/algotrading_research_btc_ma"
                btc_fixture_source = work / "btc_report_contract_test.cpp"
                btc_fixture_source.write_text(
                    '#define main btcMovingAverageStudyMain\n'
                    f'#include "{ROOT / "research/src/legacy/btc_moving_average.cpp"}"\n'
                    '#undef main\n'
                    'int main(int argc, char** argv) {\n'
                    '  auto definition = makeStrategyDefinitions().front();\n'
                    '  if (countSensitivityCombinations(definition) != 39) return 2;\n'
                    '  compactSensitivity = true;\n'
                    '  if (countSensitivityCombinations(definition) != 2) return 3;\n'
                    '  compactSensitivity = false;\n'
                    '  definition.sensitivityParameters = {{"btcMovingAverageLength", "BTC SMA", 0, 1, 1, true}};\n'
                    '  definition.isValidCombination = [](const ParameterValues& p) {\n'
                    '    return p.at("btcMovingAverageLength") > 0;\n'
                    '  };\n'
                    '  definition.create = [](const ParameterValues&) -> std::unique_ptr<Strategy> {\n'
                    '    throw std::runtime_error("Injected BTC report failure");\n'
                    '  };\n'
                    '  SensitivityProgressReporter progress(2);\n'
                    '  const auto result = runParameterSensitivity(definition, OHLCVData{}, 100000, 0, 0,\n'
                    '    BacktestMetricsSettings{}, std::filesystem::path(argv[1]) / "btc_rejected.csv",\n'
                    '    "fixture", progress);\n'
                    '  return result.stats.plannedRunCount == 2 && result.stats.invalidRunCount == 1 &&\n'
                    '    result.stats.failedRunCount == 1 && result.stats.successfulRunCount == 0 ? 0 : 4;\n'
                    '}\n'
                )
                btc_fixture = work / "btc_report_contract_test"
                run(["g++", "-std=c++23", "-Iresearch/src/common", *include_dirs,
                     str(btc_fixture_source), "research/src/common/backtest_metrics.cpp",
                     "research/src/common/backtest_html_report.cpp", f"-L{library}",
                     f"-Wl,-rpath,{library}", "-lalgolib", "-lfmt", "-llog4cpp", "-lboost_program_options",
                     "-o", str(btc_fixture)], log)
                run([str(btc_fixture), str(work)], log)
                rejected = rows(work / "btc_rejected.csv")
                assert [r["status"] for r in rejected] == ["invalid", "failed"]
                assert all(r["failure_reason"] and not r["final_equity"] and not r["trade_count"] for r in rejected)
                # Full parameter sweep, bounded history: 39 BTC SMA lengths per strategy.
                for study in ("btc", "btc_rerun"):
                    run([str(btc_study), *arguments, "--grid", "full", "--study-id", study], log)
                run([str(btc_study), *arguments, "--fee-taker", "0.001", "--study-id", "btc_fees"], log)
                run([str(btc_study), "--database", str(data), "--start", "20200101", "--end", "20200315",
                     "--output-root", str(work), "--study-id", "btc_prefix"], log)
                run([str(btc_study), *arguments, "--grid", "compact", "--study-id", "btc_compact"], log)
                for name in ("MRShort", "PureMom"):
                    sweep = rows(work / "btc" / f"{name}.csv")
                    assert len(sweep) == 39 and all(r["status"] == "success" for r in sweep)
                    parameters = (["btcMovingAverageLength", "entryAtrLength", "entryAtrMultiple", "heldBars",
                                   "maxPositionsOpen", "maxRankingPosition", "quantityPercent", "rankerRocLength",
                                   "rsiEntry", "rsiLength"] if name == "MRShort" else
                                  ["btcMovingAverageLength", "heldBars", "maxPositionsOpen", "maxRankingPosition",
                                   "quantityPercent", "rocLength"])
                    assert list(sweep[0]) == ["study_id", "strategy", "run_id", "status", "failure_reason",
                        *parameters, "net_return_percent", "annualized_return_percent", "max_drawdown_percent",
                        "sharpe_ratio", "sortino_ratio", "calmar_ratio", "profit_factor", "expectancy_per_trade",
                        "trade_count", "exposure_percent", "turnover_multiple", "win_rate_percent", "average_win",
                        "average_loss", "max_consecutive_losses", "worst_loss_streak", "average_holding_bars",
                        "net_profit", "final_equity"]
                    assert [float(r["btcMovingAverageLength"]) for r in sweep] == list(range(10, 201, 5))
                    assert [{k: v for k, v in r.items() if k != "study_id"} for r in sweep] == [
                        {k: v for k, v in r.items() if k != "study_id"}
                        for r in rows(work / "btc_rerun" / f"{name}.csv")]
                    compact = rows(work / "btc_compact" / f"{name}.csv")
                    assert [float(r["btcMovingAverageLength"]) for r in compact] == [10, 200]
                    baseline = rows(work / "btc/reports" / f"{name}_metrics.csv")[0]
                    at_fifty = next(r for r in sweep if r["btcMovingAverageLength"] == "50")
                    for key in ("net_return_percent", "trade_count", "final_equity"):
                        assert at_fifty[key] == baseline[key]
                    assert len({r["final_equity"] for r in sweep}) > 1, "BTC SMA sweep did not affect results"
                    for study, initial_study in (("btc", "initial"), ("btc_fees", "initial_fees")):
                        for suffix in ("trades", "account"):
                            assert (work / study / "reports" / f"{name}_{suffix}.csv").read_bytes() == (
                                work / initial_study / f"{name}_{suffix}.csv").read_bytes()
                    for suffix in (".html", "_trades.csv", "_account.csv"):
                        assert (work / "btc/reports" / f"{name}{suffix}").read_bytes() == (
                            work / "btc_rerun/reports" / f"{name}{suffix}").read_bytes()
                    prefix = rows(work / "btc_prefix/reports" / f"{name}_account.csv")
                    assert prefix == rows(work / "btc/reports" / f"{name}_account.csv")[:len(prefix)]
                    assert "BTC moving-average length" in (work / "btc/reports" / f"{name}.html").read_text()
                for row in rows(work / "btc/parameter_grid.csv"):
                    if row["parameter_name"] == "btcMovingAverageLength":
                        assert (row["enabled"], row["minimum"], row["maximum"], row["spacing"]) == (
                            "yes", "10", "200", "5")
                    else:
                        assert row["enabled"] == "no"
                metadata = rows(work / "btc/study_metadata.csv")
                assert len(metadata) == 3
                assert list(metadata[0]) == ["record_type", "study_id", "created_utc", "database_path",
                    "initial_balance", "fee_maker", "fee_taker", "commission_entry_factor", "commission_exit_factor",
                    "periods_per_year", "exclude_simulated_trades", "sensitivity_monte_carlo_enabled", "strategy",
                    "planned_runs", "successful_runs", "invalid_runs", "failed_runs"]
                for row in metadata[1:]:
                    assert (row["planned_runs"], row["successful_runs"], row["invalid_runs"], row["failed_runs"]) == (
                        "39", "39", "0", "0")
                run([str(btc_study), *arguments, "--study-id", "btc"], log, expected=1)
                run([str(btc_study), "--start", "20200230"], log, expected=1)
                run([str(btc_study), *arguments, "--strategy", "unknown", "--study-id", "bad_btc"], log, expected=1)
                print("RESEARCH-BTC-MA: PASS: 78 bounded full-grid runs, compact endpoints, baseline/fee parity, "
                      "deterministic CSV/HTML, prefix invariance and invalid/failed metadata counters")
                for executable, study in [(current, "current"), (current, "rerun")]:
                    run([str(executable), *arguments, "--grid", "compact", "--study-id", study], log)
                # The public fast runner independently configures the same supported baseline.
                env = os.environ.copy()
                env.update(ALGOTRADING_REPLAY_DATABASE_PATH=str(data),
                           ALGOTRADING_REPLAY_OUTPUT_DIR=str(work / "fast"),
                           ALGOTRADING_REPLAY_START_DATE="20200101",
                           ALGOTRADING_REPLAY_END_DATE="20200416",
                           ALGOTRADING_REPLAY_SKIP_INTERNAL_REALTEST="1")
                run([str(ROOT / "build/research/src/legacy/algotrading_research")], log, env)
                baseline = work / "current/reports/PureRSI_trades.csv"
                assert baseline.read_bytes() == (work / "initial/PureRSI_trades.csv").read_bytes()
                assert (work / "current/reports/PureRSI_account.csv").read_bytes() == (
                    work / "initial/PureRSI_account.csv").read_bytes()
                assert baseline.read_bytes() == (work / "fast/pure_rsi_vol_target_trades.csv").read_bytes()
                trades = rows(baseline)
                account = rows(work / "current/reports/PureRSI_account.csv")
                assert len(account) == 107
                assert all(int(a["timestamp"]) < int(b["timestamp"]) for a, b in zip(account, account[1:]))
                closed = [trade for trade in trades if trade["exited"] == "1"]
                realized = sum(float(trade["pnl"]) for trade in closed)
                marked = sum(float(trade["pnl"]) for trade in trades)
                assert math.isclose(float(account[-1]["balance"]), 100000 + realized, abs_tol=1e-7)
                assert math.isclose(float(account[-1]["equity"]), 100000 + marked, abs_tol=1e-7)
                studies = [rows(work / study / "PureRSI.csv") for study in ("current", "rerun")]
                check_report_contract(work / "current", "PureRSI", sensitivity=True)
                for study in studies:
                    assert len(study) == 8
                    assert sum(row["status"] == "success" for row in study) == 6
                    assert sum(row["status"] == "invalid" for row in study) == 2
                    assert not any(row["status"] == "failed" for row in study)
                for a, b in zip(studies[0], studies[1]):
                    assert {k: v for k, v in a.items() if k != "study_id"} == {
                        k: v for k, v in b.items() if k != "study_id"}
                baseline_row = next(row for row in studies[0] if row["rsiLength"] == "7"
                                    and row["rsiEntry"] == "80" and row["rsiExit"] == "70")
                assert int(baseline_row["trade_count"]) == len(closed)
                assert math.isclose(float(baseline_row["final_equity"]), float(account[-1]["equity"]), abs_tol=1e-7)
                asset_dates = {}
                with data.open(newline="") as source:
                    for bar in csv.DictReader(source):
                        date = int(bar["date"].replace("-", ""))
                        if 20200101 <= date <= 20200416:
                            asset_dates.setdefault(bar["symbol"], set()).add(date)
                expected_holding = sum(sum(int(t["start_ts"]) <= date <= int(t["end_ts"])
                                           for date in asset_dates[t["coin"]]) for t in closed) / len(closed)
                assert math.isclose(float(baseline_row["average_holding_bars"]), expected_holding, abs_tol=1e-7)
                # A shorter input horizon must leave all earlier account decisions unchanged.
                run([str(current), "--database", str(data), "--start", "20200101",
                     "--end", "20200315", "--output-root", str(work), "--study-id", "prefix"], log)
                prefix = rows(work / "prefix/reports/PureRSI_account.csv")
                assert prefix == account[:len(prefix)], "Future bars changed earlier account history"
                assert len({t["trade_id"] for t in trades}) == len(trades), "Duplicate campaigns"
                assert all(int(t["start_ts"]) > 20200101 and int(t["end_ts"]) <= 20200416 for t in trades)
                run([str(current), "--database", str(data), "--start", "20200101",
                     "--end", "20200110", "--output-root", str(work), "--study-id", "warmup"], log)
                assert not rows(work / "warmup/reports/PureRSI_trades.csv")
                assert all(float(a["equity"]) == 100000 for a in rows(work / "warmup/reports/PureRSI_account.csv"))
                html = (work / "current/reports/PureRSI.html").read_text()
                assert html == (work / "rerun/reports/PureRSI.html").read_text()
                # Exercise the full CURRENT selection through the public reporting CLI.
                run([str(current), *arguments, "--grid", "none", "--strategy", "all",
                     "--study-id", "breakouts"], log)
                for name in ("PureRSI", "DonchianBreakout", "XHBreakout", "XHBreakout_ATR"):
                    check_report_contract(work / "breakouts", name)
                run([str(current), *arguments, "--strategy", "DonchianBreakout",
                     "--study-id", "donchian"], log)
                donchian = rows(work / "donchian/reports/DonchianBreakout_trades.csv")
                donchian_account = rows(work / "donchian/reports/DonchianBreakout_account.csv")
                assert donchian and len(donchian_account) == 107
                assert math.isclose(float(donchian_account[-1]["equity"]),
                                    100000 + sum(float(t["pnl"]) for t in donchian), abs_tol=1e-7)
                donchian_metrics = rows(work / "donchian/reports/DonchianBreakout_metrics.csv")[0]
                assert donchian_metrics == rows(work / "breakouts/reports/DonchianBreakout_metrics.csv")[0] | {"study_id": "donchian"}
                run([str(current), *arguments, "--study-id", "current"], log, expected=1)
                run([str(current), "--start", "20200230"], log, expected=1)
                run([str(current), *arguments, "--strategy", "UnknownStrategy", "--study-id", "unsupported"], log, expected=1)
                for name, count in (("XHBreakout", 4), ("XHBreakout_ATR", 8)):
                    run([str(current), *arguments, "--strategy", name, "--grid", "compact",
                         "--study-id", name], log)
                    study = work / name
                    grid_rows = rows(study / f"{name}.csv")
                    assert len(grid_rows) == count and all(r["status"] == "success" for r in grid_rows)
                    check_report_contract(study, name, sensitivity=True)
                    campaigns = rows(study / "reports" / f"{name}_trades.csv")
                    history = rows(study / "reports" / f"{name}_account.csv")
                    assert campaigns and len(history) == 107
                    assert len({t["trade_id"] for t in campaigns}) == len(campaigns)
                    assert math.isclose(float(history[-1]["equity"]),
                                        100000 + sum(float(t["pnl"]) for t in campaigns), abs_tol=1e-7)
                    run([str(current), "--database", str(data), "--start", "20200101",
                         "--end", "20200315", "--output-root", str(work), "--strategy", name,
                         "--study-id", f"{name}_prefix"], log)
                    shorter = rows(work / f"{name}_prefix/reports/{name}_account.csv")
                    assert shorter == history[:len(shorter)], f"Future bars affected {name}"
                    run([str(current), *arguments, "--strategy", name, "--fee-taker", "0.001",
                         "--study-id", f"{name}_fees"], log)
                    paid = rows(work / f"{name}_fees/reports/{name}_trades.csv")
                    assert paid and all(float(t["commission"]) > 0 for t in paid)
                    for trade in paid:
                        if trade["exited"] == "1":
                            expected_pnl = ((float(trade["exit_price"]) - float(trade["entry_price"]))
                                            * float(trade["peak_quantity"]) - float(trade["commission"]))
                            assert math.isclose(expected_pnl, float(trade["pnl"]), abs_tol=1e-7)
                    metrics = rows(study / "reports" / f"{name}_metrics.csv")[0]
                    for suffix in ("trades", "account"):
                        assert (study / "reports" / f"{name}_{suffix}.csv").read_bytes() == (
                            work / "breakouts/reports" / f"{name}_{suffix}.csv").read_bytes()
                    print(f"{name} CURRENT equity: {metrics['final_equity']}; "
                          f"{count} compact runs, contracts, prefix and fees PASS")
                # The migrated robustness consumer has its own ranges/contracts,
                # but its baseline must equal the shared CURRENT XH economics.
                robustness = ROOT / "build/research/src/legacy/algotrading_research_xhbreakout"
                # Exercise the actual orchestration's rejection/failure paths
                # without expanding the production CLI with test-only switches.
                rejection_source = work / "robustness_rejection_test.cpp"
                rejection_source.write_text(
                    '#define main researchXHStudyMain\n'
                    f'#include "{ROOT / "research/src/legacy/testing_xhbreakout.cpp"}"\n'
                    '#undef main\n'
                    'int main(int argc, char** argv) {\n'
                    '  auto definitions = makeStrategyDefinitions();\n'
                    '  if (countSensitivityCombinations(definitions[0]) != 2508 ||\n'
                    '      countSensitivityCombinations(definitions[1]) != 2299) return 2;\n'
                    '  OHLCVData data;\n'
                    '  for (Timestamp day : {20200101U, 20200102U, 20200103U})\n'
                    '    data.data["BTC"][day] = {100, 101, 99, 100, 1000};\n'
                    '  auto definition = definitions[1];\n'
                    '  definition.sensitivityParameters = {{"atrMultiplier", "ATR multiplier", 0, 1, 1, true}};\n'
                    '  BacktestMetricsSettings settings;\n'
                    '  SensitivityProgressReporter progress(2);\n'
                    '  const auto result = runParameterSensitivity(definition, data, 100000, 0, 0, settings,\n'
                    '      std::filesystem::path(argv[1]) / "invalid.csv", "fixture", progress);\n'
                    '  if (result.stats.plannedRunCount != 2 || result.stats.invalidRunCount != 1 ||\n'
                    '      result.stats.successfulRunCount != 1 || result.stats.failedRunCount != 0) return 3;\n'
                    '  definition.create = [](const ParameterValues&) -> std::unique_ptr<Strategy> {\n'
                    '    throw std::runtime_error("Injected research failure");\n'
                    '  };\n'
                    '  const auto failure = runParameterSensitivity(definition, data, 100000, 0, 0, settings,\n'
                    '      std::filesystem::path(argv[1]) / "failed.csv", "fixture", progress);\n'
                    '  return failure.stats.invalidRunCount == 1 && failure.stats.failedRunCount == 1 &&\n'
                    '         failure.stats.successfulRunCount == 0 ? 0 : 4;\n'
                    '}\n'
                )
                rejection_fixture = work / "robustness_rejection_test"
                run(["g++", "-std=c++23", "-Iresearch/src/common", *include_dirs,
                     str(rejection_source), "research/src/common/backtest_metrics.cpp",
                     "research/src/common/backtest_html_report.cpp", f"-L{library}",
                     f"-Wl,-rpath,{library}", "-lalgolib", "-lfmt", "-llog4cpp", "-lboost_program_options",
                     "-o", str(rejection_fixture)], log)
                run([str(rejection_fixture), str(work)], log)
                invalid_rows = rows(work / "invalid.csv")
                failed_rows = rows(work / "failed.csv")
                assert [r["status"] for r in invalid_rows] == ["invalid", "success"]
                assert [r["status"] for r in failed_rows] == ["invalid", "failed"]
                assert all(r["failure_reason"] and not r["final_equity"]
                           for r in failed_rows)
                run([str(robustness), *arguments, "--grid", "none", "--study-id", "robust_full_ranges"], log)
                full_ranges = rows(work / "robust_full_ranges/parameter_grid.csv")
                expected_ranges = {
                    "xH": (10, 100, 5, "yes"),
                    "momentumLength": (20, 60, 4, "yes"),
                    "quantityPercent": (2, 20, 2, "no"),
                    "maxPositionsOpen": (1, 20, 1, "no"),
                    "fastMovingAverageLength": (4, 15, 1, "yes"),
                    "atrLength": (10, 30, 2, "yes"),
                    "atrMultiplier": (1, 10, 1, "no"),
                }
                for row in full_ranges:
                    if row["parameter_name"] in expected_ranges:
                        low, high, spacing, enabled = expected_ranges[row["parameter_name"]]
                        assert (float(row["minimum"]), float(row["maximum"]), float(row["spacing"]),
                                row["enabled"]) == (low, high, spacing, enabled)
                for study in ("robust", "robust_rerun"):
                    run([str(robustness), *arguments, "--grid", "compact", "--study-id", study], log)
                run([str(robustness), "--database", str(data), "--start", "20200101",
                     "--end", "20200315", "--output-root", str(work), "--study-id", "robust_prefix"], log)
                run([str(robustness), *arguments, "--fee-taker", "0.001", "--study-id", "robust_fees"], log)
                for name in ("XHBreakout", "XHBreakout_ATR"):
                    report = work / "robust/reports"
                    for suffix in ("trades", "account"):
                        assert (report / f"{name}_{suffix}.csv").read_bytes() == (
                            work / name / "reports" / f"{name}_{suffix}.csv").read_bytes()
                        assert rows(report / f"{name}_{suffix}.csv") == rows(
                            work / f"robust_rerun/reports/{name}_{suffix}.csv")
                    assert (report / f"{name}.html").read_bytes() == (
                        work / f"robust_rerun/reports/{name}.html").read_bytes()
                    study_rows = rows(work / "robust" / f"{name}.csv")
                    assert len(study_rows) == 8 and all(r["status"] == "success" for r in study_rows)
                    assert study_rows[0].keys() == rows(work / name / f"{name}.csv")[0].keys()
                    history = rows(report / f"{name}_account.csv")
                    shorter = rows(work / f"robust_prefix/reports/{name}_account.csv")
                    assert shorter == history[:len(shorter)]
                    assert rows(work / f"robust_fees/reports/{name}_trades.csv") == rows(
                        work / f"{name}_fees/reports/{name}_trades.csv")
                metadata = rows(work / "robust/study_metadata.csv")
                expected_columns = (
                    "record_type,study_id,created_utc,database_path,initial_balance,fee_maker,fee_taker,"
                    "commission_entry_factor,commission_exit_factor,periods_per_year,database_timeframe,"
                    "annualization_benchmark_symbol,exclude_simulated_trades,sensitivity_monte_carlo_enabled,"
                    "strategy,planned_runs,successful_runs,invalid_runs,failed_runs"
                ).split(",")
                assert list(metadata[0]) == expected_columns
                assert len(metadata) == 3
                for row in metadata[1:]:
                    assert (row["planned_runs"], row["successful_runs"], row["invalid_runs"], row["failed_runs"]) == (
                        "8", "8", "0", "0")
                run([str(robustness), *arguments, "--study-id", "robust"], log, expected=1)
                run([str(robustness), "--start", "20200230"], log, expected=1)
                run([str(robustness), *arguments, "--strategy", "unknown", "--study-id", "bad_robust"], log, expected=1)
                print("RESEARCH-XH-ROBUSTNESS: PASS: 16 compact runs; original ranges/metadata; "
                      "baseline and fee exports equal CURRENT HTML; deterministic reports and causal prefixes")
                run([str(current), *arguments, "--fee-taker", "0.001", "--study-id", "fees"], log)
                fee_trades = rows(work / "fees/reports/PureRSI_trades.csv")
                assert all(float(t["commission"]) > 0 for t in fee_trades)
                for trade in fee_trades:
                    if trade["exited"] == "1":
                        net = ((float(trade["exit_price"]) - float(trade["entry_price"]))
                               * float(trade["peak_quantity"]) - float(trade["commission"]))
                        assert math.isclose(net, float(trade["pnl"]), abs_tol=1e-7)
                print(f"RESEARCH-HTML: PASS: 107 bars, {len(closed)} closed campaigns, "
                      "CURRENT fast exports equal; deterministic HTML/grid; contracts/fees/cutoff checked")
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read())
                raise


if __name__ == "__main__":
    main()
