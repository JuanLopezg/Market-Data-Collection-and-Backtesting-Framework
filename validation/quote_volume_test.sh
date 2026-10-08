#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
includes=()
while IFS= read -r dir; do includes+=("-I$dir"); done < <(find "$ROOT/lib/src" -type d)
g++ -std=c++20 -O0 "${includes[@]}" -I"$ROOT/live_trading/market_data_service/src" \
    "$ROOT/validation/quote_volume_test.cpp" "$ROOT/live_trading/market_data_service/src/market_store.cpp" \
    -L"$ROOT/build/lib/src" -Wl,-rpath,"$ROOT/build/lib/src" -lalgolib -lsqlite3 -lfmt -llog4cpp -o "$TMP/test"
"$TMP/test" "$TMP/market.db"
