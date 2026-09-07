#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_ROOT="$ROOT/validation/logs/restart_suite"
STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_ROOT="$LOG_ROOT/$STAMP"
SCOPE="${1:-all}"

case "$SCOPE" in
    all|core|remaining) ;;
    *)
        echo "Usage: bash validation/restart_suite.sh [all|core|remaining]" >&2
        echo "  all       33A + 33B + 33C + all 33D service cases (default)" >&2
        echo "  core      33A + 33B + 33C only" >&2
        echo "  remaining all 33D service cases only" >&2
        exit 2
        ;;
esac

mkdir -p "$RUN_ROOT"

START_EPOCH="$(date +%s)"
CURRENT_CASE=""

cleanup_wrapper() {
    local code=$?
    if [[ "$code" != "0" ]]; then
        echo
        echo "[FAIL] STEP 33E stopped at: ${CURRENT_CASE:-unknown}"
        echo "[INFO] suite logs: $RUN_ROOT"
        echo "[INFO] the failing child harness prints the exact preserved Compose project/cleanup command."
    fi
}
trap cleanup_wrapper EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

run_case() {
    local label="$1"
    local logfile="$2"
    shift 2
    CURRENT_CASE="$label"

    echo
    echo "============================================================"
    echo "STEP 33E CASE — $label"
    echo "Log: $logfile"
    echo "============================================================"

    set +e
    "$@" 2>&1 | tee "$logfile"
    local child_status=${PIPESTATUS[0]}
    set -e

    if (( child_status != 0 )); then
        echo "[FAIL] $label"
        return "$child_status"
    fi

    echo "[PASS] $label"
}

# Standalone restart scripts force a runtime rebuild by default. In the suite we
# build exactly once in the first executed group, then reuse that same image for
# all following isolated Compose topologies.
export RESTART_SKIP_RUNTIME_BUILD=0

case "$SCOPE" in
    all|core)
        run_case "33A Strategy hard restart" \
            "$RUN_ROOT/01_strategy.log" \
            bash "$SCRIPT_DIR/restart_strategy.sh"

        export RESTART_SKIP_RUNTIME_BUILD=1

        run_case "33B ReplayController mid-phase hard restart" \
            "$RUN_ROOT/02_replay_controller.log" \
            bash "$SCRIPT_DIR/restart_replay_controller.sh"

        run_case "33C SimulatedExchange hard restart" \
            "$RUN_ROOT/03_simulated_exchange.log" \
            bash "$SCRIPT_DIR/restart_simulated_exchange.sh"
        ;;
esac

case "$SCOPE" in
    all)
        # Runtime image was already rebuilt by 33A.
        export RESTART_SKIP_RUNTIME_BUILD=1
        run_case "33D remaining-service restart audit" \
            "$RUN_ROOT/04_remaining_services.log" \
            bash "$SCRIPT_DIR/restart_remaining_services.sh"
        ;;
    remaining)
        # No earlier suite case ran, so let the first 33D case rebuild once.
        export RESTART_SKIP_RUNTIME_BUILD=0
        run_case "33D remaining-service restart audit" \
            "$RUN_ROOT/01_remaining_services.log" \
            bash "$SCRIPT_DIR/restart_remaining_services.sh"
        ;;
    core) ;;
esac

END_EPOCH="$(date +%s)"
ELAPSED=$((END_EPOCH - START_EPOCH))
CURRENT_CASE=""

echo
echo "============================================================"
echo "STEP 33E RESULT: PASS"
echo "Scope        : $SCOPE"
echo "Elapsed      : ${ELAPSED}s"
echo "Suite logs   : $RUN_ROOT"
case "$SCOPE" in
    all)
        echo "Validated    : 33A + 33B + 33C + all 33D service cases"
        echo "Guarantee    : every restart path completed and remained distributed == fast exact"
        ;;
    core)
        echo "Validated    : 33A + 33B + 33C"
        echo "Note         : 33D remaining-service cases were intentionally not run"
        ;;
    remaining)
        echo "Validated    : all STEP 33D remaining-service cases through the STEP 33E orchestrator"
        echo "Note         : already-closed 33A/33B/33C cases were intentionally not rerun"
        ;;
esac
echo "============================================================"
