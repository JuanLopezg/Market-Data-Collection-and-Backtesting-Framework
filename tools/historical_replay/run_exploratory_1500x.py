#!/usr/bin/env python3
"""T16.5 exploratory historical distributed replay at 1500x.

This runner exercises the currently connected production-like path through
NotionalOrderPlan.  It intentionally does NOT claim fill/PnL equivalence: the
current notional-plan boundary is not yet wired into simulated order submission.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
from typing import Dict, Optional

DEFAULT_SPEED = 1500.0
DEFAULT_SIMULATED_START = "2020-04-10T12:00:00Z"
DEFAULT_TARGET_COMPLETED = 20200708
RUNTIME_CONSUMERS = [
    "simulated-exchange",
    "exchange-gateway",
    "execution-state",
    "order-planner",
    "portfolio-risk",
    "strategy",
]
ALL_RUNTIME = ["historical-market-data", *RUNTIME_CONSUMERS]


def die(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def run(cmd, *, cwd: pathlib.Path, capture=False, check=True, env=None) -> str:
    printable = " ".join(shlex.quote(str(x)) for x in cmd)
    print(f"+ {printable}", flush=True)
    result = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        check=False,
        env=env,
    )
    if check and result.returncode != 0:
        if capture and result.stdout:
            print(result.stdout, file=sys.stderr)
        die(f"command failed with exit code {result.returncode}: {printable}")
    return result.stdout or ""


def parse_env_file(path: pathlib.Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    if not path.exists():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def parse_utc(value: str) -> dt.datetime:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as exc:
        die(f"invalid UTC timestamp {value!r}; expected YYYY-MM-DDTHH:MM:SSZ")
    return parsed.replace(tzinfo=dt.timezone.utc)


def parse_yyyymmdd(value: int) -> dt.date:
    try:
        return dt.datetime.strptime(str(value), "%Y%m%d").date()
    except ValueError:
        die(f"invalid YYYYMMDD target: {value}")


def expected_wall_seconds(simulated_start: str, target_completed: int, speed: float) -> float:
    start = parse_utc(simulated_start)
    target_day = parse_yyyymmdd(target_completed)
    # A daily candle target_completed becomes fully visible at the following UTC midnight.
    target_visible = dt.datetime.combine(
        target_day + dt.timedelta(days=1), dt.time(), tzinfo=dt.timezone.utc
    )
    delta = (target_visible - start).total_seconds()
    if delta <= 0:
        die("target completed date must become visible after the simulated start")
    return delta / speed


def compose_base(root: pathlib.Path):
    compose_file = root / "deploy/historical_replay/docker-compose.yml"
    env_file = root / "deploy/historical_replay/.env"
    return [
        "docker", "compose",
        "--env-file", str(env_file),
        "-f", str(compose_file),
    ]


def compose(root: pathlib.Path, args, *, capture=False, check=True, env=None) -> str:
    return run([*compose_base(root), *args], cwd=root, capture=capture, check=check, env=env)


def pg_scalar(root: pathlib.Path, sql: str) -> str:
    env = parse_env_file(root / "deploy/historical_replay/.env")
    user = env.get("POSTGRES_USER", "algotrading")
    db = env.get("POSTGRES_DB", "algotrading_historical_replay")
    out = compose(
        root,
        ["exec", "-T", "postgres", "psql", "-U", user, "-d", db, "-Atqc", sql],
        capture=True,
        check=False,
    )
    return out.strip().splitlines()[-1].strip() if out.strip() else ""


def pg_int(root: pathlib.Path, sql: str) -> int:
    value = pg_scalar(root, sql)
    try:
        return int(value or 0)
    except ValueError:
        return 0


def checkpoints(root: pathlib.Path) -> Dict[str, object]:
    def safe_int(sql: str) -> int:
        return pg_int(root, sql)

    snapshot = pg_scalar(
        root,
        "SELECT COALESCE(snapshot::text,'{}') FROM trading_runtime_state WHERE singleton=TRUE;",
    )
    snapshot_obj: Dict[str, object] = {}
    if snapshot:
        try:
            snapshot_obj = json.loads(snapshot)
        except json.JSONDecodeError:
            snapshot_obj = {"raw": snapshot}

    return {
        "strategy_latest": safe_int(
            "SELECT COALESCE(MAX(timestamp),0) FROM strategy_market_update_checkpoint;"
        ),
        "strategy_days": safe_int("SELECT COUNT(*) FROM strategy_market_update_checkpoint;"),
        "risk_latest": safe_int(
            "SELECT COALESCE(MAX(timestamp),0) FROM portfolio_risk_live_decision_checkpoint;"
        ),
        "risk_days": safe_int("SELECT COUNT(*) FROM portfolio_risk_live_decision_checkpoint;"),
        "planner_latest": safe_int(
            "SELECT COALESCE(MAX(timestamp),0) FROM order_planner_live_notional_checkpoint;"
        ),
        "planner_days": safe_int("SELECT COUNT(*) FROM order_planner_live_notional_checkpoint;"),
        "execution_last_decision": int(snapshot_obj.get("last_bar_close_timestamp", 0) or 0),
        "execution_last_execution": int(snapshot_obj.get("last_execution_timestamp", 0) or 0),
        "cash": snapshot_obj.get("account_cash"),
        "positions": snapshot_obj.get("account_positions", {}),
        "tracked_orders": len(snapshot_obj.get("orders", []) or []),
        "fills": safe_int("SELECT COUNT(*) FROM trading_fills;"),
        "sim_exchange_latest": safe_int(
            "SELECT COALESCE(MAX(latest_timestamp),0) FROM simulated_exchange_state;"
        ),
        "sim_exchange_cash": pg_scalar(
            root, "SELECT COALESCE(MAX(cash),0)::text FROM simulated_exchange_state;"
        ),
    }


def target_committed(root: pathlib.Path, target: int) -> bool:
    logs = compose(root, ["logs", "--no-color", "historical-market-data"], capture=True, check=False)
    return bool(re.search(rf"event=historical_day_committed\s+date={target}(?:\s|$)", logs))


EXPECTED_ONE_SHOT_SERVICES = {
    "market-data-volume-init",
    "market-data-db-init",
}


def container_failures(root: pathlib.Path) -> str:
    output = compose(root, ["ps", "--all"], capture=True, check=False)
    bad = []
    for line in output.splitlines():
        low = line.lower()
        # The two bootstrap jobs are intentionally one-shot. Exited (0) means success
        # and must not abort the replay. Non-zero exits remain failures.
        if any(service in low for service in EXPECTED_ONE_SHOT_SERVICES):
            if "exited (0)" in low:
                continue
        if "exited" in low or "dead" in low or "unhealthy" in low or "restarting" in low:
            bad.append(line)
    return "\n".join(bad)


def save_evidence(root: pathlib.Path, report: Dict[str, object]) -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = root / "deploy/historical_replay/run" / f"t16_5_1500x_{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "summary.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    logs = compose(root, ["logs", "--no-color"], capture=True, check=False)
    (out_dir / "compose.log").write_text(logs, encoding="utf-8")
    ps = compose(root, ["ps", "--all"], capture=True, check=False)
    (out_dir / "compose_ps.txt").write_text(ps, encoding="utf-8")
    (out_dir / "time.env").write_text(
        (root / "deploy/historical_replay/run/time.env").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    return out_dir


def print_status(cp: Dict[str, object], target: int) -> None:
    print(
        "STATUS "
        f"target={target} strategy={cp['strategy_latest']} risk={cp['risk_latest']} "
        f"execution={cp['execution_last_decision']} planner={cp['planner_latest']} "
        f"fills={cp['fills']} positions={len(cp.get('positions', {}) or {})}",
        flush=True,
    )


def dry_run_plan(args) -> int:
    wall = expected_wall_seconds(args.simulated_start, args.target_completed, args.speed)
    print("T16.5 exploratory replay plan")
    print(f"speed={args.speed:g}x")
    print(f"simulated_start={args.simulated_start}")
    print(f"target_completed={args.target_completed}")
    print(f"expected_wall_to_visibility_seconds={wall:.1f}")
    print(f"expected_wall_to_visibility_minutes={wall/60.0:.2f}")
    print("acceptance_boundary=NotionalOrderPlan (fills/PnL are NOT expected yet)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the T16.5 90-day exploratory historical replay at 1500x")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--speed", type=float, default=DEFAULT_SPEED)
    parser.add_argument("--simulated-start", default=DEFAULT_SIMULATED_START)
    parser.add_argument("--target-completed", type=int, default=DEFAULT_TARGET_COMPLETED)
    parser.add_argument("--poll-seconds", type=float, default=5.0)
    parser.add_argument("--status-seconds", type=float, default=60.0)
    parser.add_argument("--drain-seconds", type=float, default=180.0)
    parser.add_argument("--max-wall-seconds", type=float, default=7200.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if not (args.speed > 0.0):
        die("speed must be positive")
    if args.poll_seconds <= 0 or args.status_seconds <= 0 or args.drain_seconds <= 0:
        die("poll/status/drain seconds must be positive")

    if args.dry_run:
        return dry_run_plan(args)

    root = pathlib.Path(args.root).resolve()
    required = [
        root / "deploy/historical_replay/docker-compose.yml",
        root / "deploy/historical_replay/.env",
        root / "deploy/historical_replay/build_runtime_bundle.sh",
        root / "tools/historical_replay/create_time_env.py",
        root / "tools/historical_replay/normalize_historical_csv.py",
    ]
    for path in required:
        if not path.exists():
            die(f"missing required file: {path}")

    env_values = parse_env_file(root / "deploy/historical_replay/.env")
    data_setting = env_values.get("HISTORICAL_DATA_PATH", "../../storage/databases/1d_cmc.csv")
    data_path = pathlib.Path(data_setting)
    if not data_path.is_absolute():
        data_path = (root / "deploy/historical_replay" / data_path).resolve()
    if not data_path.exists():
        die(f"historical dataset not found: {data_path}")

    normalized_data = root / "deploy/historical_replay/run/1d_cmc_by_date.csv"
    print(f"Normalizing historical dataset by date,symbol -> {normalized_data}", flush=True)
    normalize_out = run(
        [sys.executable, str(root / "tools/historical_replay/normalize_historical_csv.py"),
         "--input", str(data_path), "--output", str(normalized_data)],
        cwd=root, capture=True,
    ).strip()
    print(normalize_out, flush=True)
    try:
        normalized_meta = json.loads(normalize_out.splitlines()[-1])
    except (json.JSONDecodeError, IndexError) as exc:
        die(f"cannot parse normalized dataset metadata: {exc}")

    start_day = parse_utc(args.simulated_start).date().isoformat()
    target_day = parse_yyyymmdd(args.target_completed).isoformat()
    if normalized_meta["min_date"] > start_day:
        die(f"dataset starts at {normalized_meta['min_date']}, after simulated start {start_day}")
    if normalized_meta["max_date"] < target_day:
        die(f"dataset ends at {normalized_meta['max_date']}, before target {target_day}")

    compose_env = os.environ.copy()
    compose_env["HISTORICAL_DATA_PATH"] = str(normalized_data)
    # docker compose interpolation gives shell environment precedence over --env-file.
    os.environ["HISTORICAL_DATA_PATH"] = str(normalized_data)

    wall_expected = expected_wall_seconds(args.simulated_start, args.target_completed, args.speed)
    if wall_expected > args.max_wall_seconds:
        die(
            f"configured target needs about {wall_expected:.0f}s wall time, above --max-wall-seconds={args.max_wall_seconds:.0f}"
        )

    print("============================================================")
    print("T16.5 — EXPLORATORY HISTORICAL REPLAY @ 1500x")
    print("============================================================")
    print(f"dataset_source={data_path}")
    print(f"dataset_normalized={normalized_data}")
    print(f"dataset_range={normalized_meta['min_date']}..{normalized_meta['max_date']} rows={normalized_meta['rows']}")
    print(f"speed={args.speed:g}x")
    print(f"simulated_start={args.simulated_start}")
    print(f"target_completed={args.target_completed}")
    print(f"expected_wall_minutes≈{wall_expected/60.0:.2f}")
    print("boundary=NotionalOrderPlan; fills/PnL not accepted in this step")

    run(["docker", "compose", "version"], cwd=root)
    run([str(root / "deploy/historical_replay/build_runtime_bundle.sh")], cwd=root)

    time_env = root / "deploy/historical_replay/run/time.env"
    time_env.parent.mkdir(parents=True, exist_ok=True)
    time_env.write_text("# placeholder used only while building the image\n", encoding="utf-8")

    compose(root, ["--profile", "runtime", "config"], capture=True)
    compose(root, ["--profile", "runtime", "build"])
    compose(root, ["--profile", "runtime", "down", "-v", "--remove-orphans"], check=False)
    compose(root, ["up", "-d", "nats", "postgres"])
    # Ensure the fresh named SQLite volume is writable by the runtime UID before any service starts.
    compose(root, ["run", "--rm", "market-data-volume-init"])

    # Sample real UTC only after image build + infrastructure bootstrap, minimizing
    # unwanted simulated-time advance before business services start.
    run(
        [
            sys.executable,
            str(root / "tools/historical_replay/create_time_env.py"),
            "--speed", format(args.speed, ".17g"),
            "--simulated-reference", args.simulated_start,
            "--output", str(time_env),
        ],
        cwd=root,
    )

    # Create canonical SQLite + schema without publishing any historical day.
    compose(root, ["--profile", "runtime", "run", "--rm", "market-data-db-init"])

    # Bring up consumers before the feeder so prerequisites/reconciliation can settle.
    compose(root, ["--profile", "runtime", "up", "-d", "--no-build", *RUNTIME_CONSUMERS])
    print("Waiting 8 real seconds for subscriptions/reconciliation bootstrap...", flush=True)
    time.sleep(8.0)
    failures = container_failures(root)
    if failures:
        print(failures, file=sys.stderr)
        die("a service exited/restarted/unhealthy before the historical feeder started")

    compose(root, ["--profile", "runtime", "up", "-d", "--no-build", "historical-market-data"])

    started = time.monotonic()
    next_status = started
    reached = False
    while time.monotonic() - started <= args.max_wall_seconds:
        if target_committed(root, args.target_completed):
            reached = True
            break
        now = time.monotonic()
        if now >= next_status:
            cp = checkpoints(root)
            print_status(cp, args.target_completed)
            failures = container_failures(root)
            if failures:
                print(failures, file=sys.stderr)
                die("a replay service exited/restarted/unhealthy before target completion")
            next_status = now + args.status_seconds
        time.sleep(args.poll_seconds)

    if not reached:
        report = {"result": "FAIL", "reason": "target_not_committed", **checkpoints(root)}
        evidence = save_evidence(root, report)
        die(f"target {args.target_completed} was not committed; evidence: {evidence}")

    print(f"Target market-data day {args.target_completed} committed; stopping feeder.")
    compose(root, ["stop", "historical-market-data"])

    drain_started = time.monotonic()
    final_cp: Dict[str, object] = {}
    while time.monotonic() - drain_started <= args.drain_seconds:
        final_cp = checkpoints(root)
        print_status(final_cp, args.target_completed)
        required_latest = [
            int(final_cp["strategy_latest"]),
            int(final_cp["risk_latest"]),
            int(final_cp["execution_last_decision"]),
            int(final_cp["planner_latest"]),
        ]
        if all(value >= args.target_completed for value in required_latest):
            break
        time.sleep(args.poll_seconds)

    required_latest = [
        int(final_cp.get("strategy_latest", 0)),
        int(final_cp.get("risk_latest", 0)),
        int(final_cp.get("execution_last_decision", 0)),
        int(final_cp.get("planner_latest", 0)),
    ]
    pass_pipeline = all(value >= args.target_completed for value in required_latest)

    # Scan high-severity evidence. Reconciliation-blocked/fatal is a hard exploratory failure.
    all_logs = compose(root, ["logs", "--no-color"], capture=True, check=False)
    critical_lines = [
        line for line in all_logs.splitlines()
        if "event=fatal" in line or "event=reconciliation_blocked" in line
    ]

    report: Dict[str, object] = {
        "result": "PASS" if pass_pipeline and not critical_lines else "FAIL",
        "scope": "planning_pipeline_through_notional_order_plan",
        "speed": args.speed,
        "simulated_start": args.simulated_start,
        "target_completed": args.target_completed,
        "wall_seconds": round(time.monotonic() - started, 3),
        "expected_wall_seconds": round(wall_expected, 3),
        "critical_log_lines": critical_lines,
        "execution_boundary_note": (
            "Current new replay is not yet wired from NotionalOrderPlan into simulated order submission; "
            "fills/PnL are therefore not acceptance criteria in T16.5."
        ),
        **final_cp,
    }
    evidence = save_evidence(root, report)

    # Freeze business services after evidence capture; keep NATS/Postgres alive for inspection.
    compose(root, ["stop", *RUNTIME_CONSUMERS], check=False)

    print("============================================================")
    print(json.dumps(report, indent=2, sort_keys=True))
    print(f"evidence={evidence}")
    if report["result"] != "PASS":
        print("FAIL: T16.5 exploratory replay did not reach a clean planning checkpoint", file=sys.stderr)
        return 1

    print("PASS: T16.5 1500x exploratory replay reached the target through NotionalOrderPlan")
    print("NOTE: this is NOT a fill/PnL equivalence PASS; execution wiring is still pending.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
