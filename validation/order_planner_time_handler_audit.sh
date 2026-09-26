#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/order_planner_service/src/order_planner_service_main.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"

grep -q '#include "time_handler_factory.h"' "$FILE" \
  || fail "OrderPlanner does not include the shared TimeHandler factory"
grep -q 'const TimeHandlerConfig time_config_;' "$FILE" \
  || fail "OrderPlanner does not retain canonical TimeHandler config"
grep -q 'const TimeHandler time_handler_;' "$FILE" \
  || fail "OrderPlanner does not own a local TimeHandler"
grep -q 'time_config_(TimeHandlerFactory::loadConfigFromEnvironment())' "$FILE" \
  || fail "OrderPlanner does not load canonical time config from environment"
grep -q 'time_handler_(TimeHandlerFactory::create(time_config_))' "$FILE" \
  || fail "OrderPlanner does not build TimeHandler from canonical config"

grep -q 'Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)' "$FILE" \
  || fail "OrderPlanner completed-day helper is not parameterized by TimeHandler"
grep -q 'getCurrentUtcDate(timeHandler.getTime())' "$FILE" \
  || fail "OrderPlanner completed-day helper does not use business time"

business_calls="$(grep -c 'newestCompletedBusinessUtcDate(time_handler_)' "$FILE" || true)"
[[ "$business_calls" -eq 2 ]] \
  || fail "expected exactly 2 OrderPlanner business-day reads through TimeHandler, found $business_calls"

grep -q 'interruptibleBusinessWaitUntil(' "$FILE" \
  || fail "OrderPlanner does not have an interruptible business-time wait"
grep -q 'computeNextMidnightUTC(time_handler_.getTime())' "$FILE" \
  || fail "OrderPlanner next UTC boundary is not derived from business time"

system_now_count="$(grep -c 'std::chrono::system_clock::now()' "$FILE" || true)"
[[ "$system_now_count" -eq 0 ]] \
  || fail "OrderPlanner still has direct system_clock::now() reads: $system_now_count"

# Technical loop cadence remains monotonic/real.
grep -q 'std::chrono::steady_clock::now()' "$FILE" \
  || fail "OrderPlanner technical loop no longer uses steady_clock"
grep -q 'std::this_thread::sleep_for' "$FILE" \
  || fail "OrderPlanner no longer has real technical sleep_for polling"

if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--runtime-mode|--simulation-id' "$FILE"; then
  fail "legacy distributed clock residue found in OrderPlanner"
fi

echo "PASS: T10 OrderPlanner business/technical time separation audit"
