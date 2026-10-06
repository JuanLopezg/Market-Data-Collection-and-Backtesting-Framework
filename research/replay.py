#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from datetime import datetime
from functools import lru_cache
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from typing import Iterable

# Canonical replay front door. Old step-specific runners remain historical only.
FULL_START = "2020-01-01"
FULL_END = "2025-10-13"

ROOT = Path(__file__).resolve().parents[1]
RUN_ROOT = ROOT / "deploy/historical_replay/run/research_replay"
DATASET = ROOT / "deploy/historical_replay/run/1d_cmc_by_date.csv"
MAPPING = ROOT / "config/historical_replay/step56a_source_symbol_map_v1.csv"
REALTEST_CSV = ROOT / "storage/backtests/final_tests/pureRSI.csv"
HISTORICAL_DATA = ROOT / "storage/databases/1d_cmc.csv"
RUNTIME_BUNDLE_LIB = ROOT / "deploy/historical_replay/.runtime_bundle/lib"

CANONICAL_RUNNER_SOURCE = ROOT / "research/src/canonical/canonical_replay.cpp"
CANONICAL_RUNNER_BIN = RUN_ROOT / "bin/algotrading_replay_full"

INCLUDE_DIRS = [
    "account", "analytics", "backtest", "common_types", "contracts", "data_types",
    "exchange", "execution", "filter", "indicator", "market", "persistence",
    "portfolio", "position", "ranker", "rebalance", "recovery", "risk", "runtime",
    "signal", "sizing", "strategy", "strategy/strategies", "testing", "transport",
    "universe", "utils",
]

FULL_RUN_SOURCES = [
    "research/src/canonical/canonical_replay.cpp",
    "lib/src/exchange/mock_reconciliation_ledger_parity_v1.cpp",
    "lib/src/exchange/mock_snapshot_user_stream_recovery_v1.cpp",
    "lib/src/exchange/mock_fault_chaos_rate_limit_v1.cpp",
    "lib/src/exchange/mock_account_margin_positions_accounting_v1.cpp",
    "lib/src/exchange/mock_deterministic_matching_fill_v1.cpp",
    "lib/src/exchange/mock_order_admission_lifecycle_v1.cpp",
    "lib/src/exchange/mock_recovery_codec_v1.cpp",
    "lib/src/runtime/strategy_signal_engine.cpp",
    "lib/src/runtime/full_system_replay_runtime_v1.cpp",
    "lib/src/runtime/portfolio_risk_engine.cpp",
    "lib/src/runtime/notional_order_planner_engine.cpp",
    "lib/src/runtime/rolling_market_state.cpp",
    "lib/src/runtime/time_handler.cpp",
    "lib/src/strategy/strategy.cpp",
    "lib/src/universe/universe_selector.cpp",
    "lib/src/universe/liquidity_universe.cpp",
    "lib/src/ranker/ranker.cpp",
    "lib/src/ranker/indicator_ranker.cpp",
    "lib/src/indicator/indicator_engine.cpp",
    "lib/src/indicator/indicator_calculators.cpp",
    "lib/src/indicator/indicator_spec.cpp",
]

FAST_RUN_SOURCES = [
    "research/src/common/backtest_helpers.cpp",
    "research/src/common/realtest.cpp",
    "research/src/legacy/backtesting_main.cpp",
]


def q(cmd: Iterable[object]) -> str:
    return " ".join(shlex.quote(str(x)) for x in cmd)


