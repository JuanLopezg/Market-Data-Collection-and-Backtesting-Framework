#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
SRC="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
fail(){ echo "FAIL: $*" >&2; exit 1; }
grep -q 'configuredBootstrapCompletedUtcDate' "$SRC" || fail 'missing immutable bootstrap helper'
grep -q 'TimeHandlerFactory::SIMULATED_REFERENCE_ENV' "$SRC" || fail 'bootstrap anchor is not derived from shared simulated reference'
grep -q 'bootstrap_completed_date_(configuredBootstrapCompletedUtcDate(time_config_))' "$SRC" || fail 'runtime does not snapshot bootstrap anchor from immutable config'
grep -q 'target < canonicalFrontier && !anchoredBootstrapTarget' "$SRC" || fail 'moving canonical frontier can still discard the immutable bootstrap day'
grep -q 'reason=awaiting_immutable_bootstrap_date' "$SRC" || fail 'missing fail-closed protection if bootstrap notification ordering is violated'
grep -q 'event=immutable_bootstrap_business_anchor' "$SRC" || fail 'missing bootstrap anchor instrumentation'
grep -q 'latestRankingDateAtOrBefore(newestCompleted)' "$SRC" || fail 'T21 canonical-frontier catch-up was lost'
grep -q 'signal_state_catchup_processed' "$SRC" || fail 'T21 internal catch-up instrumentation was lost'
grep -q 'bus_.poll(market_update_subscription_, 32, fetchTimeoutMs)' "$SRC" || fail 'T21 fast retained-backlog drain was lost'
if grep -q 'std::chrono::system_clock::now' "$SRC"; then fail 'Strategy reintroduced direct system_clock business time'; fi
echo 'PASS: fresh accelerated bootstrap is anchored to immutable shared simulated-reference, not sampled startup time'
echo 'PASS: moving canonical frontier cannot skip the reference-completed bootstrap day'
echo 'PASS: T21 canonical catch-up and fast retained-backlog drain remain intact'
