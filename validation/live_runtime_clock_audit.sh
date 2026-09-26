#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
SERVICES=(
  market_data_service
  strategy_service
  portfolio_risk_service
  execution_state_service
  order_planner_service
  exchange_gateway
  execution_service
)
PATTERN='ServiceClockContext|service_clock\.h|clock_->|--runtime-mode|--simulation-id|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST'
failed=0

for service in "${SERVICES[@]}"; do
  dir="$ROOT/live_trading/$service"
  [[ -d "$dir" ]] || continue
  if grep -RInE "$PATTERN" "$dir"; then
    echo "FAIL: old replay/shared-clock dependency found in LIVE service: $service" >&2
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  exit 1
fi

echo "PASS: production LIVE service sources contain no old replay/shared-clock dependency markers."
echo "NOTE: replay_controller/simulated_exchange_service and replay-only lib contracts are intentionally outside this audit."
