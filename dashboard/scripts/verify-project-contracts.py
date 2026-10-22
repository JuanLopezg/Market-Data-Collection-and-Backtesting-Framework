#!/usr/bin/env python3
"""Fail-fast source audit for the current dashboard/trading integration contracts.

This script never reads .env files and never prints secrets. It only checks that
known source files still contain the contract/table/subject markers used by
the current real-source mapping.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
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
    "dashboard/dashboard-api/Dockerfile": [
        "-o /out/dashboard-watchdog ./cmd/dashboard-watchdog",
        "-o /out/dashboard-alert-notifier ./cmd/dashboard-alert-notifier",
    ],
    "dashboard/dashboard-api/internal/provider/real_alerts_audit.go": [
        "mergeSymbolMapAlerts", "mergeLedgerAlerts",
        "LEDGER_INTEGRITY_BLOCKED", "SYMBOL_MAP_UNAVAILABLE",
    ],
    "dashboard/dashboard-api/internal/server/server.go": [
        "POST /api/manual-control/route", "POST /api/alerts-audit/acknowledge",
    ],
    "dashboard/dashboard-api/internal/notifier/telegram.go": [
        'telegramAPIBaseURL      = "https://api.telegram.org"',
        "TelegramReceiptVersion",
    ],
    "dashboard/dashboard-api/internal/notifier/engine.go": ['"notify:"'],
    "dashboard/dashboard-api/cmd/dashboard-alert-notifier/main.go": ['"/data/notifier/telegram"'],
}

# Retain the shared source boundaries from the retired standalone deployment gates.
# Manual requests may reach execution-state through its folder; notification and
# acknowledgement code must remain independent of trading commands/credentials.
API = "dashboard/dashboard-api"
FORBIDDEN = [
    ([f"{API}/cmd/dashboard-watchdog", f"{API}/internal/alertstore"],
     r"PRIVATE_KEY|mnemonic|seed phrase|/exchange|submit.*order|cancel.*order"),
    ([f"{API}/internal/notifier", f"{API}/cmd/dashboard-alert-notifier", f"{API}/internal/alertack"],
     r"Publish\(|/exchange|submit.*order|cancel.*order|modify.*order|PRIVATE_KEY|MNEMONIC|SEED_PHRASE|DASHBOARD_POSTGRES_DSN|DASHBOARD_NATS_URL"),
    ([f"{API}/internal/manualaudit", f"{API}/internal/provider/manual_control.go", f"{API}/internal/server/server.go"],
     r"Publish\("),
    ([f"{API}/cmd", f"{API}/internal"],
     r"PRIVATE_KEY|MNEMONIC|SEED_PHRASE|HYPERLIQUID_[A-Z0-9_]*SECRET|API_WALLET_[A-Z0-9_]*KEY|DASHBOARD_TELEGRAM_API_BASE_URL"),
]


def check_observability_boundaries(root: Path) -> list[str]:
    failures = []
    for paths, pattern in FORBIDDEN:
        for relative in paths:
            path = root / relative
            if not path.exists():
                failures.append(f"missing source boundary: {relative}")
                continue
            sources = sorted(path.rglob("*.go")) if path.is_dir() else [path]
            for source in sources:
                if re.search(pattern, source.read_text(encoding="utf-8")):
                    failures.append(f"{source.relative_to(root)}: forbidden observability dependency ({pattern})")

    postgres = root / API / "internal/integration/postgres"
    for source in sorted(postgres.glob("*.go")):
        if source.name.endswith("_test.go"):
            continue
        if re.search(r"\b(INSERT|UPDATE|DELETE|ALTER|DROP|TRUNCATE|CREATE\s+TABLE)\b", source.read_text(encoding="utf-8")):
            failures.append(f"{source.relative_to(root)}: mutating SQL in trading read adapter")

    for relative in ("dashboard/.env.example", "dashboard/.env.production.example"):
        path = root / relative
        if not path.is_file():
            failures.append(f"missing credential example: {relative}")
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            name, separator, value = line.partition("=")
            if separator and name.strip() in ("DASHBOARD_TELEGRAM_BOT_TOKEN", "DASHBOARD_TELEGRAM_CHAT_ID") and value.strip():
                failures.append(f"{relative}: Telegram example credential must be empty")
    return failures


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

    failures.extend(check_observability_boundaries(root))

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
