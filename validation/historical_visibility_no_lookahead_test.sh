#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
OUT="${TMPDIR:-/tmp}/algotrading_t16_historical_visibility"

g++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/lib/src/data_types" \
  -I"$ROOT/lib/src/utils" \
  -I"$ROOT/live_trading/historical_market_data_service/src" \
  "$ROOT/validation/historical_visibility_no_lookahead_test.cpp" \
  "$ROOT/live_trading/historical_market_data_service/src/historical_csv_source.cpp" \
  "$ROOT/lib/src/utils/time_handler.cpp" \
  -o "$OUT"

"$OUT"
rm -f "$OUT"
