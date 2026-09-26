#!/usr/bin/env python3
"""T17 short distributed historical execution replay.

Acceptance boundary:
  close(T) -> NotionalOrderPlan -> executable quantity -> T+1 OPEN ->
  ExchangeGateway -> SimulatedExchange -> Fill -> ExecutionState.

The default window is intentionally short (~3.4 wall minutes at 1500x after the
feeder starts) so T17 validates real economic execution without another long run.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
from typing import Dict, Iterable, List, Optional, Tuple

DEFAULT_SPEED = 1500.0
DEFAULT_SIMULATED_START = "2020-04-10T12:00:00Z"
DEFAULT_TARGET_COMPLETED = 20200413
RUNTIME_CONSUMERS = [
    "simulated-exchange",
    "exchange-gateway",
    "execution-state",
    "order-planner",
    "portfolio-risk",
    "strategy",
]
EXPECTED_ONE_SHOT_SERVICES = {"market-data-volume-init", "market-data-db-init"}


def die(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def run(cmd, *, cwd: pathlib.Path, capture=False, check=True, env=None) -> str:
    printable = " ".join(shlex.quote(str(x)) for x in cmd)
    print(f"+ {printable}", flush=True)
    result = subprocess.run(
        [str(x) for x in cmd], cwd=str(cwd), text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        check=False, env=env,
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
    except ValueError:
        die(f"invalid UTC timestamp {value!r}; expected YYYY-MM-DDTHH:MM:SSZ")
    return parsed.replace(tzinfo=dt.timezone.utc)


def parse_yyyymmdd(value: int) -> dt.date:
    try:
        return dt.datetime.strptime(str(value), "%Y%m%d").date()
    except ValueError:
        die(f"invalid YYYYMMDD date: {value}")


def yyyymmdd(value: dt.date) -> int:
    return int(value.strftime("%Y%m%d"))


def next_day(value: int) -> int:
    return yyyymmdd(parse_yyyymmdd(value) + dt.timedelta(days=1))


def expected_wall_seconds(simulated_start: str, target_completed: int, speed: float) -> float:
    start = parse_utc(simulated_start)
    target_day = parse_yyyymmdd(target_completed)
    target_visible = dt.datetime.combine(
        target_day + dt.timedelta(days=1), dt.time(), tzinfo=dt.timezone.utc
    )
    delta = (target_visible - start).total_seconds()
    if delta <= 0:
        die("target completed date must become visible after simulated start")
    return delta / speed


def compose_base(root: pathlib.Path) -> List[str]:
    return [
        "docker", "compose",
        "--env-file", str(root / "deploy/historical_replay/.env"),
        "-f", str(root / "deploy/historical_replay/docker-compose.yml"),
    ]


def compose(root: pathlib.Path, args, *, capture=False, check=True, env=None) -> str:
    return run([*compose_base(root), *args], cwd=root, capture=capture, check=check, env=env)


def pg_rows(root: pathlib.Path, sql: str) -> List[str]:
    env = parse_env_file(root / "deploy/historical_replay/.env")
    user = env.get("POSTGRES_USER", "algotrading")
    db = env.get("POSTGRES_DB", "algotrading_historical_replay")
    out = compose(
        root,
        ["exec", "-T", "postgres", "psql", "-U", user, "-d", db, "-Atqc", sql],
        capture=True, check=False,
    )
    return [line.strip() for line in out.splitlines() if line.strip()]


def pg_scalar(root: pathlib.Path, sql: str) -> str:
    rows = pg_rows(root, sql)
    return rows[-1] if rows else ""


def pg_int(root: pathlib.Path, sql: str) -> int:
    value = pg_scalar(root, sql)
    try:
        return int(value or 0)
    except ValueError:
        return 0


def checkpoints(root: pathlib.Path) -> Dict[str, object]:
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
        "strategy_latest": pg_int(root, "SELECT COALESCE(MAX(timestamp),0) FROM strategy_market_update_checkpoint;"),
        "strategy_days": pg_int(root, "SELECT COUNT(*) FROM strategy_market_update_checkpoint;"),
        "risk_latest": pg_int(root, "SELECT COALESCE(MAX(timestamp),0) FROM portfolio_risk_live_decision_checkpoint;"),
        "risk_days": pg_int(root, "SELECT COUNT(*) FROM portfolio_risk_live_decision_checkpoint;"),
        "planner_latest": pg_int(root, "SELECT COALESCE(MAX(timestamp),0) FROM order_planner_live_notional_checkpoint;"),
        "planner_days": pg_int(root, "SELECT COUNT(*) FROM order_planner_live_notional_checkpoint;"),
        "execution_last_decision": int(snapshot_obj.get("last_bar_close_timestamp", 0) or 0),
        "execution_last_execution": int(snapshot_obj.get("last_execution_timestamp", 0) or 0),
        "cash": snapshot_obj.get("account_cash"),
        "positions": snapshot_obj.get("account_positions", {}),
        "tracked_orders": len(snapshot_obj.get("orders", []) or []),
        "fills": pg_int(root, "SELECT COUNT(*) FROM trading_fills;"),
        "sim_exchange_latest": pg_int(root, "SELECT COALESCE(MAX(latest_timestamp),0) FROM simulated_exchange_state;"),
        "sim_exchange_cash": pg_scalar(root, "SELECT COALESCE(MAX(cash),0)::text FROM simulated_exchange_state;"),
    }


def target_committed(root: pathlib.Path, target: int) -> bool:
    logs = compose(root, ["logs", "--no-color", "historical-market-data"], capture=True, check=False)
    return bool(re.search(rf"event=historical_day_committed\s+date={target}(?:\s|$)", logs))


def container_failures(root: pathlib.Path) -> str:
    output = compose(root, ["ps", "--all"], capture=True, check=False)
    bad = []
    for line in output.splitlines():
        low = line.lower()
        if any(service in low for service in EXPECTED_ONE_SHOT_SERVICES) and "exited (0)" in low:
            continue
        if "exited" in low or "dead" in low or "unhealthy" in low or "restarting" in low:
            bad.append(line)
    return "\n".join(bad)


def load_market_prices(path: pathlib.Path, keys: Iterable[Tuple[int, str]]) -> Dict[Tuple[int, str], Tuple[float, float]]:
    wanted = set(keys)
    result: Dict[Tuple[int, str], Tuple[float, float]] = {}
    if not wanted:
        return result
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            date = int(row["date"].replace("-", ""))
            key = (date, row["symbol"])
            if key in wanted:
                result[key] = (float(row["open"]), float(row["close"]))
    return result


def economic_validation(root: pathlib.Path, normalized_data: pathlib.Path) -> Dict[str, object]:
    snapshot_text = pg_scalar(
        root,
        "SELECT COALESCE(snapshot::text,'{}') FROM trading_runtime_state WHERE singleton=TRUE;",
    )
    snapshot = json.loads(snapshot_text or "{}")
    tracked_orders = {int(item["order_id"]): item for item in snapshot.get("orders", [])}

    fill_rows = pg_rows(
        root,
        "SELECT json_build_object("
        "'fill_id',fill_id,'order_id',order_id,'strategy_id',strategy_id,"
        "'timestamp',timestamp,'coin',coin,'side',side,'quantity',quantity,"
        "'price',price,'commission',commission)::text "
        "FROM trading_fills ORDER BY fill_id;",
    )
    fills = [json.loads(row) for row in fill_rows]
    fills_by_order: Dict[int, List[dict]] = {}
    for fill in fills:
        fills_by_order.setdefault(int(fill["order_id"]), []).append(fill)

    plan_rows = pg_rows(
        root,
        "SELECT plan_payload FROM order_planner_live_notional_checkpoint ORDER BY timestamp;",
    )
    planned: Dict[int, dict] = {}
    for row in plan_rows:
        payload = json.loads(row)
        for order in payload.get("submit_orders", []):
            oid = int(order["order_id"])
            if oid in planned and planned[oid] != order:
                raise RuntimeError(f"conflicting planned order id {oid}")
            planned[oid] = order

    if not planned:
        return {"ok": False, "errors": ["no planned submit orders"], "planned_submit_orders": 0, "fills_checked": len(fills)}
    if not fills:
        return {"ok": False, "errors": ["no fills produced"], "planned_submit_orders": len(planned), "fills_checked": 0}

    keys = set()
    for order in planned.values():
        decision = int(order["decision_timestamp"])
        coin = str(order["coin"])
        keys.add((decision, coin))
        keys.add((next_day(decision), coin))
    prices = load_market_prices(normalized_data, keys)

    errors: List[str] = []
    checks: List[dict] = []
    for oid, plan in sorted(planned.items()):
        tracked = tracked_orders.get(oid)
        if tracked is None:
            errors.append(f"order {oid}: missing from execution-state snapshot")
            continue

        decision = int(plan["decision_timestamp"])
        execution = next_day(decision)
        coin = str(plan["coin"])
        reference_close = float(plan["reference_close"])
        notional = float(plan["notional_usd"])
        expected_qty = notional / reference_close
        actual_qty = float(tracked["quantity"])

        if int(tracked["created_at"]) != decision:
            errors.append(f"order {oid}: created_at != decision_timestamp")
        if int(tracked["active_from"]) != execution:
            errors.append(f"order {oid}: active_from != T+1")
        if not math.isclose(actual_qty, expected_qty, rel_tol=1e-12, abs_tol=1e-12):
            errors.append(f"order {oid}: quantity {actual_qty} != notional/close {expected_qty}")

        close_key = (decision, coin)
        open_key = (execution, coin)
        if close_key not in prices:
            errors.append(f"order {oid}: missing CSV close(T) for {coin}/{decision}")
        elif not math.isclose(reference_close, prices[close_key][1], rel_tol=1e-12, abs_tol=1e-12):
            errors.append(f"order {oid}: reference_close != canonical CSV close(T)")

        order_fills = fills_by_order.get(oid, [])
        if len(order_fills) != 1:
            errors.append(f"order {oid}: expected exactly one full fill, got {len(order_fills)}")
            continue
        fill = order_fills[0]
        if int(fill["timestamp"]) != execution:
            errors.append(f"order {oid}: fill timestamp != T+1")
        if str(fill["coin"]) != coin:
            errors.append(f"order {oid}: fill coin mismatch")
        fill_qty = float(fill["quantity"])
        if not math.isclose(fill_qty, actual_qty, rel_tol=1e-12, abs_tol=1e-12):
            errors.append(
                f"order {oid}: fill quantity {fill_qty!r} != executable quantity {actual_qty!r} "
                f"(abs_error={abs(fill_qty - actual_qty)!r})"
            )
        if open_key not in prices:
            errors.append(f"order {oid}: missing CSV open(T+1) for {coin}/{execution}")
        elif not math.isclose(float(fill["price"]), prices[open_key][0], rel_tol=1e-12, abs_tol=1e-12):
            errors.append(f"order {oid}: fill price != canonical CSV open(T+1)")

        checks.append({
            "order_id": oid,
            "coin": coin,
            "decision_timestamp": decision,
            "execution_timestamp": execution,
            "notional_usd": notional,
            "reference_close": reference_close,
            "quantity": actual_qty,
            "fill_quantity": fill_qty,
            "fill_quantity_abs_error": abs(fill_qty - actual_qty),
            "fill_price": float(fill["price"]),
        })

    extra_fill_orders = sorted(set(fills_by_order) - set(planned))
    if extra_fill_orders:
        errors.append(f"fills exist for unknown planned orders: {extra_fill_orders}")

    return {
        "ok": not errors,
        "errors": errors,
        "planned_submit_orders": len(planned),
        "fills_checked": len(fills),
        "checks": checks,
    }


def save_evidence(root: pathlib.Path, report: Dict[str, object]) -> pathlib.Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = root / "deploy/historical_replay/run" / f"t17_execution_{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "summary.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (out_dir / "compose.log").write_text(
        compose(root, ["logs", "--no-color"], capture=True, check=False), encoding="utf-8"
    )
    (out_dir / "compose_ps.txt").write_text(
        compose(root, ["ps", "--all"], capture=True, check=False), encoding="utf-8"
    )
    time_env = root / "deploy/historical_replay/run/time.env"
    if time_env.exists():
        (out_dir / "time.env").write_text(time_env.read_text(encoding="utf-8"), encoding="utf-8")
    return out_dir


def print_status(cp: Dict[str, object], target: int) -> None:
    print(
        "STATUS "
        f"target={target} strategy={cp['strategy_latest']} risk={cp['risk_latest']} "
        f"execution_decision={cp['execution_last_decision']} execution_at={cp['execution_last_execution']} "
        f"planner={cp['planner_latest']} fills={cp['fills']} "
        f"positions={len(cp.get('positions', {}) or {})}",
        flush=True,
    )


def dry_run_plan(args) -> int:
    wall = expected_wall_seconds(args.simulated_start, args.target_completed, args.speed)
    print("T17 short execution replay plan")
    print(f"speed={args.speed:g}x")
    print(f"simulated_start={args.simulated_start}")
    print(f"target_completed={args.target_completed}")
    print(f"target_execution_day={next_day(args.target_completed)}")
    print(f"expected_wall_to_visibility_seconds={wall:.1f}")
    print(f"expected_wall_to_visibility_minutes={wall/60.0:.2f}")
    print("acceptance_boundary=Fill at canonical open(T+1), quantity from exact close(T)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run T17 short historical execution replay")
    parser.add_argument("--root", default=".")
    parser.add_argument("--speed", type=float, default=DEFAULT_SPEED)
    parser.add_argument("--simulated-start", default=DEFAULT_SIMULATED_START)
    parser.add_argument("--target-completed", type=int, default=DEFAULT_TARGET_COMPLETED)
    parser.add_argument("--poll-seconds", type=float, default=3.0)
    parser.add_argument("--status-seconds", type=float, default=30.0)
    parser.add_argument("--drain-seconds", type=float, default=120.0)
    parser.add_argument("--max-wall-seconds", type=float, default=900.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.speed <= 0:
        die("speed must be positive")
    if args.poll_seconds <= 0 or args.status_seconds <= 0 or args.drain_seconds <= 0:
        die("poll/status/drain seconds must be positive")
    if args.dry_run:
        return dry_run_plan(args)

    root = pathlib.Path(args.root).resolve()
    for required in [
        root / "deploy/historical_replay/docker-compose.yml",
        root / "deploy/historical_replay/.env",
        root / "deploy/historical_replay/build_runtime_bundle.sh",
        root / "tools/historical_replay/create_time_env.py",
        root / "tools/historical_replay/normalize_historical_csv.py",
    ]:
        if not required.exists():
            die(f"missing required file: {required}")

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
    normalized_meta = json.loads(normalize_out.splitlines()[-1])

    start_day = parse_utc(args.simulated_start).date().isoformat()
    target_day = parse_yyyymmdd(args.target_completed).isoformat()
    execution_day = parse_yyyymmdd(next_day(args.target_completed)).isoformat()
    if normalized_meta["min_date"] > start_day:
        die(f"dataset starts at {normalized_meta['min_date']}, after simulated start {start_day}")
    if normalized_meta["max_date"] < execution_day:
        die(f"dataset ends at {normalized_meta['max_date']}, before required T+1 open {execution_day}")

    os.environ["HISTORICAL_DATA_PATH"] = str(normalized_data)
    wall_expected = expected_wall_seconds(args.simulated_start, args.target_completed, args.speed)
    if wall_expected > args.max_wall_seconds:
        die(f"target needs about {wall_expected:.0f}s after feeder start, above max wall {args.max_wall_seconds:.0f}s")

    print("============================================================")
    print("T17 — SHORT HISTORICAL EXECUTION REPLAY")
    print("============================================================")
    print(f"dataset_range={normalized_meta['min_date']}..{normalized_meta['max_date']} rows={normalized_meta['rows']}")
    print(f"speed={args.speed:g}x")
    print(f"simulated_start={args.simulated_start}")
    print(f"target_completed={args.target_completed}")
    print(f"target_execution_day={next_day(args.target_completed)}")
    print(f"expected_wall_minutes≈{wall_expected/60.0:.2f}")
    print("boundary=close(T) sizing -> T+1 open fill -> persisted ExecutionState")

    run(["docker", "compose", "version"], cwd=root)
    run([str(root / "deploy/historical_replay/build_runtime_bundle.sh")], cwd=root)

    time_env = root / "deploy/historical_replay/run/time.env"
    time_env.parent.mkdir(parents=True, exist_ok=True)
    time_env.write_text("# placeholder used only while building the image\n", encoding="utf-8")

    compose(root, ["--profile", "runtime", "config"], capture=True)
    compose(root, ["--profile", "runtime", "build"])
    compose(root, ["--profile", "runtime", "down", "-v", "--remove-orphans"], check=False)
    compose(root, ["up", "-d", "nats", "postgres"])
    compose(root, ["run", "--rm", "market-data-volume-init"])

    run([
        sys.executable, str(root / "tools/historical_replay/create_time_env.py"),
        "--speed", format(args.speed, ".17g"),
        "--simulated-reference", args.simulated_start,
        "--output", str(time_env),
    ], cwd=root)

    compose(root, ["--profile", "runtime", "run", "--rm", "market-data-db-init"])
    compose(root, ["--profile", "runtime", "up", "-d", "--no-build", *RUNTIME_CONSUMERS])
    print("Waiting 8 real seconds for subscriptions/reconciliation bootstrap...", flush=True)
    time.sleep(8.0)
    failures = container_failures(root)
    if failures:
        print(failures, file=sys.stderr)
        die("runtime service failed before feeder start")

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
                die("runtime service failed before target")
            next_status = now + args.status_seconds
        time.sleep(args.poll_seconds)

    if not reached:
        report = {"result": "FAIL", "reason": "target_not_committed", **checkpoints(root)}
        evidence = save_evidence(root, report)
        die(f"target not committed; evidence={evidence}")

    print(f"Target close day {args.target_completed} committed; stopping feeder and draining execution.")
    compose(root, ["stop", "historical-market-data"])

    target_execution = next_day(args.target_completed)
    drain_started = time.monotonic()
    final_cp: Dict[str, object] = {}
    while time.monotonic() - drain_started <= args.drain_seconds:
        final_cp = checkpoints(root)
        print_status(final_cp, args.target_completed)
        if (
            int(final_cp.get("strategy_latest", 0)) >= args.target_completed
            and int(final_cp.get("risk_latest", 0)) >= args.target_completed
            and int(final_cp.get("planner_latest", 0)) >= args.target_completed
            and int(final_cp.get("execution_last_decision", 0)) >= args.target_completed
            and int(final_cp.get("execution_last_execution", 0)) >= target_execution
            and int(final_cp.get("sim_exchange_latest", 0)) >= target_execution
            and int(final_cp.get("fills", 0)) > 0
        ):
            break
        time.sleep(args.poll_seconds)

    all_logs = compose(root, ["logs", "--no-color"], capture=True, check=False)
    critical_tokens = [
        "event=fatal",
        "event=reconciliation_blocked",
        "event=notional_plan_invalid",
        "event=notional_plan_state_mismatch",
        "event=submit_conflict",
        "event=fill_failed",
    ]
    critical_lines = [line for line in all_logs.splitlines() if any(token in line for token in critical_tokens)]

    try:
        economics = economic_validation(root, normalized_data)
    except Exception as exc:
        economics = {"ok": False, "errors": [f"economic validation exception: {exc}"]}

    pipeline_ok = (
        int(final_cp.get("strategy_latest", 0)) >= args.target_completed
        and int(final_cp.get("risk_latest", 0)) >= args.target_completed
        and int(final_cp.get("planner_latest", 0)) >= args.target_completed
        and int(final_cp.get("execution_last_decision", 0)) >= args.target_completed
        and int(final_cp.get("execution_last_execution", 0)) >= target_execution
        and int(final_cp.get("sim_exchange_latest", 0)) >= target_execution
        and int(final_cp.get("fills", 0)) > 0
    )

    report: Dict[str, object] = {
        "result": "PASS" if pipeline_ok and bool(economics.get("ok")) and not critical_lines else "FAIL",
        "scope": "close_T_notional_to_T_plus_1_open_fill",
        "speed": args.speed,
        "simulated_start": args.simulated_start,
        "target_completed": args.target_completed,
        "target_execution_day": target_execution,
        "wall_seconds": round(time.monotonic() - started, 3),
        "expected_wall_seconds": round(wall_expected, 3),
        "critical_log_lines": critical_lines,
        "economic_validation": economics,
        **final_cp,
    }
    evidence = save_evidence(root, report)
    compose(root, ["stop", *RUNTIME_CONSUMERS], check=False)

    print("============================================================")
    print(json.dumps(report, indent=2, sort_keys=True))
    print(f"evidence={evidence}")
    if report["result"] != "PASS":
        print("FAIL: T17 short execution replay did not satisfy economic acceptance", file=sys.stderr)
        return 1

    print("PASS: T17 close(T) notional plans executed at canonical open(T+1) with persisted fills")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
