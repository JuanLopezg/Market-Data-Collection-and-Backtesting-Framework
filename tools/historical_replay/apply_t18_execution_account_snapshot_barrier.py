#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
PATH = ROOT / "live_trading/execution_state_service/src/execution_state_service_main.cpp"
BACKUP = ROOT / "deploy/historical_replay/run/t18_execution_account_snapshot_barrier_backup.cpp"

MARKER = "event=replay_account_snapshot_waiting_for_execution_plan"

OLD = '''            // LIVE market-data no longer publishes MarketSliceSnapshot. This small bridge
            // exposes the already durable ExecutionState account at close T so
            // PortfolioRisk can join StrategyIntent(T) + AccountSnapshot(T).
            publishAccountSnapshot(
                update.completed_through,
                "account-snapshot:market-data:" + std::to_string(update.completed_through),
                update.metadata.message_id
            );
'''

NEW = '''            // Historical replay must make the close-T account snapshot causal with
            // execution at T.  Orders decided on T-1 execute at open(T), so publishing
            // AccountSnapshot(T) before that execution cycle has been applied and fully
            // consumed makes PortfolioRisk depend on CPU/NATS latency.  The immutable
            // bootstrap day is the only exception: it intentionally exposes the initial
            // account before any replay execution cycle exists.
            if (replay_bootstrap_completed_date_.has_value() &&
                update.completed_through > *replay_bootstrap_completed_date_) {
                if (engine_.lastExecutionTimestamp() < update.completed_through) {
                    LG_DEBUG(
                        "service=execution-state event=replay_account_snapshot_waiting_for_execution_plan timestamp={} last_execution_timestamp={} disposition=retry",
                        update.completed_through,
                        engine_.lastExecutionTimestamp()
                    );
                    return DurableMessageDisposition::Retry;
                }

                if (active_execution_cycle_.has_value() &&
                    active_execution_cycle_->execution_timestamp <= update.completed_through) {
                    LG_DEBUG(
                        "service=execution-state event=replay_account_snapshot_waiting_for_execution_cycle timestamp={} active_execution_timestamp={} disposition=retry",
                        update.completed_through,
                        active_execution_cycle_->execution_timestamp
                    );
                    return DurableMessageDisposition::Retry;
                }
            }

            // LIVE market-data no longer publishes MarketSliceSnapshot. This small bridge
            // exposes the already durable ExecutionState account at close T so
            // PortfolioRisk can join StrategyIntent(T) + AccountSnapshot(T).
            publishAccountSnapshot(
                update.completed_through,
                "account-snapshot:market-data:" + std::to_string(update.completed_through),
                update.metadata.message_id
            );
'''


def main():
    if not PATH.exists():
        raise SystemExit(f"missing expected source: {PATH}")

    text = PATH.read_text()
    if MARKER in text:
        print(f"SKIP already patched: {PATH}")
        return 0

    required = [
        "replay_bootstrap_completed_date_",
        "configuredReplayBootstrapCompletedUtcDate",
        "active_execution_cycle_",
        "engine_.lastExecutionTimestamp()",
        '"account-snapshot:market-data:"',
    ]
    missing = [x for x in required if x not in text]
    if missing:
        raise SystemExit(
            "FAIL: expected T18 event-time prerequisites are missing: " + ", ".join(missing)
        )

    count = text.count(OLD)
    if count != 1:
        raise SystemExit(
            f"FAIL: expected exactly one market-data account snapshot publish block, found {count}; no changes made"
        )

    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(PATH, BACKUP)
    try:
        PATH.write_text(text.replace(OLD, NEW, 1))
    except Exception:
        shutil.copy2(BACKUP, PATH)
        raise

    print("PASS: T18 execution account-snapshot causal barrier applied")
    print(f"backup={BACKUP}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
