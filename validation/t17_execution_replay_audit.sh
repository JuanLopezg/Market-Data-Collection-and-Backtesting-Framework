#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"

echo "============================================================"
echo "HISTORICAL EXECUTION BRIDGE / OPEN-ONLY PARSER AUDIT"
echo "============================================================"

python3 - <<'PY'
from pathlib import Path

feeder = Path('live_trading/historical_market_data_service/src/historical_market_data_service_main.cpp').read_text()
source_h = Path('live_trading/historical_market_data_service/src/historical_csv_source.h').read_text()
source_cpp = Path('live_trading/historical_market_data_service/src/historical_csv_source.cpp').read_text()
execution = Path('live_trading/execution_state_service/src/execution_state_service_main.cpp').read_text()

assert 'class HistoricalOpenCsvSource' in source_h
assert 'Parse exactly the visible-at-open prefix' in source_cpp
assert 'std::getline(stream, openField' in source_cpp
assert 'opening prices for' in feeder
assert 'MessageSubjects::EXECUTION_PRICES' in feeder
assert '*nextOpen > businessDate' in feeder
assert 'previousDay(static_cast<unsigned int>(day.date))' in feeder

assert 'MessageSubjects::NOTIONAL_ORDER_PLAN' in execution
assert 'execution-state-notional-plans' in execution
assert 'order.notional_usd / order.reference_close' in execution
assert 'nextDay(static_cast<unsigned int>(plan.decision_timestamp))' in execution
assert 'engine_.applyOrderPlan(' in execution
assert 'Applied order plan for' in execution

# T17 must not reintroduce shared-clock transport/authority.
for forbidden in ('ClockState', 'CLOCK_STATE', 'CLOCK_SYNC_REQUEST', 'CLOCK_CONTROL'):
    assert forbidden not in feeder, f'feeder reintroduced {forbidden}'
    assert forbidden not in execution, f'execution-state reintroduced {forbidden}'

print('PASS: current historical structural execution bridge audit')
PY

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/open_source_test.cpp" <<'CPP'
#include <cassert>
#include <fstream>
#include <string>
#include "historical_csv_source.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = argv[1];
    {
        std::ofstream out(path);
        out << "date,symbol,open,high,low,close,volume\n";
        out << "2020-04-10,BTC,100,NOT_VISIBLE,NOT_VISIBLE,NOT_VISIBLE,NOT_VISIBLE\n";
        out << "2020-04-10,ETH,20,NOT_VISIBLE,NOT_VISIBLE,NOT_VISIBLE,NOT_VISIBLE\n";
        out << "2020-04-11,BTC,101,STILL_HIDDEN,STILL_HIDDEN,STILL_HIDDEN,STILL_HIDDEN\n";
    }

    HistoricalOpenCsvSource source(path);
    assert(source.nextDate().value() == 20200410);
    const auto first = source.readNextDay();
    assert(first.date == 20200410);
    assert(first.prices.size() == 2);
    assert(first.prices.at("BTC") == 100.0);
    assert(first.prices.at("ETH") == 20.0);
    assert(source.nextDate().value() == 20200411);
    const auto second = source.readNextDay();
    assert(second.date == 20200411);
    assert(second.prices.at("BTC") == 101.0);
    return 0;
}
CPP

g++ -std=c++20 -Wall -Wextra -Werror \
  -Ilib/src/data_types \
  -Ilive_trading/historical_market_data_service/src \
  "$TMP/open_source_test.cpp" \
  live_trading/historical_market_data_service/src/historical_csv_source.cpp \
  -o "$TMP/open_source_test"
"$TMP/open_source_test" "$TMP/open_source_unit.csv"

echo "PASS: T17 open-only anti-lookahead parser unit test"
echo "PASS: historical execution bridge and open-only parser audit"
