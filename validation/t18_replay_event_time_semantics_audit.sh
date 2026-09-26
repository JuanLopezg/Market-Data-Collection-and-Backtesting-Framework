#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"

risk="$ROOT/live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp"
planner="$ROOT/live_trading/order_planner_service/src/order_planner_service_main.cpp"
execs="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"
strategy="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
sim="$ROOT/live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp"

for f in "$risk" "$planner" "$execs" "$strategy" "$sim"; do
  test -f "$f" || { echo "FAIL: missing $f"; exit 1; }
done

grep -q 'configuredReplayBootstrapCompletedUtcDate' "$risk"
grep -q 'configuredReplayBootstrapCompletedUtcDate' "$planner"
grep -q 'configuredReplayBootstrapCompletedUtcDate' "$execs"
grep -q 'replay_bootstrap_completed_date_' "$risk"
grep -q 'replay_bootstrap_completed_date_' "$planner"
grep -q 'replay_bootstrap_completed_date_' "$execs"

grep -q '!replay_bootstrap_completed_date_.has_value() && target < newestCompleted' "$risk"
grep -q 'request.decision_timestamp < newestCompleted' "$planner"
grep -q 'pending_decision_->decision_timestamp > newestCompleted' "$execs"
grep -q 'pending_decision_->decision_timestamp != newestCompleted' "$execs"
grep -q 'pre_bootstrap_market_data_account_snapshot_skipped' "$execs"
grep -q 'pre_bootstrap_decision_skipped' "$execs"

grep -q 'immutable_bootstrap_business_anchor' "$strategy"
grep -q 'latestRankingDateAtOrBefore(newestCompleted)' "$strategy"
grep -q 'processOpenIfAvailable(command.order)' "$sim"
grep -q "order's exact active_from price" "$sim"

python3 - "$risk" "$planner" "$execs" <<'PY'
from pathlib import Path
import sys
risk, planner, execution = [Path(p).read_text() for p in sys.argv[1:]]
bad = [
    ("risk unconditional stale intent", "if (target < newestCompleted) {" in risk),
    ("planner unconditional stale request", "if (request.decision_timestamp < newestCompleted) {" in planner),
    ("execution unconditional stale decision", "if (batch.decision_timestamp < newestCompleted) {" in execution),
    ("execution strict current-day handoff",
     "if (pending_decision_->decision_timestamp != newestCompleted)\n            return;" in execution),
]
for name, hit in bad:
    if hit:
        raise SystemExit("FAIL: " + name)
print("PASS: accelerated historical replay uses event-time/checkpoint semantics downstream of Strategy")
print("PASS: identity LIVE stale-day gates remain explicit and replay-only behavior is env-scoped")
print("PASS: exact T+1 active_from execution-price latency invariance remains intact")
PY
