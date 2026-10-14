#!/usr/bin/env bash
# Current-code runner for the canonical venue / MOCK exchange / replay-runtime component
# tests (formerly the numbered Step47a-Step57 gates).
#
# Those gates also verified SHA-256 manifests of the source snapshot they were accepted
# on; the restructure moved and changed those files, so the manifests can no longer
# match, and regenerating them would prove nothing (validation/README.md). The frozen
# specifications stay in docs/venue/step*/. This script builds the current library
# sources once and runs every component test against them.
#
# Usage: bash validation/mock_component_tests.sh            (WSL/Linux, needs g++ and libfmt)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-c++}"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

INCLUDES=()
for dir in "$ROOT"/lib/src/*/; do
    INCLUDES+=("-I$dir")
done
INCLUDES+=("-I$ROOT/lib/src")

# Library sources the tests link against (the canonical replay binary uses the same set).
SOURCES=(
    lib/src/exchange/mock/orders.cpp
    lib/src/exchange/mock/matching.cpp
    lib/src/exchange/mock/account.cpp
    lib/src/exchange/mock/recovery.cpp
    lib/src/exchange/mock/recovery_codec.cpp
    lib/src/exchange/mock/reconciliation.cpp
    lib/src/exchange/mock/faults.cpp
    lib/src/runtime/replay_runtime.cpp
    lib/src/strategy/strategy_signal_engine.cpp
    lib/src/strategy/strategy.cpp
    lib/src/risk/portfolio_risk_engine.cpp
    lib/src/risk/target_decisions.cpp
    lib/src/execution/planning/notional_order_planner.cpp
    lib/src/market/rolling_market_state.cpp
    lib/src/universe/liquidity_universe.cpp
    lib/src/ranker/indicator_ranker.cpp
    lib/src/indicator/indicator_engine.cpp
    lib/src/indicator/indicator_calculators.cpp
    lib/src/indicator/indicator_spec.cpp
    lib/src/utils/time_handler.cpp
    lib/src/logging/logger.cpp
    lib/src/recovery/reconciler.cpp
    lib/src/account/position_ledger.cpp
)

# Test file, the language standard it was written for, and extra sources it needs.
TESTS=(
    "step47a_multi_exchange_architecture_test.cpp c++17"
    "step48_canonical_multi_venue_adapter_test.cpp c++17"
    "step49_mock_venue_catalog_test.cpp c++17"
    "step50_mock_order_admission_lifecycle_test.cpp c++17"
    "step51_deterministic_matching_fill_test.cpp c++17"
    "step52_mock_account_margin_positions_accounting_test.cpp c++17"
    "step53_mock_snapshot_user_stream_recovery_test.cpp c++17"
    "step54_mock_reconciliation_ledger_parity_test.cpp c++17"
    "step55_fault_chaos_rate_limit_test.cpp c++17"
    "step56_full_system_replay_runtime_test.cpp c++20"
    "step57_manual_control_mock_pipeline_test.cpp c++20"
    "order_manager_stale_update_test.cpp c++20"
    "reconciler_quantity_tolerance_test.cpp c++20"
    "position_ledger_test.cpp c++20"
    "manual_chain_test.cpp c++20 lib/src/transport/message_json.cpp lib/src/execution/execution_engine.cpp lib/src/execution/planning/quantity_order_planner.cpp lib/src/execution/planning/planning_state.cpp live_trading/execution_state_service/src/manual_requests.cpp"
)
INCLUDES+=("-I$ROOT/live_trading/execution_state_service/src")

echo "Building ${#SOURCES[@]} library sources once"
OBJECTS=()
for source in "${SOURCES[@]}"; do
    object="$BUILD/$(echo "$source" | tr '/' '_').o"
    "$CXX" -std=c++20 -O1 -Wall -Wextra -Werror -pedantic "${INCLUDES[@]}" -c "$ROOT/$source" -o "$object"
    OBJECTS+=("$object")
done

failed=()
for entry in "${TESTS[@]}"; do
    read -r test standard extra <<<"$entry"
    name="${test%.cpp}"
    printf '%-60s ' "$name"
    extra_sources=()
    for source in ${extra:-}; do
        extra_sources+=("$ROOT/$source")
    done
    if ! "$CXX" -std="$standard" -O1 -Wall -Wextra -Werror -pedantic "${INCLUDES[@]}" \
            "$ROOT/validation/$test" "${extra_sources[@]}" "${OBJECTS[@]}" -pthread -lfmt -o "$BUILD/$name" 2>"$BUILD/$name.compile.log"; then
        echo "COMPILE FAIL"
        sed -n '1,20p' "$BUILD/$name.compile.log"
        failed+=("$name")
        continue
    fi
    if (cd "$BUILD" && "$BUILD/$name" >"$BUILD/$name.log" 2>&1); then
        echo "PASS"
    else
        echo "FAIL"
        tail -20 "$BUILD/$name.log"
        failed+=("$name")
    fi
done

if ((${#failed[@]})); then
    echo "Component tests failed: ${failed[*]}"
    exit 1
fi
echo "All ${#TESTS[@]} component tests pass"
