#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
need() {
  local file="$1" marker="$2" label="$3"
  grep -Fq -- "$marker" "$ROOT/$file" || bad "$label"
}
forbid() {
  local file="$1" marker="$2" label="$3"
  if grep -Fq -- "$marker" "$ROOT/$file"; then bad "$label"; fi
}

# Re-run all previous pre-exchange source/deploy gates first.
for audit in \
  live_runtime_clock_audit.sh \
  live_daily_gate_audit.sh \
  live_sqlite_ownership_audit.sh \
  live_contract_identity_audit.sh \
  live_deploy_topology_audit.sh; do
  bash "$ROOT/validation/$audit" "$ROOT" || fail=1
done

# Canonical causal chain must be wired explicitly.
need 'live_trading/market_data_service/src/update_publisher.cpp' \
  'MessageSubjects::MARKET_DATA_UPDATED' \
  'MarketData must publish market.data.updated.v1'
need 'live_trading/strategy_service/src/strategy_service_main.cpp' \
  'MessageSubjects::MARKET_DATA_UPDATED' \
  'Strategy must consume market.data.updated.v1'
need 'live_trading/strategy_service/src/strategy_service_main.cpp' \
  'MessageSubjects::STRATEGY_INTENTS' \
  'Strategy must publish StrategyIntentBatch'
need 'live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp' \
  'MessageSubjects::STRATEGY_INTENTS' \
  'PortfolioRisk must consume StrategyIntentBatch'
need 'live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp' \
  'MessageSubjects::ACCOUNT_SNAPSHOT' \
  'PortfolioRisk must consume the daily AccountSnapshot'
need 'live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp' \
  'MessageSubjects::DECISION_BATCH' \
  'PortfolioRisk must publish DecisionBatch'
need 'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'MessageSubjects::DECISION_BATCH' \
  'ExecutionState must consume DecisionBatch'
need 'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'MessageSubjects::NOTIONAL_ORDER_PLANNING_REQUEST' \
  'ExecutionState must publish the notional planning request'
need 'live_trading/order_planner_service/src/order_planner_service_main.cpp' \
  'MessageSubjects::NOTIONAL_ORDER_PLANNING_REQUEST' \
  'OrderPlanner must consume the notional planning request'
need 'live_trading/order_planner_service/src/order_planner_service_main.cpp' \
  'MessageSubjects::NOTIONAL_ORDER_PLAN' \
  'OrderPlanner must stop at NotionalOrderPlan'

# The pre-exchange path must stop before executable exchange commands.
for file in \
  live_trading/strategy_service/src/strategy_service_main.cpp \
  live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp \
  live_trading/execution_state_service/src/execution_state_service_main.cpp \
  live_trading/order_planner_service/src/order_planner_service_main.cpp; do
  forbid "$file" 'MessageSubjects::SUBMIT_ORDER' \
    "$file must not submit an executable exchange order before STEP 7/8"
  forbid "$file" 'MessageSubjects::BACKEND_SUBMIT_ORDER' \
    "$file must not address an exchange backend before STEP 7/8"
done

# The exact close(T) selected by ExecutionState must remain embedded in the plan.
need 'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'loadExactClosingPrices' \
  'ExecutionState must source exact close(T) from canonical SQLite'
need 'live_trading/order_planner_service/src/order_planner_service_main.cpp' \
  'output.reference_closes = request.reference_closes;' \
  'OrderPlanner must preserve the exact close(T) snapshot unchanged'
need 'lib/src/contracts/notional_order_planning.h' \
  'double reference_close = 0.0;' \
  'Each planned economic order must expose reference_close'
need 'lib/src/contracts/notional_order_planning.h' \
  'double delta_notional_usd = 0.0;' \
  'Each planned economic order must expose signed delta_notional_usd'
need 'lib/src/contracts/notional_order_planning.h' \
  'std::string economic_order_id;' \
  'Each planned economic order must expose deterministic economic identity'

# Acceptance/log observability markers needed by the runtime verifier.
need 'live_trading/market_data_service/src/update_publisher.cpp' \
  'event=market_data_updated_published' \
  'MarketData runtime acceptance marker missing'
need 'live_trading/strategy_service/src/strategy_service_main.cpp' \
  'event=strategy_intents_published' \
  'Strategy runtime acceptance marker missing'
need 'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'event=market_data_account_snapshot_published' \
  'ExecutionState daily account snapshot acceptance marker missing'
need 'live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp' \
  'event=decision_published' \
  'PortfolioRisk runtime acceptance marker missing'
need 'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'event=notional_planning_request_published' \
  'ExecutionState planning acceptance marker missing'
need 'live_trading/order_planner_service/src/order_planner_service_main.cpp' \
  'event=notional_order_plan_published' \
  'OrderPlanner runtime acceptance marker missing'

if [[ $fail -ne 0 ]]; then
  exit 1
fi

echo 'PASS: pre-exchange LIVE causal-chain source/deploy audit passed.'
echo 'NOTE: this is a structural gate. Run live_pre_exchange_runtime_acceptance.sh after a real daily cycle for runtime acceptance.'
