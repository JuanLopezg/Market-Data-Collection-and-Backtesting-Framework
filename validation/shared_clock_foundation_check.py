#!/usr/bin/env python3
"""STEP 35B — static/source gate for the REPLAY logical-clock foundation.

This gate intentionally checks only the shared contract, SimulatedClock primitive and
replay-controller clock authority/persistence. It does NOT claim that every distributed
service is consuming the clock yet; that is the next wiring/synchronization sub-step.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def read(root: Path, rel: str) -> str:
    path = root / rel
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()

    failures: list[str] = []

    clock_h = read(root, "lib/src/runtime/clock.h")
    state_h = read(root, "lib/src/contracts/clock_state.h")
    codec_h = read(root, "lib/src/transport/contract_json_codec.h")
    codec_cpp = read(root, "lib/src/transport/contract_json_codec.cpp")
    subjects = read(root, "lib/src/transport/transport_subjects.h")
    controller = read(root, "live_trading/replay_controller/src/replay_controller_main.cpp")
    compose = read(root, "deploy/distributed_replay/docker-compose.yml")

    require("class SimulatedClock final : public Clock" in clock_h,
            "SimulatedClock implementation missing", failures)
    require("revision < revision_" in clock_h,
            "SimulatedClock stale-revision guard missing", failures)
    require("logicalTime < logical_time_" in clock_h,
            "SimulatedClock backwards-time guard missing", failures)
    require("class SystemClock final : public Clock" in clock_h and
            "std::chrono::system_clock::now()" in clock_h,
            "LIVE/TESTNET SystemClock boundary must remain simple", failures)

    for token in (
        "struct ClockState", "simulation_id", "logical_time", "revision",
        "speed_multiplier", "paused", "SimulationClockMode"
    ):
        require(token in state_h, f"ClockState contract missing token: {token}", failures)

    require('CLOCK_STATE = "simulation.clock.state.v1"' in subjects,
            "canonical CLOCK_STATE subject missing", failures)
    require("runtimeSubjects()" in subjects and "CLOCK_STATE" in subjects,
            "CLOCK_STATE is not part of the REPLAY JetStream runtime subject set", failures)

    require("encode(const ClockState& value)" in codec_h and
            "decodeClockState" in codec_h,
            "ClockState codec declarations missing", failures)
    require("encode(const ClockState& value)" in codec_cpp and
            "ClockState decodeClockState" in codec_cpp,
            "ClockState codec implementation missing", failures)

    for token in (
        "replay_controller_clock_state",
        "loadClockState",
        "recordClockState",
        "clock_state_",
        "advanceClock",
        "publishClockState",
        "TransportSubjects::CLOCK_STATE",
        "SimulationClockMode::MaxSpeed",
    ):
        require(token in controller, f"Replay clock authority missing token: {token}", failures)

    # Strong source-order invariant: durable clock checkpoint must happen before the publish
    # inside advanceClock so crash-after-COMMIT/pre-publish can recover deterministically.
    start = controller.find("void advanceClock(Timestamp logicalTime)")
    end = controller.find("void recoverClockThrough(Timestamp logicalTime)", start + 1)
    if start < 0 or end < 0:
        failures.append("advanceClock body not found")
    else:
        body = controller[start:end]
        record_at = body.find("recordClockState(next)")
        assign_at = body.find("clock_state_ = next")
        publish_at = body.find("publishClockState(*clock_state_)", assign_at + 1)
        require(record_at >= 0 and assign_at > record_at and publish_at > assign_at,
                "clock authority must persist -> install state -> publish in that order", failures)

    # The logical clock advances before each replay release. Existing steady_clock barrier
    # deadlines must remain technical/process time rather than being converted to fake time.
    close_release = re.search(
        r"advanceClock\(decisionTimestamp\);\s*publishRelease\(\s*"
        r"MarketDataReleaseKind::ClosedSlice",
        controller,
        re.S,
    )
    open_release = re.search(
        r"advanceClock\(executionTimestamp\);\s*publishRelease\(\s*"
        r"MarketDataReleaseKind::ExecutionOpen",
        controller,
        re.S,
    )
    require(close_release is not None,
            "decision CLOSE release is not preceded by logical-clock advance", failures)
    require(open_release is not None,
            "execution OPEN release is not preceded by logical-clock advance", failures)
    require("std::chrono::steady_clock::now()" in controller,
            "technical replay barrier must continue using steady_clock", failures)

    require("--simulation-id" in controller,
            "replay-controller stable simulation identity option missing", failures)
    require("--simulation-id" in compose and "REPLAY_SIMULATION_ID" in compose,
            "distributed replay compose does not expose REPLAY_SIMULATION_ID", failures)

    print("============================================================")
    print("STEP 35B — SHARED LOGICAL CLOCK FOUNDATION")
    print("============================================================")
    print(f"root                       : {root}")
    print(f"SimulatedClock             : {'PASS' if 'class SimulatedClock' in clock_h else 'FAIL'}")
    print(f"ClockState contract/codec  : {'PASS' if not any('ClockState' in f for f in failures) else 'FAIL'}")
    print(f"JetStream clock subject    : {'PASS' if 'simulation.clock.state.v1' in subjects else 'FAIL'}")
    print(f"Replay authority           : {'PASS' if 'advanceClock' in controller else 'FAIL'}")
    print(f"Durable clock checkpoint   : {'PASS' if 'replay_controller_clock_state' in controller else 'FAIL'}")
    followers_present = "ServiceClockContext" in read(root, "live_trading/strategy_service/src/strategy_service_main.cpp")
    print(f"Service clock followers    : {'PRESENT (STEP 35C)' if followers_present else 'NOT YET'}")

    if failures:
        print("\n[FAIL] foundation findings:")
        for failure in failures:
            print(f"  - {failure}")
        print("\nSTEP 35B CLOCK FOUNDATION RESULT: FAIL")
        return 1

    print("\nSTEP 35B CLOCK FOUNDATION RESULT: PASS")
    print("Foundation/authority only. Do not declare the full shared-clock phase PASS yet.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
