#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"

grep -q '#include "time_handler_factory.h"' "$FILE" || fail "TimeHandler factory is not included"
grep -q 'TimeHandler time_handler_' "$FILE" || fail "SimulatedExchange does not own a local TimeHandler"
grep -q 'TimeHandlerFactory::createFromEnvironment()' "$FILE" || fail "SimulatedExchange does not use the common TimeHandler factory"
grep -q 'businessTimeReady' "$FILE" || fail "business/event-time gate is missing"
grep -q 'execution_prices_market_time' "$FILE" || fail "execution market time is not gated by local business time"
grep -q 'TransportSubjects::tradingRuntimeSubjects()' "$FILE" || fail "runtime stream still depends on replay clock subjects"

if grep -Eq 'ServiceClockContext|service_clock\.h|clock_->|parseRuntimeMode|runtimeModeName|--runtime-mode|--simulation-id|simulation_id|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST' "$FILE"; then
  echo "Legacy clock residue found in SimulatedExchange:" >&2
  grep -nE 'ServiceClockContext|service_clock\.h|clock_->|parseRuntimeMode|runtimeModeName|--runtime-mode|--simulation-id|simulation_id|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST' "$FILE" >&2 || true
  exit 1
fi

# T12 must not introduce host system_clock/steady_clock as business time inside the service.
# NATS poll timeouts remain technical and are represented by bus_.poll(... poll_timeout_ms).
if grep -Eq 'std::chrono::system_clock::now\(|std::chrono::steady_clock::now\(|std::this_thread::sleep_' "$FILE"; then
  echo "Unexpected direct host-time/sleep use found in SimulatedExchange:" >&2
  grep -nE 'std::chrono::system_clock::now\(|std::chrono::steady_clock::now\(|std::this_thread::sleep_' "$FILE" >&2 || true
  exit 1
fi

echo "PASS: T12 SimulatedExchange local TimeHandler / legacy-clock removal audit"
