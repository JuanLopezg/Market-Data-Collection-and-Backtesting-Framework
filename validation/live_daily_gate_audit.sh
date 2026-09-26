#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
fail=0

require_marker() {
  local file="$1"
  local marker="$2"
  if ! grep -Fq -- "$marker" "$ROOT/$file"; then
    echo "FAIL: missing marker '$marker' in $file"
    fail=1
  fi
}

forbid_marker() {
  local file="$1"
  local marker="$2"
  if grep -Fq -- "$marker" "$ROOT/$file"; then
    echo "FAIL: forbidden marker '$marker' in $file"
    fail=1
  fi
}

# MarketData: previous completed UTC day, commit then publish, scheduled daily run.
require_marker "live_trading/market_data_service/src/market_data_service_main.cpp" "getPreviousDayDate(getCurrentUtcDate(std::chrono::system_clock::now()))"
require_marker "live_trading/market_data_service/src/market_data_service_main.cpp" "event=next_daily_run_scheduled"
require_marker "live_trading/market_data_service/src/market_data_service_main.cpp" "publisher.publish(summary)"

# Strategy: future rejected, stale notifications drained, checkpointed day sleeps to next midnight.
require_marker "live_trading/strategy_service/src/strategy_service_main.cpp" "event=stale_market_update_skipped"
require_marker "live_trading/strategy_service/src/strategy_service_main.cpp" "event=daily_checkpoint_complete"
require_marker "live_trading/strategy_service/src/strategy_service_main.cpp" "computeNextMidnightUTC"

# PortfolioRisk: only current day economic intent; deterministic close-boundary account snapshot.
require_marker "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp" "event=stale_strategy_intents"
require_marker "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp" "event=stale_account_snapshot_skipped"
require_marker "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp" "account-snapshot:market-data:"
require_marker "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp" "ON CONFLICT(state_key,timestamp) DO NOTHING"
require_marker "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp" "event=daily_checkpoint_complete"

# OrderPlanner: exact current completed day only, checkpoint then true wait until next UTC day.
require_marker "live_trading/order_planner_service/src/order_planner_service_main.cpp" "event=stale_notional_request_skipped"
require_marker "live_trading/order_planner_service/src/order_planner_service_main.cpp" "event=daily_checkpoint_complete"
require_marker "live_trading/order_planner_service/src/order_planner_service_main.cpp" "computeNextMidnightUTC"

# ExecutionState: economic inputs gated to current T, execution truth remains continuously polled.
require_marker "live_trading/execution_state_service/src/execution_state_service_main.cpp" "event=stale_market_data_account_snapshot_skipped"
require_marker "live_trading/execution_state_service/src/execution_state_service_main.cpp" "event=stale_decision_skipped"
require_marker "live_trading/execution_state_service/src/execution_state_service_main.cpp" "Execution truth is always serviced"
require_marker "live_trading/execution_state_service/src/execution_state_service_main.cpp" "bus_.poll(exchange_event_subscription_"

# No reintroduction of old clock into the economic LIVE services.
for file in \
  live_trading/strategy_service/src/strategy_service_main.cpp \
  live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp \
  live_trading/order_planner_service/src/order_planner_service_main.cpp \
  live_trading/execution_state_service/src/execution_state_service_main.cpp; do
  forbid_marker "$file" "ServiceClockContext"
  forbid_marker "$file" "clock_->"
  forbid_marker "$file" "--simulation-id"
done

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: LIVE daily UTC gating/checkpoint source audit passed."
