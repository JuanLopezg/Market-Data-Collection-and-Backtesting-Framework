#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
FILE="$ROOT/live_trading/exchange_gateway/src/exchange_gateway_main.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }
need() { grep -Fq -- "$2" "$1" || fail "missing expected text: $2"; }
forbid() { if grep -Eq -- "$2" "$1"; then fail "forbidden pattern found: $2"; fi; }

[[ -f "$FILE" ]] || fail "missing $FILE"

echo "============================================================"
echo "T11 — EXCHANGE GATEWAY TIMEHANDLER AUDIT"
echo "============================================================"

need "$FILE" '#include "time_handler_factory.h"'
need "$FILE" 'TimeHandler time_handler_;'
need "$FILE" 'TimeHandlerFactory::createFromEnvironment()'
need "$FILE" 'currentBusinessUtcDate(time_handler_)'
need "$FILE" 'newestCompletedUtcDate(time_handler_)'
need "$FILE" 'validateBackendEventTime('
need "$FILE" 'value.update.timestamp'
need "$FILE" 'value.fill.timestamp'
need "$FILE" 'value.snapshot.timestamp'
need "$FILE" 'plan.decision_timestamp > newestCompleted'
need "$FILE" 'adapter_->poll(options_.poll_timeout_ms);'
need "$FILE" 'bus_.poll('

# ExchangeGateway must not source business time directly from host wall-clock.
forbid "$FILE" 'system_clock::now[[:space:]]*\('

# T11 must not reintroduce the retired distributed-clock control plane.
forbid "$FILE" 'ServiceClockContext|SimulatedClock|ClockState|ClockControl|ClockSyncRequest|simulation\.clock\.|--runtime-mode|--simulation-id|simulation_id|runtime_mode'

# Preserve exchange/backend truth: T11 validates timestamps, it does not overwrite them.
if grep -Eq 'value\.(update|fill|snapshot)\.timestamp[[:space:]]*=' "$FILE"; then
    fail "gateway must not rewrite backend event timestamps"
fi
if grep -Eq 'value\.metadata\.produced_at[[:space:]]*=' "$FILE"; then
    fail "gateway must not rewrite backend produced_at"
fi

echo "PASS: T11 ExchangeGateway business/technical time separation audit"
