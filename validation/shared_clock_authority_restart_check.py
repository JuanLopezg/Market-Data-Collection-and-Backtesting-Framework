#!/usr/bin/env python3
"""STEP 35E — source gate for replay-authority durable clock recovery.

The shared clock authority already persists ClockState in PostgreSQL before publishing it.
This gate proves that source ordering still preserves that contract and that the historical
replay harness can tolerate deliberate replay-controller replacement.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def read(root: Path, rel: str) -> str:
    path = root / rel
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def ordered(text: str, *tokens: str) -> bool:
    pos = -1
    for token in tokens:
        nxt = text.find(token, pos + 1)
        if nxt < 0:
            return False
        pos = nxt
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    failures: list[str] = []

    controller = read(root, "live_trading/replay_controller/src/replay_controller_main.cpp")
    harness = read(root, "tools/distributed_compare/run_historical_compare.sh")
    suite = read(root, "validation/shared_clock_authority_restart_suite.sh")
    clock_h = read(root, "lib/src/runtime/clock.h")

    require(controller, "replay-controller source missing", failures)
    require(harness, "historical distributed harness missing", failures)
    require(suite, "STEP 35E suite missing", failures)

    # Durable authority: COMMIT must happen before process-local install and publish.
    advance_start = controller.find("void advanceClock(Timestamp logicalTime)")
    advance_end = controller.find("void recoverClockThrough", advance_start)
    advance = controller[advance_start:advance_end] if advance_start >= 0 and advance_end > advance_start else ""
    require(
        ordered(
            advance,
            "checkpoint_store_->recordClockState(next)",
            "authority_clock_.synchronize",
            "clock_state_ = next",
            "publishClockState(*clock_state_)",
        ),
        "ClockState must be committed before local install and NATS publish",
        failures,
    )

    # Recovery: PostgreSQL clock is loaded, identity-checked and installed into SimulatedClock.
    constructor_start = controller.find("explicit ReplayControllerRuntime(Options options)")
    destructor_start = controller.find("~ReplayControllerRuntime()", constructor_start)
    constructor = controller[constructor_start:destructor_start] if constructor_start >= 0 and destructor_start > constructor_start else ""
    for token in (
        "decisions_ready_ = checkpoint_store_->loadDecisions()",
        "executions_complete_ = checkpoint_store_->loadExecutions()",
        "clock_state_ = checkpoint_store_->loadClockState()",
        "Recovered logical clock simulation_id does not match --simulation-id",
        "authority_clock_.synchronize",
        "event=replay_recovery_completed",
        "clock_recovered={}",
        "clock_time={}",
        "clock_revision={}",
    ):
        require(token in constructor, f"authority recovery prerequisite missing: {token}", failures)

    require(
        ordered(
            constructor,
            "clock_state_ = checkpoint_store_->loadClockState()",
            "authority_clock_.synchronize",
            "event=replay_recovery_completed",
        ),
        "recovered durable clock must be installed before recovery is announced",
        failures,
    )
    require(
        "if (clock_state_) {" in constructor and
        "publishClockState(*clock_state_)" in constructor and
        "scheduleClockRefresh()" in constructor,
        "recovered authority does not re-emit/schedule the durable ClockState",
        failures,
    )

    # Same logical time on restart must never synthesize a new revision.
    require(
        "logicalTime == clock_state_->logical_time" in advance and
        "publishClockState(*clock_state_)" in advance and
        "return;" in advance,
        "same logical time does not preserve the recovered revision",
        failures,
    )
    require("Replay logical clock attempted to move backwards" in controller,
            "authority rollback guard missing", failures)
    require("Replay clock cannot move durable state backwards" in controller,
            "PostgreSQL rollback guard missing", failures)
    require("Replay clock same revision conflicts with durable state" in controller,
            "same-revision durable conflict guard missing", failures)
    require("recoverClockThrough" in controller and "recovered_cycle_skipped" in controller,
            "completed-prefix recovery path missing", failures)
    require("event=recovered_decision_barrier" in controller,
            "in-flight decision/execution phase recovery marker missing", failures)

    # Harness must explicitly tolerate the controller id replacement we inject.
    require("--allow-controller-recreate" in harness and "allow_controller_recreate=1" in harness,
            "historical harness cannot allow deliberate controller replacement", failures)
    require("replay-controller replacement detected" in harness,
            "historical harness does not track replacement controller id", failures)

    # 35E itself must avoid the restart:on-failure race and prove PostgreSQL state while
    # the authority process is absent, before recreating exactly one final controller.
    # Recovery log matching must be field-based, not dependent on field adjacency. STEP 35F
    # inserted clock_mode/speed/paused between clock_revision and simulation_id.
    require(
        "recovery_log_matches()" in suite and
        '[[ "$line" == *"clock_recovered=true"* ]]' in suite and
        '[[ "$line" == *"clock_time=$logical_time"* ]]' in suite and
        '[[ "$line" == *"clock_revision=$revision"* ]]' in suite and
        '[[ "$line" == *"simulation_id=$sim"* ]]' in suite,
        "authority recovery log gate must match required fields independently of log field order",
        failures,
    )
    require(
        'grep -Fq "clock_recovered=true clock_time=$held_time clock_revision=$held_revision simulation_id=$held_sim"' not in suite,
        "stale adjacent-field recovery matcher remains in authority restart suite",
        failures,
    )

    for token, message in (
        ("docker update --restart=no", "authority SIGKILL is not isolated from Docker restart policy"),
        ("RestartCount", "authority suite does not prove no intermediate auto-restart"),
        ("clock state remained durable while authority was absent", "authority-offline durable-state proof missing"),
        ("exact durable clock recovered", "exact durable recovery assertion missing"),
        ("authority did not advance while causal blocker remained paused", "post-restart no-advance proof missing"),
        ("DISTRIBUTED_FAST_COMPARE: PASS", "economic exactness gate missing"),
        ("decision execution", "both decision and execution barrier cases are not represented"),
    ):
        require(token in suite, message, failures)

    require("class SimulatedClock final : public Clock" in clock_h,
            "SimulatedClock missing", failures)

    print("============================================================")
    print("STEP 35E — REPLAY AUTHORITY RESTART SOURCE GATE")
    print("============================================================")
    print(f"root                           : {root}")
    print(f"commit-before-publish ordering : {'PASS' if not failures or ordered(advance, 'checkpoint_store_->recordClockState(next)', 'authority_clock_.synchronize', 'clock_state_ = next', 'publishClockState(*clock_state_)') else 'CHECK'}")
    print(f"durable clock load/install     : {'PASS' if 'clock_state_ = checkpoint_store_->loadClockState()' in constructor and 'authority_clock_.synchronize' in constructor else 'FAIL'}")
    print(f"same-revision restart path     : {'PASS' if 'logicalTime == clock_state_->logical_time' in advance else 'FAIL'}")
    print(f"rollback/conflict guards       : {'PASS' if 'Replay clock cannot move durable state backwards' in controller and 'same revision conflicts' in controller else 'FAIL'}")
    print(f"controller recreate harness    : {'PASS' if '--allow-controller-recreate' in harness else 'FAIL'}")
    print(f"SIGKILL policy isolation       : {'PASS' if 'docker update --restart=no' in suite else 'FAIL'}")
    print(f"decision + execution phases    : {'PASS' if 'decision execution' in suite else 'FAIL'}")
    robust_recovery_match = (
        "recovery_log_matches()" in suite and
        '[[ "$line" == *"clock_revision=$revision"* ]]' in suite and
        'grep -Fq "clock_recovered=true clock_time=$held_time clock_revision=$held_revision simulation_id=$held_sim"' not in suite
    )
    print(f"recovery log field matching    : {'PASS' if robust_recovery_match else 'FAIL'}")

    if failures:
        print("\n[FAIL] 35E findings:")
        for failure in failures:
            print(f"  - {failure}")
        print("\nSTEP 35E SOURCE GATE RESULT: FAIL")
        return 1

    print("\nSTEP 35E SOURCE GATE RESULT: PASS")
    print("Run shared_clock_authority_restart_suite.sh for injected authority recovery proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
