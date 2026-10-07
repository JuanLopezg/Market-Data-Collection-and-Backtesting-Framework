#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
fail=0

require() {
  local pattern="$1" file="$2" label="$3"
  if ! grep -Fq -- "$pattern" "$ROOT/$file"; then
    echo "FAIL: $label"
    fail=1
  fi
}

require 'LiveExecutionIdentity::notionalPlanningRequest' \
  'live_trading/execution_state_service/src/execution_state_service_main.cpp' \
  'ExecutionState must use shared deterministic planning-request identity'
require 'LiveExecutionIdentity::notionalOrderPlan' \
  'live_trading/order_planner_service/src/order_planner_service_main.cpp' \
  'OrderPlanner must use shared deterministic plan identity'
require 'LiveExecutionIdentity::plannedEconomicOrder' \
  'lib/src/execution/planning/notional_order_planner.cpp' \
  'planner engine must assign deterministic per-order economic identity'
require 'economic_order_id' \
  'lib/src/contracts/notional_order_planning.h' \
  'planned notional order must expose economic_order_id'
require 'delta_notional_usd' \
  'lib/src/contracts/notional_order_planning.h' \
  'planned notional order must expose signed delta_notional_usd'
require 'reference_close' \
  'lib/src/contracts/notional_order_planning.h' \
  'planned notional order must carry exact reference_close'
require 'Planned notional order deterministic identity mismatch' \
  'lib/src/transport/message_json.cpp' \
  'decoder must validate deterministic planned-order identity'
require 'Planned notional order reference close mismatch' \
  'lib/src/transport/message_json.cpp' \
  'decoder must validate per-order close against batch close snapshot'

if [[ $fail -ne 0 ]]; then
  exit 1
fi

echo 'PASS: LIVE contract semantics and deterministic identity audit passed.'
