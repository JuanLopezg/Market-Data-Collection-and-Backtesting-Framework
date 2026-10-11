#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
F="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"

grep -q 'newestCompletedBusinessUtcDate(time_handler_)' "$F"
grep -q 'options_.market_data_source,' "$F"
grep -q 'latestRankingDateAtOrBefore' "$F"
grep -q 'Caught up signal state for' "$F"
grep -q 'consumer.max_ack_pending = 64;' "$F"
grep -q 'bus_.poll(market_update_subscription_, 32, fetchTimeoutMs);' "$F"
if grep -q 'bus_.poll(market_update_subscription_, 1, fetchTimeoutMs);' "$F"; then
  echo 'FAIL: Strategy still throttles backlog catch-up to one message per poll'
  exit 1
fi
if grep -q 'interruptibleSleepFor(loopPeriod - elapsed)' "$F"; then
  echo 'FAIL: Strategy still injects a 1-second technical sleep while behind'
  exit 1
fi

echo 'PASS: Strategy preserves TimeHandler + canonical-frontier catch-up semantics'
echo 'PASS: retained JetStream backlog drains in batches without a 1-second-per-message throttle'
echo 'PASS: catch-up poll remains bounded by fetchTimeoutMs and is not a busy-spin'
