#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OUT="${2:-$ROOT/deploy/historical_replay/run/step58_dashboard/bin/step58_dashboard_simulation_runner}"
CXX="${CXX:-c++}"
JOBS="${STEP58_BUILD_JOBS:-}"
if [[ -z "$JOBS" ]]; then
  JOBS="$(nproc 2>/dev/null || printf '4')"
fi
if (( JOBS > 8 )); then JOBS=8; fi
if (( JOBS < 1 )); then JOBS=1; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/obj" "$(dirname "$OUT")"

INCLUDES=(
 -I"$ROOT/lib/src/account" -I"$ROOT/lib/src/analytics" -I"$ROOT/lib/src/logging"
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/data_types" -I"$ROOT/lib/src/exchange"
 -I"$ROOT/lib/src/execution" -I"$ROOT/lib/src/filter" -I"$ROOT/lib/src/indicator"
 -I"$ROOT/lib/src/market" -I"$ROOT/lib/src/portfolio" -I"$ROOT/lib/src/position"
 -I"$ROOT/lib/src/ranker" -I"$ROOT/lib/src/rebalance" -I"$ROOT/lib/src/risk"
 -I"$ROOT/lib/src/runtime" -I"$ROOT/lib/src/utils" -I"$ROOT/lib/src/signal" -I"$ROOT/lib/src/sizing"
 -I"$ROOT/lib/src/strategy" -I"$ROOT/lib/src/universe"
)
printf '%s\n' "${INCLUDES[@]}" > "$TMP/includes.rsp"
SOURCES=(
 tools/historical_replay/step58_dashboard_simulation_runner.cpp
 lib/src/strategy/strategy_signal_engine.cpp
 lib/src/risk/portfolio_risk_engine.cpp
 lib/src/execution/planning/notional_order_planner.cpp
 lib/src/market/rolling_market_state.cpp
 lib/src/utils/time_handler.cpp
 lib/src/strategy/strategy.cpp
 lib/src/universe/liquidity_universe.cpp
 lib/src/ranker/indicator_ranker.cpp
 lib/src/indicator/indicator_engine.cpp
 lib/src/indicator/indicator_calculators.cpp
 lib/src/indicator/indicator_spec.cpp
)

# Compile in bounded parallelism. GCC/Clang read the include list from a response file.
: > "$TMP/jobs"
for i in "${!SOURCES[@]}"; do printf '%s\t%s\n' "$i" "${SOURCES[$i]}" >> "$TMP/jobs"; done
export ROOT TMP CXX
xargs -P "$JOBS" -n 2 bash -c '
  set -euo pipefail
  idx="$1"; rel="$2"
  "$CXX" -std=c++20 -Wall -Wextra -Werror -pedantic @"$TMP/includes.rsp" -c "$ROOT/$rel" -o "$TMP/obj/$idx.o"
' _ < "$TMP/jobs"

"$CXX" "$TMP"/obj/*.o -pthread -o "$TMP/runner"
chmod 0755 "$TMP/runner"
mv "$TMP/runner" "$OUT"
printf 'STEP58_BUILD_OK=%s\n' "$OUT"
