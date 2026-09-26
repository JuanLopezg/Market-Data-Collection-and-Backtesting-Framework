#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"

grep -q '#include "time_handler_factory.h"' "$FILE" \
  || fail "ExecutionState does not include the shared TimeHandler factory"
grep -q 'TimeHandler time_handler_;' "$FILE" \
  || fail "ExecutionState does not own a local TimeHandler"
grep -q 'time_handler_(TimeHandlerFactory::createFromEnvironment())' "$FILE" \
  || fail "ExecutionState does not construct TimeHandler from canonical env config"
grep -q 'Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)' "$FILE" \
  || fail "newestCompletedUtcDate is not parameterized by TimeHandler"
grep -q 'floor<std::chrono::days>(timeHandler.getTime())' "$FILE" \
  || fail "newestCompletedUtcDate does not use business time"

business_calls="$(grep -c 'const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);' "$FILE" || true)"
[[ "$business_calls" -eq 3 ]] \
  || fail "expected exactly 3 ExecutionState business-day gates through TimeHandler, found $business_calls"

# A single direct system_clock read remains intentionally: it is only a technical
# nonce for the startup exchange-snapshot request message id, not economic time.
system_now_count="$(grep -c 'std::chrono::system_clock::now()' "$FILE" || true)"
[[ "$system_now_count" -eq 1 ]] \
  || fail "expected exactly one technical system_clock::now() nonce, found $system_now_count"
grep -q 'const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();' "$FILE" \
  || fail "the remaining system_clock::now() is not the expected technical nonce"

# Continuous reconciliation/event processing must remain technical real/monotonic time.
grep -q 'std::chrono::steady_clock::now()' "$FILE" \
  || fail "ExecutionState technical loop no longer uses steady_clock"
grep -q 'std::this_thread::sleep_for(interval - elapsed)' "$FILE" \
  || fail "ExecutionState technical polling cadence no longer uses real sleep_for"

if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--runtime-mode|--simulation-id' "$FILE"; then
  fail "legacy distributed clock residue found in ExecutionState"
fi

echo "PASS: T9 ExecutionState business/technical time separation audit"
