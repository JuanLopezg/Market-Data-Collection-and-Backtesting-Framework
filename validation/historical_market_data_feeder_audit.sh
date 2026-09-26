#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
MAIN="$ROOT/live_trading/historical_market_data_service/src/historical_market_data_service_main.cpp"
SOURCE="$ROOT/live_trading/historical_market_data_service/src/historical_csv_source.cpp"
STRATEGY="$ROOT/live_trading/strategy_service/src/strategy_service_main.cpp"
EXECUTION="$ROOT/live_trading/execution_state_service/src/execution_state_service_main.cpp"
COMPOSE="$ROOT/deploy/historical_replay/docker-compose.yml"
BUNDLE="$ROOT/deploy/historical_replay/build_runtime_bundle.sh"
LIVE_MESON="$ROOT/live_trading/meson.build"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

for f in "$MAIN" "$SOURCE" "$STRATEGY" "$EXECUTION" "$COMPOSE" "$BUNDLE" "$LIVE_MESON"; do
  [[ -f "$f" ]] || fail "missing $f"
done

echo "============================================================"
echo "T15 — HISTORICAL MARKET-DATA FEEDER AUDIT"
echo "============================================================"

FORBIDDEN='ServiceClockContext|SimulatedClock|ClockState|ClockControl|ClockSyncRequest|simulation\.clock\.|--runtime-mode|--simulation-id|replay-controller'
if grep -En "$FORBIDDEN" "$MAIN" "$SOURCE"; then
  fail "historical feeder contains legacy shared-clock dependency"
fi
pass "historical feeder has no legacy shared-clock dependency"

grep -q 'TimeHandlerFactory::loadConfigFromEnvironment' "$MAIN" || fail "feeder does not load common TimeHandler config"
grep -q 'newestCompletedBusinessUtcDate(timeHandler)' "$MAIN" || fail "feeder does not gate visibility by TimeHandler business time"
if grep -Eq 'loadDatabase\(|loadDatabaseFromCSV\(' "$MAIN" "$SOURCE"; then
  fail "feeder must not preload the complete historical dataset through database_utils"
fi
grep -q 'peekDateOnly' "$SOURCE" || fail "source must inspect the next date without parsing future OHLCV"
grep -q 'no future OHLCV row has been consumed or parsed' "$SOURCE" || fail "future-row boundary invariant is not explicit"
pass "historical visibility is TimeHandler-gated and source is sequential/date-only ahead of visibility"

python3 - "$MAIN" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding='utf-8')
commit = text.find('store.commitDailyUpdate(')
publish = text.find('publishDay(bus, options, day);')
if commit < 0 or publish < 0 or not commit < publish:
    raise SystemExit('SQLite commit must appear before publish in commitAndPublish')
future_check = text.find('if (*next > newestCompleted)')
read_day = text.find('pending = source.readNextDay();')
if future_check < 0 or read_day < 0 or not future_check < read_day:
    raise SystemExit('feeder must check business visibility before consuming next source day')
print('ordering-ok')
PY
pass "commit-before-publish and pre-read visibility ordering are explicit"

grep -q -- '--market-data-source' "$STRATEGY" || fail "Strategy lacks configurable market-data source"
grep -q 'update.source != options_.market_data_source' "$STRATEGY" || fail "Strategy still hardcodes source identity"
grep -q -- '--market-data-source' "$EXECUTION" || fail "ExecutionState lacks configurable market-data source"
grep -q 'update.source != options_.market_data_source' "$EXECUTION" || fail "ExecutionState still hardcodes source identity"
pass "downstream source identity is configurable; LIVE default remains binance"

python3 - "$COMPOSE" <<'PY'
import sys, yaml
from pathlib import Path
doc = yaml.safe_load(Path(sys.argv[1]).read_text(encoding='utf-8'))
services = doc['services']
feed = services['historical-market-data']
strategy = services['strategy']
execution = services['execution-state']

def command(svc):
    return [str(x) for x in svc.get('command', [])]

fc = command(feed)
sc = command(strategy)
ec = command(execution)
for required in ('--source-id', '--market-top-n'):
    if required not in fc:
        raise SystemExit(f'feeder missing {required}')
if '--market-data-source' not in sc or '--market-data-source' not in ec:
    raise SystemExit('downstream services missing historical source configuration')
if '${HISTORICAL_MARKET_TOP_N:-1000000}' not in fc or '${HISTORICAL_MARKET_TOP_N:-1000000}' not in sc:
    raise SystemExit('feeder/strategy must share HISTORICAL_MARKET_TOP_N')
if '${HISTORICAL_MARKET_DATA_SOURCE:-historical-cmc}' not in fc or '${HISTORICAL_MARKET_DATA_SOURCE:-historical-cmc}' not in sc or '${HISTORICAL_MARKET_DATA_SOURCE:-historical-cmc}' not in ec:
    raise SystemExit('feeder/consumers must share HISTORICAL_MARKET_DATA_SOURCE')
if feed.get('profiles') != ['runtime']:
    raise SystemExit('historical feeder must be part of runtime profile')
print('compose-wiring-ok')
PY
pass "historical feeder and consumers share source/top-N configuration"

grep -q 'algotrading_historical_market_data_service' "$BUNDLE" || fail "runtime bundle does not package feeder"
grep -q "subdir('historical_market_data_service')" "$LIVE_MESON" || fail "Meson does not include historical feeder service"
pass "historical feeder is wired into build and runtime bundle"

echo "PASS: T15 historical market-data feeder audit"
