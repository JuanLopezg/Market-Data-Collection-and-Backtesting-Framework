#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
LOG_ROOT="$ROOT/validation/logs/logging_smoke"
PROJECT="algotrading_logging_smoke"

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical replay harness missing: $HARNESS" >&2; exit 1; }

mkdir -p "$LOG_ROOT"

export ALGOTRADING_LOG_DEBUG=1
export ALGOTRADING_LOG_QUIET=0
export ALGOTRADING_LOG_DIR=""

echo "============================================================"
echo "PATCH 32E VALIDATION — DISTRIBUTED SERVICE LOGGING"
echo "============================================================"

bash "$HARNESS" \
  --days 5 \
  --warmup-days 30 \
  --portfolio-mode equal-weight \
  --project "$PROJECT" \
  --nats-port 54239 \
  --nats-monitor-port 58239 \
  --postgres-port 55449 \
  --log-root "$LOG_ROOT" \
  --force-runtime-build

LATEST="$(find "$LOG_ROOT" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1)"
[[ -n "$LATEST" ]] || { echo "[FAIL] logging smoke output directory not found" >&2; exit 1; }
SERVICE_LOGS="$LATEST/services"
[[ -d "$SERVICE_LOGS" ]] || { echo "[FAIL] captured service logs not found: $SERVICE_LOGS" >&2; exit 1; }

check_event() {
    local service="$1"
    local pattern="$2"
    local file="$SERVICE_LOGS/$service.log"
    [[ -f "$file" ]] || { echo "[FAIL] service log missing: $file" >&2; exit 1; }
    grep -Fq "$pattern" "$file" || {
        echo "[FAIL] expected log event missing: service=$service pattern=$pattern" >&2
        tail -n 80 "$file" >&2 || true
        exit 1
    }
    echo "[PASS] $service -> $pattern"
}

check_event market-data "event=market_slice_published"
check_event strategy "event=strategy_intents_published"
check_event portfolio-risk "event=decision_published"
check_event order-planner "event=order_plan_published"
check_event execution-state "event=reconciliation_clean"
check_event execution-state "event=execution_cycle_complete"
check_event exchange-gateway "event=snapshot_request_forwarding"
check_event simulated-exchange "event=snapshot_published"
check_event replay-controller "[REPLAY] finished cycles=35"

for service in market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange replay-controller; do
    grep -Fq "event=logger_ready" "$SERVICE_LOGS/$service.log" || {
        echo "[FAIL] logger_ready missing for $service" >&2
        exit 1
    }
done

echo
printf 'PATCH 32E RESULT: PASS\n'
printf 'Captured logs: %s\n' "$SERVICE_LOGS"
