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
# Either in one step, or (current source) the canonical env config kept as time_config_
# (also used for the replay bootstrap date) and the TimeHandler created from it.
grep -q 'time_handler_(TimeHandlerFactory::createFromEnvironment())' "$FILE" \
  || { grep -q 'time_config_(TimeHandlerFactory::loadConfigFromEnvironment())' "$FILE" \
       && grep -q 'time_handler_(TimeHandlerFactory::create(time_config_))' "$FILE"; } \
  || fail "ExecutionState does not construct TimeHandler from canonical env config"
grep -q 'Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)' "$FILE" \
  || fail "newestCompletedUtcDate is not parameterized by TimeHandler"
grep -q 'floor<std::chrono::days>(timeHandler.getTime())' "$FILE" \
  || fail "newestCompletedUtcDate does not use business time"

business_calls="$(grep -c 'const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);' "$FILE" || true)"
[[ "$business_calls" -eq 4 ]] \
  || fail "expected exactly 4 ExecutionState business-day gates through TimeHandler, found $business_calls"

# Direct wall-clock reads are allowed only where wall time is the rule, never for the
# economic day: the snapshot-request nonce, the periodic reconciliation schedule
# (hourly UTC wall clock), and Manual Control (dashboard request age, 23:45-00:15 window,
# age of the current Binance price). Each is checked at its expected line.
system_now_count="$(grep -c 'std::chrono::system_clock::now()' "$FILE" || true)"
[[ "$system_now_count" -eq 4 ]] \
  || fail "expected exactly 4 intentional system_clock::now() reads, found $system_now_count"
grep -q 'const auto nonce = std::chrono::system_clock::now().time_since_epoch().count();' "$FILE" \
  || fail "the snapshot-request nonce no longer reads the wall clock as expected"
grep -A2 'if (!options_.periodic_reconciliation)' "$FILE" | grep -q 'const auto now = std::chrono::system_clock::now();' \
  || fail "the periodic reconciliation schedule does not read the wall clock as expected"
grep -A2 'void checkManualRequestAllowed(const ManualRequest& request) const' "$FILE" \
  | grep -q 'const auto now = std::chrono::system_clock::now();' \
  || fail "the Manual Control request check does not read the wall clock as expected"
grep -B1 'std::chrono::system_clock::now().time_since_epoch()).count();' "$FILE" | grep -q 'const auto nowMs' \
  || fail "the Manual Control price age does not read the wall clock as expected"

# Continuous reconciliation/event processing must remain technical real/monotonic time.
grep -q 'std::chrono::steady_clock::now()' "$FILE" \
  || fail "ExecutionState technical loop no longer uses steady_clock"
grep -q 'std::this_thread::sleep_for(interval - elapsed)' "$FILE" \
  || fail "ExecutionState technical polling cadence no longer uses real sleep_for"

if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|simulation\.clock\.|--runtime-mode|--simulation-id' "$FILE"; then
  fail "legacy distributed clock residue found in ExecutionState"
fi

echo "PASS: T9 ExecutionState business/technical time separation audit"
