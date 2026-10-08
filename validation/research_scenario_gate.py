#!/usr/bin/env python3
"""Bounded scenario/account/report proof. Run from the repository root under WSL."""

import ast
import base64
import csv
from datetime import date, timedelta
import html
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SLUGS = {"BargainChaser": "bargain_chaser", "ATRBreakout": "atr_breakout", "MRShort": "mr_short",
         "PureMom": "pure_mom", "PureRSI": "pure_rsi", "MRRSILong": "mr_rsi_long",
         "XHBreakout": "xh_breakout", "DonchianBreakout": "donchian_breakout"}


def rows(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def run(command, log, expected=0):
    result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    assert result.returncode == expected, (command, result.returncode)


def check_account(folder, strategy, dataset):
    stem = f"{SLUGS[strategy]}_{dataset}"
    account = rows(folder / f"{stem}_account.csv")
    trades = rows(folder / f"{stem}_trades.csv")
    assert account and all(math.isfinite(float(r["equity"])) for r in account)
    assert all(int(a["timestamp"]) < int(b["timestamp"]) for a, b in zip(account, account[1:]))
    assert len({r["trade_id"] for r in trades}) == len(trades)
    closed = [r for r in trades if r["exited"] == "1"]
    assert math.isclose(float(account[-1]["balance"]), 100000 + sum(float(r["pnl"]) for r in closed), abs_tol=1e-7)
    assert math.isclose(float(account[-1]["equity"]), 100000 + sum(float(r["pnl"]) for r in trades), abs_tol=1e-7)
    for trade in trades:
        assert int(account[0]["timestamp"]) < int(trade["start_ts"]) <= int(trade["end_ts"]) <= int(account[-1]["timestamp"])
        if trade["exited"] == "1":
            sign = -1 if strategy == "MRShort" else 1
            pnl = sign * (float(trade["exit_price"]) - float(trade["entry_price"])) * float(
                trade["peak_quantity"]) - float(trade["commission"])
            assert math.isclose(pnl, float(trade["pnl"]), abs_tol=1e-7)
    return account, trades


def check_reports(folder, datasets, data_root):
    expected_curves = {}
    for dataset in datasets:
        file_name = "stocks_dailyB.csv" if dataset == "stocks_daily" else f"{dataset}.csv"
        symbol = {"1d_cmc": "BTC", "stocks_daily": "^GSPC", "4h_binance": "BTCUSDT"}[dataset]
        with (data_root / file_name).open(newline="") as source:
            bars = [r for r in csv.DictReader(source) if r["symbol"] == symbol and
                    20200101 <= int(r["date"].replace("-", "")) <= 20200416]
        bars.sort(key=lambda r: r["date"].replace("-", ""))
        expected_curves[dataset] = [100000 * float(r["close"]) / float(bars[0]["close"]) for r in bars]
    for name, slug in SLUGS.items():
        text = (folder / f"{slug}_all_datasets.html").read_text()
        charts = re.findall(r'srcdoc="([^"]*)"', text)
        assert len(charts) == len(datasets)
        for dataset, embedded in zip(datasets, charts):
            chart = html.unescape(embedded)
            assert "Plotly.newPlot" in chart and "Equity" in chart and "Balance" in chart
            benchmark, _ = json.JSONDecoder().raw_decode(chart[chart.index('{"x":['):])
            assert "buy & hold equity" in benchmark["name"]
            expected = expected_curves[dataset]
            assert len(benchmark["x"]) == len(expected)
            assert all(math.isclose(a, b, abs_tol=1e-8) for a, b in zip(benchmark["y"], expected))
    return expected_curves


def check_correlations(folder, work, log, expected_benchmark):
    text = (folder / "1d_cmc_strategy_equity_correlations.html").read_text()
    embedded = json.loads(re.search(r"const embeddedStrategies = (\[.*?\]);", text, re.S)[1])
    assert len(embedded) == 8
    series = []
    for entry in embedded:
        dates = ast.literal_eval(base64.b64decode(entry["datesBase64"]).decode())
        values = ast.literal_eval(base64.b64decode(entry["equityBase64"]).decode())
        account = rows(folder / f"{SLUGS[entry['name']]}_1d_cmc_account.csv")
        assert len(dates) == len(values) == len(account) == 107
        assert all(math.isclose(a, float(b["equity"]), abs_tol=5.1e-7) for a, b in zip(values, account))
        series.append({"dates": dates, "values": values})
    # Execute the preserved calculations with Node, without a browser or UI tests.
    node = shutil.which("node") or shutil.which("node.exe")
    if not node and Path("/mnt/c/Program Files/nodejs/node.exe").is_file():
        node = "/mnt/c/Program Files/nodejs/node.exe"
    assert node, "Correlation validation requires the installed Node runtime"
    functions = text[text.index("  function returnsByDate("):text.index("  function calculateCorrelations(")]
    benchmark_source = text[text.index("  const embeddedBtc ="):text.index("  const palette =")]
    script = work / "correlations.js"
    script.write_text(functions + benchmark_source + "\nconst curves=" + json.dumps(series) + ";\n" +
                      "curves.push({dates:embeddedBtc.x,values:embeddedBtc.y});\n" +
                      "console.log(JSON.stringify({benchmark:embeddedBtc,"
                      "matrix:curves.map(a=>curves.map(b=>pearsonCorrelation(a,b)))}));\n")
    script_path = str(script)
    if node.endswith(".exe"):
        script_path = subprocess.check_output(["wslpath", "-w", script_path], text=True).strip()
    result = subprocess.run([node, script_path], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr
    result_data = json.loads(result.stdout)
    benchmark = result_data["benchmark"]
    assert benchmark["x"] == series[0]["dates"]
    assert all(math.isclose(a, b, abs_tol=1e-8) for a, b in zip(benchmark["y"], expected_benchmark))
    series.append({"dates": benchmark["x"], "values": benchmark["y"]})
    matrix = result_data["matrix"]
    for i, left in enumerate(series):
        a = [current / previous - 1 for previous, current in zip(left["values"], left["values"][1:])]
        for j, right in enumerate(series):
            b = [current / previous - 1 for previous, current in zip(right["values"], right["values"][1:])]
            da = [v - sum(a) / len(a) for v in a]
            db = [v - sum(b) / len(b) for v in b]
            denominator = math.sqrt(sum(v*v for v in da) * sum(v*v for v in db))
            expected = 1 if i == j else sum(x*y for x, y in zip(da, db)) / denominator if denominator else None
            assert matrix[i][j]["observations"] == 106
            actual = matrix[i][j]["value"]
            assert actual is None if expected is None else math.isclose(actual, expected, abs_tol=1e-12)
    log.write("Correlation JavaScript matches independent daily-return Pearson calculations\n")


def check_failure_accounting(executable, work, log):
    missing = work / "missing"
    missing.mkdir()
    run([str(executable), "--databases-dir", str(missing), "--output-root", str(work),
         "--study-id", "skipped"], log, expected=1)
    skipped = json.loads((work / "skipped/study_metadata.json").read_text())
    assert skipped["successful_runs"] == 0 and skipped["failed_or_skipped_runs"] == 3
    assert all(r["status"] == "skipped" and r["failure_reason"] for r in skipped["runs"])
    empty = work / "empty"
    empty.mkdir()
    (empty / "1d_cmc.csv").write_text("date,symbol,open,high,low,close,volume\n")
    run([str(executable), "--databases-dir", str(empty), "--output-root", str(work),
         "--study-id", "failed"], log, expected=1)
    failed = json.loads((work / "failed/study_metadata.json").read_text())
    assert failed["successful_runs"] == 0 and failed["failed_or_skipped_runs"] == 3
    assert all(r["status"] == "failed" and r["failure_reason"] for r in failed["runs"])


def main(build=True):
    # Keep temporary scripts on the shared filesystem so Windows Node can read them.
    with tempfile.TemporaryDirectory(prefix="scenario-gate-", dir=ROOT / "storage") as temporary:
        work = Path(temporary)
        with (work / "gate.log").open("w+") as log:
            try:
                if build:
                    run(["meson", "compile", "-C", "build", "-j", "3", "algotrading_research_multi_strategy",
                         "algotrading_research_html", "algotrading_research_initial_params", "algotrading_research_btc_ma"], log)
                executable = ROOT / "build/research/src/legacy/algotrading_research_multi_strategy"
                data = ROOT / "storage/databases"
                datasets = ["1d_cmc", "stocks_daily", "4h_binance"]
                arguments = ["--databases-dir", str(data), "--output-root", str(work), "--strategy", "all",
                             "--dataset", ",".join(datasets), "--start", "20200101", "--end", "20200416"]
                for study in ("current", "rerun"):
                    run([str(executable), *arguments, "--study-id", study], log)
                run([str(executable), *arguments, "--fee-taker", "0.001", "--study-id", "fees"], log)
                prefix_arguments = arguments.copy()
                prefix_arguments[prefix_arguments.index("--end") + 1] = "20200315"
                run([str(executable), *prefix_arguments, "--study-id", "prefix"], log)
                for name in SLUGS:
                    for dataset in datasets:
                        account, trades = check_account(work / "current", name, dataset)
                        fee_account, fee_trades = check_account(work / "fees", name, dataset)
                        assert all(float(t["commission"]) > 0 for t in fee_trades)
                        prefix = rows(work / "prefix" / f"{SLUGS[name]}_{dataset}_account.csv")
                        assert prefix == account[:len(prefix)], f"Future bars changed {name}/{dataset} decisions"
                for path in (work / "current").iterdir():
                    assert path.read_bytes() == (work / "rerun" / path.name).read_bytes(), path.name
                manifest = json.loads((work / "current/study_metadata.json").read_text())
                assert manifest["successful_runs"] == 24 and manifest["failed_or_skipped_runs"] == 0
                assert all(r["status"] == "success" for r in manifest["runs"])
                expected_settings = {"1d_cmc": ("BTC", 50, 365), "stocks_daily": ("^GSPC", 50, 252),
                                     "4h_binance": ("BTCUSDT", 300, 2190)}
                for entry in manifest["runs"]:
                    assert (entry["benchmark"], entry["benchmark_sma_bars"], entry["periods_per_year"]) == (
                        expected_settings[entry["dataset"]])
                benchmark_curves = check_reports(work / "current", datasets, data)
                check_correlations(work / "current", work, log, benchmark_curves["1d_cmc"])
                html_runner = ROOT / "build/research/src/legacy/algotrading_research_html"
                for study, extra in (("html", []), ("html_fees", ["--fee-taker", "0.001"])):
                    run([str(html_runner), "--strategy", "all", "--start", "20200101", "--end", "20200416",
                         "--output-root", str(work), "--study-id", study, *extra], log)
                    scenario = "fees" if extra else "current"
                    for name in ("PureRSI", "XHBreakout", "DonchianBreakout"):
                        for suffix in ("trades", "account"):
                            assert (work / study / "reports" / f"{name}_{suffix}.csv").read_bytes() == (
                                work / scenario / f"{SLUGS[name]}_1d_cmc_{suffix}.csv").read_bytes()
                # Warm the original 300-bar intraday regime filter on bounded synthetic data.
                fixture_data = work / "fixture_data"
                fixture_data.mkdir()
                with (fixture_data / "4h_binance.csv").open("w", newline="") as output:
                    writer = csv.writer(output)
                    writer.writerow(["date", "symbol", "open", "high", "low", "close", "volume"])
                    for i in range(420):
                        day = (date(2020, 1, 1) + timedelta(days=i)).isoformat()
                        for symbol, close in (("BTCUSDT", 100 + 20 * math.sin(i/17)),
                                              ("ASSET", 100 + i*0.07 + 10 * math.sin(i/11))):
                            writer.writerow([day, symbol, close, close+3, close-3, close, 1000])
                run([str(executable), "--databases-dir", str(fixture_data), "--output-root", str(work),
                     "--dataset", "4h_binance", "--strategy", "MRShort,PureMom", "--end", "20210223",
                     "--study-id", "benchmark_fixture"], log)
                for name in ("MRShort", "PureMom"):
                    _, trades = check_account(work / "benchmark_fixture", name, "4h_binance")
                    assert trades, f"Configurable benchmark was not exercised: {name}"
                run([str(executable), "--output-root", str(work), "--study-id", "defaults"], log)
                defaults = json.loads((work / "defaults/study_metadata.json").read_text())
                assert len(defaults["runs"]) == 3
                run([str(executable), *arguments, "--study-id", "current"], log, expected=1)
                for extra in (["--start", "20200230"], ["--strategy", "unknown"], ["--dataset", "unknown"],
                              ["--strategy", "PureRSI,PureRSI"]):
                    run([str(executable), "--output-root", str(work), *extra], log, expected=1)
                check_failure_accounting(executable, work, log)
                print("RESEARCH-SCENARIOS: PASS: eight strategies / three real datasets; isolated accounts, long/short PnL, "
                      "fees, prefixes, deterministic reports, HTML baseline parity, configurable benchmark, "
                      "bounded buy-and-hold and actual JavaScript return correlations")
            except Exception:
                log.flush()
                print((work / "gate.log").read_text())
                raise


if __name__ == "__main__":
    main()
