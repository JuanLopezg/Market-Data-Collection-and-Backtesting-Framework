#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"

grep -q '#include "time_handler_factory.h"' "$FILE" \
  || fail "Strategy does not include the shared TimeHandler factory"
grep -q 'TimeHandlerFactory::loadConfigFromEnvironment()' "$FILE" \
  || fail "Strategy does not load canonical TimeHandler configuration"
grep -q 'TimeHandlerFactory::create(time_config_)' "$FILE" \
  || fail "Strategy does not construct TimeHandler from canonical config"
grep -q 'newestCompletedBusinessUtcDate(time_handler_)' "$FILE" \
  || fail "Strategy completed-day gating is not based on TimeHandler"
grep -q 'computeNextMidnightUTC(time_handler_.getTime())' "$FILE" \
  || fail "Strategy next business UTC boundary is not based on TimeHandler"
grep -q 'interruptibleBusinessWaitUntil' "$FILE" \
  || fail "Strategy has no interruptible business-time wait"

if grep -q 'std::chrono::system_clock::now()' "$FILE"; then
  echo "Unexpected direct system_clock::now() uses:" >&2
  grep -n 'std::chrono::system_clock::now()' "$FILE" >&2
  fail "Strategy still reads host wall clock directly"
fi

# Technical cadence must remain real and must not be speed-scaled: a real sleep_for while
# waiting for the next business day, and the bus poll timeout while draining messages.
grep -q 'std::this_thread::sleep_for(technicalPollInterval);' "$FILE" \
  || fail "Strategy technical interruptible polling sleep is missing"
grep -q 'bus_.poll(market_update_subscription_, 32, fetchTimeoutMs);' "$FILE" \
  || fail "Strategy message draining no longer uses the bounded technical poll timeout"

# Legacy distributed clock must not be reintroduced into Strategy.
if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--simulation-id|--runtime-mode' "$FILE"; then
  echo "Legacy clock residue found in Strategy:" >&2
  grep -nE 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--simulation-id|--runtime-mode' "$FILE" >&2 || true
  fail "legacy distributed clock was reintroduced into Strategy"
fi

echo "PASS: T7 Strategy business/technical time separation audit"
