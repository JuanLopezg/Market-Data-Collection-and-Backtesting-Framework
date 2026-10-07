#!/usr/bin/env python3
"""Fail-fast source audit for the current dashboard/trading integration contracts.

This script never reads .env files and never prints secrets. It only checks that
known source files still contain the contract/table/subject markers used by
the current real-source mapping.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

CHECKS = {
    "lib/src/transport/message_subjects.h": [
        '"market.data.updated.v1"',
        '"strategy.intents.v1"',
        '"decision.batch.v1"',
        '"execution.plan.notional.request.v1"',
        '"execution.plan.notional.v1"',
        '"execution.event.order_update.v1"',
        '"execution.event.fill.v1"',
        '"execution.account.snapshot.v1"',
        '"execution.cycle.complete.v1"',
        '"execution.exchange.snapshot.v1"',
    ],
    "lib/src/persistence/postgres_state_store.cpp": [
        "trading_runtime_state",
        "trading_fills",
        '"account_cash"',
        '"account_positions"',
        '"orders"',
        '"processed_fill_ids"',
    ],
    "live_trading/strategy_service/src/strategy_service_main.cpp": [
        "strategy_service_metadata",
        "strategy_market_update_checkpoint",
    ],
    "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp": [
        "portfolio_risk_service_metadata",
        "portfolio_risk_live_account_checkpoint",
        "portfolio_risk_live_decision_checkpoint",
    ],
    "live_trading/order_planner_service/src/order_planner_service_main.cpp": [
        "order_planner_live_notional_checkpoint",
    ],
    "live_trading/market_data_service/src/market_store.cpp": [
        "tracked_pairs",
        "ohlcv_data",
        "date_of_start",
        "market_volume_rank_daily",
    ],
    "deploy/live/docker-compose.yml": [
        "market-data:",
        "strategy:",
        "portfolio-risk:",
        "execution-state:",
        "order-planner:",
        "exchange-gateway:",
        'profiles: ["exchange-edge"]',
    ],
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, help="Path to algoTrading project root")
    args = parser.parse_args()

    root = Path(args.project_root).expanduser().resolve()
    failures: list[str] = []
    checked = 0

    for relative, markers in CHECKS.items():
        path = root / relative
        if not path.is_file():
            failures.append(f"missing file: {relative}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for marker in markers:
            checked += 1
            if marker not in text:
                failures.append(f"{relative}: missing audited marker {marker!r}")

    if failures:
        print("DASHBOARD SOURCE AUDIT: FAIL")
        for failure in failures:
            print(f"- {failure}")
        print("Do not enable RealProvider until the mapping is re-audited.")
        return 1

    print(f"DASHBOARD SOURCE AUDIT: PASS ({checked} markers)")
    print("Audited source contracts still match the current dashboard mapping.")
    print("This confirms source markers only; it does not validate runtime connectivity, accounting completeness or private execution readiness.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
