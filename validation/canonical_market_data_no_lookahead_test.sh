#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
OUT="${TMPDIR:-/tmp}/algotrading_t16_canonical_no_lookahead"

g++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$ROOT/lib/src/data_types" \
  -I"$ROOT/lib/src/market" \
  "$ROOT/validation/canonical_market_data_no_lookahead_test.cpp" \
  "$ROOT/lib/src/market/canonical_market_data_reader.cpp" \
  -lsqlite3 \
  -o "$OUT"

"$OUT"
rm -f "$OUT"
