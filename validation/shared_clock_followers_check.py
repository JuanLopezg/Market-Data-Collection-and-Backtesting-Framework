#!/usr/bin/env python3
"""STEP 35C — source gate for REPLAY clock followers and causal event guards.

This gate verifies wiring and architecture. The companion shell suite performs the actual
incremental build plus a short distributed==fast replay and checks that every follower
reports clock synchronization.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FOLLOWERS = {
    "market-data": "live_trading/market_data_service/src/market_data_service_main.cpp",
    "strategy": "live_trading/strategy_service/src/strategy_service_main.cpp",
    "portfolio-risk": "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp",
    "order-planner": "live_trading/order_planner_service/src/order_planner_service_main.cpp",
    "execution-state": "live_trading/execution_state_service/src/execution_state_service_main.cpp",
    "exchange-gateway": "live_trading/exchange_gateway/src/exchange_gateway_main.cpp",
    "simulated-exchange": "live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp",
}

EXPECTED_GUARDS = {
    "market-data": 1,
    "strategy": 1,
    "portfolio-risk": 3,
    "order-planner": 1,
    "execution-state": 6,
    "exchange-gateway": 3,
    "simulated-exchange": 4,
}


def read(root: Path, rel: str) -> str:
    path = root / rel
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def require(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    failures: list[str] = []

    service_clock = read(root, "lib/src/runtime/service_clock.h")
    runtime_mode = read(root, "lib/src/runtime/runtime_mode.h")
    sync_contract = read(root, "lib/src/contracts/clock_sync_request.h")
    subjects = read(root, "lib/src/transport/transport_subjects.h")
    codec_h = read(root, "lib/src/transport/contract_json_codec.h")
    codec_cpp = read(root, "lib/src/transport/contract_json_codec.cpp")
    controller = read(root, "live_trading/replay_controller/src/replay_controller_main.cpp")
    adapter_h = read(root, "lib/src/exchange/nats_backend_exchange_gateway_adapter.h")
    adapter_cpp = read(root, "lib/src/exchange/nats_backend_exchange_gateway_adapter.cpp")
    compose = read(root, "deploy/distributed_replay/docker-compose.yml")
    clock_h = read(root, "lib/src/runtime/clock.h")

    require("enum class RuntimeMode" in runtime_mode and "Replay" in runtime_mode,
            "RuntimeMode bootstrap contract missing", failures)
    require("class ServiceClockContext final" in service_clock,
            "ServiceClockContext missing", failures)
    require("SystemClock system_clock_" in service_clock and
            "std::unique_ptr<SimulatedClock> simulated_clock_" in service_clock,
            "ServiceClockContext must own SystemClock plus REPLAY SimulatedClock", failures)

    # Strong LIVE safety boundary: constructor must return before entering ANY REPLAY
    # bootstrap path.  STEP 35D deliberately evolved the REPLAY order to request the
    # authoritative snapshot *before* binding the durable ClockState consumer, then block
    # until a fresh authority confirmation is installed.  The old 35C gate encoded the
    # pre-35D subscribe->request ordering and therefore became a stale false-negative.
    live_guard = re.search(
        r"if\s*\(options_\.runtime_mode\s*!=\s*RuntimeMode::Replay\)\s*return\s*;",
        service_clock,
    )
    live_return = live_guard.start() if live_guard else -1
    replay_create = service_clock.find("simulated_clock_ = std::make_unique<SimulatedClock>()")
    replay_request = service_clock.find('requestSynchronizationBestEffort("startup")')
    replay_subscribe = service_clock.find("clock_subscription_ = bus_.subscribe")
    replay_bootstrap = service_clock.find("synchronizeReplayBootstrap();")
    require(
        live_return >= 0
        and replay_create > live_return
        and replay_request > replay_create
        and replay_subscribe > replay_request
        and replay_bootstrap > replay_subscribe,
        "LIVE/TESTNET must return before REPLAY fake-clock bootstrap; "
        "REPLAY startup must request authority before durable bind and then block for sync",
        failures,
    )

    require("authorize(Timestamp businessTime)" in service_clock and
            "state_->logical_time >= businessTime" in service_clock,
            "clock-before-event authorization gate missing", failures)
    require("gateMessage" in service_clock and "metadata" in service_clock and
            "produced_at" in service_clock,
            "business payload produced_at causal gate missing", failures)
    require("clock_synchronized" in service_clock,
            "first follower synchronization is not auditable in logs", failures)

    require("struct ClockSyncRequest" in sync_contract and "requester_id" in sync_contract and
            "known_revision" in sync_contract,
            "ClockSyncRequest contract incomplete", failures)
    require('CLOCK_SYNC_REQUEST = "simulation.clock.sync.request.v1"' in subjects,
            "CLOCK_SYNC_REQUEST subject missing", failures)
    require("encode(const ClockSyncRequest& value)" in codec_h and
            "decodeClockSyncRequest" in codec_h and
            "encode(const ClockSyncRequest& value)" in codec_cpp and
            "ClockSyncRequest decodeClockSyncRequest" in codec_cpp,
            "ClockSyncRequest codec missing", failures)

    for token in (
        "clock_sync_subscription_",
        "onClockSyncRequest",
        "TransportSubjects::CLOCK_SYNC_REQUEST",
        "replay-clock-sync:",
        "authority_clock_",
        "authority_clock_.synchronize",
    ):
        require(token in controller, f"Replay authority resync support missing: {token}", failures)

    # The authority must service restart sync requests while waiting on normal barriers.
    barrier_start = controller.find("void waitBarrier")
    barrier_end = controller.find("public:", barrier_start)
    barrier_body = controller[barrier_start:barrier_end] if barrier_start >= 0 and barrier_end > barrier_start else ""
    require("bus_.poll(clock_sync_subscription_" in barrier_body,
            "Replay controller does not poll clock sync requests during barriers", failures)

    guard_total = 0
    for service, rel in FOLLOWERS.items():
        text = read(root, rel)
        require(text, f"Follower source missing: {rel}", failures)
        require("RuntimeMode runtime_mode = RuntimeMode::Live" in text,
                f"{service}: safe LIVE default missing", failures)
        require("--runtime-mode" in text and "--simulation-id" in text,
                f"{service}: bootstrap clock options missing", failures)
        require("std::unique_ptr<ServiceClockContext> clock_" in text,
                f"{service}: ServiceClockContext wiring missing", failures)
        require("TransportSubjects::tradingRuntimeSubjects()" in text and
                "options_.runtime_mode == RuntimeMode::Replay" in text,
                f"{service}: LIVE/TESTNET stream bootstrap still includes fake-clock subjects",
                failures)
        require("clock_->poll(" in text,
                f"{service}: clock subscription is not polled before business loop", failures)
        guards = text.count("clock_->guard(")
        guard_total += guards
        require(guards >= EXPECTED_GUARDS[service],
                f"{service}: expected at least {EXPECTED_GUARDS[service]} causal guards, found {guards}",
                failures)

    require("setEventTimeGate" in adapter_h and "event_time_gate_" in adapter_h and
            adapter_cpp.count("event_time_gate_") >= 3,
            "Private simulated-exchange -> gateway event path is not causally clock-gated", failures)
    gateway = read(root, FOLLOWERS["exchange-gateway"])
    # STEP 35D moved the adapter behind unique_ptr so clock bootstrap can complete before
    # backend transport construction.  Accept both the original object form and the
    # current pointer form; the semantic requirement is the same gate callback.
    gateway_gate_wired = (
        ("adapter_.setEventTimeGate" in gateway or "adapter_->setEventTimeGate" in gateway)
        and "clock_->authorize" in gateway
    )
    require(gateway_gate_wired,
            "Exchange gateway did not connect adapter backend events to the shared clock", failures)

    # Compose must explicitly opt the seven followers into REPLAY. The same binaries default LIVE.
    replay_mode_count = len(re.findall(r"- --runtime-mode\s*\n\s*- replay", compose))
    require(replay_mode_count == len(FOLLOWERS),
            f"Compose must mark exactly {len(FOLLOWERS)} followers as REPLAY; found {replay_mode_count}", failures)
    require(compose.count("- --simulation-id") >= len(FOLLOWERS) + 1,
            "Compose does not propagate optional simulation identity to followers + authority", failures)

    require("class SystemClock final : public Clock" in clock_h and
            "return std::chrono::system_clock::now();" in clock_h,
            "LIVE/TESTNET SystemClock boundary was changed unexpectedly", failures)

    print("============================================================")
    print("STEP 35C — REPLAY CLOCK FOLLOWERS / CAUSAL SYNC SOURCE GATE")
    print("============================================================")
    print(f"root                         : {root}")
    print(f"clock followers wired        : {len(FOLLOWERS)}/7")
    print(f"business subscription guards : {guard_total}")
    print(f"runtime mode compose entries : {replay_mode_count}/7")
    print(f"restart resync request path   : {'PASS' if 'onClockSyncRequest' in controller else 'FAIL'}")
    print(f"gateway backend event gate    : {'PASS' if 'setEventTimeGate' in adapter_h else 'FAIL'}")
    print("LIVE fake-clock dependency    : NONE by bootstrap branch")

    if failures:
        print("\n[FAIL] 35C findings:")
        for failure in failures:
            print(f"  - {failure}")
        print("\nSTEP 35C SOURCE GATE RESULT: FAIL")
        return 1

    print("\nSTEP 35C SOURCE GATE RESULT: PASS")
    print("Source wiring only. Run shared_clock_followers_suite.sh for build + distributed proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
