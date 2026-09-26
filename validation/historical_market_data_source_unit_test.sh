#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
OUT="${TMPDIR:-/tmp}/algotrading_t15_historical_source_test"

g++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/lib/src/data_types" \
  -I"$ROOT/live_trading/historical_market_data_service/src" \
  "$ROOT/validation/historical_market_data_source_unit_test.cpp" \
  "$ROOT/live_trading/historical_market_data_service/src/historical_csv_source.cpp" \
  -o "$OUT"

"$OUT"
rm -f "$OUT"
