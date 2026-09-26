#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/market_data_service/src/market_data_service_main.cpp"
CLIENT="$ROOT/live_trading/market_data_service/src/binance_market_data_client.cpp"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

[[ -f "$FILE" ]] || fail "missing $FILE"
[[ -f "$CLIENT" ]] || fail "missing $CLIENT"

grep -q '#include "time_handler_factory.h"' "$FILE" \
    || fail "market-data does not include TimeHandler factory"
grep -q 'TimeHandlerFactory::loadConfigFromEnvironment()' "$FILE" \
    || fail "market-data does not load canonical time configuration"
grep -q 'TimeHandlerFactory::create(timeConfig)' "$FILE" \
    || fail "market-data does not construct TimeHandler from canonical config"
grep -q 'newestCompletedBusinessUtcDate(timeHandler)' "$FILE" \
    || fail "completed-day gating is not based on TimeHandler"
grep -q 'nextScheduledBusinessRun' "$FILE" \
    || fail "daily boundary scheduling is not based on TimeHandler"
grep -q 'timeHandler.getTime()' "$FILE" \
    || fail "market-data has no business-time reads"

# These exact host-now forms previously drove economic date/boundary decisions and must be gone.
if grep -q 'getCurrentUtcDate(std::chrono::system_clock::now())' "$FILE"; then
    fail "business completed-day calculation still reads host system_clock directly"
fi
if grep -q 'computeNextMidnightUTC(std::chrono::system_clock::now())' "$FILE"; then
    fail "business midnight calculation still reads host system_clock directly"
fi

# Technical retry/backoff deliberately remains on real/process time.
grep -q 'retryAt = std::chrono::system_clock::now()' "$FILE" \
    || fail "market-data retry is no longer explicitly real technical time"
grep -q 'interruptibleTechnicalWaitUntil' "$FILE" \
    || fail "market-data technical retry wait is missing"
grep -q 'std::this_thread::sleep_for' "$CLIENT" \
    || fail "Binance technical HTTP backoff no longer uses real sleep_for"

# T6 must not couple MarketData back to the legacy shared-clock control plane.
if grep -Eq 'ServiceClockContext|SimulatedClock|ClockState|CLOCK_STATE|simulation_id|--runtime-mode|--simulation-id' "$FILE"; then
    fail "market-data reintroduced a legacy shared-clock dependency"
fi

echo "PASS: T6 MarketData business/technical time separation audit"