def run(
    cmd: list[object],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    log: Path | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    print("+ " + q(cmd), flush=True)
    command = [str(x) for x in cmd]
    if log is None:
        proc = subprocess.run(command, cwd=str(cwd), env=env, text=True, check=False)
    else:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w", encoding="utf-8") as fh:
            proc = subprocess.run(
                command,
                cwd=str(cwd),
                env=env,
                text=True,
                stdout=fh,
                stderr=subprocess.STDOUT,
                check=False,
            )
    if check and proc.returncode != 0:
        if log is not None and log.exists():
            print(log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        raise SystemExit(f"ERROR: command failed ({proc.returncode}): {q(cmd)}")
    return proc


def bundle_env() -> dict[str, str]:
    env = os.environ.copy()
    if RUNTIME_BUNDLE_LIB.is_dir():
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = (
            str(RUNTIME_BUNDLE_LIB)
            if not existing
            else str(RUNTIME_BUNDLE_LIB) + os.pathsep + existing
        )
    return env


def ensure_required_files() -> None:
    required = [
        DATASET,
        MAPPING,
        REALTEST_CSV,
        HISTORICAL_DATA,
    ]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise SystemExit("ERROR: required replay/reference files missing:\n  " + "\n  ".join(missing))


@lru_cache(maxsize=1)
def historical_dates() -> tuple[str, ...]:
    if not DATASET.is_file():
        raise SystemExit(f"ERROR: historical replay dataset missing: {DATASET}")
    dates: list[str] = []
    previous = None
    with DATASET.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if not reader.fieldnames or "date" not in reader.fieldnames:
            raise SystemExit("ERROR: historical replay CSV lacks date column")
        for row in reader:
            value = row["date"]
            if value != previous:
                dates.append(value)
                previous = value
    if not dates:
        raise SystemExit("ERROR: historical replay dataset has no dates")
    return tuple(dates)


def resolve_window(args: argparse.Namespace) -> tuple[str, str, int]:
    start = getattr(args, "start", FULL_START) or FULL_START
    dates = historical_dates()
    try:
        first = dates.index(start)
    except ValueError as exc:
        raise SystemExit(f"ERROR: --start is not an available historical day: {start}") from exc

    days = getattr(args, "days", None)
    if days is None:
        try:
            last = dates.index(FULL_END)
        except ValueError as exc:
            raise SystemExit(f"ERROR: frozen full-history end is absent: {FULL_END}") from exc
        if first > last:
            raise SystemExit("ERROR: replay start is after frozen full-history end")
        return start, dates[last], last - first + 1

    if days <= 0:
        raise SystemExit("ERROR: --days must be a positive integer")
    last = first + days - 1
    if last >= len(dates) or dates[last] > FULL_END:
        available = sum(1 for value in dates[first:] if value <= FULL_END)
        raise SystemExit(
            f"ERROR: --days={days} exceeds available history from {start}; available={available}"
        )
    return start, dates[last], days


def sources_newer(binary: Path, sources: Iterable[str | Path]) -> bool:
    if not binary.is_file():
        return True
    mtime = binary.stat().st_mtime
    for source in sources:
        path = source if isinstance(source, Path) else ROOT / source
        if path.is_file() and path.stat().st_mtime > mtime:
            return True
    return False


def build_full_runner(force: bool = False) -> Path:
    if not force and not sources_newer(CANONICAL_RUNNER_BIN, FULL_RUN_SOURCES):
        return CANONICAL_RUNNER_BIN

    CANONICAL_RUNNER_BIN.parent.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX", "c++")
    includes = [f"-I{ROOT / 'lib/src' / directory}" for directory in INCLUDE_DIRS]
    cmd = [
        cxx,
        "-std=c++20",
        "-O3",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        *includes,
        *(str(ROOT / rel) for rel in FULL_RUN_SOURCES),
        "-pthread",
        "-o",
        str(CANONICAL_RUNNER_BIN),
    ]
    run(cmd)
    CANONICAL_RUNNER_BIN.chmod(0o755)
    return CANONICAL_RUNNER_BIN


def build_fast_runner(force: bool = False) -> Path:
    build = ROOT / "build"
    candidates = [
        build / "research/src/algotrading_research",
        build / "research/algotrading_research",
        build / "algotrading_research",
    ]

    def locate() -> Path | None:
        for candidate in candidates:
            if candidate.is_file():
                return candidate
        if build.is_dir():
            for candidate in sorted(build.rglob("algotrading_research")):
                if candidate.is_file():
                    return candidate
        return None

    current = locate()
    source_paths = [ROOT / rel for rel in FAST_RUN_SOURCES]
    stale = force or current is None or sources_newer(current, source_paths)
    if stale:
        meson = shutil.which("meson")
        if meson is None:
            raise SystemExit(
                "ERROR: Meson is required to build the existing research Backtester target."
            )
        if not (build / "meson-private/coredata.dat").is_file():
            run([meson, "setup", str(build)], cwd=ROOT)
        run([meson, "compile", "-C", str(build), "algotrading_research"], cwd=ROOT)
        current = locate()

    if current is None:
        raise SystemExit("ERROR: algotrading_research binary was not produced by Meson")
    return current


def packed_date(value: object) -> str:
    text = str(value).strip()
    if text.endswith(".0"):
        text = text[:-2]
    if len(text) == 8 and text.isdigit():
        return f"{text[:4]}-{text[4:6]}-{text[6:]}"
    raise SystemExit(f"ERROR: expected packed YYYYMMDD timestamp, got {value!r}")


def parse_reference_date(value: str) -> str:
    value = value.strip()
    for fmt in ("%m/%d/%Y", "%m/%d/%y", "%Y-%m-%d"):
        try:
            return datetime.strptime(value, fmt).date().isoformat()
        except ValueError:
            pass
    raise SystemExit(f"ERROR: unsupported RealTest date: {value!r}")


def parse_number(value: object) -> float:
    text = str(value).strip()
    negative = text.startswith("(") and text.endswith(")")
    if negative:
        text = text[1:-1]
    text = text.replace("$", "").replace("%", "").replace(",", "").strip()
    result = 0.0 if not text else float(text)
    return -result if negative else result


def dynamic_tolerance_percent(trade_id: int) -> float:
    return min(20.0 + float(trade_id) * 0.2, 50.0)


def within_realtest_tolerance(trade_id: int, reference: float, candidate: float) -> bool:
    denominator = abs(reference)
    if denominator < 1e-9:
        return abs(candidate) < 1e-9
    percent = abs(candidate - reference) / denominator * 100.0
    return percent <= dynamic_tolerance_percent(trade_id)


def normalize_realtest_phase(value: str) -> str:
    phase = value.strip().lower()
    if phase in ("open", "close"):
        return phase
    return phase or "unknown"


def load_realtest_reference(path: Path = REALTEST_CSV) -> list[dict]:
    rows: list[dict] = []
    with path.open(newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        required = {
            "Trade", "Strategy", "Symbol", "Side", "DateIn", "TimeIn",
            "QtyIn", "PriceIn", "DateOut", "TimeOut", "PriceOut", "Profit",
        }
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise SystemExit(f"ERROR: RealTest CSV header is not compatible: {path}")
        for row in reader:
            rows.append(
                {
                    "id": int(row["Trade"]),
                    "coin": row["Symbol"].strip(),
                    "direction": row["Side"].strip().capitalize(),
                    "start": parse_reference_date(row["DateIn"]),
                    "start_phase": normalize_realtest_phase(row["TimeIn"]),
                    "end": parse_reference_date(row["DateOut"]),
                    "end_phase": normalize_realtest_phase(row["TimeOut"]),
                    "entry": parse_number(row["PriceIn"]),
                    "exit": parse_number(row["PriceOut"]),
                    "size": parse_number(row["QtyIn"]),
                    "pnl": parse_number(row["Profit"]),
                    # RealTest PureRSI baseline has no trading commission.
                    "commission": 0.0,
                    "exited": True,
                }
            )
    return rows
def load_fast_candidate_trades(path: Path) -> list[dict]:
    trades: list[dict] = []
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            direction = int(row["direction"])
            trades.append(
                {
                    "id": int(row["trade_id"]),
                    "coin": row["coin"],
                    "direction": "Long" if direction > 0 else "Short" if direction < 0 else "Flat",
                    "start": packed_date(row["start_ts"]),
                    "start_phase": "open",
                    "end": packed_date(row["end_ts"]),
                    # Historical Backtester timestamps are daily midnight/open timestamps.
                    "end_phase": "open",
                    "entry": float(row["entry_price"]),
                    "exit": float(row["exit_price"]),
                    "size": float(row["peak_quantity"]),
                    "pnl": float(row["pnl"]),
                    "commission": float(row.get("commission", "0") or 0.0),
                    "exited": row["exited"] in ("1", "true", "TRUE", "YES"),
                }
            )
    return sorted(trades, key=lambda trade: trade["id"])


@lru_cache(maxsize=4096)
def historical_close(date: str, coin: str) -> float | None:
    if not DATASET.is_file():
        return None
    with DATASET.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if row.get("date") == date and row.get("symbol") == coin:
                try:
                    value = float(row["close"])
                except (KeyError, TypeError, ValueError):
                    return None
                return value if math.isfinite(value) and value > 0.0 else None
    return None


def trades_from_fill_csv(path: Path, cutoff: str) -> list[dict]:
    open_trades: dict[tuple[int, str], dict] = {}
    closed: list[dict] = []
    next_trade_id = 1

    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        fills = sorted(list(reader), key=lambda row: int(row["fill_id"]))

    for row in fills:
        strategy_id = int(row["strategy_id"])
        coin = row["coin"]
        key = (strategy_id, coin)
        side = int(row["side"])
        quantity = float(row["quantity"])
        price = float(row["price"])
        fill_date = packed_date(row["timestamp"])
        signed = quantity if side == 0 else -quantity

        state = open_trades.get(key)
        if state is None:
            if signed <= 0.0:
                raise SystemExit(f"ERROR: canonical PureRSI candidate opens non-long campaign: {row}")
            state = {
                "id": next_trade_id,
                "coin": coin,
                "direction": "Long",
                "start": fill_date,
                "start_phase": "open",
                "end": fill_date,
                "end_phase": "open",
                "entry": price,
                "exit": 0.0,
                "size": quantity,
                "pnl": 0.0,
                "commission": float(row.get("commission", "0") or 0.0),
                "exited": False,
                "net": quantity,
                "cash_flow": -quantity * price,
            }
            next_trade_id += 1
            open_trades[key] = state
            continue

        if signed > 0.0:
            state["net"] += quantity
            state["cash_flow"] -= quantity * price
            state["size"] = max(state["size"], state["net"])
            state["end"] = fill_date
            state["end_phase"] = "open"
            state["commission"] += float(row.get("commission", "0") or 0.0)
            continue

        close_quantity = min(quantity, state["net"])
        state["cash_flow"] += close_quantity * price
        state["net"] -= close_quantity
        state["end"] = fill_date
        state["end_phase"] = "open"
        state["commission"] += float(row.get("commission", "0") or 0.0)
        if quantity - close_quantity > 1e-10:
            raise SystemExit("ERROR: canonical PureRSI fill would flip through flat in one fill")
        if abs(state["net"]) <= 1e-10:
            state["exit"] = price
            state["pnl"] = state["cash_flow"]
            state["exited"] = True
            closed.append({k: v for k, v in state.items() if k not in ("net", "cash_flow")})
            del open_trades[key]

    result = closed
    for state in open_trades.values():
        candidate = {k: v for k, v in state.items() if k not in ("net", "cash_flow")}
        candidate["end"] = cutoff
        candidate["end_phase"] = "open"
        mark = historical_close(cutoff, candidate["coin"])
        if mark is None:
            candidate["exit"] = 0.0
            candidate["pnl"] = 0.0
        else:
            candidate["exit"] = mark
            candidate["pnl"] = (
                state["cash_flow"] + state["net"] * mark - candidate["commission"]
            )
        candidate["exited"] = False
        result.append(candidate)
    return sorted(result, key=lambda trade: trade["id"])


def write_candidate_trades(path: Path, trades: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "id", "coin", "direction", "start", "start_phase", "end", "end_phase",
        "entry", "exit", "size", "pnl", "commission", "exited",
    )
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for trade in trades:
            writer.writerow({key: trade[key] for key in fields})


def numeric_difference_percent(reference: float, candidate: float) -> float:
    denominator = abs(reference)
    if denominator < 1e-9:
        return 0.0 if abs(candidate) < 1e-9 else 1_000_000.0
    return abs(candidate - reference) / denominator * 100.0


def trade_time_label(date: str, phase: str) -> str:
    return f"{date} {phase}"


def print_trade(label: str, trade: dict | None) -> None:
    print(f"\n{label}")
    if trade is None:
        print("  <no trade>")
        return
    print(f"  id         : {trade['id']}")
    print(f"  coin       : {trade['coin']}")
    print(f"  direction  : {trade['direction']}")
    print(f"  start      : {trade_time_label(trade['start'], trade.get('start_phase', 'open'))}")
    print(f"  end        : {trade_time_label(trade['end'], trade.get('end_phase', 'open'))}")
    print(f"  entry      : {trade['entry']:.12g}")
    print(f"  exit       : {trade['exit']:.12g}")
    print(f"  quantity   : {trade['size']:.12g}")
    print(f"  pnl        : {trade['pnl']:.12g}")
    print(f"  commission : {trade.get('commission', 0.0):.12g}")
    print(f"  exited     : {'YES' if trade.get('exited', False) else 'NO'}")


def print_numeric_difference(name: str, reference: float, candidate: float, tolerance: float) -> None:
    diff = candidate - reference
    pct = numeric_difference_percent(reference, candidate)
    print(f"  {name}")
    print(f"    RealTest  : {reference:.12g}")
    print(f"    Candidate : {candidate:.12g}")
    print(f"    Difference: {diff:.12g}")
    print(f"    Diff %    : {pct:.12g}%")
    print(f"    Tolerance : {tolerance:.12g}%")


def wait_for_next_mismatch() -> bool:
    if os.environ.get("REALTEST_NONINTERACTIVE") == "1":
        return False
    value = input("\nENTER = siguiente diferencia; q + ENTER = terminar revisión: ").strip().lower()
    return value == "q"


def ask_manual_realtest_acceptance(mismatch_count: int) -> bool:
    if os.environ.get("REALTEST_NONINTERACTIVE") == "1":
        return mismatch_count == 0

    print("\n============================================================")
    print("VALIDACIÓN MANUAL REALTEST")
    print("============================================================")
    print(f"Diferencias mostradas: {mismatch_count}")
    while True:
        value = input("¿Consideras válida la comparación completa? [s/n]: ").strip().lower()
        if value in ("s", "si", "sí", "y", "yes"):
            return True
        if value in ("n", "no"):
            return False
        print("Responde 's' o 'n'.")


def compare_candidate_trades_to_realtest(
    candidate_trades: list[dict],
    start: str,
    cutoff: str,
    run_dir: Path,
    *,
    realtest_csv: Path = REALTEST_CSV,
    manual_review: bool = True,
) -> dict:
    """Compare directly against the PureRSI RealTest CSV.

    This deliberately mirrors the old research comparator philosophy:
      * RealTest CSV is the source of truth.
      * Match by exact entry day + symbol (entry phase is also checked).
      * Closed trades compare exit, prices, quantity and PnL.
      * Only RealTest trades whose entry date is inside [start, cutoff] are compared.
      * A trade still open beyond a partial cutoff compares entry only.
      * Numeric equality uses the same historical dynamic tolerance as
        research/src/common/realtest.cpp.
      * No mismatch/exception is pre-approved in code. Every difference is
        reported and the human running the full comparison decides whether
        the observed set is acceptable.
    """
    references = [
        trade for trade in load_realtest_reference(realtest_csv)
        if start <= trade["start"] <= cutoff
    ]

    # Same deterministic ordering as the historical terminal comparator.
    references = sorted(references, key=lambda t: (t["start"], t["coin"], t["id"]))
    candidates = sorted(candidate_trades, key=lambda t: (t["start"], t["coin"], t["id"]))
    consumed: set[int] = set()

    rows: list[dict] = []
    mismatches: list[dict] = []
    fully_matched = 0
    closed_compared = 0
    open_entry_only_compared = 0

    for comparison_index, reference in enumerate(references, 1):
        available = [
            candidate for candidate in candidates
            if candidate["id"] not in consumed
            and candidate["start"] == reference["start"]
            and candidate["coin"] == reference["coin"]
        ]
        candidate = available[0] if len(available) == 1 else None
        reasons: list[str] = []
        tolerance = dynamic_tolerance_percent(reference["id"])

        if len(available) > 1:
            reasons.append("duplicate entry-date+coin candidate")
        elif candidate is None:
            reasons.append("missing candidate trade")
        else:
            consumed.add(candidate["id"])
            if candidate["direction"] != reference["direction"]:
                reasons.append("direction")
            if candidate.get("start_phase", "open") != reference.get("start_phase", "open"):
                reasons.append("entry_time")
            if not within_realtest_tolerance(reference["id"], reference["entry"], candidate["entry"]):
                reasons.append("entry_price")
            if not within_realtest_tolerance(reference["id"], reference["size"], candidate["size"]):
                reasons.append("quantity")
            if abs(candidate.get("commission", 0.0) - reference.get("commission", 0.0)) > 1e-12:
                reasons.append("commission")

            reference_closed = reference["end"] <= cutoff
            if reference_closed:
                closed_compared += 1
                if candidate["end"] != reference["end"]:
                    reasons.append("exit_date")
                if candidate.get("end_phase", "open") != reference.get("end_phase", "open"):
                    reasons.append("exit_time")
                if candidate.get("exited", False) != reference.get("exited", True):
                    reasons.append("exited_state")
                if not within_realtest_tolerance(reference["id"], reference["exit"], candidate["exit"]):
                    reasons.append("exit_price")
                if not within_realtest_tolerance(reference["id"], reference["pnl"], candidate["pnl"]):
                    reasons.append("pnl")
                comparison = "FULL_CLOSED_TRADE"
            else:
                open_entry_only_compared += 1
                comparison = "OPEN_ENTRY_ONLY"

        status = "MATCH" if not reasons else "DIFFERENT"
        if not reasons:
            fully_matched += 1

        row = {
            "comparison_index": comparison_index,
            "reference_trade_id": reference["id"],
            "candidate_trade_id": "" if candidate is None else candidate["id"],
            "coin": reference["coin"],
            "comparison": "OPEN_ENTRY_ONLY" if reference["end"] > cutoff else "FULL_CLOSED_TRADE",
            "status": status,
            "reasons": ";".join(reasons),
            "reference_start": trade_time_label(reference["start"], reference.get("start_phase", "open")),
            "candidate_start": "" if candidate is None else trade_time_label(candidate["start"], candidate.get("start_phase", "open")),
            "reference_end": trade_time_label(reference["end"], reference.get("end_phase", "open")),
            "candidate_end": "" if candidate is None else trade_time_label(candidate["end"], candidate.get("end_phase", "open")),
            "reference_entry": reference["entry"],
            "candidate_entry": "" if candidate is None else candidate["entry"],
            "reference_exit": reference["exit"],
            "candidate_exit": "" if candidate is None else candidate["exit"],
            "reference_quantity": reference["size"],
            "candidate_quantity": "" if candidate is None else candidate["size"],
            "reference_pnl": reference["pnl"],
            "candidate_pnl": "" if candidate is None else candidate["pnl"],
            "reference_commission": reference.get("commission", 0.0),
            "candidate_commission": "" if candidate is None else candidate.get("commission", 0.0),
            "tolerance_percent": tolerance,
        }
        rows.append(row)
        if reasons:
            mismatches.append({
                "comparison_index": comparison_index,
                "reference": reference,
                "candidate": candidate,
                "reasons": reasons,
                "tolerance": tolerance,
            })

    unmatched_candidates = [candidate for candidate in candidates if candidate["id"] not in consumed]
    for candidate in unmatched_candidates:
        row = {
            "comparison_index": len(rows) + 1,
            "reference_trade_id": "",
            "candidate_trade_id": candidate["id"],
            "coin": candidate["coin"],
            "comparison": "UNMATCHED_CANDIDATE",
            "status": "DIFFERENT",
            "reasons": "candidate has no RealTest entry-date+coin match",
            "reference_start": "",
            "candidate_start": trade_time_label(candidate["start"], candidate.get("start_phase", "open")),
            "reference_end": "",
            "candidate_end": trade_time_label(candidate["end"], candidate.get("end_phase", "open")),
            "reference_entry": "",
            "candidate_entry": candidate["entry"],
            "reference_exit": "",
            "candidate_exit": candidate["exit"],
            "reference_quantity": "",
            "candidate_quantity": candidate["size"],
            "reference_pnl": "",
            "candidate_pnl": candidate["pnl"],
            "reference_commission": "",
            "candidate_commission": candidate.get("commission", 0.0),
            "tolerance_percent": "",
        }
        rows.append(row)
        mismatches.append({
            "comparison_index": row["comparison_index"],
            "reference": None,
            "candidate": candidate,
            "reasons": ["candidate has no RealTest entry-date+coin match"],
            "tolerance": None,
        })

    run_dir.mkdir(parents=True, exist_ok=True)
    csv_path = run_dir / "realtest_trade_comparison.csv"
    fieldnames = tuple(rows[0].keys()) if rows else (
        "comparison_index", "reference_trade_id", "candidate_trade_id", "coin",
        "comparison", "status", "reasons",
    )
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print("\n============================================================")
    print("REALTEST DIRECT TRADE COMPARISON")
    print("============================================================")
    print(f"RealTest CSV             : {realtest_csv}")
    print(f"Comparison window        : {start}..{cutoff}")
    print(f"RealTest trades checked  : {len(references)}")
    print(f"Candidate trades total   : {len(candidates)}")
    print(f"Fully matched            : {fully_matched}")
    print(f"Differences              : {len(mismatches)}")
    print("Known exceptions in code : NONE")
    print("Numeric policy           : historical research RealTest tolerance")
    print("Only differences are shown below, one at a time.")

    stopped_early = False
    for number, mismatch in enumerate(mismatches, 1):
        print("\n\n============================================================")
        print(f"MISMATCH #{number}")
        print(f"REAL COMPARISON INDEX: {mismatch['comparison_index']}")
        print("============================================================")
        print_trade("REALTEST", mismatch["reference"])
        print_trade("CANDIDATE", mismatch["candidate"])
        print("\nDIFFERENCES")
        for reason in mismatch["reasons"]:
            print(f"  - {reason}")

        reference = mismatch["reference"]
        candidate = mismatch["candidate"]
        if reference is not None and candidate is not None:
            tolerance = mismatch["tolerance"]
            print_numeric_difference("entry", reference["entry"], candidate["entry"], tolerance)
            print_numeric_difference("quantity", reference["size"], candidate["size"], tolerance)
            if reference["end"] <= cutoff:
                print_numeric_difference("exit", reference["exit"], candidate["exit"], tolerance)
                print_numeric_difference("pnl", reference["pnl"], candidate["pnl"], tolerance)

        if manual_review and wait_for_next_mismatch():
            stopped_early = True
            break

    manual_accepted: bool | None
    if manual_review:
        if stopped_early:
            print("\nRevisión interrumpida antes de mostrar todas las diferencias; no se puede aceptar.")
            manual_accepted = False
        else:
            manual_accepted = ask_manual_realtest_acceptance(len(mismatches))
    else:
        manual_accepted = None

    automatic_match = len(mismatches) == 0
    accepted = automatic_match if not manual_review else bool(manual_accepted)
    result = "PASS" if accepted else "FAIL"

    print("\n============================================================")
    print("REALTEST COMPARISON SUMMARY")
    print("============================================================")
    print(f"Fully matched                  : {fully_matched}")
    print(f"Different/missing/unmatched    : {len(mismatches)}")
    print(f"Closed trades fully compared   : {closed_compared}")
    print(f"Open trades entry-only compared: {open_entry_only_compared}")
    print(f"Manual accepted                : {manual_accepted if manual_review else 'not requested'}")
    print(f"Comparison CSV                 : {csv_path}")

    summary = {
        "result": result,
        "policy": "DIRECT_REALTEST_MANUAL_REVIEW_V1",
        "start": start,
        "cutoff": cutoff,
        "realtestCsv": str(realtest_csv.relative_to(ROOT)) if realtest_csv.is_relative_to(ROOT) else str(realtest_csv),
        "referenceTradesInWindow": len(references),
        "candidateTradesInWindow": len(candidates),
        "fullyMatched": fully_matched,
        "differences": len(mismatches),
        "closedTradesFullyCompared": closed_compared,
        "openTradesEntryOnlyCompared": open_entry_only_compared,
        "unmatchedCandidateTrades": len(unmatched_candidates),
        "knownExceptionsInCode": 0,
        "manualReview": manual_review,
        "manualAccepted": manual_accepted,
        "reviewStoppedEarly": stopped_early,
        "comparisonCsv": str(csv_path.relative_to(ROOT)) if csv_path.is_relative_to(ROOT) else str(csv_path),
        "openTradeRule": "entry date/time + entry price + quantity only; exit/PnL ignored until closed",
        "numericTolerance": "historical research policy: min(20% + 0.2%*trade_id, 50%)",
    }
    (run_dir / "realtest_comparison_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    if not accepted:
        raise SystemExit("ERROR: RealTest comparison was not manually accepted")
    return summary
def dashboard_up() -> None:
    env_file = ROOT / "deploy/live/.env"
    cmd = ["docker", "compose"]
    if env_file.is_file():
        cmd += ["--env-file", str(env_file)]
    cmd += [
        "-f", str(ROOT / "dashboard/docker-compose.yml"),
        "-f", str(ROOT / "dashboard/docker-compose.simulation.yml"),
        "up", "-d", "--build", "--force-recreate",
    ]
    run(cmd, cwd=ROOT / "dashboard")


def execute_fast(args: argparse.Namespace) -> dict:
    ensure_required_files()
    start, end, selected_days = resolve_window(args)
    binary = build_fast_runner(args.rebuild)
    label = args.label or (f"fast_{selected_days}d" if args.days else "fast_full")
    run_dir = RUN_ROOT / label
    run_dir.mkdir(parents=True, exist_ok=True)
    trades_path = run_dir / "trades.csv"
    log = run_dir / "research.log"

    env = bundle_env()
    env.update(
        {
            "ALGOTRADING_REPLAY_DATABASE_PATH": str(HISTORICAL_DATA),
            "ALGOTRADING_REPLAY_OUTPUT_DIR": str(run_dir / "fast_artifacts"),
            "ALGOTRADING_REPLAY_START_DATE": start,
            "ALGOTRADING_REPLAY_END_DATE": end,
            "ALGOTRADING_REPLAY_TRADES_CSV": str(trades_path),
            "ALGOTRADING_REPLAY_SKIP_INTERNAL_REALTEST": "1",
        }
    )
    run([binary], env=env, log=log)

    candidate = load_fast_candidate_trades(trades_path)
    realtest = compare_candidate_trades_to_realtest(candidate, start, end, run_dir)
    summary = {
        "result": "PASS",
        "mode": "fast",
        "engine": "existing research Backtester",
        "components": "research-only; no canonical MOCK venue; no dashboard",
        "start": start,
        "end": end,
        "days": selected_days,
        "candidateTrades": len(candidate),
        "openCandidateTrades": sum(1 for trade in candidate if not trade["exited"]),
        "closedCandidateTrades": sum(1 for trade in candidate if trade["exited"]),
        "realTest": realtest,
        "trades": str(trades_path.relative_to(ROOT)),
        "log": str(log.relative_to(ROOT)),
    }
    (run_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def execute_full(args: argparse.Namespace) -> dict:
    ensure_required_files()
    start, end, selected_days = resolve_window(args)
    runner = build_full_runner(args.rebuild)
    mode = args.mode
    label = args.label or (f"{mode}_{selected_days}d" if args.days else f"{mode}_full")
    run_dir = RUN_ROOT / label
    run_dir.mkdir(parents=True, exist_ok=True)

    durable = run_dir / "mock_state"
    summary_path = run_dir / "runtime_summary.json"
    fills = run_dir / "fills.csv"
    checkpoint_state = run_dir / "replay_checkpoint_v1.txt"

    if args.resume and not args.label:
        raise SystemExit("ERROR: --resume requires an explicit --label")
    if args.stop_after_days is not None:
        if args.stop_after_days <= 0 or args.stop_after_days >= selected_days:
            raise SystemExit(
                f"ERROR: --stop-after-days must be between 1 and {selected_days - 1}"
            )
    if args.checkpoint_every is not None and args.checkpoint_every <= 0:
        raise SystemExit("ERROR: --checkpoint-every must be a positive integer")

    cmd: list[object] = [
        runner,
        "--mode", mode,
        "--csv", DATASET,
        "--mapping", MAPPING,
        "--durable", durable,
        "--output", summary_path,
        "--fills-output", fills,
        "--start", start,
        "--end", end,
        "--profile", "realtest-parity",
        "--speed", str(args.speed),
        "--checkpoint-state", checkpoint_state,
    ]

    if args.resume:
        cmd += ["--resume"]
    if args.checkpoint_every is not None:
        cmd += ["--checkpoint-every", str(args.checkpoint_every)]
    if args.stop_after_days is not None:
        cmd += ["--stop-after-days", str(args.stop_after_days)]

    if args.pace_all and (args.pace_start or args.pace_end):
        raise SystemExit("ERROR: use either --pace-all or --pace-start/--pace-end, not both")
    if args.pace_all:
        cmd += ["--pace-start", start, "--pace-end", end]
    elif args.pace_start or args.pace_end:
        if not args.pace_start or not args.pace_end:
            raise SystemExit("ERROR: --pace-start and --pace-end must be provided together")
        if args.pace_start < start or args.pace_end > end:
            raise SystemExit("ERROR: paced range must be inside the selected replay window")
        cmd += ["--pace-start", args.pace_start, "--pace-end", args.pace_end]

    if mode == "dashboard":
        state_dir = ROOT / "deploy/historical_replay/run/step58_dashboard"
        state_dir.mkdir(parents=True, exist_ok=True)
        if args.dashboard_up:
            dashboard_up()

        visual_start = args.visual_start or start
        visual_end = args.visual_end or end
        if bool(args.visual_start) != bool(args.visual_end):
            raise SystemExit("ERROR: --visual-start and --visual-end must be provided together")
        if visual_start < start or visual_end > end or visual_start > visual_end:
            raise SystemExit("ERROR: visual range must be inside the selected replay window")

        delay_ms = args.ui_delay_ms
        if args.visual_day_minutes is not None:
            delay_ms = int(round(args.visual_day_minutes * 60_000 / 2.0))

        cmd += [
            "--dashboard-state-dir", state_dir,
            "--visual-start", visual_start,
            "--visual-end", visual_end,
            "--ui-delay-ms", str(delay_ms),
        ]
        print("Dashboard: http://localhost:8080")
        print(
            f"Visual window: {visual_start}..{visual_end}; "
            f"{delay_ms} ms per OPEN/CLOSE phase"
        )

    log = run_dir / "runtime.log"
    run(cmd, log=log)

    runtime_summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if args.stop_after_days is not None:
        if runtime_summary.get("result") != "CHECKPOINTED":
            raise SystemExit("ERROR: replay did not stop at the requested safe checkpoint")
        summary = {
            "result": "CHECKPOINTED",
            "mode": mode,
            "start": start,
            "end": end,
            "days": selected_days,
            "daysProcessed": runtime_summary["daysProcessed"],
            "checkpointDate": runtime_summary["checkpointDate"],
            "checkpointState": str(checkpoint_state.relative_to(ROOT)),
            "runtimeSummary": str(summary_path.relative_to(ROOT)),
        }
        (run_dir / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        return summary

    if runtime_summary.get("result") != "PASS":
        raise SystemExit("ERROR: canonical full-system runtime did not finish PASS")
    if runtime_summary.get("reconciliation") != "CLEAN":
        raise SystemExit("ERROR: final reconciliation is not CLEAN")
    if runtime_summary.get("routeSafe") is not True:
        raise SystemExit("ERROR: final full-system runtime is not route-safe")

    candidate = trades_from_fill_csv(fills, end)
    candidate_trades_path = run_dir / "candidate_trades.csv"
    write_candidate_trades(candidate_trades_path, candidate)
    realtest = compare_candidate_trades_to_realtest(candidate, start, end, run_dir)

    summary = {
        "result": "PASS",
        "mode": mode,
        "start": start,
        "end": end,
        "days": selected_days,
        "runtimeSummary": str(summary_path.relative_to(ROOT)),
        "fills": str(fills.relative_to(ROOT)),
        "candidateTrades": str(candidate_trades_path.relative_to(ROOT)),
        "fullRunFingerprint": runtime_summary["fullRunFingerprint"],
        "economicFingerprint": runtime_summary["economicFingerprint"],
        "streamFingerprint": runtime_summary["streamFingerprint"],
        "ledgerHeadHash": runtime_summary["ledgerHeadHash"],
        "canonicalFills": runtime_summary["canonicalFills"],
        "openCandidateTrades": sum(1 for trade in candidate if not trade["exited"]),
        "closedCandidateTrades": sum(1 for trade in candidate if trade["exited"]),
        "reconciliation": runtime_summary["reconciliation"],
        "routeSafe": runtime_summary["routeSafe"],
        "realTest": realtest,
    }
    (run_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Canonical algoTrading replay front door. Three modes, optional partial-history "
            "--days, and RealTest verification at the selected cutoff."
        )
    )
    sub = p.add_subparsers(dest="mode", required=True)

    def window_args(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--label")
        sp.add_argument(
            "--start",
            default=FULL_START,
            help=f"first historical replay day (default {FULL_START})",
        )
        sp.add_argument(
            "--days",
            type=int,
            help=(
                "run exactly N available historical days starting at --start; "
                "the cutoff is the Nth day and RealTest is truncated to that cutoff"
            ),
        )
        sp.add_argument("--rebuild", action="store_true")

    fast = sub.add_parser(
        "fast",
        help="research-only Backtester: no canonical MOCK venue and no dashboard",
    )
    window_args(fast)
    fast.set_defaults(func=execute_fast)

    def full_args(sp: argparse.ArgumentParser) -> None:
        window_args(sp)
        sp.add_argument(
            "--speed",
            type=float,
            default=1500.0,
            help="TimeHandler speed for explicitly paced dates",
        )
        sp.add_argument(
            "--pace-all",
            action="store_true",
            help="pace the entire selected replay window through TimeHandler",
        )
        sp.add_argument("--pace-start", help="first selected date to pace through TimeHandler")
        sp.add_argument("--pace-end", help="last selected date to pace through TimeHandler")
        sp.add_argument(
            "--checkpoint-every",
            type=int,
            help="persist a safe day-boundary restart checkpoint every N processed days",
        )
        sp.add_argument(
            "--stop-after-days",
            type=int,
            help="test restart: stop cleanly after N processed days at a durable checkpoint",
        )
        sp.add_argument(
            "--resume",
            action="store_true",
            help="resume the explicit --label from its last safe day-boundary checkpoint",
        )

    system = sub.add_parser(
        "system",
        help="full Strategy/Risk/Planner/CanonicalVenueAdapter/MOCK pipeline, no dashboard",
    )
    full_args(system)
    system.set_defaults(func=execute_full)

    dashboard = sub.add_parser(
        "dashboard",
        help="same full pipeline plus Step58 dashboard simulation provider",
    )
    full_args(dashboard)
    dashboard.add_argument("--visual-start")
    dashboard.add_argument("--visual-end")
    dashboard.add_argument("--ui-delay-ms", type=int, default=0)
    dashboard.add_argument(
        "--visual-day-minutes",
        type=float,
        help=(
            "display pacing only; 10 means ~5 min OPEN + 5 min CLOSE per historical day"
        ),
    )
    dashboard.add_argument(
        "--dashboard-up",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="start/rebuild dashboard with docker-compose.simulation.yml",
    )
    dashboard.set_defaults(func=execute_full)
    return p


def main() -> int:
    args = parser().parse_args()
    if hasattr(args, "speed") and (not math.isfinite(args.speed) or args.speed <= 0):
        raise SystemExit("ERROR: --speed must be finite and positive")
    if getattr(args, "visual_day_minutes", None) is not None:
        if not math.isfinite(args.visual_day_minutes) or args.visual_day_minutes <= 0:
            raise SystemExit("ERROR: --visual-day-minutes must be finite and positive")

    summary = args.func(args)
    if summary.get("result") == "CHECKPOINTED":
        print("\n============================================================")
        print("CANONICAL REPLAY: CHECKPOINTED — READY TO RESUME")
        print("============================================================")
        print(f"mode={summary['mode']}")
        print(
            f"window={summary['start']}..{summary['end']} "
            f"days={summary['days']}"
        )
        print(
            f"checkpointDate={summary['checkpointDate']} "
            f"daysProcessed={summary['daysProcessed']}"
        )
        print(f"checkpointState={summary['checkpointState']}")
        return 0

    print("\n============================================================")
    print("CANONICAL REPLAY: PASS")
    print("============================================================")
    print(f"mode={summary['mode']}")
    print(f"window={summary['start']}..{summary['end']} days={summary['days']}")
    if "canonicalFills" in summary:
        print(f"canonicalFills={summary['canonicalFills']}")
        print(f"fullRunFingerprint={summary['fullRunFingerprint']}")
    rt = summary.get("realTest", {})
    print(
        "RealTest=PASS (direct CSV comparison); "
        f"differences={rt.get('differences', 0)}; "
        f"manualAccepted={rt.get('manualAccepted')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
