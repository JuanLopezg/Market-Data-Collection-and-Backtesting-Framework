#!/usr/bin/env python3
from __future__ import annotations
import argparse
import csv
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

INCLUDE_DIRS = [
    "common_types", "utils", "data_types", "contracts", "transport", "market",
    "position", "account", "analytics", "backtest", "signal", "portfolio", "risk",
    "sizing", "rebalance", "execution", "exchange", "runtime", "persistence",
    "recovery", "testing", "strategy", "strategy/strategies", "ranker", "indicator",
    "universe", "filter",
]


def run(cmd, cwd: Path, *, capture=False, check=True, env=None):
    print("+ " + " ".join(shlex.quote(str(x)) for x in cmd), flush=True)
    p = subprocess.run(
        [str(x) for x in cmd], cwd=str(cwd), text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        env=env, check=False,
    )
    if check and p.returncode:
        if capture and p.stdout:
            print(p.stdout, file=sys.stderr)
        raise SystemExit(f"ERROR: command failed ({p.returncode})")
    return p


def envfile(path: Path):
    out = {}
    if not path.is_file():
        return out
    for raw in path.read_text().splitlines():
        s = raw.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        out[k.strip()] = v.strip().strip('"').strip("'")
    return out


def compose_base(root: Path):
    return [
        "docker", "compose",
        "--env-file", str(root / "deploy/historical_replay/.env"),
        "-f", str(root / "deploy/historical_replay/docker-compose.yml"),
    ]


def compile_bridge(root: Path, output: Path):
    source = root / "tools/historical_replay/t19_realtest_compare.cpp"
    if not source.is_file():
        raise SystemExit(f"ERROR: missing T19 RealTest bridge source: {source}")
    lib = root / "build/lib/src/libalgolib.so"
    if not lib.is_file():
        raise SystemExit(f"ERROR: build library missing: {lib}; run ninja -C build -j8 first")

    cflags = subprocess.check_output(
        ["pkg-config", "--cflags", "libpq", "libnats", "fmt", "log4cpp"], text=True
    ).split()
    libs = subprocess.check_output(
        ["pkg-config", "--libs", "libpq", "libnats", "fmt", "log4cpp"], text=True
    ).split()
    includes = [f"-I{root / 'lib/src' / d}" for d in INCLUDE_DIRS]
    cmd = [
        "g++", "-std=c++23", "-O0", "-g", "-Wall", "-Wextra", "-Wpedantic",
        str(source), str(root / "research/src/realtest.cpp"),
        f"-I{root / 'research/src'}", *includes, *cflags,
        str(lib), *libs, f"-Wl,-rpath,{root / 'build/lib/src'}", "-pthread",
        "-o", str(output),
    ]
    run(cmd, root)


def export_fills(root: Path, output: Path):
    e = envfile(root / "deploy/historical_replay/.env")
    user = e.get("POSTGRES_USER", "algotrading")
    db = e.get("POSTGRES_DB", "algotrading_historical_replay")
    sql = (
        "SELECT fill_id,order_id,strategy_id,timestamp,coin,side,quantity,price,commission "
        "FROM trading_fills ORDER BY timestamp,fill_id;"
    )
    cmd = [
        *compose_base(root), "exec", "-T", "postgres", "psql", "-U", user, "-d", db,
        "-At", "-F", "|", "-c", sql,
    ]
    p = run(cmd, root, capture=True)
    rows = [line.strip() for line in p.stdout.splitlines() if line.strip()]
    if not rows:
        raise SystemExit("ERROR: T19 RealTest gate found no persisted fills")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["fill_id", "order_id", "strategy_id", "timestamp", "coin", "side", "quantity", "price", "commission"])
        for row in rows:
            parts = row.split("|")
            if len(parts) != 9:
                raise SystemExit(f"ERROR: invalid persisted fill row: {row}")
            writer.writerow(parts)
    print(f"T19 RealTest exported fills: {len(rows)} -> {output}")
    return len(rows)


def main():
    ap = argparse.ArgumentParser(description="T19 exact research RealTest gate for new TimeHandler replay")
    ap.add_argument("--root", default=".")
    ap.add_argument("--label", default="t19_full_history")
    ap.add_argument("--mode", choices=("equal-weight", "vol-target"), default="equal-weight")
    ap.add_argument("--realtest-csv", default="storage/backtests/final_tests/pureRSI.csv")
    ap.add_argument("--historical-data", default="storage/databases/1d_cmc.csv")
    ap.add_argument("--compile-only", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    evidence = root / "deploy/historical_replay/run"
    evidence.mkdir(parents=True, exist_ok=True)
    binary = evidence / "t19_realtest_compare"
    compile_bridge(root, binary)
    run([str(binary), "--help"], root)
    print("PASS: T19 RealTest bridge compiled against exact research comparator")
    if args.compile_only:
        return 0

    realtest = (root / args.realtest_csv).resolve() if not Path(args.realtest_csv).is_absolute() else Path(args.realtest_csv)
    historical = (root / args.historical_data).resolve() if not Path(args.historical_data).is_absolute() else Path(args.historical_data)
    if not realtest.is_file():
        raise SystemExit(f"ERROR: RealTest CSV not found: {realtest}")
    if not historical.is_file():
        raise SystemExit(f"ERROR: historical data not found: {historical}")

    fills_csv = evidence / f"{args.label}_realtest_fills.csv"
    comparison_csv = evidence / f"{args.label}_realtest_comparison.csv"
    log_path = evidence / f"{args.label}_realtest.log"
    fill_count = export_fills(root, fills_csv)

    env = os.environ.copy()
    env["REALTEST_NONINTERACTIVE"] = "1"
    cmd = [
        str(binary), "--fills-csv", str(fills_csv),
        "--realtest-csv", str(realtest),
        "--historical-data", str(historical),
        "--comparison-csv", str(comparison_csv),
        "--portfolio-mode", args.mode,
    ]
    print("+ " + " ".join(shlex.quote(str(x)) for x in cmd), flush=True)
    with log_path.open("w") as log:
        p = subprocess.run(cmd, cwd=str(root), text=True, stdout=log, stderr=subprocess.STDOUT, env=env, check=False)
    if p.returncode:
        print(log_path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        raise SystemExit(f"ERROR: T19 RealTest bridge failed ({p.returncode})")

    # Show the exact research output in the campaign log as well as preserving it on disk.
    text = log_path.read_text(encoding="utf-8", errors="replace")
    print(text)

    checker = root / "tools/historical_replay/check_t19_realtest_baseline.py"
    run([sys.executable, str(checker), "--root", ".", "--mode", args.mode, "--log", str(log_path)], root)

    summary = {
        "result": "PASS",
        "mode": args.mode,
        "fill_count": fill_count,
        "realtest_csv": str(realtest),
        "historical_data": str(historical),
        "log": str(log_path),
        "comparison_csv": str(comparison_csv),
        "policy": "exact research/src/realtest.cpp",
        "accepted_difference_policy": "locked exact identities from tools/distributed_compare/realtest_known_baseline.json",
    }
    out = evidence / f"{args.label}_realtest_summary.json"
    out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"evidence={out}")
    print("PASS: T19 aggregate baseline + exact RealTest known-exception gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
