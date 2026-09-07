#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

CORE_SERVICES = {
    "market-data": "live_trading/market_data_service/src/market_data_service_main.cpp",
    "strategy": "live_trading/strategy_service/src/strategy_service_main.cpp",
    "portfolio-risk": "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp",
    "order-planner": "live_trading/order_planner_service/src/order_planner_service_main.cpp",
    "execution-state": "live_trading/execution_state_service/src/execution_state_service_main.cpp",
    "exchange-gateway": "live_trading/exchange_gateway/src/exchange_gateway_main.cpp",
    "simulated-exchange": "live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp",
}


def read(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise RuntimeError(f"missing required file: {rel}")
    return path.read_text(encoding="utf-8")


def require(condition: bool, label: str, failures: list[str]) -> None:
    print(f"{label:<38}: {'PASS' if condition else 'FAIL'}")
    if not condition:
        failures.append(label)


def body_between(text: str, start: str, end: str) -> str:
    a = text.find(start)
    b = text.find(end, a + len(start)) if a >= 0 else -1
    if a < 0 or b < 0:
        return ""
    return text[a:b]


def main() -> int:
    failures: list[str] = []
    subjects = read("lib/src/transport/transport_subjects.h")
    service_clock = read("lib/src/runtime/service_clock.h")
    controller = read("live_trading/replay_controller/src/replay_controller_main.cpp")
    override = read("validation/live_clock_isolation.override.yml")
    isolation_suite = read("validation/live_clock_isolation_suite.sh")
    suite = read("validation/shared_clock_acceptance_suite.sh")
    pre_dashboard = read("validation/pre_dashboard_suite.sh")
    replay_restart = read("validation/restart_replay_controller.sh")

    print("============================================================")
    print("STEP 35G — FINAL SHARED-CLOCK ACCEPTANCE SOURCE GATE")
    print("============================================================")
    print(f"root{'':<33}: {ROOT}")

    trading = body_between(subjects, "tradingRuntimeSubjects()", "runtimeSubjects()")
    runtime = body_between(subjects, "runtimeSubjects()", "exchangeGatewayControlSubjects()")
    fake_tokens = ("CLOCK_STATE", "CLOCK_SYNC_REQUEST", "CLOCK_CONTROL")
    require(
        bool(trading) and all(token not in trading for token in fake_tokens),
        "LIVE subjects exclude fake clock", failures,
    )
    require(
        bool(runtime) and all(token in runtime for token in fake_tokens),
        "REPLAY subjects include fake clock", failures,
    )

    ctor = body_between(service_clock, "ServiceClockContext(Options options", "~ServiceClockContext()")
    early_return = ctor.find("options_.runtime_mode != RuntimeMode::Replay")
    sim_create = ctor.find("std::make_unique<SimulatedClock>()")
    sync_request = ctor.find("requestSynchronizationBestEffort(\"startup\")")
    subscribe = ctor.find("bus_.subscribe(")
    require(
        min(early_return, sim_create, sync_request, subscribe) >= 0
        and early_return < sim_create < sync_request < subscribe,
        "LIVE/Testnet exit before fake bootstrap", failures,
    )
    require(
        "SystemClock system_clock_" in service_clock
        and "if (simulated_clock_)" in service_clock
        and "return system_clock_" in service_clock,
        "SystemClock remains production clock", failures,
    )
    require(
        "if (!replay())\n            return true;" in service_clock,
        "LIVE business gate ignores fake time", failures,
    )

    service_ok = True
    direct_fake_refs = []
    for name, rel in CORE_SERVICES.items():
        text = read(rel)
        ok = (
            "RuntimeMode runtime_mode = RuntimeMode::Live;" in text
            and "RuntimeMode::Replay" in text
            and "TransportSubjects::runtimeSubjects()" in text
            and "TransportSubjects::tradingRuntimeSubjects()" in text
            and "ServiceClockContext" in text
        )
        service_ok &= ok
        if any(token in text for token in ("TransportSubjects::CLOCK_STATE", "TransportSubjects::CLOCK_CONTROL", "TransportSubjects::CLOCK_SYNC_REQUEST", "simulation.clock.")):
            direct_fake_refs.append(name)
    require(service_ok, "7 services use mode-sensitive wiring", failures)

    readiness_flush_ok = True
    for name, rel in CORE_SERVICES.items():
        text = read(rel)
        marker = text.find("event=service_ready")
        flush = text.find("std::cout.flush();", marker) if marker >= 0 else -1
        # Keep the flush local to the readiness marker rather than accepting an unrelated
        # process flush elsewhere in a large service source file.
        readiness_flush_ok &= marker >= 0 and flush >= 0 and (flush - marker) < 1200
    require(readiness_flush_ok, "7 service_ready markers flush", failures)

    require(not direct_fake_refs, "No business service owns clock subjects", failures)

    control_refs = []
    for path in (ROOT / "live_trading").rglob("*.cpp"):
        text = path.read_text(encoding="utf-8")
        if "TransportSubjects::CLOCK_CONTROL" in text:
            control_refs.append(path.relative_to(ROOT).as_posix())
    require(
        control_refs == ["live_trading/replay_controller/src/replay_controller_main.cpp"],
        "ClockControl consumer is authority-only", failures,
    )

    require(
        "--simulation-id" not in override
        and "${CLOCK_ISOLATION_RUNTIME_MODE:-live}" in override
        and override.count("--runtime-mode") == 7,
        "LIVE/Testnet probe has no simulation id", failures,
    )

    require(
        "CLOCK_ISOLATION_HISTORICAL_DATA" in isolation_suite
        and "isolation_fixture.csv" in isolation_suite
        and "date,symbol,open,high,low,close,volume" in isolation_suite
        and 'HISTORICAL_DATA_PATH="$ISOLATION_DATA"' in isolation_suite,
        "LIVE probe uses tiny OHLCV fixture", failures,
    )
    require(
        "CLOCK_ISOLATION_READY_TIMEOUT_SECONDS" in isolation_suite
        and "readiness timeout=" in isolation_suite
        and "event=service_ready" in isolation_suite,
        "LIVE readiness gate stays bounded", failures,
    )

    required_suite_tokens = (
        "clock_audit.sh",
        "shared_clock_foundation_check.sh",
        "shared_clock_followers_suite.sh",
        "shared_clock_restart_resync_suite.sh",
        "shared_clock_authority_restart_suite.sh",
        "shared_clock_controls_suite.sh",
        "live_clock_isolation_suite.sh",
        "pre_dashboard_suite.sh",
        "run_validation.sh",
        "SHARED CLOCK ACCEPTANCE RESULT: PASS",
    )
    require(
        all(token in suite for token in required_suite_tokens),
        "Closure suite aggregates all proof gates", failures,
    )
    require(
        "Next           : STEP 35 Trading Control Dashboard" not in pre_dashboard,
        "Stale pre-dashboard next-step removed", failures,
    )
    require(
        "db_decisions == db_executions + 1" in replay_restart
        and "stable_midphase >= 8" in replay_restart
        and "execution_open_logs" in replay_restart,
        "33B durable stable phase gate", failures,
    )

    print()
    if failures:
        print("STEP 35G SOURCE GATE RESULT: FAIL")
        for failure in failures:
            print(f"[FAIL] {failure}")
        return 1

    print("STEP 35G SOURCE GATE RESULT: PASS")
    print("Run shared_clock_acceptance_suite.sh full for the closure proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
