#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
F="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
H="$ROOT/lib/src/market/canonical_market_data_reader.h"
C="$ROOT/lib/src/market/canonical_market_data_reader.cpp"

grep -q 'std::string market_data_source = "binance"' "$F"
grep -q -- '--market-data-source' "$F"
grep -q 'update.source != options_.market_data_source' "$F"
grep -q 'market_data_source={}.*time_speed={}.*time_identity={}' "$F"
grep -q 'newestCompletedBusinessUtcDate(time_handler_)' "$F"
grep -q 'TimeHandlerFactory::create' "$F"
! grep -q 'event=service_ready mode=live time_source=system_clock' "$F"
echo 'PASS: Strategy historical CLI/source contract and TimeHandler runtime restored'

grep -q 'latestRankingDateAtOrBefore' "$F"
grep -q 'canonical_frontier_not_visible' "$F"
grep -q 'target < canonicalFrontier' "$F"
grep -q 'latestRankingDateAtOrBefore' "$H"
grep -q 'CanonicalMarketDataReader::latestRankingDateAtOrBefore' "$C"
echo 'PASS: T21 stale/catch-up frontier remains canonical SQLite availability capped by TimeHandler'

grep -q 'future_completed_date' "$F"
grep -q 'signal_state_catchup_processed' "$F"
grep -q 'published=false' "$F"
echo 'PASS: anti-lookahead guard and bounded internal catch-up remain present'
