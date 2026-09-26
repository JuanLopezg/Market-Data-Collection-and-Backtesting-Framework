#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
fail=0

say_fail() { echo "FAIL: $*" >&2; fail=1; }

# Direct sqlite3 C API for the canonical market database must not be spread
# through LIVE service code.  Do NOT scan all of lib/src here: that tree also
# contains unrelated SQLite users (backtest result storage, legacy/test state
# stores, generic database utilities) which are outside this LIVE market-data
# ownership rule.
while IFS= read -r hit; do
  [[ -z "$hit" ]] && continue
  case "$hit" in
    *"live_trading/market_data_service/src/market_data_store.cpp"*) ;;
    *) say_fail "unexpected direct SQLite C API use in LIVE service: $hit" ;;
  esac
done < <(grep -RIn --exclude='*.md' --exclude='*.txt' --exclude='*.sh' -E 'sqlite3_(open|prepare|step|exec|bind|column|close|finalize)' \
  "$ROOT/live_trading" 2>/dev/null || true)

# The shared canonical reader is the one intentionally approved read-only SQLite
# implementation for LIVE consumers. Its safety is checked below (exact date +
# PRAGMA query_only=ON), so unrelated lib SQLite implementations are irrelevant
# to this audit.

# Only Strategy, PortfolioRisk and ExecutionState should expose the canonical
# market DB as a LIVE read dependency. OrderPlanner/Gateway must consume data
# already carried in contracts.
for service in order_planner_service exchange_gateway; do
  if grep -RIn --exclude='*.md' --exclude='*.txt' -E 'CanonicalMarketDataReader|--market-data-db|market_data_db' \
      "$ROOT/live_trading/$service" >/dev/null 2>&1; then
    say_fail "$service must not directly open canonical SQLite"
  fi
done

for service in strategy_service portfolio_risk_service execution_state_service; do
  if ! grep -RIn --exclude='*.md' --exclude='*.txt' 'CanonicalMarketDataReader' \
      "$ROOT/live_trading/$service" >/dev/null 2>&1; then
    say_fail "$service is expected to use the shared read-only canonical reader"
  fi
done

EXEC="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"
PLANNER="$ROOT/live_trading/order_planner_service/src/order_planner_service_main.cpp"
READER_H="$ROOT/lib/src/market/canonical_market_data_reader.h"
READER_CPP="$ROOT/lib/src/market/canonical_market_data_reader.cpp"

# Exact T lookup: no generic/fallback naming and no fallback SQL pattern.
grep -q 'loadExactClosingPrices' "$EXEC" || say_fail "ExecutionState must source exact close(T) through loadExactClosingPrices"
grep -q 'loadExactClosingPrices' "$READER_H" || say_fail "canonical reader exact-close API missing"
if grep -q 'loadClosingPrices' "$READER_H" "$READER_CPP" "$EXEC"; then
  say_fail "legacy ambiguous loadClosingPrices API still present"
fi

grep -q 'WHERE date = ? AND pair IN' "$READER_CPP" || say_fail "exact close(T) SQL query missing"
grep -q 'missing close(T)' "$READER_CPP" || say_fail "exact close(T) missing-symbol fail-safe missing"
grep -q 'PRAGMA query_only=ON' "$READER_CPP" || say_fail "read-only reader query_only hardening missing"

# Same snapshot must be propagated request -> output unchanged; planner must not requery market DB.
grep -q 'output.reference_closes = request.reference_closes;' "$PLANNER" || \
  say_fail "OrderPlanner must propagate the exact reference close snapshot unchanged"
grep -q 'requiredReferenceCloseSymbols' "$PLANNER" || \
  say_fail "OrderPlanner exact reference-close coverage validation missing"

# Unrelated account inventory must not expand the market DB dependency for planning.
if sed -n '/std::set<Coin> notionalPlanningSymbols/,/return symbols;/p' "$EXEC" | grep -q 'account().positions'; then
  say_fail "ExecutionState planning close set still includes unrelated account-level inventory"
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: LIVE SQLite ownership and exact close(T) propagation audit passed."
