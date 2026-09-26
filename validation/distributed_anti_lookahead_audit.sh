#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"

FEEDER="$ROOT/live_trading/historical_market_data_service/src/historical_market_data_service_main.cpp"
SOURCE="$ROOT/live_trading/historical_market_data_service/src/historical_csv_source.cpp"
READER="$ROOT/lib/src/market/canonical_market_data_reader.cpp"
STRATEGY="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
RISK="$ROOT/live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp"
EXECUTION="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"
PLANNER="$ROOT/live_trading/order_planner_service/src/order_planner_service_main.cpp"
GATEWAY="$ROOT/live_trading/exchange_gateway/src/exchange_gateway_main.cpp"
SIM="$ROOT/live_trading/simulated_exchange_service/src/simulated_exchange_service_main.cpp"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }
need() { grep -Fq -- "$2" "$1" || fail "$3"; }

for f in "$FEEDER" "$SOURCE" "$READER" "$STRATEGY" "$RISK" "$EXECUTION" "$PLANNER" "$GATEWAY" "$SIM"; do
  [[ -f "$f" ]] || fail "missing $f"
done

echo "============================================================"
echo "T16 — DISTRIBUTED ANTI-LOOKAHEAD AUDIT"
echo "============================================================"

# 1) Historical visibility: the date boundary is checked before consuming/parsing the
# next day's OHLCV. SQLite ahead of business time is rejected on startup.
need "$FEEDER" 'if (*next > newestCompleted)' 'historical feeder does not stop before a future source date'
need "$FEEDER" 'pending = source.readNextDay();' 'historical feeder does not consume days sequentially'
need "$FEEDER" 'if (checkpoint > newestAtStartup)' 'historical feeder accepts SQLite ahead of business time'
need "$SOURCE" 'break; // Crucially, no future OHLCV row has been consumed or parsed.' 'historical CSV source future-row boundary missing'
python3 - "$FEEDER" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding='utf-8')
check = text.find('if (*next > newestCompleted)')
read = text.find('pending = source.readNextDay();')
if check < 0 or read < 0 or check >= read:
    raise SystemExit('future-date visibility check must occur before readNextDay')
PY
pass "historical feeder checks visibility before consuming future OHLCV"

# 2) Canonical SQLite reader: every strategy/risk window has an SQL upper bound equal
# to the requested economic date; execution closes are exact-date only.
need "$READER" 'WHERE date BETWEEN ? AND ?' 'canonical reader lacks bounded OHLCV window'
need "$READER" 'sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(date));' 'canonical reader does not bind requested date as upper bound'
need "$READER" 'SELECT pair, close FROM ohlcv_data WHERE date = ?' 'execution close reader is not exact-date scoped'
need "$READER" 'WHERE date = ? AND rank <= ?' 'strategy ranking is not target-date scoped'
pass "canonical ranking/history/reference-close queries are bounded by T"

# 3) Strategy: reject future MarketDataUpdated before constructing indicator/universe
# inputs. Strategy engine receives only the reader's bounded raw_data/market_data.
need "$STRATEGY" 'if (target > newestCompleted)' 'Strategy lacks future MarketDataUpdated guard'
need "$STRATEGY" 'market_reader_.loadWindow(' 'Strategy does not use bounded canonical window'
need "$STRATEGY" 'engine_->onBarClose(window.raw_data, window.market_data, date)' 'Strategy bypasses bounded window for indicator/universe calculation'
pass "Strategy indicator windows and universe are downstream of bounded date T"

# 4) PortfolioRisk: both account and signal dates are future-gated and the lookback is
# loaded through the bounded reader ending at target T.
need "$RISK" 'if (snapshot.timestamp > newestCompleted)' 'PortfolioRisk lacks future AccountSnapshot guard'
need "$RISK" 'if (target > newestCompleted)' 'PortfolioRisk lacks future StrategyIntent guard'
need "$RISK" 'market_reader_.loadMarketDataWindow(' 'PortfolioRisk does not use bounded market lookback'
need "$RISK" 'target,' 'PortfolioRisk bounded reader is not keyed by target T'
pass "PortfolioRisk inputs/lookbacks are future-gated and bounded by T"

# 5) ExecutionState / OrderPlanner: future decisions/updates/requests are rejected; the
# only market price used for planning is exact close(T).
need "$EXECUTION" 'if (update.completed_through > newestCompleted)' 'ExecutionState lacks future market-update guard'
need "$EXECUTION" 'if (batch.decision_timestamp > newestCompleted)' 'ExecutionState lacks future decision guard'
need "$EXECUTION" 'loadExactClosingPrices(decision.decision_timestamp, symbols)' 'ExecutionState does not use exact close(T)'
need "$PLANNER" 'if (request.decision_timestamp > newestCompleted)' 'OrderPlanner lacks future planning-request guard'
pass "ExecutionState and OrderPlanner cannot plan from future daily economics"

# 6) Gateway / simulated exchange: edge events are checked against local business time.
need "$GATEWAY" 'if (eventTime > businessToday || producedAt > businessToday)' 'Gateway backend-event future guard missing'
need "$GATEWAY" 'plan.decision_timestamp > newestCompleted' 'Gateway future plan guard missing'
need "$SIM" 'if (timestamp <= businessNow)' 'SimulatedExchange business-time readiness check missing'
need "$SIM" 'businessTimeReady(prices.timestamp, "execution_prices_market_time")' 'SimulatedExchange does not gate execution-price market time'
need "$SIM" 'businessTimeReady(command.order.created_at, "backend_submit_order_created_at")' 'SimulatedExchange does not gate submit event time'
need "$SIM" 'businessTimeReady(command.requested_at, "backend_cancel_order_requested_at")' 'SimulatedExchange does not gate cancel event time'
pass "Gateway and SimulatedExchange reject/retry future economic events"

# T16 must not solve anti-lookahead by reintroducing the old distributed clock.
if grep -En 'ServiceClockContext|ClockState|ClockControl|ClockSyncRequest|simulation\.clock\.' \
    "$FEEDER" "$STRATEGY" "$RISK" "$EXECUTION" "$PLANNER" "$GATEWAY" "$SIM"; then
  fail "legacy shared-clock dependency found in active historical replay path"
fi
pass "anti-lookahead path remains local-TimeHandler only"

echo "PASS: T16 distributed anti-lookahead audit"
