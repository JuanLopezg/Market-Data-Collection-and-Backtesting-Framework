#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"

files=(
  "live_trading/market_data_service/src/market_data_service_main.cpp"
  "live_trading/strategy_service/src/strategy_service_main.cpp"
  "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp"
  "live_trading/execution_state_service/src/execution_state_service_main.cpp"
  "live_trading/order_planner_service/src/order_planner_service_main.cpp"
  "live_trading/exchange_gateway/src/exchange_gateway_main.cpp"
  "live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp"
)

legacy_regex='ServiceClockContext|SimulatedClock|ClockState|ClockControl|ClockSyncRequest|TransportSubjects::CLOCK_(STATE|CONTROL|SYNC_REQUEST)|runtimeSubjects[[:space:]]*\(|--runtime-mode|--simulation-id|simulation\.clock\.'
failed=0

printf '%s\n' '============================================================'
printf '%s\n' 'T13 — NEW HISTORICAL REPLAY / NO SHARED CLOCK AUDIT'
printf '%s\n' '============================================================'

for relative in "${files[@]}"; do
  path="$ROOT/$relative"
  if [[ ! -f "$path" ]]; then
    echo "FAIL missing: $relative"
    failed=1
    continue
  fi

  if grep -En "$legacy_regex" "$path" >/tmp/t13_clock_matches.$$ 2>/dev/null; then
    echo "FAIL legacy clock dependency in $relative"
    cat /tmp/t13_clock_matches.$$
    failed=1
  else
    echo "PASS no legacy clock dependency: $relative"
  fi

  if ! grep -Eq 'TimeHandlerFactory::(createFromEnvironment|loadConfigFromEnvironment|create)' "$path"; then
    echo "FAIL TimeHandler factory not found in $relative"
    failed=1
  fi
done
rm -f /tmp/t13_clock_matches.$$ || true

# The new run-config generator itself must remain a pure local bootstrap utility.
generator="$ROOT/tools/historical_replay/create_time_env.py"
if [[ ! -f "$generator" ]]; then
  echo "FAIL missing historical replay time env generator"
  failed=1
else
  forbidden_generator='nats|postgres|ClockState|ClockControl|ClockSyncRequest|ServiceClockContext|SimulatedClock|simulation_id|pause|resume'
  if grep -Ein "$forbidden_generator" "$generator" >/tmp/t13_generator_matches.$$ 2>/dev/null; then
    echo "FAIL generator contains forbidden distributed-clock/control-plane concepts"
    cat /tmp/t13_generator_matches.$$
    failed=1
  else
    echo "PASS time env generator is local/config-only"
  fi
  rm -f /tmp/t13_generator_matches.$$ || true
fi

# T13 deliberately does NOT delete the old replay controller yet. It is legacy and
# must not be used by the new path; T23 removes the remaining legacy files after the
# historical replay path has passed acceptance.
if [[ -f "$ROOT/live_trading/replay_controller/src/replay_controller_main.cpp" ]]; then
  echo "INFO legacy replay-controller still present (expected until cleanup; do not use it for new replay)"
fi

if [[ "$failed" -ne 0 ]]; then
  echo "FAIL: T13 historical replay still depends on shared logical clock"
  exit 1
fi

echo "PASS: T13 new historical replay path has no shared-clock dependency"
