#!/usr/bin/env python3
"""STEP 35D — source gate for hard-restart clock re-synchronization proof.

The runtime path was introduced in STEP 35C. This gate verifies the restart-hardened
control plane: non-blocking startup sync identity, control-plane-first service bootstrap,
explicit sync response plus same-revision authority refresh fallback, durable authority
state, barrier-time sync polling, and causal guards.
"""
from __future__ import annotations

import argparse
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


def read(root: Path, rel: str) -> str:
    path = root / rel
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


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
    controller = read(root, "live_trading/replay_controller/src/replay_controller_main.cpp")
    clock_h = read(root, "lib/src/runtime/clock.h")
    subjects = read(root, "lib/src/transport/transport_subjects.h")
    suite = read(root, "validation/shared_clock_restart_resync_suite.sh")

    require("class ServiceClockContext final" in service_clock,
            "ServiceClockContext missing", failures)
    require('requestSynchronizationBestEffort("startup")' in service_clock and
            "event=clock_sync_requested" in service_clock,
            "followers do not request an explicit startup re-sync", failures)
    startup_request_pos = service_clock.find('requestSynchronizationBestEffort("startup")')
    clock_bind_pos = service_clock.find('clock_subscription_ = bus_.subscribe(')
    require(startup_request_pos >= 0 and clock_bind_pos >= 0 and startup_request_pos < clock_bind_pos,
            "hard-restart sync request must be published before rebinding the durable clock consumer", failures)
    require("unsynchronized_retry" in service_clock and
            "SYNC_REQUEST_RETRY_INTERVAL" in service_clock and
            "std::chrono::steady_clock::now()" in service_clock,
            "unsynchronized followers do not retry clock sync using technical monotonic time", failures)
    require("requestIdentitySuffix" in service_clock and "#include <random>" not in service_clock and "randomSuffix" not in service_clock,
            "restart-critical sync request identity must be non-blocking (no random_device)", failures)
    require("event=clock_sync_request_attempt" in service_clock,
            "sync request has no pre-publish audit marker", failures)
    require("ack_wait_ms = 5000" in service_clock,
            "clock durable AckWait was not shortened for fast dead-client redelivery", failures)
    require("event=clock_synchronized" in service_clock,
            "follower synchronization has no auditable runtime marker", failures)
    require("synchronizeReplayBootstrap" in service_clock and
            "event=clock_bootstrap_ready" in service_clock and
            "authoritative_confirmation_observed_" in service_clock,
            "REPLAY follower does not synchronously install a fresh authority snapshot before business bootstrap", failures)
    require("sync_request_ids_" in service_clock and
            "replay-controller-refresh" in service_clock,
            "bootstrap does not distinguish fresh authority confirmation from stale durable redelivery", failures)
    require("std::cout.flush()" in service_clock,
            "restart-critical clock audit markers are not explicitly flushed to Docker stdout", failures)
    require("state_->logical_time >= businessTime" in service_clock and
            "return DurableMessageDisposition::Retry;" in service_clock,
            "business events are not held behind the logical-time gate", failures)
    require("value.revision < state_->revision" in service_clock and
            "same_revision_conflict" in service_clock,
            "follower monotonic/stale-revision protections are incomplete", failures)

    for token in (
        "replay_controller_clock_state",
        "loadClockState()",
        "recordClockState",
        "onClockSyncRequest",
        "clock_sync_subscription_",
        "bus_.poll(clock_sync_subscription_",
        "replay-clock-sync:",
        "event=clock_sync_response",
        "authority_clock_.synchronize",
        "maybeRefreshClockState",
        "event=clock_state_refresh",
        "replay-clock-refresh:",
    ):
        require(token in controller, f"replay authority restart/resync prerequisite missing: {token}", failures)

    # A sync response needs a fresh transport identity even when the semantic clock revision
    # is unchanged, otherwise JetStream duplicate suppression could hide the recovery tick.
    require(
        '"replay-clock-sync:" + options_.simulation_id + ":" +' in controller and
        "request.metadata.message_id" in controller,
        "clock sync response does not derive a fresh message id from the request",
        failures,
    )

    require('CLOCK_SYNC_REQUEST = "simulation.clock.sync.request.v1"' in subjects,
            "clock sync request subject missing", failures)
    require('CLOCK_STATE = "simulation.clock.state.v1"' in subjects,
            "clock state subject missing", failures)

    # The injected hard-kill must not race Compose restart:on-failure. Otherwise an
    # automatic intermediate process can consume/re-request ClockState before the final
    # replacement is created, yielding a false controller-side success.
    require("docker update --restart=no" in suite and "RestartPolicy.Name" in suite,
            "35D harness does not isolate SIGKILL from Docker restart:on-failure", failures)
    require("RestartCount" in suite and "no automatic intermediate restart" in suite,
            "35D harness does not prove the victim stayed stopped before removal", failures)
    require("request_message_id" in suite and "correlation_id=$request_message_id" in suite,
            "35D harness does not correlate authority response to the final replacement request", failures)
    require("final replacement restored restart policy: on-failure" in suite,
            "35D harness does not verify Compose restart policy is restored on replacement", failures)
    require("event=clock_bootstrap_ready" in suite,
            "35D harness does not require the final process to finish authoritative clock bootstrap", failures)

    wired = 0
    guarded = 0
    for service, rel in FOLLOWERS.items():
        text = read(root, rel)
        require(text, f"missing follower source: {rel}", failures)
        if "std::unique_ptr<ServiceClockContext> clock_" in text:
            wired += 1
        else:
            failures.append(f"{service}: ServiceClockContext not wired")
        if "clock_->guard(" in text or "clock_->authorize(" in text:
            guarded += 1
        else:
            failures.append(f"{service}: no logical-clock business gate found")
        require("clock_->poll(" in text,
                f"{service}: clock follower subscription is not polled", failures)

    # Restart-critical clock bootstrap must precede service-specific heavy recovery.
    bootstrap_order = {
        "market-data": (
            "clock_ = std::make_unique<ServiceClockContext>",
            "source_ = std::make_unique<HistoricalReleaseSource>",
        ),
        "strategy": (
            "clock_ = std::make_unique<ServiceClockContext>",
            "checkpoint_store_ = std::make_unique<StrategyCheckpointStore>",
        ),
        "portfolio-risk": (
            "clock_ = std::make_unique<ServiceClockContext>",
            "checkpoint_store_ = std::make_unique<PortfolioRiskCheckpointStore>",
        ),
        "execution-state": (
            "clock_ = std::make_unique<ServiceClockContext>",
            "store_ = std::make_unique<PostgresStateStore>",
        ),
        "exchange-gateway": (
            "clock_ = std::make_unique<ServiceClockContext>",
            "adapter_ = std::make_unique<NatsBackendExchangeGatewayAdapter>",
        ),
    }
    bootstrap_first = 0
    for service, (clock_token, heavy_token) in bootstrap_order.items():
        text = read(root, FOLLOWERS[service])
        clock_pos = text.find(clock_token)
        heavy_pos = text.find(heavy_token)
        if clock_pos >= 0 and heavy_pos >= 0 and clock_pos < heavy_pos:
            bootstrap_first += 1
        else:
            failures.append(f"{service}: clock control plane does not precede heavy recovery/bootstrap")

    require("class SimulatedClock final : public Clock" in clock_h and
            "revision cannot move backwards" in clock_h and
            "logical time cannot move backwards" in clock_h,
            "SimulatedClock monotonic protections missing", failures)

    print("============================================================")
    print("STEP 35D — HARD-RESTART CLOCK RESYNC SOURCE GATE")
    print("============================================================")
    print(f"root                         : {root}")
    print(f"followers restart-capable    : {wired}/7")
    print(f"followers causally gated     : {guarded}/7")
    startup_retry_ok = 'requestSynchronizationBestEffort("startup")' in service_clock and 'unsynchronized_retry' in service_clock
    print(f"startup + retry sync request : {'PASS' if startup_retry_ok else 'FAIL'}")
    prebind_ok = startup_request_pos >= 0 and clock_bind_pos >= 0 and startup_request_pos < clock_bind_pos
    print(f"startup request before bind  : {'PASS' if prebind_ok else 'FAIL'}")
    print(f"barrier-time authority reply : {'PASS' if 'bus_.poll(clock_sync_subscription_' in controller else 'FAIL'}")
    print(f"durable authority checkpoint : {'PASS' if 'replay_controller_clock_state' in controller else 'FAIL'}")
    print(f"fresh sync response identity : {'PASS' if 'replay-clock-sync:' in controller else 'FAIL'}")
    print(f"non-blocking sync identity    : {'PASS' if 'requestIdentitySuffix' in service_clock and '#include <random>' not in service_clock and 'randomSuffix' not in service_clock else 'FAIL'}")
    print(f"control-plane-first bootstrap : {bootstrap_first}/5 hardened services")
    print(f"same-revision refresh fallback: {'PASS' if 'event=clock_state_refresh' in controller else 'FAIL'}")
    print(f"SIGKILL restart-policy isolation: {'PASS' if 'docker update --restart=no' in suite else 'FAIL'}")
    print(f"final-process correlation      : {'PASS' if 'correlation_id=$request_message_id' in suite else 'FAIL'}")
    print(f"blocking replay bootstrap       : {'PASS' if 'synchronizeReplayBootstrap' in service_clock and 'event=clock_bootstrap_ready' in service_clock else 'FAIL'}")
    print(f"restart-critical log flush      : {'PASS' if 'std::cout.flush()' in service_clock else 'FAIL'}")

    if failures:
        print("\n[FAIL] 35D findings:")
        for failure in failures:
            print(f"  - {failure}")
        print("\nSTEP 35D SOURCE GATE RESULT: FAIL")
        return 1

    print("\nSTEP 35D SOURCE GATE RESULT: PASS")
    print("Run shared_clock_restart_resync_suite.sh for injected hard-restart proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
