#!/usr/bin/env python3
from __future__ import annotations
import argparse, datetime as dt, json, pathlib, re, shutil, subprocess, sys


def die(msg: str) -> None:
    raise SystemExit(f"ERROR: {msg}")


def ymd(v: int) -> dt.date:
    try:
        return dt.datetime.strptime(str(v), "%Y%m%d").date()
    except ValueError as e:
        die(f"invalid YYYYMMDD value {v}: {e}")


def compose_strategy_logs(root: pathlib.Path) -> str:
    cmd = [
        "docker", "compose",
        "--env-file", str(root / "deploy/historical_replay/.env"),
        "-f", str(root / "deploy/historical_replay/docker-compose.yml"),
        "logs", "--no-color", "strategy",
    ]
    p = subprocess.run(cmd, cwd=root, text=True, capture_output=True, check=False)
    if p.returncode != 0:
        die("cannot read retained Strategy container logs; do not destroy the T21 containers before regrading\n" + p.stderr.strip())
    return p.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description="Regrade already-completed T21 using Strategy-specific retained logs.")
    ap.add_argument("--root", default=".")
    ap.add_argument("--label", default="t21_gap_catchup")
    a = ap.parse_args()
    root = pathlib.Path(a.root).resolve()
    evidence = root / "deploy/historical_replay/run"
    summary_path = evidence / f"{a.label}_summary.json"
    if not summary_path.is_file():
        die(f"missing existing T21 summary: {summary_path}")
    summary = json.loads(summary_path.read_text())

    if summary.get("label") != a.label or summary.get("fault_profile") != "strategy-gap":
        die("existing summary is not the expected strategy-gap T21 scenario")
    fs = summary.get("fault_state") or {}
    if fs.get("stopped") is not True or fs.get("resumed") is not True:
        die(f"T21 fault lifecycle incomplete: {fs}")

    target = int(summary.get("target_completed") or 0)
    exec_day = int(summary.get("target_execution_day") or 0)
    pipeline_checks = {
        "strategy_latest": int(summary.get("strategy_latest") or 0) >= target,
        "risk_latest": int(summary.get("risk_latest") or 0) >= target,
        "planner_latest": int(summary.get("planner_latest") or 0) >= target,
        "execution_last_decision": int(summary.get("execution_last_decision") or 0) >= target,
        "execution_last_execution": int(summary.get("execution_last_execution") or 0) >= exec_day,
        "sim_exchange_latest": int(summary.get("sim_exchange_latest") or 0) >= exec_day,
        "fills": int(summary.get("fills") or 0) > 0,
    }
    failed = [k for k,v in pipeline_checks.items() if not v]
    if failed:
        die("T21 final pipeline evidence is incomplete: " + ", ".join(failed))
    if summary.get("critical_log_lines"):
        die("T21 contains critical log lines; refusing to regrade")
    if summary.get("fingerprint_match") is not True:
        die("T21 fingerprint_match is not true")

    logs = compose_strategy_logs(root)
    recoveries = [int(x) for x in re.findall(r"event=strategy_recovery_completed\s+checkpoint=latest-only\s+latest_timestamp=(\d{8})", logs)]
    if not recoveries:
        die("missing restarted Strategy recovery checkpoint in retained logs")
    recovered = recoveries[-1]

    catchup = [int(x) for x in re.findall(r"event=signal_state_catchup_processed\s+timestamp=(\d{8})\s+published=false", logs)]
    publishes = [(int(ts), int(days)) for ts,days in re.findall(r"event=strategy_intents_published\s+timestamp=(\d{8}).*?catchup_days=(\d+)", logs)]
    publishes = [x for x in publishes if x[0] > recovered]
    if not publishes:
        die("missing post-restart Strategy intent publication")
    published, catchup_days = publishes[-1]
    if published < target:
        die(f"post-restart Strategy publication {published} did not reach target {target}")

    start_d, end_d = ymd(recovered), ymd(published)
    expected_internal = []
    cur = start_d + dt.timedelta(days=1)
    while cur < end_d:
        expected_internal.append(int(cur.strftime("%Y%m%d")))
        cur += dt.timedelta(days=1)
    observed_internal = [x for x in catchup if recovered < x < published]
    # Preserve order but remove duplicate log copies, if any.
    observed_internal = list(dict.fromkeys(observed_internal))
    if observed_internal != expected_internal:
        die(f"catch-up sequence mismatch: expected {expected_internal}, got {observed_internal}")
    expected_days = (end_d - start_d).days
    if catchup_days != expected_days:
        die(f"catchup_days mismatch: expected {expected_days}, got {catchup_days}")

    backup = summary_path.with_suffix(".pre_t21_regrade.json")
    if not backup.exists():
        shutil.copy2(summary_path, backup)
    summary["gap_catchup_observed"] = True
    summary["result"] = "PASS"
    summary["t21_regraded_from_existing_evidence"] = True
    summary["t21_gap_evidence"] = {
        "source": "strategy-specific retained docker logs",
        "recovered_checkpoint": recovered,
        "internal_catchup_days": observed_internal,
        "published_frontier": published,
        "catchup_days": catchup_days,
        "pipeline_checks": pipeline_checks,
    }
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print("PASS: existing T21 regraded without replay")
    print(f"  recovery={recovered} internal_catchup={observed_internal} published={published} catchup_days={catchup_days}")
    print(f"  strategy={summary['strategy_latest']} risk={summary['risk_latest']} planner={summary['planner_latest']} execution={summary['execution_last_decision']}->{summary['execution_last_execution']}")
    print(f"  evidence={summary_path}")
    print(f"  backup={backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
