#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise RuntimeError(f"missing required file: {rel}")
    return path.read_text(encoding="utf-8")


def require(condition: bool, label: str, failures: list[str]) -> None:
    if condition:
        print(f"{label:<33}: PASS")
    else:
        print(f"{label:<33}: FAIL")
        failures.append(label)


def main() -> int:
    failures: list[str] = []

    contract = read("lib/src/contracts/clock_control.h")
    codec_h = read("lib/src/transport/contract_json_codec.h")
    codec_cpp = read("lib/src/transport/contract_json_codec.cpp")
    subjects = read("lib/src/transport/transport_subjects.h")
    controller = read("live_trading/replay_controller/src/replay_controller_main.cpp")
    compose = read("deploy/distributed_replay/docker-compose.yml")
    service_clock = read("lib/src/runtime/service_clock.h")

    suite = read("validation/shared_clock_controls_suite.sh")

    print("============================================================")
    print("STEP 35F — REPLAY CLOCK CONTROLS / SPEED-PACING SOURCE GATE")
    print("============================================================")
    print(f"root{'':<28}: {ROOT}")

    require(
        all(token in contract for token in (
            "ClockControlAction", "Pause", "Resume", "SetSpeed",
            "expected_revision", "SimulationClockMode"
        )),
        "ClockControl contract", failures,
    )
    require(
        "encode(const ClockControl&" in codec_h and
        "decodeClockControl" in codec_h and
        "decodeClockControl" in codec_cpp,
        "ClockControl JSON codec", failures,
    )
    require(
        'CLOCK_CONTROL = "simulation.clock.control.v1"' in subjects and
        "CLOCK_CONTROL" in subjects[subjects.index("runtimeSubjects"):],
        "REPLAY control subject", failures,
    )

    trading_start = subjects.index("tradingRuntimeSubjects")
    runtime_start = subjects.index("runtimeSubjects", trading_start)
    trading_body = subjects[trading_start:runtime_start]
    require(
        "CLOCK_CONTROL" not in trading_body,
        "LIVE fake-control dependency", failures,
    )

    require(
        "replay-controller-clock-control" in controller and
        "TransportSubjects::CLOCK_CONTROL" in controller and
        "onClockControl" in controller,
        "authority control consumer", failures,
    )
    require(
        "ClockControlAction::Pause" in controller and
        "ClockControlAction::Resume" in controller and
        "ClockControlAction::SetSpeed" in controller and
        "clock_control_applied" in controller,
        "pause/resume/speed actions", failures,
    )
    require(
        "expected_revision" in controller and
        "reason=revision_conflict" in controller and
        "clock_control_duplicate_acked" in controller,
        "stale/idempotent command guards", failures,
    )

    control_pos = controller.index("DurableMessageDisposition onClockControl")
    advance_pos = controller.index("void advanceClock", control_pos)
    control_body = controller[control_pos:advance_pos]
    commit_pos = control_body.find("recordClockState(next)")
    publish_pos = control_body.find("publishClockState(*clock_state_)")
    require(
        commit_pos >= 0 and publish_pos >= 0 and commit_pos < publish_pos,
        "control COMMIT-before-publish", failures,
    )

    require(
        "waitForClockPermissionAndPacing" in controller and
        "SimulationClockMode::MaxSpeed" in controller and
        "options_.clock_wall_scale" in controller and
        "remainingSimulatedSeconds" in controller,
        "logical pacing implementation", failures,
    )
    require(
        "Operator pause is a logical-clock state" in controller and
        "activeWait" in controller and
        "clock_state_->paused" in controller,
        "pause freezes barrier deadline", failures,
    )
    require(
        "next.mode = clock_state_ ? clock_state_->mode" in controller and
        "next.speed_multiplier = clock_state_" in controller,
        "speed persists across ticks", failures,
    )
    require(
        "state_->paused" in service_clock and "authorize(" in service_clock,
        "followers honor paused state", failures,
    )
    require(
        "--clock-speed" in controller and "--clock-wall-scale" in controller and
        "REPLAY_CLOCK_SPEED" in compose and "REPLAY_CLOCK_WALL_SCALE" in compose,
        "compose/CLI speed bootstrap", failures,
    )
    require(
        compose.count("--clock-speed") == 1 and compose.count("--clock-wall-scale") == 1,
        "controls scoped to controller", failures,
    )
    require(
        "CASE WHEN paused THEN '1' ELSE '0' END" in suite and
        '"$paused" == "0"' in suite and
        '"$paused" == "1"' in suite,
        "deterministic SQL bool harness", failures,
    )

    print()
    if failures:
        print("STEP 35F SOURCE GATE RESULT: FAIL")
        for item in failures:
            print(f"[FAIL] {item}")
        return 1

    print("STEP 35F SOURCE GATE RESULT: PASS")
    print("Run shared_clock_controls_suite.sh for runtime economic-invariance proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
