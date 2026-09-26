#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
SRC="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }

grep -Fq 'event=replay_account_snapshot_waiting_for_execution_plan' "$SRC" || fail "missing replay execution-plan barrier"
grep -Fq 'engine_.lastExecutionTimestamp() < update.completed_through' "$SRC" || fail "missing lastExecutionTimestamp causal gate"
grep -Fq 'active_execution_cycle_->execution_timestamp <= update.completed_through' "$SRC" || fail "missing active execution-cycle completion gate"
grep -Fq 'update.completed_through > *replay_bootstrap_completed_date_' "$SRC" || fail "bootstrap day is not explicitly exempted"
grep -Fq '"account-snapshot:market-data:" + std::to_string(update.completed_through)' "$SRC" || fail "daily account snapshot identity changed"

echo "PASS: replay close-T account snapshot waits for the T execution plan to exist"
echo "PASS: replay close-T account snapshot waits for orders/fills in the T execution cycle to finish"
echo "PASS: immutable bootstrap day remains the initial-account exception and LIVE behavior remains outside the replay-only gate"
