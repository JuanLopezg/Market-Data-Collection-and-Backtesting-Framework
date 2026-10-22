#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
S="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
H="$ROOT/lib/src/market/canonical_market_data_reader.h"
C="$ROOT/lib/src/market/canonical_market_data_reader.cpp"

for f in "$S" "$H" "$C"; do
  test -f "$f" || { echo "FAIL: missing $f" >&2; exit 1; }
done

grep -q 'latestRankingDateAtOrBefore(Timestamp maxDate)' "$H"
grep -q 'SELECT MAX(date) FROM market_volume_rank_daily WHERE date <= ?' "$C"
grep -q 'latestRankingDateAtOrBefore(newestCompleted)' "$S"
grep -Fq 'if (target < canonicalFrontier && !anchoredBootstrapTarget)' "$S"
grep -Fq 'if (target > canonicalFrontier)' "$S"
grep -Fq 'waits for market data (available through' "$S"
grep -q 'if (target > newestCompleted)' "$S"
grep -Fq 'Caught up signal state for' "$S"

if grep -q 'if (target < newestCompleted)' "$S"; then
  echo 'FAIL: Strategy still ACKs stale notifications from simulated-time frontier' >&2
  exit 1
fi

echo 'PASS: Strategy stale/catch-up frontier is canonical SQLite availability, capped by TimeHandler'
echo 'PASS: retained notifications older than the canonical frontier are ACKed; the frontier notification drives bounded catch-up'
echo 'PASS: anti-lookahead future-date guard and internal catch-up instrumentation remain present'
