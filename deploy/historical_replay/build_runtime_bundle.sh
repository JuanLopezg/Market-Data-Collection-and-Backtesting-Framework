#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
BUNDLE="$SCRIPT_DIR/.runtime_bundle"

# Historical replay runtime bundle, including the T15 historical feeder.
BINARIES=(
    "$BUILD/live_trading/historical_market_data_service/src/algotrading_historical_market_data_service"
    "$BUILD/live_trading/strategy_service/src/algotrading_strategy_service"
    "$BUILD/live_trading/portfolio_risk_service/src/algotrading_portfolio_risk_service"
    "$BUILD/live_trading/order_planner_service/src/algotrading_order_planner_service"
    "$BUILD/live_trading/execution_state_service/src/algotrading_execution_state_service"
    "$BUILD/live_trading/exchange_gateway/src/algotrading_exchange_gateway"
    "$BUILD/live_trading/simulated_exchange_service/src/algotrading_simulated_exchange_service"
)
ALGOLIB="$BUILD/lib/src/libalgolib.so"

for file in "${BINARIES[@]}" "$ALGOLIB"; do
    [[ -e "$file" ]] || { echo "Missing historical-replay runtime artifact: $file" >&2; exit 1; }
done

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/bin" "$BUNDLE/lib" "$BUNDLE/config"
cp "$SCRIPT_DIR/runtime.Dockerfile" "$BUNDLE/Dockerfile"
cp -a "$ROOT/config/." "$BUNDLE/config/"
cp "$ALGOLIB" "$BUNDLE/lib/"
for file in "${BINARIES[@]}"; do
    cp "$file" "$BUNDLE/bin/"
done

is_glibc_runtime() {
    case "$1" in
        libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|ld-linux-*.so.*)
            return 0 ;;
        *) return 1 ;;
    esac
}

copy_ldd_dependencies() {
    local target="$1"
    while IFS= read -r line; do
        local path=""
        if [[ "$line" == *"=> /"* ]]; then
            path="$(awk '{print $3}' <<<"$line")"
        elif [[ "$line" =~ ^[[:space:]]*/ ]]; then
            path="$(awk '{print $1}' <<<"$line")"
        fi

        [[ -n "$path" && -f "$path" ]] || continue
        local base
        base="$(basename "$path")"
        is_glibc_runtime "$base" && continue
        cp -L "$path" "$BUNDLE/lib/$base"
    done < <(ldd "$target")
}

for file in "${BINARIES[@]}" "$ALGOLIB"; do
    copy_ldd_dependencies "$file"
done

chmod +x "$BUNDLE/bin/"*
echo "T14 historical replay runtime bundle ready: $BUNDLE"
echo "Packaged executables: ${#BINARIES[@]} (including historical feeder)"
